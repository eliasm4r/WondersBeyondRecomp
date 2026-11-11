#include "wp/control_names.h"

#include <cctype>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

#include "wp/keymap.h"
#include "wp/ui_text.h"

namespace wp::control_names {

namespace {

constexpr int kFamilyCount = 6;

constexpr const char* kPadNames[padmap::kPhysicalCount][kFamilyCount] = {
    {"A", "A", "pad.cross", "pad.cross", "pad.cross", "B"},
    {"B", "B", "pad.circle", "pad.circle", "pad.circle", "A"},
    {"X", "X", "pad.square", "pad.square", "pad.square", "Y"},
    {"Y", "Y", "pad.triangle", "pad.triangle", "pad.triangle", "X"},
    {"View", "Back", "Select", "Share", "Create", "\xE2\x88\x92"},
    {"Xbox", "Xbox", "PS", "PS", "PS", "HOME"},
    {"Menu", "Start", "Start", "Options", "Options", "+"},
    {"LS", "LS", "L3", "L3", "L3", "pad.left_stick_press"},
    {"RS", "RS", "R3", "R3", "R3", "pad.right_stick_press"},
    {"LB", "LB", "L1", "L1", "L1", "L"},
    {"RB", "RB", "R1", "R1", "R1", "R"},
    {"pad.dpad_up", "pad.dpad_up", "pad.dpad_up", "pad.dpad_up", "pad.dpad_up", "pad.dpad_up"},
    {"pad.dpad_down", "pad.dpad_down", "pad.dpad_down", "pad.dpad_down", "pad.dpad_down", "pad.dpad_down"},
    {"pad.dpad_left", "pad.dpad_left", "pad.dpad_left", "pad.dpad_left", "pad.dpad_left", "pad.dpad_left"},
    {"pad.dpad_right", "pad.dpad_right", "pad.dpad_right", "pad.dpad_right", "pad.dpad_right", "pad.dpad_right"},
    {"LT", "LT", "L2", "L2", "L2", "ZL"},
    {"RT", "RT", "R2", "R2", "R2", "ZR"},
    {"pad.right_stick_up", "pad.right_stick_up", "pad.right_stick_up", "pad.right_stick_up", "pad.right_stick_up", "pad.right_stick_up"},
    {"pad.right_stick_down", "pad.right_stick_down", "pad.right_stick_down", "pad.right_stick_down", "pad.right_stick_down", "pad.right_stick_down"},
    {"pad.right_stick_left", "pad.right_stick_left", "pad.right_stick_left", "pad.right_stick_left", "pad.right_stick_left", "pad.right_stick_left"},
    {"pad.right_stick_right", "pad.right_stick_right", "pad.right_stick_right", "pad.right_stick_right", "pad.right_stick_right", "pad.right_stick_right"},
};

constexpr const char* kNamedKeys[] = {
    "MouseLeft", "MouseRight", "MouseMiddle", "MouseX1",   "MouseX2",    "WheelUp",   "WheelDown", "Backspace", "Tab",
    "Enter",     "Shift",      "Ctrl",        "Alt",       "Pause",      "CapsLock",  "Escape",    "Space",     "PageUp",
    "PageDown",  "End",        "Home",        "Left",      "Up",         "Right",     "Down",      "Insert",    "Delete",
    "LeftShift", "RightShift", "LeftCtrl",    "RightCtrl", "LeftAlt",    "RightAlt",
};

struct Numpad {
    const char* name;
    const char* symbol;
};

constexpr Numpad kNumpad[] = {
    {"Num0", "0"}, {"Num1", "1"}, {"Num2", "2"}, {"Num3", "3"}, {"Num4", "4"}, {"Num5", "5"}, {"Num6", "6"}, {"Num7", "7"},
    {"Num8", "8"}, {"Num9", "9"}, {"NumMultiply", "*"}, {"NumPlus", "+"}, {"NumMinus", "-"}, {"NumPeriod", "."}, {"NumDivide", "/"},
};

std::string utf8(uint32_t c) {
    std::string out;
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    return out;
}

uint32_t layout_character(int code) {
#ifdef _WIN32
    if (code < 0 || code > 0xFF) {
        return 0;
    }
    return MapVirtualKeyW(static_cast<UINT>(code), MAPVK_VK_TO_CHAR) & 0x7FFF;
#else
    (void)code;
    return 0;
#endif
}

}

std::string key(int code, const std::function<uint32_t(int code)>& character) {
    std::string name = keymap::key_name(code);
    for (const char* named : kNamedKeys) {
        if (name == named) {
            return ui::label(("key." + name).c_str());
        }
    }
    for (const Numpad& entry : kNumpad) {
        if (name == entry.name) {
            return std::string(ui::label("key.numpad")) + " " + entry.symbol;
        }
    }
    if (name.size() >= 2 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1]))) {
        return name;
    }
    uint32_t c = character(code);
    if (c >= 0x20 && c != 0x7F && c <= 0xFFFF) {
        if (c >= 'a' && c <= 'z') {
            c -= 'a' - 'A';
        }
        return utf8(c);
    }
    return name;
}

std::string key(int code) {
    return key(code, layout_character);
}

std::string keys(const std::vector<int>& codes) {
    std::string result;
    for (int code : codes) {
        std::string name = key(code);
        if (!name.empty()) {
            result += (result.empty() ? "" : ", ") + name;
        }
    }
    return result.empty() ? "\xE2\x80\x94" : result;
}

std::string pad_button(int physical, padmap::Family family) {
    if (physical < 0 || physical >= padmap::kPhysicalCount) {
        return "";
    }
    const char* name = kPadNames[physical][static_cast<int>(family)];
    return std::strncmp(name, "pad.", 4) == 0 ? ui::label(name) : name;
}

std::string pad_binding(const padmap::Binding& binding, padmap::Family family) {
    std::string result;
    for (padmap::Combo combo : binding) {
        std::string part;
        for (int i = 0; i < padmap::kPhysicalCount; i++) {
            if (combo & (padmap::Combo{1} << i)) {
                part += (part.empty() ? "" : " + ") + pad_button(i, family);
            }
        }
        if (!part.empty()) {
            result += (result.empty() ? "" : ", ") + part;
        }
    }
    return result.empty() ? "\xE2\x80\x94" : result;
}

}
