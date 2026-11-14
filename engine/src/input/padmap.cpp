#include "wp/padmap.h"

#include <algorithm>
#include <cctype>

#include "wp/game.h"
#include "wp/input.h"
#include "wp/settings.h"

namespace wp::padmap {

namespace {

struct Action {
    const char* name;
    uint32_t button;
    const char* sideways;
    const char* upright;
};

constexpr Action kActions[kActionCount] = {
    {"a", input::kButtonA, "north,right_shoulder", "south,right_shoulder"},
    {"b", input::kButtonB, "right_trigger", "east,right_trigger"},
    {"one", input::kButtonOne, "east,west", "west"},
    {"two", input::kButtonTwo, "south", "north"},
    {"plus", input::kButtonPlus, "start", "start"},
    {"minus", input::kButtonMinus, "back", "back"},
    {"home", input::kButtonHome, "guide", "guide"},
    {"shake", input::kMotionShake, "left_trigger", "left_trigger"},
    {"swing_up", input::kMotionSwingUp, "left_shoulder+dpad_up", "left_shoulder+dpad_up"},
    {"swing_down", input::kMotionSwingDown, "left_shoulder+dpad_down", "left_shoulder+dpad_down"},
    {"tilt_left", input::kMotionTiltLeft, "left_shoulder+right_stick_left", "left_shoulder+right_stick_left"},
    {"tilt_right", input::kMotionTiltRight, "left_shoulder+right_stick_right", "left_shoulder+right_stick_right"},
    {"tilt_up", input::kMotionTiltUp, "left_shoulder+right_stick_up", "left_shoulder+right_stick_up"},
    {"tilt_down", input::kMotionTiltDown, "left_shoulder+right_stick_down", "left_shoulder+right_stick_down"},
    {"shake_left", input::kMotionShakeLeft, "left_shoulder", "left_shoulder"},
    {"shake_right", input::kMotionShakeRight, "right_shoulder", "right_shoulder"},
    {"pointer", 0, "left_stick", "left_stick"},
};

constexpr const char* kPhysicalNames[kPhysicalCount] = {
    "south", "east", "west", "north", "back", "guide", "start", "left_stick", "right_stick",
    "left_shoulder", "right_shoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "left_trigger", "right_trigger",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

std::string trimmed_lower(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = text.find_last_not_of(" \t\r\n");
    std::string result = text.substr(first, last - first + 1);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t end = text.find(separator, start);
        parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            return parts;
        }
        start = end + 1;
    }
}

int lowest(Combo combo) {
    for (int i = 0; i < kPhysicalCount; i++) {
        if (combo & (Combo{1} << i)) {
            return i;
        }
    }
    return kPhysicalCount;
}

Binding parse_text(const std::string& text) {
    Binding binding;
    for (const std::string& part : split(text, ',')) {
        Combo combo = 0;
        for (const std::string& name : split(part, '+')) {
            int physical = physical_from_name(trimmed_lower(name));
            if (physical >= 0) {
                combo |= Combo{1} << physical;
            }
        }
        if (combo != 0 && std::find(binding.begin(), binding.end(), combo) == binding.end()) {
            binding.push_back(combo);
        }
    }
    std::sort(binding.begin(), binding.end(), [](Combo a, Combo b) { return lowest(a) != lowest(b) ? lowest(a) < lowest(b) : a < b; });
    return binding;
}

Combo mask_of(const Pressed& pressed) {
    Combo mask = 0;
    for (int i = 0; i < kPhysicalCount; i++) {
        if (pressed.held[i]) {
            mask |= Combo{1} << i;
        }
    }
    return mask;
}

}

const char* action_name(size_t action) {
    return action < kActionCount ? kActions[action].name : "";
}

uint32_t action_button(size_t action) {
    return action < kActionCount ? kActions[action].button : 0;
}

std::string setting_key(size_t action) {
    return std::string("pad.") + action_name(action);
}

const char* physical_name(int physical) {
    return physical >= 0 && physical < kPhysicalCount ? kPhysicalNames[physical] : "";
}

int physical_from_name(const std::string& name) {
    for (int i = 0; i < kPhysicalCount; i++) {
        if (name == kPhysicalNames[i]) {
            return i;
        }
    }
    return -1;
}

Binding default_binding(size_t action, bool sideways) {
    if (action >= kActionCount) {
        return {};
    }
    return parse_text(sideways ? kActions[action].sideways : kActions[action].upright);
}

Binding parse_binding(const std::string& text, size_t action, bool sideways) {
    if (trimmed_lower(text) == "none") {
        return {};
    }
    Binding binding = parse_text(text);
    return binding.empty() ? default_binding(action, sideways) : binding;
}

