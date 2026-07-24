// SPDX-License-Identifier: GPL-3.0
// spectrum_capture.cpp - WASAPI loopback FFT 频谱捕获实现

// 必须在 COM 头文件之前包含 windows.h
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include "render/spectrum_capture.h"
#include "core/constants.h"
#include "util/logger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

#include <kiss_fft.h>
#include <kiss_fftr.h>

#pragma comment(lib, "ole32.lib")

namespace moekoe {

namespace {

// FFT 点数
constexpr int FFT_SIZE = 1024;

// Hann 窗
void ApplyHannWindow(std::vector<float>& buf) {
    const size_t n = buf.size();
    for (size_t i = 0; i < n; ++i) {
        float w = 0.5f * (1.0f - std::cos(2.0f * 3.14159265358979323846f * static_cast<float>(i) / static_cast<float>(n - 1)));
        buf[i] *= w;
    }
}

// 对数频段合并：将 FFT 幅值 bins 映射到 numBands 个对数间距频段
std::vector<float> LogBands(const std::vector<float>& magnitudes, int numBands) {
    std::vector<float> bands(static_cast<size_t>(numBands), 0.0f);
    const size_t numBins = magnitudes.size();
    if (numBins == 0) return bands;

    // 对数映射：band 0 对应最低频，band N-1 对应最高频
    // 使用 mel-like 对数尺度
    const float minFreq = 20.0f;
    const float maxFreq = 22000.0f;
    const float sampleRate = 44100.0f;  // 典型值
    const float freqPerBin = sampleRate / static_cast<float>(FFT_SIZE);
    const float logMin = std::log10(minFreq);
    const float logMax = std::log10(maxFreq);

    for (int b = 0; b < numBands; ++b) {
        float t = static_cast<float>(b) / static_cast<float>(numBands);
        float freqLow = std::pow(10.0f, logMin + t * (logMax - logMin));
        float freqHigh = std::pow(10.0f, logMin + (t + 1.0f / static_cast<float>(numBands)) * (logMax - logMin));
        int binLow = (std::max)(0, static_cast<int>(freqLow / freqPerBin));
        int binHigh = (std::min)(static_cast<int>(numBins) - 1, static_cast<int>(freqHigh / freqPerBin));

        float sum = 0.0f;
        int count = 0;
        for (int i = binLow; i <= binHigh; ++i) {
            sum += magnitudes[static_cast<size_t>(i)];
            ++count;
        }
        bands[static_cast<size_t>(b)] = (count > 0) ? (sum / static_cast<float>(count)) : 0.0f;
    }

    return bands;
}

} // namespace

struct SpectrumCapture::Impl {
    std::unique_ptr<std::thread> captureThread;
    std::vector<float>          spectrumOutput;
    std::vector<float>          smoothSpectrum;
    mutable std::mutex          spectrumMutex;

    static constexpr size_t kRingBufferSize = FFT_SIZE * 2;
    std::vector<float>          ringBuffer;
    size_t                      ringWritePos{0};
    size_t                      ringSamplesAvailable{0};
    std::mutex                  ringMutex;

