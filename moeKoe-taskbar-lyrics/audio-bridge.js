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
(function () {
    'use strict';

    if (window.__taskbarLyricsAudioBridge) return;
    window.__taskbarLyricsAudioBridge = true;

    var NativeAudio = window.Audio;
    if (typeof NativeAudio !== 'function') return;

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
    } catch (e) {
        // 极端情况下属性不可写：退回原构造器，后续 seek 会找不到实例
        return;
    }

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

    window.addEventListener('message', function (event) {
        if (event.source !== window) return;
        var data = event.data;
        if (!data || data.channel !== 'taskbar-lyrics' || data.type !== 'seek') return;

        var target = Number(data.position);
        if (!isFinite(target) || target < 0) return;

        var audio = pickAudio();
        if (!audio) return;
        try {
            var max = isFinite(audio.duration) ? audio.duration : 0;
            audio.currentTime = max > 0 ? Math.min(target, max) : target;
        } catch (e) {
            // 忽略：媒体尚未可 seek 时浏览器会抛错
        }
    });
})();
