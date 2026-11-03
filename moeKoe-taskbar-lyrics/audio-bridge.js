// SPDX-License-Identifier: GPL-3.0
// audio-bridge.js - 页面主世界（MAIN world）音频桥
//
// MoeKoeMusic 的音乐播放器由 `new Audio()` 创建（AudioController.js），
// 这个 Audio 对象不会挂到 DOM 上，所以隔离世界里的 content script 无法通过
// document.querySelector('audio') 拿到它；直接改 DOM audio 也是无效的。
//
// 本脚本在 document_start 注入到页面主世界，包装 Audio 构造函数以记录所有
// 实例，然后接收 content script 通过 window.postMessage 发来的 seek 请求，
// 对“正在使用”的实例设置 currentTime，从而真正改变播放器的播放位置。
//
// 链路回执写入页面 localStorage（__tl_ack / __tl_bridge），便于从磁盘侧核查。
(function () {
    'use strict';

    function mark(key, value) {
        try {
            // 附加窗口标识：主窗口与桌面歌词窗口共用一套 localStorage，
            // 不区分来源的话两个窗口的标记会互相覆盖，无法定位真正播放的实例
            var tag = 'main';
            try { tag = /lyrics/.test(location.hash) ? 'lyrics' : 'main'; } catch (e) {}
            localStorage.setItem(key + '@' + tag, value);
        } catch (e) {}
    }

    if (window.__taskbarLyricsAudioBridge) return;
    window.__taskbarLyricsAudioBridge = true;

    mark('__tl_audio', 'injected');

    var NativeAudio = window.Audio;
    if (typeof NativeAudio !== 'function') {
        mark('__tl_audio', 'no-audio-ctor');
        return;
    }

    var instances = [];
    window.__taskbarLyricsAudioInstances = instances;

    function WrappedAudio(...args) {
        const audio = new NativeAudio(...args);
        instances.push(audio);
        return audio;
    }
    WrappedAudio.prototype = NativeAudio.prototype;
    try {
        Object.setPrototypeOf(WrappedAudio, NativeAudio);
    } catch (e) {}

    try {
        window.Audio = WrappedAudio;
        mark('__tl_audio', 'wrapped');
    } catch (e) {
        // 极端情况下属性不可写：仍可通过 play 钩子捕获实例
        mark('__tl_audio', 'wrap-failed:' + e.message);
    }

    // 兜底：包装 play()，捕获任何通过 HTMLMediaElement 播放的实例
    // （即使 Audio 构造器被页面其它代码重新赋值，也不会漏掉播放器）
    try {
        var nativePlay = HTMLMediaElement.prototype.play;
        HTMLMediaElement.prototype.play = function () {
            if (instances.indexOf(this) < 0) instances.push(this);
            return nativePlay.apply(this, arguments);
        };
    } catch (e) {}

    // 兜底：包装 addEventListener，播放器必然会绑定 timeupdate/ended 等事件，
    // 借此捕获“在注入之前就已创建”的播放器实例
    try {
        var nativeAdd = HTMLMediaElement.prototype.addEventListener;
        HTMLMediaElement.prototype.addEventListener = function () {
            if (instances.indexOf(this) < 0) instances.push(this);
            return nativeAdd.apply(this, arguments);
        };
    } catch (e) {}

    // 诊断：记录每一次 currentTime 写入（含调用栈首帧），
    // 用于定位“seek 后被页面改回原位置”的具体代码位置
    try {
        var ctDesc = Object.getOwnPropertyDescriptor(HTMLMediaElement.prototype, 'currentTime');
        if (ctDesc && ctDesc.set) {
            var ctWrites = [];
            Object.defineProperty(HTMLMediaElement.prototype, 'currentTime', {
                configurable: true,
                enumerable: ctDesc.enumerable,
                get: function () { return ctDesc.get.call(this); },
                set: function (value) {
                    if (instances.indexOf(this) < 0) instances.push(this);
                    try {
                        var stack = (new Error().stack || '').split('\n')[1] || '';
                        ctWrites.push(Math.round(Number(value) * 10) / 10 + ' via ' + stack.trim().slice(0, 140));
                        if (ctWrites.length > 6) ctWrites.shift();
                        localStorage.setItem('__tl_ct', ctWrites.join(' || '));
                    } catch (e) {}
                    return ctDesc.set.call(this, value);
                }
            });
        }
    } catch (e) {}

    // 选出“正在使用”的播放器：优先正在播放，其次时长最长
    function pickAudio() {
        var best = null;
        var bestScore = -1;
        for (var i = instances.length - 1; i >= 0; i--) {
            var a = instances[i];
            var dur = isFinite(a.duration) ? a.duration : 0;
            if (dur <= 0) continue;
            var score = dur + (a.paused ? 0 : 100000);
            if (score > bestScore) {
                bestScore = score;
                best = a;
            }
        }
        return best;
    }

    function ack(payload) {
        try {
            payload.channel = 'taskbar-lyrics';
            payload.instances = instances.length;
            window.postMessage(payload, '*');
        } catch (e) {}
    }

    // 诊断：输出所有已捕获媒体实例的状态，判断“被 seek 的对象”是否就是正在播放的那个
    function describeInstances() {
        var parts = [];
        for (var i = 0; i < instances.length && i < 4; i++) {
            var a = instances[i];
            var src = '';
            try { src = String(a.src || a.currentSrc || ''); } catch (e) {}
            parts.push('[' + i + '] dur=' + (isFinite(a.duration) ? Math.round(a.duration) : -1) +
                       ' paused=' + a.paused + ' ct=' + Math.round(a.currentTime) +
                       ' rs=' + a.readyState + ' src=' + src.slice(-40));
        }
        var domAudio = 0;
        try { domAudio = document.querySelectorAll('audio,video').length; } catch (e) {}
        return 'n=' + instances.length + '|dom=' + domAudio + '|sameCtor=' +
               (window.Audio === WrappedAudio) + '|' + parts.join(' ');
    }

    // 持续上报所有媒体实例的实时状态（每秒一次），
    // 便于在不触发任何操作时观察哪个实例真正在播放
    setInterval(function () {
        mark('__tl_live', describeInstances() + '|t=' + Date.now());
    }, 1000);

    // 捕获 media 元素数据事件：playing 状态变更时立即上报该窗口的实例快照，
    // 便于区分“真正在出声的窗口”与“影子窗口”
    try {
        document.addEventListener('playing', function (e) {
            if (instances.indexOf(e.target) < 0) instances.push(e.target);
            mark('__tl_playing', describeInstances());
        }, true);
    } catch (e) {}

    // content script 的心跳探测：证明本脚本确实注入到了主世界
    window.addEventListener('message', function (event) {
        if (event.source !== window) return;
        var data = event.data;
        if (!data || data.channel !== 'taskbar-lyrics') return;
        if (data.type === 'ping') {
            ack({ type: 'bridge-ready' });
            mark('__tl_inst', describeInstances());
        }
    });

    window.addEventListener('message', function (event) {
        if (event.source !== window) return;
        var data = event.data;
        if (!data || data.channel !== 'taskbar-lyrics' || data.type !== 'seek') return;

        var target = Number(data.position);
        if (!isFinite(target) || target < 0) return;

        var audio = pickAudio();
        mark('__tl_inst', describeInstances());
        if (!audio) {
            ack({ type: 'seek-ack', applied: -1, duration: -1, error: 'no-audio-instance' });
            return;
        }
        try {
            // 对所有“音乐级时长”的媒体实例都执行 seek：
            // 页面可能在注入之前就创建了播放器实例，这里宁可多写也不漏写，
            // 非当前播放器的实例即使被写入也不影响实际播放。
            var applied = -1;
            var maxDur = 0;
            var count = 0;
            for (var i = 0; i < instances.length; i++) {
                var a = instances[i];
                var d = isFinite(a.duration) ? a.duration : 0;
                if (d <= 30) continue;
                try {
                    a.currentTime = Math.min(target, d);
                    if (applied < 0) {
                        applied = a.currentTime;
                        maxDur = d;
                    }
                    count++;
                } catch (e) {}
            }
            ack({
                type: 'seek-ack',
                applied: applied,
                duration: maxDur,
                error: count ? '' : 'all-seek-failed'
            });
            // 700ms 后复核：若位置又回到原值，说明页面有代码把 currentTime 改回去了
            setTimeout(function () {
                try {
                    mark('__tl_after', 'ct=' + Math.round(audio.currentTime * 10) / 10 +
                         '|paused=' + audio.paused + '|seekable=' + audio.seekable.length +
                         '|count=' + count);
                } catch (e) {}
            }, 700);
        } catch (e) {
            // 媒体尚未可 seek 时浏览器会抛错
            ack({ type: 'seek-ack', applied: -1, duration: -1, error: e.message });
        }
    });
})();
