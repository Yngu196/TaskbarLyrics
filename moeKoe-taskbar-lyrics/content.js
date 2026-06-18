// content.js — 自动启用 MoeKoeMusic API 模式
// 
// 在 MoeKoeMusic 页面加载时主动检测 localStorage.settings.apiMode，
// 若为 "off" 则改为 "on"，使 MoeKoeMusic 前端同步该状态到主进程。
// 
// 注意：修改后需要重启 MoeKoeMusic 才能让 main.js 重新读取 apiMode 并启动 WS 服务。

(function () {
    try {
        let settings = JSON.parse(localStorage.getItem('settings') || '{}');
        if (settings.apiMode !== 'on') {
            settings.apiMode = 'on';
            localStorage.setItem('settings', JSON.stringify(settings));
            console.log('[TaskbarLyrics] API mode set to "on" in localStorage');
        }
    } catch (e) {
        console.warn('[TaskbarLyrics] Failed to set apiMode:', e);
    }
})();
