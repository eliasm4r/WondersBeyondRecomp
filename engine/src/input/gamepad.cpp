#include "wp/gamepad.h"
#include "wp/padmap.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "wp/input.h"
#include "wp/log.h"
#include "wp/settings.h"

namespace wp::gamepad {

namespace {

constexpr float kGravity = 9.80665f;
constexpr float kShakeJolt = 0.9f;
constexpr auto kShakeHold = std::chrono::milliseconds(250);
// How far a stick must be pushed to count as a direction, from the "stick dead zone" setting.
float stick_threshold() {
    static const settings::LiveNumber percent("input.stick_deadzone", "WP_STICK_DEADZONE");
    return std::clamp(percent(), 10, 90) / 100.0f;
}
constexpr float kTriggerThreshold = 0.5f;
constexpr float kPointerPerRadian = 2.3f;
constexpr float kStickPointerSpeed = 1.6f;
constexpr float kStickDeadZone = 0.15f;
constexpr float kPointerLimit = 1.3f;
constexpr float kStillGyro = 0.08f;
constexpr float kStillAccel = 0.4f;
constexpr float kStillSeconds = 1.0f;
constexpr auto kPollInterval = std::chrono::milliseconds(4);
constexpr auto kRumbleRefresh = std::chrono::milliseconds(1000);
constexpr uint32_t kRumbleDuration = 2000;
constexpr uint16_t kRumbleStrength = 0xC000;

struct Snapshot {
    bool connected = false;
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    float left_x = 0.0f;
    float left_y = 0.0f;
    float right_trigger = 0.0f;
    float left_trigger = 0.0f;
    float right_x = 0.0f;
    float right_y = 0.0f;
    bool has_accel = false;
    float accel[3] = {0.0f, 0.0f, 0.0f};
    bool shaking = false;
    bool pointer_valid = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool nintendo_layout = false;
};

struct Pad {
    SDL_Gamepad* handle = nullptr;
    SDL_JoystickID id = 0;
    bool has_gyro = false;
    bool has_accel = false;
    bool nintendo_layout = false;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_active = false;
    bool recenter_held = false;
    float bias[3] = {};
    float still_sum[3] = {};
    float still_accel[3] = {};
    float still_time = 0.0f;
    int still_samples = 0;
    bool rumble = false;
    std::chrono::steady_clock::time_point rumble_refresh{};
    bool aiming = false;
    std::chrono::steady_clock::time_point shake_until{};
    int player = -1;
    padmap::Family family = padmap::Family::Xbox;
    bool pressing = false;
};

std::mutex g_mutex;
std::array<Snapshot, kSlots> g_snapshots;
std::array<std::atomic<bool>, kSlots> g_rumble{};
std::array<std::atomic<int>, kSlots> g_player{};
std::once_flag g_started;
std::atomic<int> g_shown_family{static_cast<int>(padmap::Family::Xbox)};

padmap::Family family_of(SDL_GamepadType type) {
    switch (type) {
    case SDL_GAMEPAD_TYPE_XBOX360:
        return padmap::Family::Xbox360;
    case SDL_GAMEPAD_TYPE_PS3:
        return padmap::Family::PlayStation3;
    case SDL_GAMEPAD_TYPE_PS4:
        return padmap::Family::PlayStation4;
    case SDL_GAMEPAD_TYPE_PS5:
        return padmap::Family::PlayStation5;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        return padmap::Family::Switch;
    default:
        return padmap::Family::Xbox;
    }
}

const char* family_name(padmap::Family family) {
    switch (family) {
    case padmap::Family::Xbox360:
        return "Xbox 360";
    case padmap::Family::PlayStation3:
        return "PlayStation 3";
    case padmap::Family::PlayStation4:
        return "PlayStation 4";
    case padmap::Family::PlayStation5:
        return "PlayStation 5";
    case padmap::Family::Switch:
        return "Switch";
    default:
        return "Xbox";
    }
}

bool enabled() {
    static const settings::LiveFlag value("input.gamepads", "WP_GAMEPAD");
    return value();
}

float axis(SDL_Gamepad* pad, SDL_GamepadAxis which) {
    return static_cast<float>(SDL_GetGamepadAxis(pad, which)) / 32767.0f;
}

std::vector<std::string> device_keys(const SDL_JoystickID* ids, int count) {
    std::vector<std::string> keys;
    for (int i = 0; i < count; i++) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        int ordinal = 0;
        for (int earlier = 0; earlier < i; earlier++) {
            const char* other = SDL_GetGamepadNameForID(ids[earlier]);
            if (name && other && std::strcmp(name, other) == 0) {
                ordinal++;
            }
        }
        keys.push_back(padmap::device_key(name ? name : "?", ordinal));
    }
    return keys;
}

void open_at(std::array<Pad, kSlots>& pads, SDL_JoystickID id, size_t slot) {
    SDL_Gamepad* handle = SDL_OpenGamepad(id);
    if (!handle) {
        return;
    }
    Pad& pad = pads[slot];
    pad = Pad{};
    pad.handle = handle;
    pad.id = id;
    pad.has_gyro = SDL_GamepadHasSensor(handle, SDL_SENSOR_GYRO) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_GYRO, true);
    pad.has_accel = SDL_GamepadHasSensor(handle, SDL_SENSOR_ACCEL) && SDL_SetGamepadSensorEnabled(handle, SDL_SENSOR_ACCEL, true);
    pad.pointer_active = pad.has_gyro;
    pad.nintendo_layout = SDL_GetGamepadButtonLabel(handle, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B;
    pad.family = family_of(SDL_GetGamepadType(handle));
    const char* name = SDL_GetGamepadName(handle);
    log::write("input", "gamepad %s connected as Wii Remote %u, gyroscope %s, accelerometer %s, %s buttons", name ? name : "?", static_cast<unsigned>(slot + 1),
               pad.has_gyro ? "yes" : "no", pad.has_accel ? "yes" : "no", family_name(pad.family));
}

void close(std::array<Pad, kSlots>& pads, SDL_JoystickID id) {
    for (size_t slot = 0; slot < kSlots; slot++) {
        Pad& pad = pads[slot];
        if (pad.handle && pad.id == id) {
            SDL_CloseGamepad(pad.handle);
            pad = Pad{};
            log::write("input", "gamepad of Wii Remote %u disconnected", static_cast<unsigned>(slot + 1));
        }
    }
}

void reconcile(std::array<Pad, kSlots>& pads) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    std::vector<std::string> keys = device_keys(ids, count);
    std::vector<int> current(static_cast<size_t>(count), -1);
    for (int i = 0; ids && i < count; i++) {
        for (size_t slot = 0; slot < kSlots; slot++) {
            if (pads[slot].handle && pads[slot].id == ids[i]) {
                current[static_cast<size_t>(i)] = static_cast<int>(slot);
            }
        }
    }
    std::vector<int> plan = padmap::plan_slots(keys, current, settings::text("input.ignored_gamepads", nullptr), settings::text("input.gamepad_players", nullptr),
                                               settings::flag("input.player1_keyboard_only", nullptr));
    for (size_t slot = 0; slot < kSlots; slot++) {
        if (!pads[slot].handle) {
            continue;
        }
        bool keep = false;
        for (int i = 0; ids && i < count; i++) {
            keep = keep || (ids[i] == pads[slot].id && plan[static_cast<size_t>(i)] == static_cast<int>(slot));
        }
        if (!keep) {
            close(pads, pads[slot].id);
        }
    }
    for (int i = 0; ids && i < count; i++) {
        int slot = plan[static_cast<size_t>(i)];
        if (slot >= 0 && !pads[static_cast<size_t>(slot)].handle) {
            open_at(pads, ids[i], static_cast<size_t>(slot));
        }
    }
    SDL_free(ids);
}

