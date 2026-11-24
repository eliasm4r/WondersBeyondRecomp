#include "wp/disc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "wp/disc_textures.h"
#include "wp/log.h"
#include "wp/memory.h"

namespace wp::disc {

namespace {

std::atomic<uint32_t> g_field{0};

constexpr size_t kEntrySize = 12;
constexpr uint32_t kOffsetShift = 2;
constexpr uint64_t kBootOffset = 0;
constexpr uint64_t kBi2Offset = 0x440;
constexpr uint64_t kApploaderOffset = 0x2440;
constexpr uint32_t kDolOffsetField = 0x420;
constexpr uint32_t kFstOffsetField = 0x424;

struct FileEntry {
    uint64_t offset;
    uint64_t size;
    std::string path;
};

std::vector<FileEntry> g_files;
std::string g_root;

uint32_t be32(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24) | (static_cast<uint32_t>(data[offset + 1]) << 16) |
           (static_cast<uint32_t>(data[offset + 2]) << 8) | data[offset + 3];
}

bool read_file(const std::string& path, std::vector<uint8_t>& data) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    uint8_t buffer[65536];
    size_t count;
    while ((count = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
        data.insert(data.end(), buffer, buffer + count);
    }
    std::fclose(file);
    return true;
}

uint64_t file_size(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return 0;
    }
    std::fseek(file, 0, SEEK_END);
    uint64_t size = static_cast<uint64_t>(std::ftell(file));
    std::fclose(file);
    return size;
}

void add_system_file(const std::string& name, uint64_t offset) {
    std::string path = g_root + "/sys/" + name;
    g_files.push_back({offset, file_size(path), path});
}

bool add_files(const std::vector<uint8_t>& fst) {
    if (fst.size() < kEntrySize) {
        return false;
    }
    uint32_t count = be32(fst, 8);
    size_t names = static_cast<size_t>(count) * kEntrySize;
    if (names > fst.size()) {
        return false;
    }
    std::vector<std::pair<std::string, uint32_t>> directories;
    directories.push_back({"", count});
    for (uint32_t index = 1; index < count; index++) {
        while (directories.size() > 1 && index >= directories.back().second) {
            directories.pop_back();
        }
        size_t entry = index * kEntrySize;
        bool is_directory = fst[entry] != 0;
        uint32_t name_offset = be32(fst, entry) & 0x00FFFFFF;
        size_t start = names + name_offset;
        const void* end = start < fst.size() ? std::memchr(&fst[start], 0, fst.size() - start) : nullptr;
        if (end == nullptr) {
            return false;
        }
        std::string name(reinterpret_cast<const char*>(&fst[start]));
        if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\") != std::string::npos) {
            return false;
        }
        std::string parent;
        for (const auto& directory : directories) {
            parent += directory.first;
        }
        if (is_directory) {
            directories.push_back({name + "/", be32(fst, entry + 8)});
        } else {
            uint64_t offset = static_cast<uint64_t>(be32(fst, entry + 4)) << kOffsetShift;
            g_files.push_back({offset, be32(fst, entry + 8), g_root + "/files/" + parent + name});
        }
    }
    return true;
}

}

bool mount(const std::string& extracted_directory) {
    g_root = extracted_directory;
    g_files.clear();
    std::vector<uint8_t> boot;
    std::vector<uint8_t> fst;
    if (!read_file(g_root + "/sys/boot.bin", boot) || !read_file(g_root + "/sys/fst.bin", fst) || boot.size() < 0x440) {
        return false;
    }
    add_system_file("boot.bin", kBootOffset);
    add_system_file("bi2.bin", kBi2Offset);
    add_system_file("apploader.img", kApploaderOffset);
    add_system_file("main.dol", static_cast<uint64_t>(be32(boot, kDolOffsetField)) << kOffsetShift);
    add_system_file("fst.bin", static_cast<uint64_t>(be32(boot, kFstOffsetField)) << kOffsetShift);
    if (!add_files(fst)) {
        return false;
    }
    std::sort(g_files.begin(), g_files.end(), [](const FileEntry& a, const FileEntry& b) { return a.offset < b.offset; });
    return true;
}

