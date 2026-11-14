#include "wp/frame_pacing.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace wp::pacing {

namespace {

constexpr double kOnFrame = 1e-3;
constexpr uint32_t kMaxImages = 3;
constexpr double kFineGrain = 0.05;
constexpr int kLowestInterpolatedRate = 85;
std::atomic<int> g_display_hz{0};

}

FramePlan plan_frame(double& next, double period, int rate, uint32_t max_images) {
    FramePlan plan;
    max_images = std::min(max_images, kMaxImages);
    double fastest = rate / 60.0;
    double grain = std::fmod(fastest, 1.0) == 0.0 ? 1.0 : std::fmod(fastest, 0.5) == 0.0 ? 0.5 : kFineGrain;
    double outputs = std::min(std::max(fastest, std::round(rate * period / grain) * grain), static_cast<double>(max_images + 1));
    double step = 1.0 / outputs;
    plan.slot = period * step;
    if (next <= 0.0 || std::fmod(outputs, 1.0) == 0.0) {
        next = step;
    }
    while (next < 1.0 - kOnFrame && plan.images < max_images) {
        plan.fractions[plan.images++] = static_cast<float>(next);
        next += step;
    }
    plan.show_real = next < 1.0 + kOnFrame;
    next = plan.show_real ? 0.0 : next - 1.0;
    return plan;
}

int capped_rate(int fps, int display_hz) {
    return display_hz > 0 ? std::min(fps, display_hz) : fps;
}

bool interpolation_wanted(int fps, int display_hz) {
    return fps > 60 && capped_rate(fps, display_hz) >= kLowestInterpolatedRate;
}

void set_display_hz(int hz) {
    g_display_hz = hz;
}

int display_hz() {
    return g_display_hz;
}

int output_rate(int fps) {
    return capped_rate(fps, g_display_hz);
}

AlarmPeriods frame_alarm(uint64_t input, uint64_t period, uint64_t pending, int fps, bool movie) {
    AlarmPeriods result{period, pending};
    if (input == 0) {
        return result;
    }
    if (fps == 45 && !movie) {
        uint64_t real = input * 4 / 3;
        uint64_t marker = input * 2 + 1;
        if (period == real && pending == real) {
            result = {marker, marker};
        } else if (period != marker || pending != marker) {
            result.pending = real;
        }
        return result;
    }
    result.pending = fps >= 60 && !movie ? input : input * 2;
    return result;
}

}
