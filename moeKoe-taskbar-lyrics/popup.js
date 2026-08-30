// MoeKoe Taskbar Lyrics - Popup Script
//
// v0.4.1 安全加固:
//   - 端口可配置化（从 storage 读取，默认值兜底）
//   - 连接状态指示器增强（重连次数、最后连接时间、重连中止提示）
//   - HTTP 回退接口保留鉴权头

// ── 运行时常量 ──────────────────────────────────────────────────────────────────
const DEFAULT_WS_PORT = 6520;
const DEFAULT_HTTP_PORT = 6523;
const AUTH_HEADER = 'X-MoeKoe-Token';
let httpPort = DEFAULT_HTTP_PORT;

document.addEventListener('DOMContentLoaded', () => {
    const statusDot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const wsStatus = document.getElementById('wsStatus');
    const processStatus = document.getElementById('processStatus');
    const btnReconnect = document.getElementById('btnReconnect');
    const diagPanel = document.getElementById('diagPanel');
    const diagText = document.getElementById('diagText');

    // ---- 工具函数 ----

    // 加载端口配置（从 storage 读取，默认值兜底）
    async function loadPortConfig() {
        try {
            const cfg = await chrome.storage.local.get(['wsPort', 'httpPort']);
            httpPort = cfg.httpPort || DEFAULT_HTTP_PORT;
        } catch (e) {
            httpPort = DEFAULT_HTTP_PORT;
        }
    }

    function formatTime(ts) {
        if (!ts) return '--';
        const d = new Date(ts);
        return d.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    }

    function updateConnectionUI(connected, extra = {}) {
        statusDot.className = 'status-dot ' + (connected ? 'on' : 'off');
        // 将 WebSocket 连接状态映射为 API 模式状态：连接成功 = 开启，其余（未连接/重连中/重连中止）= 关闭
        wsStatus.textContent = connected ? '开启' : '关闭';
        wsStatus.style.color = connected ? '#52c41a' : '#ff4d4f';
        statusText.textContent = connected ? '开启' : '关闭';

        // 增强状态信息：显示重连次数和最后连接时间
        if (extra.reconnectAttempts != null && !connected) {
            const infoEl = document.getElementById('reconnectInfo');
            if (!infoEl) {
                const el = document.createElement('span');
                el.id = 'reconnectInfo';
                el.style.cssText = 'font-size:11px;color:#999;display:block;margin-top:2px;';
                statusText.parentNode.insertBefore(el, statusText.nextSibling);
            }
            document.getElementById('reconnectInfo').textContent =
                `重连 ${extra.reconnectAttempts}/50 次` +
                (extra.lastConnectTime ? ` | 上次连接 ${formatTime(extra.lastConnectTime)}` : '');
        }
    }

    // 更新托管进程状态显示（只读，不可手动控制）
    function updateHostUI(status) {
        if (!processStatus) return;

        if (!status || !status.success) {
            // API 调用失败（bridge 未连接或未授权）
            processStatus.textContent = '未授权';
            processStatus.style.color = '#faad14'; // warning 黄色
            return;
        }

        const host = status.host;
        if (!host) {
            processStatus.textContent = '未知';
            processStatus.style.color = '#999';
            return;
        }

        if (host.running) {
            processStatus.textContent = '运行中 (托管)';
            processStatus.style.color = '#52c41a';
        } else if (host.authorized) {
            processStatus.textContent = '已停止 (托管)';
            processStatus.style.color = '#ff4d4f';
        } else {
            processStatus.textContent = '未授权';
            processStatus.style.color = '#faad14';
        }
    }

    // 从 background 获取动态鉴权令牌
    async function getAuthToken() {
        try {
            const response = await new Promise((resolve) => {
                chrome.runtime.sendMessage({ type: 'getAuthToken' }, resolve);
            });
            return response ? response.token : null;
        } catch (_) {
            return null;
        }
    }

    // 构造携带本地鉴权头的通用 fetch 选项（独立运行模式备用）
    async function buildAuthHeaders(extra) {
        const token = await getAuthToken();
        const headers = Object.assign({}, extra || {});
        if (token) headers[AUTH_HEADER] = token;
        return headers;
    }

    // 通过 Native Host Bridge 查询 EXE 状态（首选）
    async function checkHostStatus() {
        try {
            const response = await new Promise((resolve) => {
                chrome.runtime.sendMessage({ type: 'getHostStatus' }, resolve);
            });
            return response;
        } catch (_) {
            return { success: false, message: 'background 通信失败' };
        }
    }

    // 回退：通过 HTTP /ping 检测（独立运行模式）
    async function checkProcessRunningHTTP() {
        try {
            const controller = new AbortController();
            setTimeout(() => controller.abort(), 2000);
            const r = await fetch(`http://127.0.0.1:${httpPort}/ping`, {
                method: 'GET',
                headers: await buildAuthHeaders(),
                signal: controller.signal
            });
            if (!r.ok) return false;
            try {
                const data = await r.json();
                return !!(data && data.service === 'MoeKoeTaskbarLyrics');
            } catch (_) {
                return false;
            }
        } catch (_) {
            return false;
        }
    }

    // 综合状态检测：优先 Native Host API，回退 HTTP
    async function detectProcessStatus() {
        // 首选：Native Host Bridge
        const hostStatus = await checkHostStatus();
        if (hostStatus && hostStatus.success) {
            updateHostUI(hostStatus);
            return hostStatus.host && hostStatus.host.running;
        }

        // 回退：HTTP ping（独立运行模式，无 bridge 连接）
        const running = await checkProcessRunningHTTP();
        if (running) {
            if (processStatus) {
                processStatus.textContent = '运行中 (独立)';
                processStatus.style.color = '#52c41a';
            }
        } else {
            if (processStatus) {
                processStatus.textContent = '未运行';
                processStatus.style.color = '#ff4d4f';
            }
        }
        return running;
    }

    // ---- 初始化 ----

    loadPortConfig().then(() => {
        chrome.runtime.sendMessage({ type: 'getStatus' }, (response) => {
            if (response) updateConnectionUI(response.connected, response);
        });
    });

    detectProcessStatus();

    chrome.runtime.onMessage.addListener((message) => {
        switch (message.type) {
            case 'connectionStatus':
                updateConnectionUI(message.connected, message);
                break;
            case 'lyrics': {
                const currentLyric = document.getElementById('currentLyric');
                if (currentLyric && message.data && message.data.currentLine) {
                    currentLyric.textContent = message.data.currentLine;
                }
                break;
            }
            case 'nativeEvent':
                // Native Host 上报事件（可扩展处理）
                console.log('[Popup] Native Event:', message.data);
                break;
            default:
                break;
        }
    });

    // 定期刷新进程状态（每 5 秒）
    setInterval(detectProcessStatus, 5000);

    // ---- 重新连接按钮 ----

    btnReconnect.addEventListener('click', () => {
        chrome.runtime.sendMessage({ type: 'reconnect' }, () => {
            setTimeout(() => {
                chrome.runtime.sendMessage({ type: 'getStatus' }, (r) => {
                    if (r) updateConnectionUI(r.connected);
                });
            }, 1000);
        });
    });
});
