// MoeKoe Taskbar Lyrics - Popup Script
//
// v0.4.1 安全加固:
//   - 连接状态指示器增强（重连次数、最后连接时间、重连中止提示）

document.addEventListener('DOMContentLoaded', () => {
    const statusDot = document.getElementById('statusDot');
    const statusText = document.getElementById('statusText');
    const wsStatus = document.getElementById('wsStatus');
    const btnReconnect = document.getElementById('btnReconnect');

    // ---- 工具函数 ----

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

    // ---- 初始化 ----

    chrome.runtime.sendMessage({ type: 'getStatus' }, (response) => {
        // background 未就绪时 lastError 会被置位，必须读取，否则 Chrome 控制台持续告警
        if (chrome.runtime.lastError) return;
        if (response) updateConnectionUI(response.connected, response);
    });

    chrome.runtime.onMessage.addListener((message) => {
        switch (message.type) {
            case 'connectionStatus':
                updateConnectionUI(message.connected, message);
                break;
            case 'nativeEvent':
                // Native Host 上报事件（可扩展处理）
                console.log('[Popup] Native Event:', message.data);
                break;
            default:
                break;
        }
    });

    // ---- 重新连接按钮 ----

    btnReconnect.addEventListener('click', () => {
        chrome.runtime.sendMessage({ type: 'reconnect' }, () => {
            if (chrome.runtime.lastError) {
                console.warn('[Popup] reconnect 发送失败:', chrome.runtime.lastError.message);
            }
            setTimeout(() => {
                chrome.runtime.sendMessage({ type: 'getStatus' }, (r) => {
                    if (chrome.runtime.lastError) return;
                    if (r) updateConnectionUI(r.connected);
                });
            }, 1000);
        });
    });
});
