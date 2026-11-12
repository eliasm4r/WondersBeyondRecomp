#include "wp/gamepad.h"

namespace wp::gamepad {

bool connected(uint32_t) {
    return false;
}

State poll(uint32_t, bool) {
    return State{};
}

void set_outputs(uint32_t, bool, uint8_t) {}

bool camera_stick(float& x, float& y) {
    x = 0.0f;
    y = 0.0f;
    return false;
}

uint32_t menu_buttons() {
    return 0;
}

padmap::Family shown_family() {
    return padmap::Family::Xbox;
}

}
