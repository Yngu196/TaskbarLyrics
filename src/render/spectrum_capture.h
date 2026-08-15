// SPDX-License-Identifier: GPL-3.0
// spectrum_capture.h - WASAPI loopback 音频频谱捕获（PIMPL）
#pragma once

#include <atomic>
#include <memory>
#include <vector>

namespace moekoe {

class SpectrumCapture {
public:
    SpectrumCapture();
    ~SpectrumCapture();

    SpectrumCapture(const SpectrumCapture&) = delete;
    SpectrumCapture& operator=(const SpectrumCapture&) = delete;

    bool Start(int wsPort = 6520);
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // 播放活动提示：主线程在播放中但频谱无数据时调用，
    // 触发采集线程重建会话（捕获激活之后才启动的音频渲染进程）
    void NotifyPlaybackActive();

    // 设置运行时频谱参数（dB 映射区间、内部频段数）
    void SetParams(float dbCeil, float dbFloor, int numBands);

    // 返回归一化 [0,1] 的各频段幅度
    std::vector<float> GetSpectrum(int numBands = 32);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

} // namespace moekoe