void calibrate(Pad& pad, const float gyro[3], const float accel[3], float dt) {
    float accel_change = 0.0f;
    float rate = 0.0f;
    for (int i = 0; i < 3; i++) {
        rate = std::max(rate, std::fabs(gyro[i] - pad.bias[i]));
        if (pad.still_samples > 0) {
            accel_change = std::max(accel_change, std::fabs(accel[i] - pad.still_accel[i]));
        }
    }
    if (rate > kStillGyro || accel_change > kStillAccel) {
        pad.still_time = 0.0f;
        pad.still_samples = 0;
        for (int i = 0; i < 3; i++) {
            pad.still_sum[i] = 0.0f;
        }
        return;
    }
    if (pad.still_samples == 0) {
        for (int i = 0; i < 3; i++) {
            pad.still_accel[i] = accel[i];
        }
    }
    for (int i = 0; i < 3; i++) {
        pad.still_sum[i] += gyro[i];
    }
    pad.still_samples++;
    pad.still_time += dt;
    if (pad.still_time >= kStillSeconds) {
        for (int i = 0; i < 3; i++) {
            pad.bias[i] = pad.still_sum[i] / static_cast<float>(pad.still_samples);
            pad.still_sum[i] = 0.0f;
        }
        pad.still_time = 0.0f;
        pad.still_samples = 0;
    }
}

