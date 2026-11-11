#include "wp/color.h"

#include <algorithm>
#include <cmath>

#include "wp/settings.h"

namespace wp::color {

namespace {

float fraction(const settings::LiveNumber& number, const Range& range) {
    return static_cast<float>(std::clamp(number(), range.low, range.high)) / 100.0f;
}

}

Adjust current() {
    static const settings::LiveNumber brightness(kBrightness.key, nullptr);
    static const settings::LiveNumber contrast(kContrast.key, nullptr);
    static const settings::LiveNumber saturation(kSaturation.key, nullptr);
    static const settings::LiveNumber gamma(kGamma.key, nullptr);
    return {fraction(brightness, kBrightness), fraction(contrast, kContrast), fraction(saturation, kSaturation), fraction(gamma, kGamma)};
}

bool neutral(const Adjust& adjust) {
    return adjust.brightness == 1.0f && adjust.contrast == 1.0f && adjust.saturation == 1.0f && adjust.gamma == 1.0f;
}

uint32_t apply(uint32_t rgb, const Adjust& adjust) {
    if (neutral(adjust)) {
        return rgb;
    }
    float c[3] = {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(rgb & 0xFF) / 255.0f};
    for (float& value : c) {
        value = ((value - 0.5f) * adjust.contrast + 0.5f) * adjust.brightness;
    }
    float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    uint32_t result = 0;
    for (float value : c) {
        value = std::clamp(luma + (value - luma) * adjust.saturation, 0.0f, 1.0f);
        value = std::pow(value, 1.0f / adjust.gamma);
        result = (result << 8) | static_cast<uint32_t>(std::lround(value * 255.0f));
    }
    return (rgb & 0xFF000000u) | result;
}

void apply(std::vector<uint32_t>& pixels, const Adjust& adjust) {
    if (neutral(adjust)) {
        return;
    }
    for (uint32_t& pixel : pixels) {
        pixel = apply(pixel, adjust);
    }
}

}
