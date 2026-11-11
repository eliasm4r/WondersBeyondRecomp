#pragma once

#include <cstdint>
#include <vector>

namespace wp::color {

struct Range {
    const char* key;
    int low;
    int high;
};

inline constexpr Range kBrightness{"video.brightness", 50, 150};
inline constexpr Range kContrast{"video.contrast", 50, 150};
inline constexpr Range kSaturation{"video.saturation", 0, 200};
inline constexpr Range kGamma{"video.gamma", 50, 150};
inline constexpr Range kRanges[] = {kBrightness, kContrast, kSaturation, kGamma};

struct Adjust {
    float brightness = 1.0f;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float gamma = 1.0f;
};

Adjust current();
bool neutral(const Adjust& adjust);
uint32_t apply(uint32_t rgb, const Adjust& adjust);
void apply(std::vector<uint32_t>& pixels, const Adjust& adjust);

}