padmap::Pressed physical_state(const Snapshot& current) {
    padmap::Pressed pressed;
    for (int i = 0; i < padmap::kLeftTrigger; i++) {
        pressed.held[i] = current.buttons[i];
    }
    pressed.held[padmap::kLeftTrigger] = current.left_trigger > kTriggerThreshold;
    pressed.held[padmap::kRightTrigger] = current.right_trigger > kTriggerThreshold;
    pressed.held[padmap::kRightStickUp] = current.right_y < -stick_threshold();
    pressed.held[padmap::kRightStickDown] = current.right_y > stick_threshold();
    pressed.held[padmap::kRightStickLeft] = current.right_x < -stick_threshold();
    pressed.held[padmap::kRightStickRight] = current.right_x > stick_threshold();
    return pressed;
}

Snapshot read(Pad& pad, float dt) {
    Snapshot snapshot;
    SDL_Gamepad* handle = pad.handle;
    snapshot.connected = true;
    snapshot.nintendo_layout = pad.nintendo_layout;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++) {
        snapshot.buttons[i] = SDL_GetGamepadButton(handle, static_cast<SDL_GamepadButton>(i));
    }
    snapshot.left_x = axis(handle, SDL_GAMEPAD_AXIS_LEFTX);
    snapshot.left_y = axis(handle, SDL_GAMEPAD_AXIS_LEFTY);
    snapshot.right_trigger = axis(handle, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    snapshot.left_trigger = axis(handle, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    float right_x = axis(handle, SDL_GAMEPAD_AXIS_RIGHTX);
    float right_y = axis(handle, SDL_GAMEPAD_AXIS_RIGHTY);
    snapshot.right_x = right_x;
    snapshot.right_y = right_y;
    bool aiming = padmap::held(padmap::current_mapping()[padmap::kPointerAction], physical_state(snapshot));
    if (aiming && !pad.aiming) {
        pad.pointer_x = 0.0f;
        pad.pointer_y = 0.0f;
    }
    pad.aiming = aiming;
    pad.pointer_active = aiming;
    bool motion_mode = snapshot.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER];
    if (aiming && !motion_mode && (std::fabs(right_x) > kStickDeadZone || std::fabs(right_y) > kStickDeadZone)) {
        pad.pointer_x += right_x * kStickPointerSpeed * dt;
        pad.pointer_y += right_y * kStickPointerSpeed * dt;
    }
    if (pad.has_accel) {
        snapshot.has_accel = SDL_GetGamepadSensorData(handle, SDL_SENSOR_ACCEL, snapshot.accel, 3);
    }
    // A gamepad shaken by hand jolts far less than a Wii Remote, so a clear jolt counts as the game's full shake.
    auto now = std::chrono::steady_clock::now();
    if (snapshot.has_accel) {
        float g = std::sqrt(snapshot.accel[0] * snapshot.accel[0] + snapshot.accel[1] * snapshot.accel[1] + snapshot.accel[2] * snapshot.accel[2]) / kGravity;
        if (std::fabs(g - 1.0f) > kShakeJolt) {
            pad.shake_until = now + kShakeHold;
        }
    }
    snapshot.shaking = now < pad.shake_until;
    float gyro[3] = {};
    if (pad.has_gyro && SDL_GetGamepadSensorData(handle, SDL_SENSOR_GYRO, gyro, 3)) {
        if (snapshot.has_accel) {
            calibrate(pad, gyro, snapshot.accel, dt);
        }
        if (aiming) {
            pad.pointer_x -= (gyro[1] - pad.bias[1]) * dt * kPointerPerRadian;
            pad.pointer_y -= (gyro[0] - pad.bias[0]) * dt * kPointerPerRadian;
        }
    }
    bool recenter = snapshot.buttons[SDL_GAMEPAD_BUTTON_RIGHT_STICK];
    if (recenter && !pad.recenter_held) {
        pad.pointer_x = 0.0f;
        pad.pointer_y = 0.0f;
        pad.pointer_active = true;
    }
    pad.recenter_held = recenter;
    pad.pointer_x = std::clamp(pad.pointer_x, -kPointerLimit, kPointerLimit);
    pad.pointer_y = std::clamp(pad.pointer_y, -kPointerLimit, kPointerLimit);
    snapshot.pointer_valid = pad.pointer_active && std::fabs(pad.pointer_x) < 1.0f && std::fabs(pad.pointer_y) < 1.0f;
    snapshot.pointer_x = pad.pointer_x;
    snapshot.pointer_y = pad.pointer_y;
    return snapshot;
}

