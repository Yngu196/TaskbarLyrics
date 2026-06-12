// MoeKoe Taskbar Lyrics - Popup Script
//
// v0.4.0 迁移到 Native Host 托管模式:
//   - EXE 生命周期由 MoeKoeMusic 的 nativeHostManager 管理
//   - 不再需要手动启动/停止（auto_start: true）
//   - 进程状态通过 nativeHost API 查询
//   - 保留 HTTP 端口 (6523) 作为独立运行模式下的备用接口

// ── 运行时常量 ──────────────────────────────────────────────────────────────────
const WS_PORT       = 6520;
const HTTP_PORT     = 6523;
const LOCAL_AUTH_TOKEN = 'MoeKoeTL-2k7qFb9zXm4Nv3Wc8YhRtSjP0DlQn6Bo1';
const AUTH_HEADER   = 'X-MoeKoe-Token';

document.addEventListener('DOMContentLoaded', () => {
    const statusDot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const wsStatus = document.getElementById('wsStatus');
    const processStatus = document.getElementById('processStatus');
    const btnPrev = document.getElementById('btnPrev');
    const btnToggle = document.getElementById('btnToggle');
    const btnNext = document.getElementById('btnNext');
    const btnReconnect = document.getElementById('btnReconnect');
    const diagPanel = document.getElementById('diagPanel');
    const diagText = document.getElementById('diagText');

    // ---- 工具函数 ----

    function updateConnectionUI(connected) {
        statusDot.className = 'status-dot ' + (connected ? 'on' : 'off');
        statusText.textContent = connected ? '已连接' : '等待连接';
        wsStatus.textContent = connected ? '已连接' : '未连接';
        wsStatus.style.color = connected ? '#52c41a' : '#ff4d4f';
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

    // 构造携带本地鉴权头的通用 fetch 选项（独立运行模式备用）
    function buildAuthHeaders(extra) {
        const headers = Object.assign({}, extra || {});
        headers[AUTH_HEADER] = LOCAL_AUTH_TOKEN;
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
            const r = await fetch(`http://127.0.0.1:${HTTP_PORT}/ping`, {
                method: 'GET',
                headers: buildAuthHeaders(),
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

    chrome.runtime.sendMessage({ type: 'getStatus' }, (response) => {
        if (response) updateConnectionUI(response.connected);
    });

    detectProcessStatus();

    chrome.runtime.onMessage.addListener((message) => {
        switch (message.type) {
            case 'connectionStatus':
                updateConnectionUI(message.connected);
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

    // ---- 播放控制按钮 ----

    btnPrev.addEventListener('click', () => {
        chrome.runtime.sendMessage({ type: 'control', command: 'prev' });
    });

    btnToggle.addEventListener('click', () => {
        chrome.runtime.sendMessage({ type: 'control', command: 'toggle' });
    });

    btnNext.addEventListener('click', () => {
        chrome.runtime.sendMessage({ type: 'control', command: 'next' });
    });

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
