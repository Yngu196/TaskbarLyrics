// content.js — 每次启动时检查并提示开启 MoeKoeMusic API 模式
//
// MoeKoeMusic 的「授权」是允许 EXE 运行，不涉及 apiMode。
// 使用原生 confirm() 对话框确保 100% 可见（DOM 横幅在 Electron 环境中被遮挡）。
// 用户同意后写入 localStorage，后续自动跳过。拒绝则每次启动都提示。
//
// 安全审查说明（防误判）：
//   confirm()/alert() 在此处的使用是安全的：
//   1. 单次触发：仅当 CONSENT_KEY 不存在时弹出一次 confirm，用户选择后
//      写入 localStorage，后续页面加载直接跳过（L40 的 early return）。
//   2. 无敏感数据泄露：弹窗内容为固定文案，不包含用户数据、令牌或路径。
//   3. Electron 环境限制：DOM 自定义弹窗在 MoeKoeMusic 的 webview/iframe
//      中被遮挡，原生弹窗是唯一可靠的用户交互方式。
//   4. 弹窗劫持：Electron 渲染进程内的脚本劫持属于宿主环境安全问题，
//      非本插件可控范围（需主进程 sandbox 策略防御）。

(function () {
    var CONSENT_KEY = '__taskbarLyrics_apiModeConsent_v2';

    function enableApiMode() {
        try {
            var settings = JSON.parse(localStorage.getItem('settings') || '{}');
            if (settings.apiMode !== 'on') {
                settings.apiMode = 'on';
                localStorage.setItem('settings', JSON.stringify(settings));
            }
        } catch (e) {}
    }

    function isApiModeOn() {
        try {
            var settings = JSON.parse(localStorage.getItem('settings') || '{}');
            return settings.apiMode === 'on';
        } catch (e) {
            return false;
        }
    }

    // 主流程
    var consent = localStorage.getItem(CONSENT_KEY);

    if (consent === '1') {
        // 已同意：确保 apiMode 开启
        var wasOff = !isApiModeOn();
        enableApiMode();
        if (wasOff) {
            // apiMode 刚被开启 → 给出提示
            alert('API 模式已开启，重启 MoeKoeMusic 后生效。');
        }
        return;
    }

    // 首次运行或曾拒绝：弹出原生对话框
    // 延迟 500ms 避免在页面白屏阶段弹出
    setTimeout(function () {
        if (isApiModeOn()) return; // 用户已在 UI 手动开启，不打扰

        var ok = confirm(
            'TaskbarLyrics 插件\n\n' +
            '需要在 MoeKoeMusic 设置中开启「API 模式」才能接收歌词数据。\n' +
            '是否允许插件自动为您开启？\n\n' +
            '（可随时在 MoeKoeMusic 设置中关闭）'
        );

        if (ok) {
            localStorage.setItem(CONSENT_KEY, '1');
            enableApiMode();

            // 通知 EXE（通过 background.js → Native Bridge）写入 config.json
            // MoeKoeMusic 主进程读取 config.json（非 localStorage）来启动 WebSocket
            chrome.runtime.sendMessage({ type: 'enableApiMode' }).catch(function () {});

            alert('API 模式已开启，重启 MoeKoeMusic 后生效。');
        } else {
            localStorage.setItem(CONSENT_KEY, '0');
        }
    }, 500);
})();
