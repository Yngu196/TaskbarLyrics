#include <catch2/catch_all.hpp>

#include "render/spectrum_math.h"

#include <cmath>
#include <limits>

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

using moekoe::spectrum_math::LogBands;
using moekoe::spectrum_math::MagToNormalized;

TEST_CASE("MagToNormalized clamps dB range", "[spectrum]") {
    REQUIRE(MagToNormalized(0.0f, -62.0f, -10.0f, 1024) == 0.0f);
    REQUIRE(std::abs(MagToNormalized(256.0f, -62.0f, -10.0f, 1024) - 1.0f) < 1e-5f);
    REQUIRE(std::abs(MagToNormalized(0.1f, -62.0f, -10.0f, 1024) - 0.0f) < 1e-5f);
    REQUIRE(MagToNormalized(std::numeric_limits<float>::quiet_NaN(), -62.0f, -10.0f, 1024) == 0.0f);
}

TEST_CASE("MagToNormalized rejects invalid parameters", "[spectrum]") {
    REQUIRE(MagToNormalized(1.0f, -10.0f, -10.0f, 1024) == 0.0f);
    REQUIRE(MagToNormalized(1.0f, -10.0f, -20.0f, 1024) == 0.0f);
    REQUIRE(MagToNormalized(1.0f, -62.0f, -10.0f, 0) == 0.0f);
}

TEST_CASE("LogBands produces bounded count and preserves peak", "[spectrum]") {
    std::vector<float> magnitudes(512, 0.0f);
    magnitudes[10] = 0.75f;
    magnitudes[200] = 0.40f;
    std::vector<float> bands;

    LogBands(magnitudes, 32, 48000.0f, 1024, bands);

    REQUIRE(bands.size() == 32);
    REQUIRE(std::abs(*std::max_element(bands.begin(), bands.end()) - 0.75f) < 1e-5f);
    REQUIRE(std::all_of(bands.begin(), bands.end(), [](float v) { return v >= 0.0f; }));
}

TEST_CASE("LogBands rejects invalid input without division by zero", "[spectrum]") {
    std::vector<float> bands{1.0f};
    LogBands({}, 0, 48000.0f, 1024, bands);
    REQUIRE(bands.empty());
    LogBands({}, 16, 0.0f, 1024, bands);
    REQUIRE(bands.empty());
    LogBands({}, 16, 48000.0f, 0, bands);
    REQUIRE(bands.empty());
}