std::string g_watched_part;
std::atomic<int64_t> g_watched_read{INT64_MIN / 2};

int64_t now_nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double seconds_since_read(const char* path_part) {
    if (g_watched_part != path_part) {
        g_watched_part = path_part;
    }
    return static_cast<double>(now_nanoseconds() - g_watched_read.load()) * 1e-9;
}

uint32_t field_in_path(const std::string& path) {
    static const std::string kStart = "Field/Fd";
    static const std::string kEnd = "Res.dac";
    size_t at = path.rfind(kStart);
    if (at == std::string::npos || path.size() != at + kStart.size() + 4 + kEnd.size() || path.compare(at + kStart.size() + 4, kEnd.size(), kEnd) != 0) {
        return 0;
    }
    uint32_t field = 0;
    for (size_t i = at + kStart.size(); i < at + kStart.size() + 4; i++) {
        if (path[i] < '0' || path[i] > '9') {
            return 0;
        }
        field = field * 10 + static_cast<uint32_t>(path[i] - '0');
    }
    return field;
}

uint32_t current_field() {
    return g_field.load(std::memory_order_relaxed);
}

std::mutex g_message_mutex;
std::string g_message_path;
std::string g_names_path;
std::map<std::string, std::string> g_names;

std::map<std::string, std::string> message_texts(const std::vector<uint8_t>& data) {
    constexpr uint32_t kTagStart = 0x0e;
    constexpr uint32_t kTagEnd = 0x0f;
    auto be16 = [&](size_t at) { return static_cast<uint32_t>(data[at] << 8 | data[at + 1]); };
    std::map<std::string, std::string> texts;
    for (size_t at = 0; at + 0x20 <= data.size(); at++) {
        if (std::memcmp(&data[at], "MsgStdBn", 8) != 0) {
            continue;
        }
        uint32_t sections = be16(at + 0xe);
        size_t pos = at + 0x20;
        std::vector<std::pair<std::string, uint32_t>> labels;
        std::vector<std::string> strings;
        for (uint32_t section = 0; section < sections && pos + 16 <= data.size(); section++) {
            size_t length = be32(data, pos + 4);
            size_t body = pos + 16;
            if (length < 4 || length > data.size() - body) {
                break;
            }
            size_t end = body + length;
            if (std::memcmp(&data[pos], "LBL1", 4) == 0) {
                for (uint32_t group = 0; group < be32(data, body) && body + 12 + group * 8 <= end; group++) {
                    size_t p = body + be32(data, body + 8 + group * 8);
                    for (uint32_t i = be32(data, body + 4 + group * 8); i > 0 && p < end; i--) {
                        size_t size = data[p];
                        if (p + 5 + size > end) {
                            break;
                        }
                        labels.push_back({std::string(reinterpret_cast<const char*>(&data[p + 1]), size), be32(data, p + 1 + size)});
                        p += 5 + size;
                    }
                }
            } else if (std::memcmp(&data[pos], "TXT2", 4) == 0) {
                for (uint32_t i = 0; i < be32(data, body) && body + 8 + i * 4 <= end; i++) {
                    std::string text;
                    for (size_t p = body + be32(data, body + 4 + i * 4); p + 2 <= end;) {
                        uint32_t unit = be16(p);
                        p += 2;
                        if (unit == 0 || (unit == kTagStart && p + 6 > end)) {
                            break;
                        }
                        if (unit == kTagStart) {
                            p += 6 + be16(p + 4);
                        } else if (unit == kTagEnd) {
                            p += 4;
                        } else if (unit < 0x80) {
                            text += static_cast<char>(unit);
                        } else if (unit < 0x800) {
                            text += static_cast<char>(0xc0 | unit >> 6);
                            text += static_cast<char>(0x80 | (unit & 0x3f));
                        } else {
                            text += static_cast<char>(0xe0 | unit >> 12);
                            text += static_cast<char>(0x80 | (unit >> 6 & 0x3f));
                            text += static_cast<char>(0x80 | (unit & 0x3f));
                        }
                    }
                    strings.push_back(text);
                }
            }
            pos = body + ((length + 15) & ~size_t{15});
        }
        for (const auto& [name, index] : labels) {
            if (index < strings.size()) {
                texts.emplace(name, strings[index]);
            }
        }
        at = pos - 1;
    }
    return texts;
}

