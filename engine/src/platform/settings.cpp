#include "wp/settings.h"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <vector>

#include "wp/game.h"
#include "wp/keymap.h"
#include "wp/padmap.h"

namespace wp::settings {

namespace {

struct Option {
    std::string key;
    std::string fallback;
};

const Option kBaseOptions[] = {
    {"video.scale", "1"},
    {"video.fps", "60"},
    {"video.fullscreen", "0"},
    {"video.vsync", "0"},
    {"video.show_fps", "1"},
    {"video.smooth", "1"},
    {"video.anisotropy", "1"},
    {"video.brightness", "100"},
    {"video.contrast", "100"},
    {"video.saturation", "100"},
    {"video.gamma", "100"},
    {"video.copy_filter", "1"},
    {"video.dump_textures", "0"},
    {"video.bug_recorder", "0"},
    {"video.gpu_vertices", "1"},
    {"video.keep_models", "1"},
    {"video.gpu_between", "1"},
    {"video.custom_textures", "1"},
    {"input.gamepads", "1"},
    {"input.joycon_pairs", "1"},
    {"input.real_wiimotes", "1"},
    {"input.real_wiimote_mouse", "1"},
    {"input.wake_on_mouse", "1"},
    {"input.hide_cursor", "1"},
    {"input.player1_keyboard_only", "0"},
    {"input.ignored_gamepads", ""},
    {"input.gamepad_players", ""},
    {"input.rumble", "1"},
    {"input.bindings_version", "1"},
    {"input.stick_deadzone", "50"},
    {"camera.free", "1"},
    {"camera.mouse_speed", "100"},
    {"camera.stick_speed", "100"},
    {"camera.invert_y", "0"},
    {"system.interface_language", "en"},
    {"system.language", "auto"},
    {"system.pal60", "1"},
    {"system.widescreen", "1"},
    {"video.aspect", "16:9"},
    {"game.text_speed", "1"},
    {"game.skip_movies", "1"},
    {"game.button_icons", "1"},
    {"system.options_menu", "1"},
    {"system.discord", "1"},
    {"system.check_updates", "1"},
    {"system.idle_sleep", "1"},
    {"audio.mute", "0"},
    {"audio.volume", "100"},
    {"audio.wiimote_speaker", "1"},
    {"saves.backups", "5"},
    {"overlay.show", "0"},
    {"overlay.level", "full"},
    {"overlay.corner", "top_left"},
};

const std::vector<Option>& options() {
    static const std::vector<Option> list = [] {
        std::vector<Option> result(std::begin(kBaseOptions), std::end(kBaseOptions));
        for (size_t i = 0; i < keymap::kActionCount; i++) {
            result.push_back({keymap::setting_key(i), keymap::local_defaults(i)});
        }
        for (size_t i = 0; i < padmap::kActionCount; i++) {
            result.push_back({padmap::setting_key(i), padmap::format_binding(padmap::default_binding(i, game::description().sideways_grip))});
        }
        return result;
    }();
    return list;
}

std::mutex g_mutex;
std::string g_path;
std::string g_snapshot;
std::map<std::string, std::string> g_values;
std::atomic<uint32_t> g_revision{1};

std::string fallback_of(const char* key) {
    for (const Option& option : options()) {
        if (option.key == key) {
            return option.fallback;
        }
    }
    return "";
}

std::string trimmed(const std::string& text) {
    size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = text.find_last_not_of(" \t\r");
    return text.substr(first, last - first + 1);
}

void write_file() {
    if (g_path.empty()) {
        return;
    }
    std::ostringstream out;
    std::string section;
    for (const Option& option : options()) {
        const std::string& key = option.key;
        size_t dot = key.find('.');
        std::string group = key.substr(0, dot);
        if (group != section) {
            if (!section.empty()) {
                out << '\n';
            }
            out << '[' << group << "]\n";
            section = group;
        }
        out << key.substr(dot + 1) << '=' << g_values[key] << '\n';
    }
    std::ofstream file(g_path, std::ios::binary | std::ios::trunc);
    file << out.str();
    g_snapshot = out.str();
}

std::string file_text() {
    std::ifstream file(g_path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void read_file() {
    g_snapshot = file_text();
    std::istringstream file(g_snapshot);
    std::string line;
    std::string section;
    while (std::getline(file, line)) {
        line = trimmed(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            section = trimmed(line.substr(1, line.size() - 2));
            continue;
        }
        size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string key = section + "." + trimmed(line.substr(0, equals));
        if (g_values.count(key)) {
            g_values[key] = trimmed(line.substr(equals + 1));
        }
    }
}

std::string value_of(const char* key, const char* variable) {
    if (variable) {
        if (const char* setting = std::getenv(variable)) {
            return setting;
        }
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_values.find(key);
    return it != g_values.end() ? it->second : fallback_of(key);
}

}

void load(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_path = path;
    for (const Option& option : options()) {
        g_values[option.key] = option.fallback;
    }
    read_file();
    write_file();
    g_revision++;
}

bool reload_if_changed() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) {
        return false;
    }
    if (file_text() == g_snapshot) {
        return false;
    }
    read_file();
    g_revision++;
    return true;
}

bool flag(const char* key, const char* variable) {
    std::string value = value_of(key, variable);
    return !(value == "0" || value == "false" || value == "off" || value == "no");
}

int number(const char* key, const char* variable) {
    return std::atoi(value_of(key, variable).c_str());
}

std::string text(const char* key, const char* variable) {
    return value_of(key, variable);
}

void store(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_values[key] = value;
    write_file();
    g_revision++;
}

std::vector<std::string> keys() {
    std::vector<std::string> result;
    for (const Option& option : kBaseOptions) {
        if (option.key != "input.ignored_gamepads" && option.key != "input.gamepad_players") {
            result.push_back(option.key);
        }
    }
    return result;
}

uint32_t revision() {
    return g_revision.load();
}

bool LiveFlag::operator()() const {
    uint64_t current = revision();
    uint64_t cached = cache_.load(std::memory_order_acquire);
    if ((cached >> 32) != current) {
        cached = (current << 32) | (flag(key_, variable_) ? 1u : 0u);
        cache_.store(cached, std::memory_order_release);
    }
    return (cached & 1) != 0;
}

int LiveNumber::operator()() const {
    uint64_t current = revision();
    uint64_t cached = cache_.load(std::memory_order_acquire);
    if ((cached >> 32) != current) {
        cached = (current << 32) | static_cast<uint32_t>(number(key_, variable_));
        cache_.store(cached, std::memory_order_release);
    }
    return static_cast<int>(static_cast<uint32_t>(cached));
}

}