void apply_outputs(Pad& pad, size_t slot) {
    static const settings::LiveFlag rumble_enabled("input.rumble", "WP_RUMBLE");
    bool rumble = g_rumble[slot].load() && rumble_enabled();
    auto now = std::chrono::steady_clock::now();
    if (rumble != pad.rumble || (rumble && now >= pad.rumble_refresh)) {
        pad.rumble = rumble;
        pad.rumble_refresh = now + kRumbleRefresh;
        uint16_t strength = rumble ? kRumbleStrength : 0;
        SDL_RumbleGamepad(pad.handle, strength, strength, rumble ? kRumbleDuration : 0);
    }
    int player = g_player[slot].load();
    if (player != pad.player) {
        pad.player = player;
        SDL_SetGamepadPlayerIndex(pad.handle, player);
    }
}

struct Virtual {
    SDL_JoystickID id = 0;
    SDL_Joystick* joystick = nullptr;
    unsigned number = 0;
};

bool SDLCALL virtual_rumble(void* userdata, Uint16 low, Uint16 high) {
    log::write("input", "virtual gamepad %u rumble %u %u", static_cast<Virtual*>(userdata)->number, low, high);
    return true;
}

void SDLCALL virtual_player(void* userdata, int player) {
    log::write("input", "virtual gamepad %u player index %d", static_cast<Virtual*>(userdata)->number, player);
}

void attach_virtual(std::array<Virtual, kSlots>& virtuals) {
    const char* setting = std::getenv("WP_VIRTUAL_GAMEPADS");
    int count = setting ? std::clamp(std::atoi(setting), 0, static_cast<int>(kSlots)) : 0;
    static const SDL_VirtualJoystickSensorDesc sensors[] = {{SDL_SENSOR_ACCEL, 250.0f}, {SDL_SENSOR_GYRO, 250.0f}};
    for (int i = 0; i < count; i++) {
        Virtual& pad = virtuals[i];
        pad.number = static_cast<unsigned>(i + 1);
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.nsensors = 2;
        desc.sensors = sensors;
        desc.name = "Virtual test gamepad";
        desc.userdata = &pad;
        desc.Rumble = virtual_rumble;
        desc.SetPlayerIndex = virtual_player;
        pad.id = SDL_AttachVirtualJoystick(&desc);
        pad.joystick = pad.id ? SDL_OpenJoystick(pad.id) : nullptr;
    }
}

