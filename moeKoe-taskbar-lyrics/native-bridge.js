// native-bridge.js - MoeKoeMusic Native Host 隐藏桥接页
//
// 此页面由 Electron Main Process (nativeHostManager) 以隐藏窗口方式加载：
//   - 1x1 像素、skipTaskbar、show:false
//   - 带 preload.cjs → 可用 window.electronAPI.nativeHost
//   - 运行在 chrome-extension:// 上下文 → 可用 chrome.runtime
//
// 职责：在 background.js (Service Worker) 和 Electron Main Process 之间中转消息
//
// 通信链路:
//   background → chrome.runtime.Port → bridge → electronAPI.nativeHost → Main Process → exe stdin
//   exe stdout → Main Process → electronAPI.nativeHost.onMessage → bridge → Port → background

const HOST_ID = 'taskbar-lyrics-host';
const RECONNECT_BASE_DELAY_MS = 1000;
const RECONNECT_MAX_DELAY_MS = 30000;

let currentPort = null;
let reconnectAttempts = 0;
let reconnectTimer = null;

// ── 防御性检查：确保 electronAPI 已正确加载 ──
if (!window.electronAPI || !window.electronAPI.nativeHost) {
    console.error('[NativeBridge] 致命错误: electronAPI 未定义或 preload 脚本未正确加载。桥接页无法工作。');
    // 不再继续初始化，避免后续 ReferenceError 导致页面完全崩溃
} else {

// 向 background 发送消息（端口断开后 postMessage 会同步抛出，这里吞掉等待重连恢复）
function sendToBackground(message) {
    if (!currentPort) return;
    try {
        currentPort.postMessage(message);
    } catch (e) {
        console.warn('[NativeBridge] postMessage 失败（端口可能已断开）:', e && e.message);
    }
}

// ── 接收 background 的请求，转发给 Main Process（写入 EXE stdin）──
async function handleBackgroundMessage(message) {
    if (!message || !message.type) {
        return;
    }

    switch (message.type) {
        case 'native-host:status': {
            try {
                const result = await window.electronAPI.nativeHost.getStatus(HOST_ID);
                sendToBackground({
                    type: 'native-host:response',
                    requestId: message.requestId,
                    seq: message.seq,
                    result: result
                });
            } catch (e) {
                console.error('[NativeBridge] getStatus 失败:', e);
                sendToBackground({
                    type: 'native-host:response',
                    requestId: message.requestId,
                    seq: message.seq,
                    result: { success: false, message: e.message || 'Native Host 状态查询异常' }
                });
            }
            break;
        }

        case 'native-host:send': {
            try {
                const result = await window.electronAPI.nativeHost.send(HOST_ID, message.payload);
                sendToBackground({
                    type: 'native-host:response',
                    requestId: message.requestId,
                    seq: message.seq,
                    result: result
                });
            } catch (e) {
                console.error('[NativeBridge] send 失败:', e);
                sendToBackground({
                    type: 'native-host:response',
                    requestId: message.requestId,
                    seq: message.seq,
                    result: { success: false, message: e.message || 'Native Host 发送异常' }
                });
            }
            break;
        }
    }
}

// ── 断线重连：MV3 Service Worker 挂起会永久杀死 Port，桥接页存活时必须主动重连，
//    否则 background 恢复后将一直查不到 Native Host，popup 永远显示"未授权" ──
function scheduleReconnect() {
    if (reconnectTimer) return;
    const delay = Math.min(RECONNECT_BASE_DELAY_MS * Math.pow(2, reconnectAttempts), RECONNECT_MAX_DELAY_MS);
    reconnectAttempts += 1;
    console.warn(`[NativeBridge] ${delay}ms 后重连 background（第 ${reconnectAttempts} 次）...`);
    reconnectTimer = setTimeout(() => {
        reconnectTimer = null;
        try {
            connectBackgroundPort();
        } catch (e) {
            // 扩展被重载/卸载后 chrome.runtime 失效，connect 会同步抛出，继续重试没有意义
            if (e && /Extension context invalidated/i.test(e.message || '')) {
                console.error('[NativeBridge] 扩展上下文已失效，停止重连。');
                return;
            }
            console.error('[NativeBridge] 重连失败:', e);
            scheduleReconnect();
        }
    }, delay);
}

function connectBackgroundPort() {
    const port = chrome.runtime.connect({ name: 'moekoe-native-host-bridge' });
    currentPort = port;

    port.onMessage.addListener(handleBackgroundMessage);

    port.onDisconnect.addListener(() => {
        // 归属校验：仅当前端口的断开事件才清空 currentPort
        if (currentPort === port) {
            currentPort = null;
        }
        console.warn('[NativeBridge] Background port disconnected');
        scheduleReconnect();
    });

    // 连接稳定存活 30s 才清零退避计数，避免 SW 反复秒断时退避被不断重置
    setTimeout(() => {
        if (currentPort === port) {
            reconnectAttempts = 0;
        }
    }, 30000);

    console.log('[NativeBridge] 已连接到 background');
}

connectBackgroundPort();

// ── 接收 Main Process 转发的 EXE stdout 消息，转发给 background ──
window.electronAPI.nativeHost.onMessage((payload) => {
    if (!payload || payload.hostId !== HOST_ID) {
        return;
    }

    sendToBackground({
        type: 'native-host:event',
        payload: payload.message
    });
});

} // end of electronAPI guard
