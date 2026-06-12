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

// ── 建立 background 的长连接 Port ──
const port = chrome.runtime.connect({ name: 'moekoe-native-host-bridge' });

console.log('[NativeBridge] Bridge page loaded, connecting to background...');

// ── 接收 Main Process 转发的 EXE stdout 消息，转发给 background ──
window.electronAPI.nativeHost.onMessage((payload) => {
    if (!payload || payload.hostId !== HOST_ID) {
        return;
    }

    port.postMessage({
        type: 'native-host:event',
        payload: payload.message
    });
});

// ── 接收 background 的请求，转发给 Main Process（写入 EXE stdin）──
port.onMessage.addListener(async (message) => {
    if (!message || !message.type) {
        return;
    }

    switch (message.type) {
        case 'native-host:status': {
            const result = await window.electronAPI.nativeHost.getStatus(HOST_ID);
            port.postMessage({
                type: 'native-host:response',
                requestId: message.requestId,
                result: result
            });
            break;
        }

        case 'native-host:send': {
            const result = await window.electronAPI.nativeHost.send(HOST_ID, message.payload);
            port.postMessage({
                type: 'native-host:response',
                requestId: message.requestId,
                result: result
            });
            break;
        }
    }
});

// ── 断开处理 ──
port.onDisconnect.addListener(() => {
    console.warn('[NativeBridge] Background port disconnected');
});
