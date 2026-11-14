#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::padmap {

enum Physical {
    kSouth,
    kEast,
    kWest,
    kNorth,
    kBack,
    kGuide,
    kStart,
    kLeftStick,
    kRightStick,
    kLeftShoulder,
    kRightShoulder,
    kDpadUp,
    kDpadDown,
    kDpadLeft,
    kDpadRight,
    kLeftTrigger,
    kRightTrigger,
    kRightStickUp,
    kRightStickDown,
    kRightStickLeft,
    kRightStickRight,
    kPhysicalCount
};

enum class Family { Xbox, Xbox360, PlayStation3, PlayStation4, PlayStation5, Switch };

constexpr size_t kButtonActionCount = 7;
constexpr size_t kActionCount = 17;
constexpr size_t kPointerAction = 16;

struct Pressed {
    std::array<bool, kPhysicalCount> held{};
};

using Combo = uint32_t;
using Binding = std::vector<Combo>;
using Mapping = std::array<Binding, kActionCount>;

const char* action_name(size_t action);
uint32_t action_button(size_t action);
std::string setting_key(size_t action);
const char* physical_name(int physical);
int physical_from_name(const std::string& name);
Binding default_binding(size_t action, bool sideways);
Binding parse_binding(const std::string& text, size_t action, bool sideways);
std::string format_binding(const Binding& binding);
std::string without(const std::string& text, int physical, size_t action, bool sideways);
Mapping current_mapping();
bool held(const Binding& binding, const Pressed& pressed);
uint32_t buttons(const Pressed& pressed, const Mapping& mapping);

std::string device_key(const std::string& name, int ordinal);
bool listed(const std::string& list, const std::string& key);
std::string toggled(const std::string& list, const std::string& key);
int assigned(const std::string& list, const std::string& key);
std::string assign(const std::string& list, const std::string& key, int player);
std::vector<int> plan_slots(const std::vector<std::string>& keys, const std::vector<int>& current, const std::string& ignored, const std::string& assignments,
                            bool player1_keyboard_only);

}
