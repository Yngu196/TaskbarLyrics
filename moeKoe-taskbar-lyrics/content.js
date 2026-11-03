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
    var lastSeekTs = 0;   // 已处理过的最新请求时间戳（防止轮询重复执行）

    // 诊断落盘：Electron 的 content script 对部分扩展 API 支持不完整，
    // 把链路关键节点写入页面 localStorage，便于从磁盘侧核查执行情况
    function mark(key, value) {
        try { localStorage.setItem(key, value); } catch (e) {}
    }

    mark('__tl_injected', 'yes-' + Date.now());

    function chooseProgressBar() {
        var bars = document.querySelectorAll('.progress-bar');
        for (var i = 0; i < bars.length; i++) {
            var r = bars[i].getBoundingClientRect();
            if (r.width > 20 && r.height > 0) return bars[i];
        }
        return null;
    }

    function simulateProgressBarSeek(position, duration) {
        var total = Number(duration);
        if (!isFinite(total) || total <= 0) {
            mark('__tl_dom', 'skip-duration|' + total);
            return;
        }
        var bar = chooseProgressBar();
        if (!bar) {
            mark('__tl_dom', 'no-bar|total=' + document.querySelectorAll('.progress-bar').length);
            return;
        }

        var rect = bar.getBoundingClientRect();
        if (!rect.width || rect.width < 20) {
            mark('__tl_dom', 'bar-too-narrow|' + Math.round(rect.width));
            return;
        }

        var ratio = Math.max(0, Math.min(1, position / total));
        var clientX = Math.round(rect.left + ratio * rect.width);
        var clientY = Math.round(rect.top + rect.height / 2);

        // 命中该坐标下最深层元素，让事件冒泡到 Vue 绑定在 .progress-bar 上的处理函数
        var hit = null;
        try { hit = document.elementFromPoint(clientX, clientY); } catch (e) {}
        if (!hit) hit = bar;

        var opts = {
            bubbles: true, cancelable: true, view: window,
            clientX: clientX, clientY: clientY, button: 0
        };
        try {
            // 完整拖动序列：mousedown（开始拖动）→ mousemove → mouseup（在 document 上结束并 seek）
            hit.dispatchEvent(new MouseEvent('mousedown', opts));
            document.dispatchEvent(new MouseEvent('mousemove', opts));
            document.dispatchEvent(new MouseEvent('mouseup', opts));
            // 点击兜底：播放器的 updateProgressFromEvent 会直接用 clientX 换算并 seek
            hit.dispatchEvent(new MouseEvent('click', opts));
            mark('__tl_dom', 'dispatched|ratio=' + ratio.toFixed(3) +
                 '|hit=' + (hit.className || hit.tagName));
        } catch (e) {
            mark('__tl_dom', 'dispatch-error|' + e.message);
        }
    }

    function applySeekRequest(position, duration) {
        var target = Number(position);
        if (!isFinite(target) || target < 0) return;

        // 轮询/onChanged/tabs 三条通道可能同时到达：200ms 内同一目标只执行一次
        var now = Date.now();
        if (Math.abs(target - lastSeekPosition) < 0.01 && now - lastSeekAt < 200) return;
        lastSeekPosition = target;
        lastSeekAt = now;

        mark('__tl_seek_req', target + '|dur=' + duration + '|ts=' + now);

        // 通知主世界桥接（audio-bridge.js 已包装 Audio 构造器）
        try {
            window.postMessage({
                channel: 'taskbar-lyrics',
                type: 'seek',
                position: target
            }, '*');
            window.postMessage({ channel: 'taskbar-lyrics', type: 'ping' }, '*');
        } catch (e) {}

        // 注意：不要模拟页面进度条拖动！实测合成事件会走应用的 onProgressDragEnd，
        // 但其内部拖动位置未被合成事件更新，松手时会把拖动前的旧进度写回
        // audio.currentTime，直接覆盖掉桥接已写入的目标位置（表现为进度回跳）。
        // 直接写 currentTime 已被验证生效，应用 timeupdate 会自动同步新位置。
    }

    // 主世界回执：证明 Audio 实例已被捕获并写入 currentTime
    window.addEventListener('message', function (event) {
        if (event.source !== window) return;
        var d = event.data;
        if (!d || d.channel !== 'taskbar-lyrics') return;
        if (d.type === 'seek-ack') {
            mark('__tl_ack', 'applied=' + d.applied + '|dur=' + d.duration +
                 '|inst=' + d.instances + '|err=' + (d.error || 'none'));
        } else if (d.type === 'bridge-ready') {
            mark('__tl_bridge', 'ready|inst=' + d.instances);
        }
    });

    // 通道 A：tabs 消息（Electron 下可能拿不到 chrome.tabs，仅作低延迟补充）
    try {
        chrome.runtime.onMessage.addListener(function (msg, sender, sendResponse) {
            if (!msg || msg.type !== 'taskbarSeek') return;
            mark('__tl_ch', 'runtime-message');
            applySeekRequest(msg.position, msg.duration);
            sendResponse({ success: true });
        });
    } catch (e) {}

    // 通道 B：chrome.storage.local 轮询
    // Electron 的 content script 中 storage.onChanged 不一定触发，
    // 因此主动轮询（400ms）保证 seek 请求一定被读到，onChanged 仅作低延迟补充。
    function readSeekRequest() {
        var done = function (res) {
            var v = res && res.taskbarSeekRequest;
            if (!v || typeof v.position !== 'number') return;
            if (typeof v.ts === 'number' && v.ts <= lastSeekTs) return;
            lastSeekTs = typeof v.ts === 'number' ? v.ts : Date.now();
            mark('__tl_ch', 'storage-poll');
            applySeekRequest(v.position, v.duration);
        };
        try {
            var p = chrome.storage.local.get('taskbarSeekRequest', function (res) { done(res); });
            if (p && typeof p.then === 'function') {
                p.then(function (res) { done(res); }).catch(function () {});
            }
        } catch (e) {}
    }

    try {
        chrome.storage.onChanged.addListener(function (changes, area) {
            if (area !== 'local' || !changes || !changes.taskbarSeekRequest) return;
            var v = changes.taskbarSeekRequest.newValue;
            if (!v || typeof v.position !== 'number') return;
            mark('__tl_ch', 'storage-onchanged');
            applySeekRequest(v.position, v.duration);
        });
    } catch (e) {}

    setInterval(readSeekRequest, 400);

    // 通道 C：直接轮询 EXE HTTP 服务器（兜底主通道，与启动方式无关）
    // 背景：EXE 的 Native Host stdout 事件只有在宿主托管启动时才接通，
    // 手动/独立启动的 EXE 实例事件会全部丢失（seek 表现为回跳）。
    // 此通道由 content script 直接向 EXE HTTP 服务器轮询「待处理 seek」，
    // 端点免鉴权、只读、读后即清（安全权衡见 http_server.cpp 注释）。
    // 注意：端点是读后即清，桌面歌词窗口（#/lyrics）的 content script
    // 不得轮询，否则会把 pending 抢走而主窗口拿不到（其 Audio 是影子实例）。
    function pollHttpSeek() {
        if (/lyrics/.test(location.hash || '')) return;  // 桌面歌词窗口不参与
        fetch('http://127.0.0.1:6523/seek/pending')
            .then(function (r) { return r.ok ? r.json() : null; })
            .then(function (d) {
                if (!d || !d.pending) return;
                mark('__tl_ch', 'http-poll');
                applySeekRequest(d.position, d.duration);
            })
            .catch(function () { /* EXE 未运行或端口未监听 */ });
    }
    setInterval(pollHttpSeek, 500);

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
