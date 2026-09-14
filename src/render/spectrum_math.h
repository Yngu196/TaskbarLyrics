// spectrum_math.h - 频谱纯数学逻辑（无 Win32/WASAPI 依赖）
#pragma once

#include "core/constants.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace moekoe::spectrum_math {

inline void LogBands(const std::vector<float>& magnitudes, int numBands,
                     float sampleRate, int fftSize,
                     std::vector<float>& outBands) {
    if (numBands <= 0 || fftSize <= 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0f) {
        outBands.clear();
        return;
    }
    outBands.assign(static_cast<size_t>(numBands), 0.0f);
    if (magnitudes.empty()) return;

    const float freqPerBin = sampleRate / static_cast<float>(fftSize);
    const float logMin = std::log10(constants::SPECTRUM_MIN_FREQ);
    const float logMax = std::log10(constants::SPECTRUM_MAX_FREQ);
    for (int b = 0; b < numBands; ++b) {
        const float t = static_cast<float>(b) / static_cast<float>(numBands);
        const float nextT = static_cast<float>(b + 1) / static_cast<float>(numBands);
        const float freqLow = std::pow(10.0f, logMin + t * (logMax - logMin));
        const float freqHigh = std::pow(10.0f, logMin + nextT * (logMax - logMin));
        const int binLow = (std::max)(0, static_cast<int>(freqLow / freqPerBin));
        const int binHigh = (std::min)(static_cast<int>(magnitudes.size()) - 1,
                                       static_cast<int>(freqHigh / freqPerBin));
        float peak = 0.0f;
        for (int i = binLow; i <= binHigh; ++i) {
            peak = (std::max)(peak, magnitudes[static_cast<size_t>(i)]);
        }
        outBands[static_cast<size_t>(b)] = peak;
    }
}

inline float MagToNormalized(float mag, float dbFloor, float dbCeil, int fftSize) {
    if (!std::isfinite(mag) || !std::isfinite(dbFloor) ||
        !std::isfinite(dbCeil) || dbCeil <= dbFloor || fftSize <= 0) {
        return 0.0f;
    }
    const float db = 20.0f * std::log10(mag / (static_cast<float>(fftSize) / 4.0f) + 1e-9f);
    const float value = (db - dbFloor) / (dbCeil - dbFloor);
    return (std::min)(1.0f, (std::max)(0.0f, value));
}

} // namespace moekoe::spectrum_math