    void CaptureLoop(SpectrumCapture* parent);
};

void SpectrumCapture::Impl::CaptureLoop(SpectrumCapture* parent) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Log("[Spectrum] CoInitializeEx failed: 0x%08X\n", hr);
        parent->running_ = false;
        return;
    }

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&pEnumerator));
    if (FAILED(hr) || !pEnumerator) {
        Log("[Spectrum] MMDeviceEnumerator failed: 0x%08X\n", hr);
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    IMMDevice* pDevice = nullptr;
    hr = pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    pEnumerator->Release();
    if (FAILED(hr) || !pDevice) {
        Log("[Spectrum] GetDefaultAudioEndpoint failed: 0x%08X\n", hr);
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    IAudioClient* pAudioClient = nullptr;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&pAudioClient));
    pDevice->Release();
    if (FAILED(hr) || !pAudioClient) {
        Log("[Spectrum] Activate IAudioClient failed: 0x%08X\n", hr);
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        pAudioClient->Release();
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    // 10ms buffer
    REFERENCE_TIME hnsBufferDuration = 100000; // 10ms in 100ns units
    hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                  AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  hnsBufferDuration, 0, pwfx, nullptr);
    CoTaskMemFree(pwfx);

    if (FAILED(hr)) {
        Log("[Spectrum] Initialize loopback failed: 0x%08X\n", hr);
        pAudioClient->Release();
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    IAudioCaptureClient* pCaptureClient = nullptr;
    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient),
                                  reinterpret_cast<void**>(&pCaptureClient));
    if (FAILED(hr) || !pCaptureClient) {
        pAudioClient->Release();
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    hr = pAudioClient->Start();
    if (FAILED(hr)) {
        pCaptureClient->Release();
        pAudioClient->Release();
        CoUninitialize();
        parent->running_ = false;
        return;
    }

    Log("[Spectrum] Capture started\n");
    parent->running_ = true;

    smoothSpectrum.clear();

    while (parent->running_.load()) {
        Sleep(16); // ~60fps

        UINT32 packetLength = 0;
        hr = pCaptureClient->GetNextPacketSize(&packetLength);
        if (FAILED(hr)) break;

        while (packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;
            hr = pCaptureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;

            if (pData && numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* samples = reinterpret_cast<const float*>(pData);
                const UINT32 totalSamples = numFrames * 2; // stereo interleaved

                std::lock_guard<std::mutex> lock(ringMutex);
                for (UINT32 i = 0; i < totalSamples; i += 2) {
                    // 左右声道均值
                    float mono = (samples[i] + samples[i + 1]) * 0.5f;
                    ringBuffer[ringWritePos] = mono;
                    ringWritePos = (ringWritePos + 1) % kRingBufferSize;
                    if (ringSamplesAvailable < kRingBufferSize) {
                        ringSamplesAvailable++;
                    }
                }
            }

            hr = pCaptureClient->ReleaseBuffer(numFrames);
            if (FAILED(hr)) break;

            hr = pCaptureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) break;
        }

        // 累积足够样本后执行 FFT
        size_t available;
        {
            std::lock_guard<std::mutex> lock(ringMutex);
            available = ringSamplesAvailable;
        }

        if (available >= FFT_SIZE) {
            std::vector<float> realBuf(FFT_SIZE);
            {
                std::lock_guard<std::mutex> lock(ringMutex);
                size_t readPos = (ringWritePos + kRingBufferSize - available) % kRingBufferSize;
                for (int i = 0; i < FFT_SIZE; ++i) {
                    realBuf[i] = ringBuffer[readPos];
                    readPos = (readPos + 1) % kRingBufferSize;
                }
                ringSamplesAvailable -= FFT_SIZE;
            }

            ApplyHannWindow(realBuf);

            // kiss_fftr: 实数 FFT（输入 N 个实数，输出 N/2+1 个复数）
            kiss_fftr_cfg fftCfg = kiss_fftr_alloc(FFT_SIZE, 0, nullptr, nullptr);
            if (!fftCfg) {
                // 分配失败（内存不足等极端情况），跳过本帧频谱处理
                continue;
            }
            std::vector<kiss_fft_cpx> freqOut(FFT_SIZE / 2 + 1);
            kiss_fftr(fftCfg, realBuf.data(), freqOut.data());
            free(fftCfg);

            std::vector<float> magnitudes(FFT_SIZE / 2);
            for (int i = 0; i < FFT_SIZE / 2; ++i) {
                float re = freqOut[i].r;
                float im = freqOut[i].i;
                magnitudes[static_cast<size_t>(i)] = std::sqrt(re * re + im * im);
            }

            // 对数频段
            auto rawBands = LogBands(magnitudes, constants::SPECTRUM_NUM_BANDS);

            // 归一化
            float maxVal = 0.001f;
            for (float v : rawBands) {
                if (v > maxVal) maxVal = v;
            }
            const float invMax = 1.0f / maxVal;
            for (float& v : rawBands) {
                v = (std::min)(1.0f, v * invMax);
            }

            // 指数移动平均平滑（0.35 保留 35% 旧值，响应更快）
            constexpr float smoothing = 0.35f;
            if (smoothSpectrum.size() != rawBands.size()) {
                smoothSpectrum = rawBands;
            } else {
                for (size_t i = 0; i < rawBands.size(); ++i) {
                    smoothSpectrum[i] = smoothSpectrum[i] * smoothing + rawBands[i] * (1.0f - smoothing);
                }
            }

            // 写入输出缓冲区
            {
                std::lock_guard<std::mutex> lock(spectrumMutex);
                spectrumOutput = smoothSpectrum;
            }
        }
    }

    pAudioClient->Stop();
    pCaptureClient->Release();
    pAudioClient->Release();
    CoUninitialize();
    Log("[Spectrum] Capture stopped\n");
}

SpectrumCapture::SpectrumCapture()
    : impl_(std::make_unique<Impl>()) {
    impl_->ringBuffer.resize(Impl::kRingBufferSize, 0.0f);
}

SpectrumCapture::~SpectrumCapture() {
    Stop();
}

bool SpectrumCapture::Start() {
    if (running_.load()) return true;

    running_ = true; // 先置位避免阻塞调用方
    impl_->captureThread = std::make_unique<std::thread>(
        &Impl::CaptureLoop, impl_.get(), this);
    return true;
}

void SpectrumCapture::Stop() {
    if (!running_.load()) return;

    running_ = false;
    if (impl_->captureThread && impl_->captureThread->joinable()) {
        DWORD waitResult = ::WaitForSingleObject(
            impl_->captureThread->native_handle(),
            moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
        if (waitResult == WAIT_TIMEOUT) {
            Log("[Spectrum] Thread join timed out (%d ms), detaching\n",
                moekoe::constants::THREAD_JOIN_TIMEOUT_MS);
            impl_->captureThread->detach();
        } else {
            impl_->captureThread->join();
        }
    }
    impl_->captureThread.reset();
}

std::vector<float> SpectrumCapture::GetSpectrum(int numBands) {
    if (!running_.load()) return {};

    std::lock_guard<std::mutex> lock(impl_->spectrumMutex);
    if (impl_->spectrumOutput.empty()) return {};

    // 如果请求的频段数与内部不同，重新映射
    if (numBands == static_cast<int>(impl_->spectrumOutput.size())) {
        return impl_->spectrumOutput;
    }

    // 简单下采样/上采样
    std::vector<float> result(static_cast<size_t>(numBands), 0.0f);
    const auto& src = impl_->spectrumOutput;
    for (int i = 0; i < numBands; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(numBands);
        size_t srcIdx = static_cast<size_t>(t * static_cast<float>(src.size() - 1));
        if (srcIdx < src.size()) {
            result[static_cast<size_t>(i)] = src[srcIdx];
        }
    }
    return result;
}

} // namespace moekoe