std::string format_binding(const Binding& binding) {
    std::string result;
    for (Combo combo : binding) {
        std::string part;
        for (int i = 0; i < kPhysicalCount; i++) {
            if (combo & (Combo{1} << i)) {
                part += (part.empty() ? "" : "+") + std::string(kPhysicalNames[i]);
            }
        }
        if (!part.empty()) {
            result += (result.empty() ? "" : ",") + part;
        }
    }
    return result.empty() ? "none" : result;
}

std::string without(const std::string& text, int physical, size_t action, bool sideways) {
    Binding binding = parse_binding(text, action, sideways);
    Combo single = Combo{1} << physical;
    binding.erase(std::remove(binding.begin(), binding.end(), single), binding.end());
    return format_binding(binding);
}

Mapping current_mapping() {
    bool sideways = game::description().sideways_grip;
    Mapping mapping;
    for (size_t i = 0; i < kActionCount; i++) {
        mapping[i] = parse_binding(settings::text(setting_key(i).c_str(), nullptr), i, sideways);
    }
    return mapping;
}

bool held(const Binding& binding, const Pressed& pressed) {
    Combo mask = mask_of(pressed);
    for (Combo combo : binding) {
        if (combo != 0 && (mask & combo) == combo) {
            return true;
        }
    }
    return false;
}

uint32_t buttons(const Pressed& pressed, const Mapping& mapping) {
    Combo mask = mask_of(pressed);
    std::vector<std::pair<Combo, uint32_t>> satisfied;
    for (size_t i = 0; i < kActionCount; i++) {
        for (Combo combo : mapping[i]) {
            if (combo != 0 && (mask & combo) == combo) {
                satisfied.push_back({combo, kActions[i].button});
            }
        }
    }
    uint32_t result = 0;
    for (const auto& entry : satisfied) {
        bool covered = std::any_of(satisfied.begin(), satisfied.end(), [&](const std::pair<Combo, uint32_t>& other) {
            return other.first != entry.first && (other.first & entry.first) == entry.first;
        });
        if (!covered) {
            result |= entry.second;
        }
    }
    return result;
}

std::string device_key(const std::string& name, int ordinal) {
    return name + "#" + std::to_string(ordinal + 1);
}

bool listed(const std::string& list, const std::string& key) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (item == key) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

std::string toggled(const std::string& list, const std::string& key) {
    std::string result;
    bool found = false;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (item == key) {
            found = true;
        } else if (!item.empty()) {
            result += (result.empty() ? "" : ";") + item;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!found) {
        result += (result.empty() ? "" : ";") + key;
    }
    return result;
}

int assigned(const std::string& list, const std::string& key) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t equals = item.rfind('=');
        if (equals != std::string::npos && item.compare(0, equals, key) == 0 && equals + 2 == item.size() && item[equals + 1] >= '1' && item[equals + 1] <= '4') {
            return item[equals + 1] - '0';
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return 0;
}

std::string assign(const std::string& list, const std::string& key, int player) {
    std::string result;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(';', start);
        std::string item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t equals = item.rfind('=');
        bool same = equals != std::string::npos && item.compare(0, equals, key) == 0;
        if (!same && !item.empty()) {
            result += (result.empty() ? "" : ";") + item;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (player >= 1 && player <= 4) {
        result += (result.empty() ? "" : ";") + key + "=" + std::to_string(player);
    }
    return result;
}

std::vector<int> plan_slots(const std::vector<std::string>& keys, const std::vector<int>& current, const std::string& ignored, const std::string& assignments,
                            bool player1_keyboard_only) {
    constexpr int kSlotCount = 4;
    std::vector<int> result(keys.size(), -1);
    bool used[kSlotCount] = {};
    auto usable = [&](size_t i) { return !listed(ignored, keys[i]); };
    for (size_t i = 0; i < keys.size(); i++) {
        int wanted = assigned(assignments, keys[i]);
        if (usable(i) && wanted > 0 && !used[wanted - 1]) {
            result[i] = wanted - 1;
            used[wanted - 1] = true;
        }
    }
    for (size_t i = 0; i < keys.size(); i++) {
        int slot = i < current.size() ? current[i] : -1;
        if (usable(i) && result[i] < 0 && assigned(assignments, keys[i]) == 0 && slot >= 0 && slot < kSlotCount && !used[slot]) {
            result[i] = slot;
            used[slot] = true;
        }
    }
    int first = player1_keyboard_only ? 1 : 0;
    for (size_t i = 0; i < keys.size(); i++) {
        if (!usable(i) || result[i] >= 0) {
            continue;
        }
        for (int slot = first; slot < kSlotCount; slot++) {
            if (!used[slot]) {
                result[i] = slot;
                used[slot] = true;
                break;
            }
        }
    }
    return result;
}

}
