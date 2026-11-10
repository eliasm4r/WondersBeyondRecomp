#include "wp/options_window.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/game.h"
#include "wp/gamepad.h"
#include "wp/input.h"
#include "wp/keymap.h"
#include "wp/log.h"
#include "wp/nand.h"
#include "wp/padmap.h"
#include "wp/runtime.h"
#include "wp/settings.h"
#include "wp/ui_text.h"
#include "wp/video.h"

namespace wp::options {

namespace {

constexpr uint32_t kRepeating = gamepad::kMenuUp | gamepad::kMenuDown | gamepad::kMenuLeft | gamepad::kMenuRight;
constexpr int64_t kRepeatDelay = 400;
constexpr int64_t kRepeatInterval = 120;

Menu g_menu;
bool g_shown = false;
// A gamepad binding waits for every button to be released first, so the button that opened it is not bound.
bool g_pad_capture_armed = false;

bool menu_enabled() {
    static const settings::LiveFlag value("system.options_menu", "WP_OPTIONS_MENU");
    return value();
}

constexpr unsigned kKeyBack = 0x08;
constexpr unsigned kKeyTab = 0x09;
constexpr unsigned kKeyReturn = 0x0D;
constexpr unsigned kKeyShift = 0x10;
constexpr unsigned kKeyEscape = 0x1B;
constexpr unsigned kKeySpace = 0x20;
constexpr unsigned kKeyPageUp = 0x21;
constexpr unsigned kKeyPageDown = 0x22;
constexpr unsigned kKeyLeft = 0x25;
constexpr unsigned kKeyUp = 0x26;
constexpr unsigned kKeyRight = 0x27;
constexpr unsigned kKeyDown = 0x28;
constexpr unsigned kKeyE = 'E';
constexpr unsigned kKeyQ = 'Q';

Layout current_layout(int client_height) {
    return layout(client_height, g_menu.rows().size(), g_menu.scroll());
}

int client_height() {
    int width = 0;
    int height = 0;
    return video::client_size(width, height) ? height : 0;
}

void sync(void*) {
    if (g_menu.open()) {
        show_overlay(render(g_menu, client_height()));
        if (!g_shown) {
            g_shown = true;
            input::set_blocked(true);
        }
    } else if (g_shown) {
        g_shown = false;
        hide_overlay();
        input::set_blocked(false);
    }
}

std::string act(void* window, Action action) {
    std::string changed = g_menu.handle(action);
    g_pad_capture_armed = false;
    sync(window);
    return changed;
}

std::string capture(void* window, const std::string& name) {
    std::string changed = g_menu.capture(name);
    sync(window);
    return changed;
}

bool hit(void* window, int x, int y, Layout& frame, Rect& placed, Hover& result) {
    (void)window;
    int width = 0;
    int height = 0;
    if (!video::client_size(width, height)) {
        return false;
    }
    frame = current_layout(height);
    placed = place(width, height, frame.width, frame.height);
    result = hit_test(frame, placed, x, y);
    return true;
}

}

bool menu_open() {
    return g_menu.open();
}

void toggle_menu(void* window) {
    if (!g_menu.open() && !menu_enabled()) {
        return;
    }
    g_menu.set_open(!g_menu.open());
    sync(window);
}

std::string menu_key(void* window, unsigned key, bool repeat) {
    if (!g_menu.open()) {
        return "";
    }
    if (g_menu.capturing()) {
        if (repeat) {
            return "";
        }
        if (key == kKeyEscape || g_menu.capture_kind() != Kind::Key) {
            return act(window, Action::Close);
        }
        std::string name = keymap::key_name(static_cast<int>(key));
        if (name.empty() || keymap::reserved(static_cast<int>(key))) {
            return "";
        }
        return capture(window, name);
    }
    switch (key) {
    case kKeyUp:
        return act(window, Action::Up);
    case kKeyDown:
        return act(window, Action::Down);
    case kKeyLeft:
        return act(window, Action::Previous);
    case kKeyRight:
        return act(window, Action::Next);
    case kKeyQ:
    case kKeyPageUp:
        return repeat ? "" : act(window, Action::TabPrevious);
    case kKeyE:
    case kKeyPageDown:
    case kKeyTab:
        return repeat ? "" : act(window, Action::TabNext);
    case kKeyReturn:
    case kKeySpace:
        return repeat ? "" : act(window, Action::Accept);
    case kKeyEscape:
    case kKeyBack:
        return repeat ? "" : act(window, Action::Close);
    default:
        (void)kKeyShift;
        return "";
    }
}

std::string menu_wheel(void* window, int delta) {
    if (!g_menu.open() || delta == 0) {
        return "";
    }
    g_menu.scroll_by(delta > 0 ? -1 : 1);
    sync(window);
    return "";
}

std::string menu_click(void* window, int x, int y, bool right) {
    if (!g_menu.open()) {
        return "";
    }
    if (g_menu.capturing() && g_menu.capture_kind() == Kind::Key) {
        return capture(window, right ? "MouseRight" : "MouseLeft");
    }
    if (right) {
        return act(window, Action::Close);
    }
    Layout frame;
    Rect placed;
    Hover target;
    if (!hit(window, x, y, frame, placed, target)) {
        return "";
    }
    if (target.part == Part::Close) {
        return act(window, Action::Close);
    }
    if (target.part == Part::Tab) {
        g_menu.select_tab(static_cast<size_t>(target.tab));
        sync(window);
        return "";
    }
    if (target.row < 0) {
        return "";
    }
    size_t row = static_cast<size_t>(target.row);
    g_menu.select(row);
    Kind type = g_menu.rows()[row].kind;
    if (type == Kind::Slider && target.part != Part::Label) {
        std::string changed = g_menu.set_fraction(row, slider_fraction(frame, placed, row - g_menu.scroll(), x));
        sync(window);
        return changed;
    }
    if (type == Kind::Key || type == Kind::Pad || type == Kind::Button) {
        return act(window, Action::Accept);
    }
    if (target.part == Part::Label) {
        sync(window);
        return "";
    }
    return act(window, target.part == Part::Previous ? Action::Previous : Action::Next);
}

std::string menu_drag(void* window, int x, int y) {
    if (!g_menu.open()) {
        return "";
    }
    Layout frame;
    Rect placed;
    Hover target;
    if (!hit(window, x, y, frame, placed, target) || target.row < 0 || static_cast<size_t>(target.row) != g_menu.selection()) {
        return "";
    }
    size_t row = static_cast<size_t>(target.row);
    if (g_menu.rows()[row].kind != Kind::Slider) {
        return "";
    }
    std::string changed = g_menu.set_fraction(row, slider_fraction(frame, placed, row - g_menu.scroll(), x));
    sync(window);
    return changed;
}

void menu_hover(void* window, int x, int y) {
    if (!g_menu.open()) {
        return;
    }
    Layout frame;
    Rect placed;
    Hover target;
    hit(window, x, y, frame, placed, target);
    if (g_menu.set_hover(target)) {
        sync(window);
    }
}

std::string poll_gamepads(void* window) {
    static Repeat repeat(kRepeating, kRepeatDelay, kRepeatInterval);
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    bool focused = video::window_focused();
    if (g_menu.open() && g_menu.capturing() && g_menu.capture_kind() == Kind::Pad) {
        int physical = focused ? gamepad::held_physical() : -1;
        if (!g_pad_capture_armed) {
            g_pad_capture_armed = physical < 0;
            return "";
        }
        if (physical < 0) {
            return "";
        }
        g_pad_capture_armed = false;
        repeat.update(gamepad::menu_buttons(), now);
        return capture(window, padmap::physical_name(physical));
    }
    uint32_t fired = repeat.update(focused ? gamepad::menu_buttons() : 0, now);
    if (fired & gamepad::kMenuToggle) {
        toggle_menu(window);
        return "";
    }
    if (!g_menu.open() || fired == 0) {
        return "";
    }
    if (fired & gamepad::kMenuCancel) {
        return act(window, Action::Close);
    }
    std::string changed;
    if (fired & gamepad::kMenuTabPrevious) {
        act(window, Action::TabPrevious);
    }
    if (fired & gamepad::kMenuTabNext) {
        act(window, Action::TabNext);
    }
    if (fired & gamepad::kMenuUp) {
        act(window, Action::Up);
    }
    if (fired & gamepad::kMenuDown) {
        act(window, Action::Down);
    }
    if (fired & gamepad::kMenuLeft) {
        changed = act(window, Action::Previous);
    }
    if (fired & gamepad::kMenuRight) {
        changed = act(window, Action::Next);
    }
    if (fired & gamepad::kMenuAccept) {
        changed = act(window, Action::Accept);
    }
    return changed;
}

void refresh_menu(void* window) {
    if (g_menu.open()) {
        sync(window);
    }
}

}
