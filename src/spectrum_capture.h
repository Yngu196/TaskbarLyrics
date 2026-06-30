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

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // 返回归一化 [0,1] 的各频段幅度
    std::vector<float> GetSpectrum(int numBands = 32);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

} // namespace moekoe