std::string game_language() {
    static const std::map<std::string, std::string> kCodes = {{"En", "en"}, {"Fr", "fr"}, {"Ge", "de"}, {"It", "it"}, {"Sp", "es"}};
    std::lock_guard<std::mutex> lock(g_message_mutex);
    size_t at = g_message_path.rfind('_');
    auto code = at == std::string::npos ? kCodes.end() : kCodes.find(g_message_path.substr(at + 1, 2));
    return code == kCodes.end() ? "" : code->second;
}

std::string field_name(uint32_t field) {
    std::lock_guard<std::mutex> lock(g_message_mutex);
    if (g_message_path.empty()) {
        return "";
    }
    if (g_names_path != g_message_path) {
        g_names_path = g_message_path;
        std::vector<uint8_t> packed;
        std::vector<uint8_t> data;
        bool unpacked = read_file(g_names_path, packed) && (packed.empty() || packed[0] != 0x11 ? (data = packed, true) : gx::disc_textures::unpack_lz11(packed, data));
        g_names = unpacked ? message_texts(data) : std::map<std::string, std::string>{};
    }
    char key[16];
    std::snprintf(key, sizeof key, "Fd%04u", field);
    auto found = g_names.find(key);
    if (found == g_names.end() && field >= 1000 && field < 10000) {
        std::snprintf(key, sizeof key, "Fd%02u", field / 1000);
        found = g_names.find(key);
    }
    return found != g_names.end() ? found->second : "";
}

void read(uint64_t offset, uint32_t length, uint32_t destination) {
    static const bool logging = std::getenv("WP_LOG_DISC") != nullptr;
    uint8_t* output = host(destination);
    std::fill(output, output + length, 0);
    uint64_t end = offset + length;
    auto it = std::upper_bound(g_files.begin(), g_files.end(), offset,
                               [](uint64_t value, const FileEntry& entry) { return value < entry.offset; });
    if (it != g_files.begin()) {
        --it;
    }
    for (; it != g_files.end() && it->offset < end; ++it) {
        uint64_t file_end = it->offset + it->size;
        if (file_end <= offset) {
            continue;
        }
        uint64_t start = std::max(offset, it->offset);
        uint64_t stop = std::min(end, file_end);
        auto began = std::chrono::steady_clock::now();
        if (uint32_t field = field_in_path(it->path)) {
            g_field.store(field, std::memory_order_relaxed);
        }
        if (it->path.find("ResidentMessage_") != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_message_mutex);
            g_message_path = it->path;
        }
        if (!g_watched_part.empty() && it->path.find(g_watched_part) != std::string::npos) {
            g_watched_read = now_nanoseconds();
        }
        std::FILE* file = std::fopen(it->path.c_str(), "rb");
        if (logging) {
            std::fprintf(stderr, "disc read %llx+%x -> %08x from %s%s\n", static_cast<unsigned long long>(offset), length,
                         destination, it->path.c_str(), file ? "" : " (missing)");
        }
        if (file) {
            std::fseek(file, static_cast<long>(start - it->offset), SEEK_SET);
            std::fread(output + (start - offset), 1, static_cast<size_t>(stop - start), file);
            std::fclose(file);
        }
        double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        log::write("disc", "read %u bytes from %s%s in %.2f ms", static_cast<uint32_t>(stop - start), it->path.c_str(), file ? "" : " (missing)", spent);
    }
}

}
