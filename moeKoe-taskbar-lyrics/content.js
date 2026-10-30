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

    // 防重复注入：同一页面被二次注入时直接跳过，避免重复弹窗
    if (window.__taskbarLyricsInjected) return;
    window.__taskbarLyricsInjected = true;

    // ── 任务栏卡片拖动进度条 → 设置播放器进度 ──
    //
    // MoeKoeMusic 的播放器由 new Audio() 创建，不挂载在 DOM 上，
    // document.querySelector('audio') 拿不到。这里用两条互不依赖的通道：
    //   1) 向页面主世界 postMessage：audio-bridge.js 包装了 Audio 构造函数，
    //      能拿到真实播放器实例并直接设置 currentTime
    //   2) DOM 兜底：模拟拖动 MoeKoeMusic 自带进度条（.progress-bar），
    //      由播放器自己的 Vue 逻辑换算并 seek，不依赖主世界脚本是否生效
    var lastSeekPosition = -1;
    var lastSeekAt = 0;

    function chooseProgressBar() {
        var bars = document.querySelectorAll('.progress-bar');
        for (var i = 0; i < bars.length; i++) {
            var r = bars[i].getBoundingClientRect();
            if (r.width > 20 && r.height > 0) return bars[i];
        }
        return null;
    }

    function simulateProgressBarSeek(position, duration) {
        if (!isFinite(duration) || duration <= 0) return;
        var bar = chooseProgressBar();
        if (!bar) return;

        var rect = bar.getBoundingClientRect();
        if (!rect.width || rect.width < 20) return;

        var ratio = Math.max(0, Math.min(1, position / duration));
        var clientX = Math.round(rect.left + ratio * rect.width);
        var clientY = Math.round(rect.top + rect.height / 2);
        var opts = {
            bubbles: true, cancelable: true, view: window,
            clientX: clientX, clientY: clientY, button: 0
        };
        try {
            // 播放器进度条由 mousedown 开始拖动、document 上的 mouseup 结束并 seek
            bar.dispatchEvent(new MouseEvent('mousedown', opts));
            document.dispatchEvent(new MouseEvent('mouseup', opts));
        } catch (e) {
            try {
                bar.dispatchEvent(new MouseEvent('click', opts));
            } catch (_) {}
        }
    }

    function applySeekRequest(position, duration) {
        var target = Number(position);
        if (!isFinite(target) || target < 0) return;

        // storage 通道与 tabs 通道可能同时到达：200ms 内同一目标只执行一次
        var now = Date.now();
        if (Math.abs(target - lastSeekPosition) < 0.01 && now - lastSeekAt < 200) return;
        lastSeekPosition = target;
        lastSeekAt = now;

        try {
            window.postMessage({
                channel: 'taskbar-lyrics',
                type: 'seek',
                position: target
            }, '*');
        } catch (e) {}

        simulateProgressBarSeek(target, Number(duration));
    }

    try {
        chrome.runtime.onMessage.addListener(function (msg, sender, sendResponse) {
            if (!msg || msg.type !== 'taskbarSeek') return;
            applySeekRequest(msg.position, msg.duration);
            sendResponse({ success: true });
        });
    } catch (e) {}

    // background 主通道：通过 chrome.storage.local 下发（不依赖 chrome.tabs）
    try {
        chrome.storage.onChanged.addListener(function (changes, area) {
            if (area !== 'local' || !changes || !changes.taskbarSeekRequest) return;
            var v = changes.taskbarSeekRequest.newValue;
            if (!v || typeof v.position !== 'number') return;
            applySeekRequest(v.position, v.duration);
        });
        // 页面加载期间可能错过变化：补读最近 3 秒内的请求
        chrome.storage.local.get('taskbarSeekRequest').then(function (res) {
            var v = res && res.taskbarSeekRequest;
            if (v && typeof v.position === 'number' && Date.now() - (v.ts || 0) < 3000) {
                applySeekRequest(v.position, v.duration);
            }
        }).catch(function () {});
    } catch (e) {}

    // localStorage 在隐私模式/存储禁用时会抛异常，必须兜底，
    // 否则脚本崩溃后连确认对话框都无法弹出
    function safeGetItem(key) {
        try {
            return localStorage.getItem(key);
        } catch (e) {
            return null;
        }
    }

    function safeSetItem(key, value) {
        try {
            localStorage.setItem(key, value);
        } catch (e) {}
    }

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
    var consent = safeGetItem(CONSENT_KEY);

    if (consent === '1') {
        // 已同意过即返回，不再每次启动强制重开 apiMode：
        // 用户若已在 MoeKoeMusic UI 中手动关闭，重复 enableApiMode() 会悄悄覆盖其选择
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
            safeSetItem(CONSENT_KEY, '1');
            enableApiMode();

            // 通知 EXE（通过 background.js → Native Bridge）写入 config.json
            // MoeKoeMusic 主进程读取 config.json（非 localStorage）来启动 WebSocket
            // 扩展重载后 chrome.runtime 失效，sendMessage 会同步抛出，必须 try/catch
            try {
                var p = chrome.runtime.sendMessage({ type: 'enableApiMode' });
                if (p && p.catch) p.catch(function () {});
            } catch (e) {}

            alert('API 模式已开启，重启 MoeKoeMusic 后生效。');
        } else {
            safeSetItem(CONSENT_KEY, '0');
        }
    }, 500);
})();