void drive_virtual(std::array<Virtual, kSlots>& virtuals) {
    static const auto origin = std::chrono::steady_clock::now();
    static const char* until_setting = std::getenv("WP_VIRTUAL_GAMEPADS_UNTIL");
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - origin).count();
    if (until_setting && elapsed >= std::atoll(until_setting) && virtuals[0].joystick) {
        for (Virtual& pad : virtuals) {
            if (pad.joystick) {
                SDL_CloseJoystick(pad.joystick);
                SDL_DetachVirtualJoystick(pad.id);
                pad.joystick = nullptr;
            }
        }
    }
    bool tap = elapsed % 3000 < 100;
    const float accel[3] = {0.0f, kGravity, 0.0f};
    const float gyro[3] = {0.0f, 0.0f, 0.0f};
    for (Virtual& pad : virtuals) {
        if (pad.joystick) {
            SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, tap);
            SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_ACCEL, SDL_GetTicksNS(), accel, 3);
            SDL_SendJoystickVirtualSensorData(pad.joystick, SDL_SENSOR_GYRO, SDL_GetTicksNS(), gyro, 3);
        }
    }
}

void worker() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, settings::flag("input.joycon_pairs", "WP_JOYCON_PAIRS") ? "1" : "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0");
    if (settings::flag("input.real_wiimotes", "WP_REAL_WIIMOTES")) {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "0");
    }
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        log::write("input", "SDL gamepad support unavailable: %s", SDL_GetError());
        return;
    }
    std::array<Pad, kSlots> pads;
    std::array<Virtual, kSlots> virtuals;
    attach_virtual(virtuals);
    uint32_t seen_revision = 0;
    int last_used = -1;
    auto previous = std::chrono::steady_clock::now();
    while (true) {
        drive_virtual(virtuals);
        SDL_UpdateJoysticks();
        if (settings::revision() != seen_revision) {
            seen_revision = settings::revision();
            reconcile(pads);
        }
        SDL_Event event;
        bool own_queue = !SDL_WasInit(SDL_INIT_VIDEO);
        if (own_queue) {
            SDL_PumpEvents();
        }
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, own_queue ? SDL_EVENT_FIRST : SDL_EVENT_JOYSTICK_AXIS_MOTION,
                              own_queue ? SDL_EVENT_LAST : SDL_EVENT_FINGER_DOWN - 1) > 0) {
            if (event.type == SDL_EVENT_GAMEPAD_ADDED) {
                reconcile(pads);
            } else if (event.type == SDL_EVENT_GAMEPAD_REMOVED) {
                close(pads, event.gdevice.which);
            }
        }
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - previous).count();
        previous = now;
        std::array<Snapshot, kSlots> snapshots;
        for (size_t slot = 0; slot < kSlots; slot++) {
            if (pads[slot].handle) {
                snapshots[slot] = read(pads[slot], dt);
                apply_outputs(pads[slot], slot);
                padmap::Pressed pressed = physical_state(snapshots[slot]);
                bool pressing = std::any_of(pressed.held.begin(), pressed.held.end(), [](bool held) { return held; });
                if (pressing && !pads[slot].pressing) {
                    last_used = static_cast<int>(slot);
                }
                pads[slot].pressing = pressing;
            }
        }
        int shown = last_used >= 0 && pads[static_cast<size_t>(last_used)].handle ? last_used : -1;
        for (size_t slot = 0; slot < kSlots && shown < 0; slot++) {
            if (pads[slot].handle) {
                shown = static_cast<int>(slot);
            }
        }
        g_shown_family = static_cast<int>(shown >= 0 ? pads[static_cast<size_t>(shown)].family : padmap::Family::Xbox);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_snapshots = snapshots;
        }
        std::this_thread::sleep_for(kPollInterval);
    }
}

void start() {
    std::call_once(g_started, [] {
        for (size_t slot = 0; slot < kSlots; slot++) {
            g_player[slot] = -1;
        }
        std::thread(worker).detach();
    });
}

Snapshot snapshot(uint32_t slot) {
    if (slot >= kSlots || !enabled()) {
        return Snapshot{};
    }
    start();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snapshots[slot];
}

}

bool connected(uint32_t slot) {
    return snapshot(slot).connected;
}

bool camera_stick(float& x, float& y) {
    x = 0.0f;
    y = 0.0f;
    Snapshot current = snapshot(0);
    if (!current.connected || current.buttons[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] ||
        padmap::held(padmap::current_mapping()[padmap::kPointerAction], physical_state(current))) {
        return false;
    }
    x = current.right_x;
    y = current.right_y;
    return true;
}

