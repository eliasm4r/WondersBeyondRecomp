#include "wp/attraction.h"

#include <atomic>

#include "wp/control_names.h"
#include "wp/gamepad.h"
#include "wp/input.h"
#include "wp/keymap.h"
#include "wp/padmap.h"

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

Label move_label(uint32_t move) {
    Label label;
    Arrow arrow = move == kMoveLeft ? Arrow::Left : move == kMoveUp ? Arrow::Up : move == kMoveDown ? Arrow::Down : move == kMoveRight ? Arrow::Right : Arrow::None;
    if (input::gamepad_in_use()) {
        if (arrow != Arrow::None) {
            label.arrow = arrow;
            return label;
        }
        padmap::Binding binding = padmap::current_mapping()[move == kMoveKick ? 0 : 3];
        if (binding.empty()) {
            return label;
        }
        padmap::Family family = gamepad::shown_family();
        bool playstation = family == padmap::Family::PlayStation3 || family == padmap::Family::PlayStation4 || family == padmap::Family::PlayStation5;
        constexpr Shape kShapes[] = {Shape::Cross, Shape::Circle, Shape::Square, Shape::Triangle};
        for (int i = 0; i < 4; i++) {
            if (playstation && binding.front() == (padmap::Combo{1} << i)) {
                label.shape = kShapes[i];
                return label;
            }
        }
        label.text = control_names::pad_binding({binding.front()}, family);
        return label;
    }
    keymap::Action action = arrow == Arrow::Left    ? keymap::Action::Left
                            : arrow == Arrow::Up    ? keymap::Action::Up
                            : arrow == Arrow::Down  ? keymap::Action::Down
                            : arrow == Arrow::Right ? keymap::Action::Right
                            : move == kMoveKick     ? keymap::Action::A
                                                    : keymap::Action::Two;
    std::vector<int> codes = input::bindings().of(action);
    if (codes.empty()) {
        return label;
    }
    std::string name = keymap::key_name(codes.front());
    label.arrow = name == "Left" ? Arrow::Left : name == "Up" ? Arrow::Up : name == "Down" ? Arrow::Down : name == "Right" ? Arrow::Right : Arrow::None;
    if (name == "Space") {
        label.shape = Shape::SpaceBar;
    } else if (label.arrow == Arrow::None) {
        label.text = control_names::key(codes.front());
    }
    return label;
}

}
