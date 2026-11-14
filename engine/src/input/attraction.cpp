#include "wp/attraction.h"

#include <atomic>

namespace wp::attraction {

namespace {

std::atomic<Mode> g_mode{Mode::None};
std::atomic<DanceSwing> g_swing{nullptr};
std::atomic<DancePlaying> g_playing{nullptr};

}

void set_mode(Mode mode) {
    g_mode = mode;
}

Mode mode() {
    return g_mode;
}

void set_dance(DanceSwing swing, DancePlaying playing) {
    g_swing = swing;
    g_playing = playing;
}

int dance_swing(uint32_t moves, uint32_t report) {
    DanceSwing swing = g_swing;
    return swing ? swing(moves, report) : 0;
}

bool dance_playing() {
    DancePlaying playing = g_playing;
    return playing && playing();
}

}