void set_outputs(uint32_t slot, bool rumble, uint8_t leds) {
    if (slot >= kSlots || !enabled()) {
        return;
    }
    int player = -1;
    for (int i = 0; i < 4; i++) {
        if (leds == (1u << i)) {
            player = i;
        }
    }
    g_rumble[slot] = rumble;
    g_player[slot] = player;
}

padmap::Family shown_family() {
    return static_cast<padmap::Family>(g_shown_family.load());
}

int held_physical() {
    for (uint32_t slot = 0; slot < kSlots; slot++) {
        Snapshot current = snapshot(slot);
        if (!current.connected) {
            continue;
        }
        padmap::Pressed pressed = physical_state(current);
        for (int i = 0; i < padmap::kPhysicalCount; i++) {
            if (pressed.held[i]) {
                return i;
            }
        }
    }
    return -1;
}

uint32_t menu_buttons() {
    uint32_t result = 0;
    for (uint32_t slot = 0; slot < kSlots; slot++) {
        Snapshot current = snapshot(slot);
        if (!current.connected) {
            continue;
        }
        auto held = [&](SDL_GamepadButton button) { return current.buttons[button]; };
        SDL_GamepadButton accept = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH;
        SDL_GamepadButton cancel = current.nintendo_layout ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        constexpr float kMenuStick = 0.6f;
        bool vertical = std::fabs(current.left_y) >= std::fabs(current.left_x);
        if (held(SDL_GAMEPAD_BUTTON_DPAD_UP) || (vertical && current.left_y < -kMenuStick)) {
            result |= kMenuUp;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || (vertical && current.left_y > kMenuStick)) {
            result |= kMenuDown;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || (!vertical && current.left_x < -kMenuStick)) {
            result |= kMenuLeft;
        }
        if (held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || (!vertical && current.left_x > kMenuStick)) {
            result |= kMenuRight;
        }
        if (held(accept)) {
            result |= kMenuAccept;
        }
        if (held(cancel)) {
            result |= kMenuCancel;
        }
        if (held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
            result |= kMenuTabPrevious;
        }
        if (held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
            result |= kMenuTabNext;
        }
        if (held(SDL_GAMEPAD_BUTTON_START) && (held(SDL_GAMEPAD_BUTTON_GUIDE) || held(SDL_GAMEPAD_BUTTON_BACK))) {
            result |= kMenuToggle;
        }
    }
    return result;
}

State poll(uint32_t slot, bool sideways) {
    Snapshot current = snapshot(slot);
    State state;
    if (!current.connected) {
        return state;
    }
    state.connected = true;
    auto held = [&](SDL_GamepadButton button) { return current.buttons[button]; };
    uint32_t& b = state.buttons;
    padmap::Pressed pressed = physical_state(current);
    bool motion_mode = held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    b |= padmap::buttons(pressed, padmap::current_mapping());
    if (current.shaking) {
        b |= input::kMotionShake;
    }
    bool left = held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || current.left_x < -stick_threshold();
    bool right = held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || current.left_x > stick_threshold();
    bool up = (held(SDL_GAMEPAD_BUTTON_DPAD_UP) && !motion_mode) || current.left_y < -stick_threshold();
    bool down = (held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) && !motion_mode) || current.left_y > stick_threshold();
    if (sideways) {
        b |= (left ? input::kButtonUp : 0) | (right ? input::kButtonDown : 0) | (up ? input::kButtonRight : 0) | (down ? input::kButtonLeft : 0);
    } else {
        b |= (left ? input::kButtonLeft : 0) | (right ? input::kButtonRight : 0) | (up ? input::kButtonUp : 0) | (down ? input::kButtonDown : 0);
    }
    state.pointer_valid = current.pointer_valid;
    state.pointer_x = current.pointer_x;
    state.pointer_y = current.pointer_y;
    if (current.has_accel) {
        float x = current.accel[0] / kGravity;
        float y = current.accel[1] / kGravity;
        float z = current.accel[2] / kGravity;
        state.motion_valid = true;
        if (sideways) {
            state.accel[0] = z;
            state.accel[1] = -x;
        } else {
            state.accel[0] = -x;
            state.accel[1] = -z;
        }
        state.accel[2] = y;
    }
    return state;
}

}
