#pragma once

#include <cstdint>

namespace wp::pacing {

#if defined(_WIN32) && !defined(WP_OPENGL)
constexpr bool kInBetweenImages = true;
#else
constexpr bool kInBetweenImages = false;
#endif

struct FramePlan {
    uint32_t images = 0;
    float fractions[3] = {};
    bool show_real = true;
    double slot = 0.0;
};

FramePlan plan_frame(double& next, double period, int rate, uint32_t max_images);
bool interpolation_wanted(int fps, int display_hz);
void set_display_hz(int hz);
int display_hz();
int output_rate(int fps);

struct AlarmPeriods {
    uint64_t period = 0;
    uint64_t pending = 0;
};

AlarmPeriods frame_alarm(uint64_t input, uint64_t period, uint64_t pending, int fps, bool movie);

}
