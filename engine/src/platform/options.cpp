#include "wp/options.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <utility>

#include "wp/cloud_sync.h"
#include "wp/color.h"
#include "wp/control_names.h"
#include "wp/frame_pacing.h"
#include "wp/free_camera.h"
#include "wp/game.h"
#include "wp/gamepad.h"
#include "wp/gx_render.h"
#include "wp/hd_textures.h"
#include "wp/keymap.h"
#include "wp/padmap.h"
#include "wp/play_time.h"
#include "wp/online.h"
#include "wp/save_cloud.h"
#include "wp/settings.h"
#include "wp/ui_text.h"
#include "wp/update_check.h"

namespace wp::options {

namespace {

constexpr const char* kLiveKeys[] = {
    "video.scale",          "video.fps",            "video.fullscreen",      "video.copy_filter",  "video.vsync",
    "video.show_fps",       "video.smooth",         "video.anisotropy",      "input.gamepads",     "input.rumble",
    "input.stick_deadzone", "input.real_wiimote_mouse", "input.wake_on_mouse", "input.hide_cursor", "system.options_menu",
    "audio.mute",           "audio.volume",         "audio.wiimote_speaker", "saves.backups",      "system.interface_language", "system.discord",
    "overlay.show",         "overlay.level",        "overlay.corner",        "controls.device",    "stats.play_time",
    "video.brightness",     "video.contrast",       "video.saturation",      "video.gamma",        "video.colors_reset",
    "camera.free",          "camera.mouse_speed",   "camera.stick_speed",    "camera.invert_y",    "video.bug_recorder",
    "system.check_updates", "system.update",        "video.aspect",          "game.text_speed",
    "game.skip_movies",     "game.button_icons",    "video.hd_textures",   "game.save_photos",     "cheats.berries",       "cheats.prices",        "cheats.chase_speed",   "cheats.damage_dealt",  "cheats.damage_taken",  "cheats.run_speed",     "cheats.jump",
    "input.touch",          "input.touch_size",     "input.touch_opacity",   "input.phone_shake",  "camera.touch_speed",
};

struct Language {
    const char* code;
    const char* name;
};

constexpr Language kLanguages[] = {{"auto", nullptr}, {"en", "English"},  {"de", "Deutsch"},   {"fr", "Français"},
                                   {"es", "Español"}, {"it", "Italiano"}, {"nl", "Nederlands"}};

constexpr Language kInterfaceLanguages[] = {{"fr", "Français"}, {"en", "English"}, {"es", "Español"}, {"gl", "Galego"}};

constexpr int kMaxScale = 6;
constexpr int kMaxPhoneScale = 3;

constexpr const char* kBackupCounts[] = {"0", "1", "3", "5", "10", "20"};

struct Slider {
    const char* key;
    int low;
    int high;
    int step;
};

constexpr Slider kSliders[] = {{"audio.volume", 0, 300, 5},
                               {"input.stick_deadzone", 10, 90, 5},
                               {"camera.mouse_speed", 10, 300, 10},
                               {"camera.stick_speed", 10, 300, 10},
                               {"camera.touch_speed", 10, 300, 10},
                               {"input.touch_size", 50, 200, 10},
                               {"input.touch_opacity", 10, 100, 10},
                               {color::kBrightness.key, color::kBrightness.low, color::kBrightness.high, 5},
                               {color::kContrast.key, color::kContrast.low, color::kContrast.high, 5},
                               {color::kSaturation.key, color::kSaturation.low, color::kSaturation.high, 5},
                               {color::kGamma.key, color::kGamma.low, color::kGamma.high, 5}};

constexpr const char* kTabLabels[kTabCount] = {"tab.screen", "tab.game", "tab.sound", "tab.controls", "tab.language", "tab.advanced", "tab.cheats", "tab.saves", "tab.online"};

constexpr const char* kDevice = "controls.device";
constexpr const char* kPlayTime = "stats.play_time";
#ifdef __ANDROID__
constexpr bool kPhone = true;
constexpr const char* kDeviceNames[] = {"touch", "gamepad"};
#else
constexpr bool kPhone = false;
constexpr const char* kDeviceNames[] = {"keyboard", "gamepad"};
#endif

bool between_images() {
    return pacing::kInBetweenImages && (!kPhone || std::strcmp(gx::render::api_name(), "Vulkan") == 0);
}

constexpr const char* kResetKeys = "keys.reset";
constexpr const char* kResetPad = "pad.reset";
constexpr const char* kResetColors = "video.colors_reset";
constexpr const char* kFlyCamera = "cheats.fly_camera";
constexpr const char* kUpdate = "system.update";

constexpr const char* kCloudStatus = "saves.cloud_status";
constexpr const char* kCloudCreate = "saves.cloud_create";
constexpr const char* kCloudEnter = "saves.cloud_enter";
constexpr const char* kCloudCode = "saves.cloud_code";
constexpr const char* kCloudAdd = "saves.cloud_add";
constexpr const char* kCloudErase = "saves.cloud_erase";
constexpr const char* kCloudCancel = "saves.cloud_cancel";
constexpr const char* kCloudKeepHere = "saves.cloud_keep_here";
constexpr const char* kCloudKeepOther = "saves.cloud_keep_other";
constexpr const char* kCloudOff = "saves.cloud_off";
constexpr size_t kCloudCodeLength = 10;

bool g_code_entry = false;
std::string g_code_typed;
size_t g_code_letter = 0;
bool g_code_picking = false;
bool g_keyboard_wanted = false;

std::vector<std::string> cloud_keys() {
    if (g_code_entry) {
        return {kCloudCode, kCloudAdd, kCloudErase, kCloudCancel};
    }
    if (cloud::code().empty()) {
        return {kCloudStatus, kCloudCreate, kCloudEnter};
    }
    if (cloud::status() == cloud::Status::Conflict) {
        return {kCloudStatus, kCloudKeepHere, kCloudKeepOther, kCloudOff};
    }
    return {kCloudStatus, kCloudOff};
}

std::string cloud_status_text() {
    const char* state = "saves.cloud.off";
    switch (cloud::status()) {
    case cloud::Status::Off:
        break;
    case cloud::Status::Working:
        state = "saves.cloud.working";
        break;
    case cloud::Status::Synced:
        state = "saves.cloud.synced";
        break;
    case cloud::Status::Offline:
        state = "saves.cloud.offline";
        break;
    case cloud::Status::Restart:
        state = "saves.cloud.restart";
        break;
    case cloud::Status::Conflict:
        state = "saves.cloud.conflict";
        break;
    case cloud::Status::Unknown:
        state = "saves.cloud.unknown";
        break;
    case cloud::Status::Error:
        state = "saves.cloud.error";
        break;
    }
    std::string code = cloud::code();
    return code.empty() ? ui::label(state) : cloud::shown_code(code) + " - " + ui::label(state);
}

std::string code_entry_text() {
    std::string shown = g_code_typed;
    if (shown.size() < kCloudCodeLength && g_code_picking) {
        shown.push_back(cloud::code_letters()[g_code_letter]);
    }
    shown.append(kCloudCodeLength - shown.size(), '_');
    return cloud::shown_code(shown);
}

void close_code_entry() {
    g_code_entry = false;
    g_code_picking = false;
    g_code_typed.clear();
}

void add_code_character(char character) {
    g_code_typed.push_back(character);
    if (g_code_typed.size() == kCloudCodeLength) {
        std::string code = g_code_typed;
        close_code_entry();
        cloud::link(code);
    }
}

bool cloud_action(const std::string& key, Action action) {
    if (key == kCloudCode) {
        size_t letters = std::strlen(cloud::code_letters());
        if (action == Action::Previous || action == Action::Next) {
            if (g_code_picking) {
                g_code_letter = (g_code_letter + (action == Action::Next ? 1 : letters - 1)) % letters;
            }
            g_code_picking = true;
            return true;
        }
    }
    if (action != Action::Accept) {
        return false;
    }
    if (key == kCloudCode && !g_code_picking) {
        g_keyboard_wanted = true;
    } else if (key == kCloudCode || key == kCloudAdd) {
        g_code_picking = true;
        add_code_character(cloud::code_letters()[g_code_letter]);
    } else if (key == kCloudErase) {
        if (!g_code_typed.empty()) {
            g_code_typed.pop_back();
        }
    } else if (key == kCloudCancel) {
        close_code_entry();
    } else if (key == kCloudEnter) {
        close_code_entry();
        g_code_entry = true;
        g_code_letter = 0;
        g_keyboard_wanted = true;
    } else if (key == kCloudCreate) {
        cloud::create();
    } else if (key == kCloudKeepHere) {
        cloud::keep_here();
    } else if (key == kCloudKeepOther) {
        cloud::keep_other();
    } else if (key == kCloudOff) {
        cloud::disconnect();
    } else {
        return false;
    }
    return true;
}

constexpr const char* kOnlineStatus = "online.status";
constexpr const char* kOnlineName = "online.name";
constexpr const char* kOnlineAddress = "online.address";
constexpr const char* kOnlineCode = "online.code";
constexpr const char* kOnlineLocal = "online.local";
constexpr const char* kOnlineHost = "online.host";
constexpr const char* kOnlineJoin = "online.join";
constexpr const char* kOnlineLeave = "online.leave";
constexpr const char* kOnlineText = "online.text";
constexpr const char* kOnlineErase = "online.erase";
constexpr const char* kOnlineDone = "online.done";
constexpr const char* kOnlineCancel = "online.cancel";
constexpr size_t kNameLength = 12;
constexpr size_t kAddressLength = 15;

const char* g_field = nullptr;
std::string g_field_text;
size_t g_field_letter = 0;
bool g_field_picking = false;

const char* field_letters() {
    return g_field == kOnlineAddress ? "0123456789ABCDEFGHJKMNPQRSTVWXYZ." : "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
}

size_t field_limit() {
    return g_field == kOnlineAddress ? kAddressLength : kNameLength;
}

std::vector<std::string> online_keys() {
    if (g_field) {
        return {kOnlineText, kOnlineErase, kOnlineDone, kOnlineCancel};
    }
    if (online::running()) {
        std::vector<std::string> keys = {kOnlineStatus};
        if (online::state() != online::State::Joined && !online::room_code().empty()) {
            keys.push_back(kOnlineCode);
        }
        if (online::state() == online::State::Hosting) {
            keys.push_back(kOnlineLocal);
        }
        keys.push_back(kOnlineLeave);
        return keys;
    }
    return {kOnlineStatus, kOnlineName, kOnlineAddress, kOnlineHost, kOnlineJoin};
}

std::string online_status_text() {
    size_t others = online::others();
    switch (online::state()) {
    case online::State::Off:
        return ui::label("online.state.off");
    case online::State::Connecting:
        return ui::label("online.state.connecting");
    case online::State::Failed:
        switch (online::problem()) {
        case online::Problem::NoInternet:
            return ui::label("online.state.no_internet");
        case online::Problem::NoRoom:
            return ui::label("online.state.no_room");
        case online::Problem::Full:
            return ui::label("online.state.full");
        case online::Problem::Blocked:
            return ui::label("online.state.blocked");
        default:
            return ui::label("online.state.failed");
        }
    case online::State::Hosting:
        if (others == 0) {
            return ui::label(online::room_code().empty() ? "online.state.local_only" : "online.state.waiting");
        }
        break;
    case online::State::Joined:
        if (others == 0) {
            return ui::label("online.state.searching");
        }
        break;
    }
    return std::to_string(others + 1) + " " + ui::label("online.state.players");
}

std::string field_text() {
    std::string shown = g_field_text;
    if (shown.size() < field_limit() && g_field_picking) {
        shown.push_back(field_letters()[g_field_letter]);
    }
    return shown + "_";
}

void close_field() {
    g_field = nullptr;
    g_field_picking = false;
    g_field_text.clear();
}

void add_field_character(char character) {
    if (g_field_text.size() < field_limit()) {
        g_field_text.push_back(character);
    }
}

void commit_field() {
    if (g_field == kOnlineAddress && !online::valid_address(g_field_text) && !online::valid_code(online::clean_code(g_field_text))) {
        return;
    }
    if (!g_field_text.empty()) {
        settings::store(g_field, g_field_text);
    }
    close_field();
}

bool online_action(const std::string& key, Action action) {
    if (key == kOnlineText && (action == Action::Previous || action == Action::Next)) {
        size_t letters = std::strlen(field_letters());
        if (g_field_picking) {
            g_field_letter = (g_field_letter + (action == Action::Next ? 1 : letters - 1)) % letters;
        }
        g_field_picking = true;
        return true;
    }
    if (action != Action::Accept) {
        return false;
    }
    if (key == kOnlineText) {
        if (g_field_picking) {
            add_field_character(field_letters()[g_field_letter]);
        } else {
            g_keyboard_wanted = true;
        }
    } else if (key == kOnlineErase) {
        if (!g_field_text.empty()) {
            g_field_text.pop_back();
        }
    } else if (key == kOnlineDone) {
        commit_field();
    } else if (key == kOnlineCancel) {
        close_field();
    } else if (key == kOnlineName || key == kOnlineAddress) {
        close_field();
        g_field = key == kOnlineName ? kOnlineName : kOnlineAddress;
        g_field_text = g_field == kOnlineName ? settings::text(g_field, nullptr) : "";
        g_field_letter = 0;
        g_keyboard_wanted = true;
    } else if (key == kOnlineHost) {
        online::host(settings::text(kOnlineName, nullptr));
    } else if (key == kOnlineJoin) {
        std::string target = settings::text(kOnlineAddress, nullptr);
        if (target.empty()) {
            g_field = kOnlineAddress;
            g_field_letter = 0;
            g_keyboard_wanted = true;
        } else {
            online::join(target, settings::text(kOnlineName, nullptr));
        }
    } else if (key == kOnlineLeave) {
        online::leave();
    } else {
        return false;
    }
    return true;
}

constexpr const char* kHdTextures = "video.hd_textures";

const Slider* slider_of(const std::string& key) {
    for (const Slider& slider : kSliders) {
        if (key == slider.key) {
            return &slider;
        }
    }
    return nullptr;
}

bool starts_with(const std::string& text, const char* prefix) {
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

size_t pad_action(const std::string& key) {
    for (size_t i = 0; i < padmap::kActionCount; i++) {
        if (key == padmap::setting_key(i)) {
            return i;
        }
    }
    return padmap::kActionCount;
}

bool known(const std::string& key) {
    std::vector<std::string> all = settings::keys();
    return std::find(all.begin(), all.end(), key) != all.end();
}

std::string update_text() {
    std::string text = update::version();
    switch (update::state()) {
    case update::State::Checking:
        return text + " \xC2\xB7 " + ui::label("menu.update.checking");
    case update::State::Current:
        return text + " \xC2\xB7 " + ui::label("menu.update.current");
    case update::State::Newer:
        return text + " \xC2\xB7 " + update::latest() + " " + ui::label("menu.update.newer");
    case update::State::Failed:
        return text + " \xC2\xB7 " + ui::label("menu.update.failed");
    default:
        return text;
    }
}

std::string hd_text() {
    hd_textures::Progress now = hd_textures::progress();
    switch (now.state) {
    case hd_textures::State::Reading:
        return ui::label("menu.hd.reading");
    case hd_textures::State::Downloading:
        return ui::label("menu.hd.downloading");
    case hd_textures::State::Enlarging:
        return now.total ? std::to_string(now.done) + " / " + std::to_string(now.total) : ui::label("menu.hd.enlarging");
    case hd_textures::State::Retrying:
        return std::string(ui::label("menu.hd.again")) + " " + std::to_string(now.done) + " / " + std::to_string(now.total);
    case hd_textures::State::Checking:
        return std::string(ui::label("menu.hd.check")) + " " + std::to_string(now.done) + " / " + std::to_string(now.total);
    case hd_textures::State::Done:
        return ui::label("menu.hd.done");
    case hd_textures::State::Failed:
        return ui::label("menu.hd.failed");
    default:
        return ui::label("menu.hd.start");
    }
}

std::string hd_help() {
    hd_textures::Progress now = hd_textures::progress();
    if (now.state == hd_textures::State::Idle) {
        return "";
    }
    std::vector<std::string> lines = hd_textures::describe(now);
    std::string dot = " \xC2\xB7 ";
    std::string text;
    for (const std::string& line : lines) {
        std::string part = text.empty() && line.find(dot) != std::string::npos ? line.substr(line.find(dot) + dot.size()) : line;
        text += (text.empty() ? "" : dot) + part;
    }
    return text;
}

bool enabled_text(const std::string& value) {
    return !(value == "0" || value == "false" || value == "off" || value == "no");
}

std::mutex g_overlay_mutex;
Image g_overlay;
bool g_overlay_visible = false;
uint64_t g_overlay_version = 0;

}

const char* tab_label(size_t tab) {
    return ui::label(kTabLabels[tab < kTabCount ? tab : 0]);
}

std::vector<std::string> tab_keys(size_t tab, Device device) {
    switch (tab) {
    case 0: {
        std::vector<std::string> keys = {"video.scale",      "video.fullscreen", "video.aspect", "system.widescreen", "video.fps", "video.vsync", "video.smooth", "video.anisotropy",
                                         color::kBrightness.key, color::kContrast.key, color::kSaturation.key, color::kGamma.key, kResetColors};
        if (kPhone) {
            for (const char* unused : {"video.fullscreen", "video.vsync"}) {
                keys.erase(std::find(keys.begin(), keys.end(), unused));
            }
        }
        return keys;
    }
    case 1: {
        std::vector<std::string> keys = {"game.text_speed", "game.skip_movies", "game.button_icons", "game.save_photos", "system.discord", kPlayTime};
        if (kPhone) {
            keys.erase(std::find(keys.begin(), keys.end(), "system.discord"));
        }
        return keys;
    }
    case 2:
        if (kPhone) {
            return {"audio.volume", "audio.mute"};
        }
        return {"audio.volume", "audio.mute", "audio.wiimote_speaker"};
    case 3: {
        if (device == Device::Keyboard && kPhone) {
            return {kDevice, "input.touch", "input.touch_size", "input.touch_opacity", "input.phone_shake", "camera.free", "camera.touch_speed", "camera.invert_y"};
        }
        if (device == Device::Keyboard) {
            std::vector<std::string> keys = {kDevice, "camera.free", "camera.mouse_speed", "camera.invert_y", "input.hide_cursor"};
            for (size_t i = 0; i < keymap::kActionCount; i++) {
                keys.push_back(keymap::setting_key(i));
            }
            keys.push_back(kResetKeys);
            return keys;
        }
        std::vector<std::string> keys = {kDevice, "camera.free", "camera.stick_speed", "camera.invert_y", "input.gamepads", "input.rumble", "input.stick_deadzone"};
        for (size_t i = 0; i < padmap::kActionCount; i++) {
            keys.push_back(padmap::setting_key(i));
        }
        keys.push_back(kResetPad);
        return keys;
    }
    case 4:
        return {"system.interface_language", "system.language"};
    case 5: {
        std::vector<std::string> keys = {"overlay.show", "overlay.level", "overlay.corner", "video.show_fps", "video.custom_textures"};
        if (hd_textures::available()) {
            keys.push_back(kHdTextures);
        }
        if (!kPhone) {
            keys.push_back("video.dump_textures");
            keys.push_back("video.bug_recorder");
        }
        if (update::available()) {
            keys.push_back("system.check_updates");
        }
        if (!kPhone) {
            keys.push_back(kUpdate);
        }
        return keys;
    }
    case 6:
        if (kPhone) {
            return {"cheats.run_speed", "cheats.jump", "cheats.damage_dealt", "cheats.damage_taken", "cheats.berries", "cheats.prices", "cheats.chase_speed"};
        }
        return {kFlyCamera, "cheats.run_speed", "cheats.jump", "cheats.damage_dealt", "cheats.damage_taken", "cheats.berries", "cheats.prices", "cheats.chase_speed"};
    case 7: {
        std::vector<std::string> keys = cloud_keys();
        keys.push_back("saves.backups");
        return keys;
    }
    case 8:
        return online_keys();
    default:
        return {};
    }
}

Kind kind(const std::string& key) {
    if (key == kResetKeys || key == kResetPad || key == kResetColors || key == kHdTextures || key == kFlyCamera || key == kCloudCreate || key == kCloudEnter ||
        key == kCloudAdd || key == kCloudErase || key == kCloudCancel || key == kCloudKeepHere || key == kCloudKeepOther || key == kCloudOff || key == kOnlineName ||
        key == kOnlineAddress || key == kOnlineHost || key == kOnlineJoin || key == kOnlineLeave || key == kOnlineErase || key == kOnlineDone || key == kOnlineCancel) {
        return Kind::Button;
    }
    if (key == kPlayTime || key == kUpdate || key == kCloudStatus || key == kOnlineStatus || key == kOnlineCode || key == kOnlineLocal || key == kCloudCode || key == kOnlineText) {
        return Kind::Info;
    }
    if (starts_with(key, "keys.")) {
        return Kind::Key;
    }
    if (starts_with(key, "pad.")) {
        return Kind::Pad;
    }
    if (slider_of(key)) {
        return Kind::Slider;
    }
    std::vector<std::string> options = choices(key);
    return options.size() == 2 && options[0] == "0" && options[1] == "1" ? Kind::Toggle : Kind::Choice;
}

bool live(const std::string& key) {
    if (starts_with(key, "keys.") || starts_with(key, "pad.") || starts_with(key, "online.")) {
        return true;
    }
    for (const char* entry : kLiveKeys) {
        if (key == entry) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> choices(const std::string& key) {
    if (key == "video.scale") {
        std::vector<std::string> scales;
        for (int scale = 1; scale <= (kPhone ? kMaxPhoneScale : kMaxScale); scale++) {
            scales.push_back(scale == gx::render::auto_scale() ? "0" : std::to_string(scale));
        }
        return scales;
    }
    if (key == "video.fps") {
        if (kPhone) {
            std::vector<std::string> rates = {"30", "60"};
            int highest = pacing::highest_hz();
            if (between_images()) {
                for (int rate : {90, 120, 144, 165, 240}) {
                    if (pacing::interpolation_wanted(rate, highest) && rate <= highest + 1) {
                        rates.push_back(std::to_string(rate));
                    }
                }
            }
            return rates;
        }
        if (!between_images()) {
            return {"30", "45", "60"};
        }
        return {"30", "45", "60", "90", "120", "144", "165", "240"};
    }
    if (key == "video.anisotropy") {
        if (kPhone) {
            return {"1", "2", "4"};
        }
        return {"1", "2", "4", "8", "16"};
    }
    if (key == "video.aspect") {
        if (kPhone) {
            return {"16:9", "21:9", "auto"};
        }
        return {"16:9", "16:10", "21:9", "32:9", "auto"};
    }
    if (key == "game.text_speed") {
        return {"1", "2", "3", "0"};
    }
    if (key == "cheats.berries") {
        return {"100", "200", "300", "500", "1000"};
    }
    if (key == "cheats.prices") {
        return {"100", "75", "50", "25", "0"};
    }
    if (key == "cheats.chase_speed") {
        return {"50", "75", "100", "125", "150"};
    }
    if (key == "cheats.damage_dealt") {
        return {"100", "150", "200", "300", "1000"};
    }
    if (key == "cheats.damage_taken") {
        return {"100", "150", "200", "300", "500", "1000", "0", "25", "50", "75"};
    }
    if (key == "cheats.run_speed") {
        return {"100", "125", "150", "200"};
    }
    if (key == "cheats.jump") {
        return {"100", "150", "200", "300"};
    }
    if (key == "overlay.level") {
        return {"minimal", "standard", "full"};
    }
    if (key == "overlay.corner") {
        return {"top_left", "top_right", "bottom_left", "bottom_right"};
    }
    if (key == kDevice) {
        return std::vector<std::string>(std::begin(kDeviceNames), std::end(kDeviceNames));
    }
    if (key == kPlayTime) {
        return {};
    }
    if (const Slider* slider = slider_of(key)) {
        std::vector<std::string> values;
        for (int value = slider->low; value <= slider->high; value += slider->step) {
            values.push_back(std::to_string(value));
        }
        return values;
    }
    if (key == "system.interface_language") {
        return {"fr", "en", "es", "gl"};
    }
    if (key == "system.language") {
        std::vector<std::string> codes;
        for (const Language& language : kLanguages) {
            codes.push_back(language.code);
        }
        return codes;
    }
    if (key == "saves.backups") {
        return std::vector<std::string>(std::begin(kBackupCounts), std::end(kBackupCounts));
    }
    if (starts_with(key, "keys.") || starts_with(key, "pad.")) {
        return {};
    }
    if (known(key)) {
        return {"0", "1"};
    }
    return {};
}

std::string shown_value(const std::string& key, const std::string& value) {
    const ui::Text& text = ui::text();
    if (key == "video.scale") {
        if (value == "0") {
            return std::string(ui::label("menu.scale.auto")) + " (" + std::to_string(gx::render::auto_scale()) + "x)";
        }
        return value == "1" ? text.menu_native : value + "x";
    }
    if (key == "video.fps") {
        int fps = std::atoi(value.c_str());
        int hz = pacing::highest_hz();
        if (fps > 60 && !pacing::interpolation_wanted(fps, hz)) {
            return value + " > 60 (" + std::to_string(hz) + " Hz)";
        }
        int shown = pacing::capped_rate(fps, hz);
        return shown < fps ? value + " > " + std::to_string(shown) + " (" + std::to_string(hz) + " Hz)" : value;
    }
    if (key == "video.anisotropy") {
        return value == "1" ? text.menu_off : value + "x";
    }
    if (key == "video.aspect") {
        return value == "auto" ? ui::label("menu.aspect.auto") : value == "16:9" ? ui::label("menu.aspect.original") : value;
    }
    if (starts_with(key, "cheats.")) {
        return value + " %";
    }
    if (key == "game.text_speed") {
        return value == "1" ? ui::label("menu.text_speed.original") : value == "0" ? ui::label("menu.text_speed.instant") : value + "x";
    }
    if (key == "overlay.level" || key == "overlay.corner" || key == kDevice) {
        std::string prefix = key == "overlay.level" ? "menu.level." : key == "overlay.corner" ? "menu.corner." : "menu.device.";
        return ui::label((prefix + value).c_str());
    }
    if (key == kPlayTime) {
        return value;
    }
    if (slider_of(key)) {
        return value + "%";
    }
    if (key == "system.interface_language") {
        for (const Language& language : kInterfaceLanguages) {
            if (value == language.code) {
                return language.name;
            }
        }
        return "English";
    }
    if (key == "system.language") {
        for (const Language& language : kLanguages) {
            if (value == language.code) {
                return language.name ? language.name : text.menu_system;
            }
        }
        return value;
    }
    if (key == "saves.backups") {
        return value == "0" ? text.menu_off : value;
    }
    if (starts_with(key, "keys.")) {
        return control_names::keys(keymap::parse(value).codes);
    }
    if (starts_with(key, "pad.")) {
        padmap::Binding binding = padmap::parse_binding(value, pad_action(key), game::description().sideways_grip);
        return control_names::pad_binding(binding, gamepad::shown_family());
    }
    return enabled_text(value) ? text.menu_on : text.menu_off;
}

std::string step_value(const std::string& key, const std::string& value, int step) {
    std::vector<std::string> list = choices(key);
    if (list.empty()) {
        return value;
    }
    auto found = std::find(list.begin(), list.end(), value);
    int count = static_cast<int>(list.size());
    int index = 0;
    if (found != list.end()) {
        index = static_cast<int>(found - list.begin());
    } else if (list.size() == 2 && list[0] == "0" && list[1] == "1") {
        index = enabled_text(value) ? 1 : 0;
    } else if (key == "saves.backups" || slider_of(key)) {
        int current = std::atoi(value.c_str());
        while (index + 1 < count && std::atoi(list[static_cast<size_t>(index) + 1].c_str()) <= current) {
            index++;
        }
    } else {
        return list.front();
    }
    if (slider_of(key)) {
        return list[static_cast<size_t>(std::clamp(index + step, 0, count - 1))];
    }
    index = ((index + step) % count + count) % count;
    return list[static_cast<size_t>(index)];
}

void Menu::set_open(bool open) {
    open_ = open;
    capturing_ = false;
    hover_ = Hover{};
    if (open && device_ != Device::Keyboard) {
        device_ = Device::Keyboard;
        selection_ = 0;
        scroll_ = 0;
    }
}

std::vector<std::string> Menu::keys() const {
    return tab_keys(tab_, device_);
}

bool Menu::set_hover(const Hover& hover) {
    if (hover_ == hover) {
        return false;
    }
    hover_ = hover;
    return true;
}

void Menu::select_tab(size_t tab) {
    if (tab < kTabCount && tab != tab_) {
        tab_ = tab;
        selection_ = 0;
        scroll_ = 0;
        capturing_ = false;
    }
}

void Menu::select(size_t index) {
    if (index < keys().size()) {
        selection_ = index;
        capturing_ = false;
        keep_visible();
    }
}

void Menu::scroll_by(int rows) {
    size_t count = keys().size();
    int last = count > kVisibleRows ? static_cast<int>(count - kVisibleRows) : 0;
    scroll_ = static_cast<size_t>(std::clamp(static_cast<int>(scroll_) + rows, 0, last));
}

void Menu::keep_visible() {
    if (selection_ < scroll_) {
        scroll_ = selection_;
    } else if (selection_ >= scroll_ + kVisibleRows) {
        scroll_ = selection_ + 1 - kVisibleRows;
    }
}

Kind Menu::capture_kind() const {
    std::vector<std::string> keys = this->keys();
    return selection_ < keys.size() ? kind(keys[selection_]) : Kind::Choice;
}

std::vector<Row> Menu::rows() const {
    std::vector<Row> result;
    for (const std::string& key : keys()) {
        Row row;
        row.key = key;
        row.kind = kind(key);
        row.live = live(key);
        std::string help_key = key == "video.fps" && !between_images() ? "help.video.fps_without_between" : "help." + key;
        if (capturing_ && result.size() == selection_ && (row.kind == Kind::Key || row.kind == Kind::Pad)) {
            help_key = row.kind == Kind::Key ? "help.keys" : "help.pad.capture";
        } else if (row.kind == Kind::Pad) {
            std::string name = key.substr(4);
            bool motion = name == "shake" || starts_with(name, "swing_") || starts_with(name, "tilt_");
            help_key = motion ? "pad.motion_help" : name == "pointer" ? "help.pad.pointer" : "help.keys." + name;
        }
        row.help = ui::label(help_key.c_str());
        if (key == kHdTextures) {
            std::string job = hd_help();
            if (!job.empty()) {
                row.help = job;
            }
        }
        std::string label_key = row.kind == Kind::Pad ? "keys." + key.substr(4) : key;
        row.label = ui::label(label_key.c_str());
        if (key == kDevice) {
            row.value = shown_value(key, kDeviceNames[device_ == Device::Gamepad ? 1 : 0]);
        } else if (key == kPlayTime) {
            row.value = play_time::format(play_time::seconds(), ui::language());
        } else if (key == kUpdate) {
            row.value = update_text();
        } else if (key == kOnlineStatus) {
            row.value = online_status_text();
        } else if (key == kOnlineCode) {
            row.value = online::room_code();
        } else if (key == kOnlineLocal) {
            row.value = online::local_address();
        } else if (key == kOnlineText) {
            row.value = field_text();
        } else if (key == kOnlineName || key == kOnlineAddress) {
            row.value = settings::text(key.c_str(), nullptr);
            if (row.value.empty()) {
                row.value = ui::label("saves.cloud.do_enter");
            }
        } else if (key == kOnlineHost) {
            row.value = ui::label("online.do_host");
        } else if (key == kOnlineJoin) {
            row.value = ui::label("online.do_join");
        } else if (key == kOnlineLeave) {
            row.value = ui::label("online.do_leave");
        } else if (key == kOnlineErase) {
            row.value = ui::label("saves.cloud.do_erase");
        } else if (key == kOnlineDone) {
            row.value = ui::label("saves.cloud.confirm");
        } else if (key == kOnlineCancel) {
            row.value = ui::label("saves.cloud_cancel");
        } else if (key == kCloudStatus) {
            row.value = cloud_status_text();
        } else if (key == kCloudCode) {
            row.value = code_entry_text();
        } else if (key == kCloudAdd) {
            row.value = ui::label("saves.cloud.do_add");
        } else if (key == kCloudCreate) {
            row.value = ui::label("saves.cloud.do_create");
        } else if (key == kCloudEnter) {
            row.value = ui::label("saves.cloud.do_enter");
        } else if (key == kCloudErase) {
            row.value = ui::label("saves.cloud.do_erase");
        } else if (key == kCloudCancel) {
            row.value = ui::label("saves.cloud_cancel");
        } else if (key == kCloudOff) {
            row.value = ui::label("saves.cloud.do_off");
        } else if (key == kCloudKeepHere) {
            row.value = play_time::format(play_time::seconds(), ui::language());
        } else if (key == kCloudKeepOther) {
            row.value = cloud::other_device() + " - " + play_time::format(cloud::other_seconds(), ui::language());
        } else if (key == kHdTextures) {
            row.value = hd_text();
        } else if (key == kFlyCamera) {
            row.value = shown_value("keys.fly_camera", settings::text("keys.fly_camera", nullptr));
        } else if (row.kind != Kind::Button) {
            std::string value = settings::text(key.c_str(), nullptr);
            row.value = shown_value(key, value);
            row.on = row.kind == Kind::Toggle && enabled_text(value);
            if (const Slider* slider = slider_of(key)) {
                row.fraction = std::clamp((std::atoi(value.c_str()) - slider->low) / static_cast<double>(slider->high - slider->low), 0.0, 1.0);
            }
        }
        result.push_back(row);
    }
    return result;
}

std::string Menu::capture(const std::string& name) {
    std::vector<std::string> keys = this->keys();
    if (!capturing_ || selection_ >= keys.size() || name.empty()) {
        return "";
    }
    capturing_ = false;
    const std::string& key = keys[selection_];
    settings::store(key.c_str(), name);
    int physical = padmap::physical_from_name(name);
    if (starts_with(key, "pad.") && physical >= 0) {
        bool sideways = game::description().sideways_grip;
        for (size_t i = 0; i < padmap::kActionCount; i++) {
            std::string other = padmap::setting_key(i);
            if (other == key) {
                continue;
            }
            std::string value = settings::text(other.c_str(), nullptr);
            std::string kept = padmap::without(value, physical, i, sideways);
            if (kept != padmap::format_binding(padmap::parse_binding(value, i, sideways))) {
                settings::store(other.c_str(), kept);
            }
        }
    }
    return key;
}

std::string Menu::set_toggle(size_t row, bool on) {
    std::vector<std::string> list = keys();
    if (row >= list.size() || kind(list[row]) != Kind::Toggle) {
        return "";
    }
    const std::string& key = list[row];
    if (enabled_text(settings::text(key.c_str(), nullptr)) == on) {
        return "";
    }
    settings::store(key.c_str(), on ? "1" : "0");
    return key;
}

std::string Menu::set_fraction(size_t row, double fraction) {
    std::vector<std::string> keys = this->keys();
    const Slider* slider = row < keys.size() ? slider_of(keys[row]) : nullptr;
    if (!slider) {
        return "";
    }
    int steps = (slider->high - slider->low) / slider->step;
    int value = slider->low + static_cast<int>(std::clamp(fraction, 0.0, 1.0) * steps + 0.5) * slider->step;
    settings::store(slider->key, std::to_string(value));
    return slider->key;
}

std::string Menu::handle(Action action) {
    std::vector<std::string> keys = this->keys();
    size_t count = keys.size();
    selection_ = std::min(selection_, count - 1);
    if (capturing_) {
        capturing_ = false;
        return "";
    }
    const std::string key = keys[selection_];
    Kind type = kind(key);
    bool changes_value = action == Action::Previous || action == Action::Next || action == Action::Accept;
    if (changes_value && key == kDevice) {
        device_ = device_ == Device::Keyboard ? Device::Gamepad : Device::Keyboard;
        scroll_ = 0;
        return "";
    }
    if (action == Action::Accept && key == kUpdate && update::state() == update::State::Newer) {
        update::open_page();
        return "";
    }
    bool field_was_open = g_field != nullptr;
    if (changes_value && ((starts_with(key, "saves.cloud_") && cloud_action(key, action)) || (starts_with(key, "online.") && online_action(key, action)))) {
        std::vector<std::string> updated = this->keys();
        selection_ = !field_was_open && g_field ? 0 : std::min(selection_, updated.size() - 1);
        keep_visible();
        return "";
    }
    if (changes_value && type == Kind::Info) {
        return "";
    }
    switch (action) {
    case Action::Up:
        selection_ = (selection_ + count - 1) % count;
        keep_visible();
        return "";
    case Action::Down:
        selection_ = (selection_ + 1) % count;
        keep_visible();
        return "";
    case Action::Close:
        open_ = false;
        return "";
    case Action::TabPrevious:
        select_tab((tab_ + kTabCount - 1) % kTabCount);
        return "";
    case Action::TabNext:
        select_tab((tab_ + 1) % kTabCount);
        return "";
    case Action::Accept:
        if (type == Kind::Key || type == Kind::Pad) {
            capturing_ = true;
            return "";
        }
        if (type == Kind::Button) {
            if (key == kFlyCamera) {
                free_camera::toggle_fly();
                open_ = false;
                return "";
            }
            if (key == kHdTextures) {
                hd_textures::start();
                settings::store("video.custom_textures", "1");
                return "video.custom_textures";
            }
            if (key == kResetColors) {
                for (const color::Range& range : color::kRanges) {
                    settings::store(range.key, "100");
                }
            } else if (key == kResetKeys) {
                for (size_t i = 0; i < keymap::kActionCount; i++) {
                    settings::store(keymap::setting_key(i).c_str(), keymap::local_defaults(i));
                }
            } else {
                for (size_t i = 0; i < padmap::kActionCount; i++) {
                    settings::store(padmap::setting_key(i).c_str(), padmap::format_binding(padmap::default_binding(i, game::description().sideways_grip)));
                }
            }
            return key;
        }
        if (type == Kind::Slider) {
            return "";
        }
        [[fallthrough]];
    case Action::Previous:
    case Action::Next: {
        if (type != Kind::Choice && type != Kind::Slider && !(type == Kind::Toggle && action == Action::Accept)) {
            return "";
        }
        std::string current = settings::text(key.c_str(), nullptr);
        std::string next = step_value(key, current, action == Action::Previous ? -1 : 1);
        if (next == current) {
            return "";
        }
        settings::store(key.c_str(), next);
        return key;
    }
    }
    return "";
}

bool type_code(const char* text) {
    if (g_field && text) {
        for (; *text; text++) {
            char character = static_cast<char>(std::toupper(static_cast<unsigned char>(*text)));
            if (g_field == kOnlineAddress) {
                character = character == 'O' ? '0' : (character == 'I' || character == 'L') ? '1' : character;
            }
            if (character != 0 && std::strchr(field_letters(), character)) {
                g_field_picking = false;
                add_field_character(character);
            }
        }
        return true;
    }
    if (!g_code_entry || !text) {
        return false;
    }
    for (; *text && g_code_entry; text++) {
        char character = static_cast<char>(std::toupper(static_cast<unsigned char>(*text)));
        character = character == 'O' ? '0' : (character == 'I' || character == 'L') ? '1' : character;
        if (character != 0 && std::strchr(cloud::code_letters(), character)) {
            add_code_character(character);
        }
    }
    return true;
}

bool text_field_open() {
    return g_field != nullptr || g_code_entry;
}

bool code_entry_open() {
    return g_code_entry || g_field;
}

bool take_keyboard_request() {
    bool wanted = g_keyboard_wanted;
    g_keyboard_wanted = false;
    return wanted;
}

bool code_entry_key(unsigned key) {
    if (g_field) {
        if (key == 0x08) {
            if (!g_field_text.empty()) {
                g_field_text.pop_back();
            }
        } else if (key == 0x1B) {
            close_field();
        } else if (key == 0x0D) {
            commit_field();
        } else if (key == 0x6E || key == 0xBC || key == 0xBE) {
            type_code(".");
        } else if (key >= 0x60 && key <= 0x69) {
            const char digit[2] = {static_cast<char>('0' + (key - 0x60)), 0};
            type_code(digit);
        } else if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z')) {
            const char letter[2] = {static_cast<char>(key), 0};
            type_code(letter);
        } else if (key != 0x20 && key != 0x25 && key != 0x27) {
            return false;
        }
        return true;
    }
    if (!g_code_entry) {
        return false;
    }
    if (key == 0x08) {
        if (!g_code_typed.empty()) {
            g_code_typed.pop_back();
        }
        return true;
    }
    if (key == 0x1B) {
        close_code_entry();
        return true;
    }
    if (key == 0x20 || key == 0x25 || key == 0x27) {
        return true;
    }
    if (key >= 0x60 && key <= 0x69) {
        key = '0' + (key - 0x60);
    }
    if (!((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z'))) {
        return false;
    }
    const char text[2] = {static_cast<char>(key), 0};
    type_code(text);
    return true;
}

uint32_t Repeat::update(uint32_t held, int64_t now) {
    uint32_t pressed = held & ~previous_;
    uint32_t fired = pressed;
    uint32_t repeating = held & previous_ & repeating_;
    if (pressed & repeating_) {
        next_ = now + delay_;
    } else if (repeating && now >= next_) {
        fired |= repeating;
        next_ = now + interval_;
    }
    previous_ = held;
    return fired;
}

Layout layout(int client_height, size_t total_rows, size_t scroll) {
    Layout result;
    result.unit = std::clamp(client_height / 40, 14, 40);
    int unit = result.unit;
    result.edge = std::max(3, unit * 3 / 5);
    result.padding = unit * 3;
    result.title_height = unit * 3;
    result.tabs_width = unit * 11;
    result.tab_height = unit * 2;
    result.row_height = unit * 2;
    result.help_height = unit * 3;
    result.hints_height = unit * 2;
    result.total = total_rows;
    result.scroll = scroll;
    result.width = unit * 60 + result.edge;
    result.height = result.padding * 2 + result.title_height + rows_rect(result).height + result.help_height + result.hints_height + result.edge;
    return result;
}

Rect place(int client_width, int client_height, int width, int height) {
    Rect result;
    if (client_width <= 0 || client_height <= 0 || width <= 0 || height <= 0) {
        return result;
    }
    double scale = std::min({1.0, static_cast<double>(client_width) / width, static_cast<double>(client_height) / height});
    result.width = std::max(1, static_cast<int>(width * scale));
    result.height = std::max(1, static_cast<int>(height * scale));
    result.x = (client_width - result.width) / 2;
    result.y = (client_height - result.height) / 2;
    return result;
}

Rect tab_rect(const Layout& layout, size_t tab) {
    Rect result;
    result.x = layout.padding;
    result.y = layout.padding + layout.title_height + static_cast<int>(tab) * (layout.tab_height + layout.unit / 8);
    result.width = layout.tabs_width;
    result.height = layout.tab_height;
    return result;
}

Rect rows_rect(const Layout& layout) {
    Rect result;
    result.x = layout.padding + layout.tabs_width + layout.unit;
    result.y = layout.padding + layout.title_height;
    result.width = layout.width - layout.edge - result.x - layout.padding;
    result.height = static_cast<int>(kVisibleRows) * layout.row_height + layout.unit;
    return result;
}

Rect help_rect(const Layout& layout) {
    Rect rows = rows_rect(layout);
    Rect result;
    result.x = layout.padding;
    result.y = rows.y + rows.height;
    result.width = layout.width - layout.edge - layout.padding * 2;
    result.height = layout.help_height;
    return result;
}

Rect hints_rect(const Layout& layout) {
    Rect help = help_rect(layout);
    Rect result = help;
    result.y = help.y + help.height;
    result.height = layout.hints_height;
    return result;
}

Rect row_rect(const Layout& layout, size_t visible_row) {
    Rect rows = rows_rect(layout);
    Rect result;
    result.x = rows.x + layout.unit / 2;
    result.y = rows.y + layout.unit / 2 + static_cast<int>(visible_row) * layout.row_height;
    result.width = rows.width - layout.unit * 2;
    result.height = layout.row_height;
    return result;
}

Rect control_rect(const Layout& layout, size_t visible_row) {
    Rect row = row_rect(layout, visible_row);
    Rect result;
    result.width = layout.unit * 13;
    result.height = layout.unit * 3 / 2;
    result.x = row.x + row.width - layout.unit / 2 - result.width;
    result.y = row.y + (row.height - result.height) / 2;
    return result;
}

Rect close_rect(const Layout& layout) {
    Rect result;
    int unit = layout.unit;
    result.width = unit * 2;
    result.height = unit * 2;
    result.x = layout.width - layout.edge - layout.padding - result.width;
    result.y = layout.padding + (layout.title_height - result.height) / 2;
    return result;
}

Hover hit_test(const Layout& layout, const Rect& placed, int x, int y) {
    Hover result;
    if (placed.width <= 0 || placed.height <= 0 || x < placed.x || y < placed.y || x >= placed.x + placed.width || y >= placed.y + placed.height) {
        return result;
    }
    int bx = static_cast<int>(static_cast<int64_t>(x - placed.x) * layout.width / placed.width);
    int by = static_cast<int>(static_cast<int64_t>(y - placed.y) * layout.height / placed.height);
    auto inside = [&](const Rect& rect, int margin) {
        return bx >= rect.x - margin && bx < rect.x + rect.width + margin && by >= rect.y - margin && by < rect.y + rect.height + margin;
    };
    int margin = layout.unit / 2;
    if (inside(close_rect(layout), margin)) {
        result.part = Part::Close;
        return result;
    }
    for (size_t tab = 0; tab < kTabCount; tab++) {
        if (inside(tab_rect(layout, tab), 0)) {
            result.part = Part::Tab;
            result.tab = static_cast<int>(tab);
            return result;
        }
    }
    for (size_t visible = 0; visible < kVisibleRows && layout.scroll + visible < layout.total; visible++) {
        if (!inside(row_rect(layout, visible), 0)) {
            continue;
        }
        result.row = static_cast<int>(layout.scroll + visible);
        result.part = Part::Label;
        Rect control = control_rect(layout, visible);
        if (bx >= control.x - margin && bx < control.x + control.width + margin) {
            int arrow = layout.unit * 2;
            result.part = bx < control.x + arrow ? Part::Previous : bx >= control.x + control.width - arrow ? Part::Next : Part::Value;
        }
        return result;
    }
    return result;
}

double slider_fraction(const Layout& layout, const Rect& placed, size_t visible_row, int x) {
    if (placed.width <= 0) {
        return 0.0;
    }
    Rect control = control_rect(layout, visible_row);
    int bx = static_cast<int>(static_cast<int64_t>(x - placed.x) * layout.width / placed.width);
    int left = control.x + layout.unit / 2;
    int right = control.x + control.width - layout.unit * 4;
    return std::clamp((bx - left) / static_cast<double>(right - left), 0.0, 1.0);
}

double control_fraction(const Layout& layout, const Rect& placed, size_t visible_row, int x) {
    if (placed.width <= 0) {
        return 0.0;
    }
    Rect control = control_rect(layout, visible_row);
    double bx = static_cast<double>(x - placed.x) * layout.width / placed.width;
    return std::clamp((bx - control.x) / std::max(1, control.width), 0.0, 1.0);
}

void show_overlay(Image image) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    g_overlay = std::move(image);
    g_overlay_visible = true;
    g_overlay_version++;
}

void hide_overlay() {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (!g_overlay_visible) {
        return;
    }
    g_overlay_visible = false;
    g_overlay_version++;
}

bool take_overlay(uint64_t& version, Image& image, bool& visible) {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (version == g_overlay_version) {
        return false;
    }
    version = g_overlay_version;
    visible = g_overlay_visible;
    image = std::move(g_overlay);
    g_overlay = Image{};
    return true;
}

}
