#include "wp/disc_textures.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "gx_render_common.h"
#include "wp/custom_textures.h"
#include "wp/video.h"

namespace wp::gx::disc_textures {

namespace {

constexpr size_t kLargestFile = 256u << 20;
constexpr int kDeepest = 4;
constexpr uint32_t kLargestSide = 1024;
constexpr size_t kTlutBytes = 16384 * 2;
constexpr const char* kArchive = "\x55\xAA\x38\x2D";
constexpr const char* kTpl = "\x00\x20\xAF\x30";

struct Bytes {
    const uint8_t* data = nullptr;
    size_t size = 0;

    uint32_t u16(size_t at) const {
        return at + 2 <= size ? static_cast<uint32_t>(data[at] << 8 | data[at + 1]) : 0;
    }

    uint32_t u32(size_t at) const {
        return at + 4 <= size ? static_cast<uint32_t>(data[at]) << 24 | static_cast<uint32_t>(data[at + 1]) << 16 | static_cast<uint32_t>(data[at + 2]) << 8 |
                                    data[at + 3]
                              : 0;
    }

    bool magic(size_t at, const char* text) const {
        return at + 4 <= size && std::memcmp(data + at, text, 4) == 0;
    }

    Bytes from(size_t at, size_t length = SIZE_MAX) const {
        return at < size ? Bytes{data + at, std::min(length, size - at)} : Bytes{};
    }

    std::string text(size_t at) const {
        Bytes rest = from(at);
        const void* end = rest.size ? std::memchr(rest.data, 0, rest.size) : nullptr;
        return end ? std::string(reinterpret_cast<const char*>(rest.data), static_cast<const uint8_t*>(end) - rest.data) : std::string();
    }
};

struct Export {
    std::filesystem::path folder;
    std::unordered_set<std::string> names;
};

bool unpack_lz11(Bytes in, std::vector<uint8_t>& out) {
    size_t size = in.size >= 4 ? in.data[1] | in.data[2] << 8 | in.data[3] << 16 : 0;
    size_t at = 4;
    if (size == 0 && in.size >= 8) {
        size = in.data[4] | in.data[5] << 8 | in.data[6] << 16 | static_cast<size_t>(in.data[7]) << 24;
        at = 8;
    }
    if (size == 0 || size > kLargestFile) {
        return false;
    }
    out.clear();
    out.reserve(size);
    while (out.size() < size) {
        if (at >= in.size) {
            return false;
        }
        uint32_t flags = in.data[at++];
        for (int bit = 7; bit >= 0 && out.size() < size; bit--) {
            if (((flags >> bit) & 1) == 0) {
                if (at >= in.size) {
                    return false;
                }
                out.push_back(in.data[at++]);
                continue;
            }
            uint32_t kind = at < in.size ? in.data[at] >> 4 : 0;
            size_t bytes = kind == 0 ? 3 : kind == 1 ? 4 : 2;
            if (at + bytes > in.size) {
                return false;
            }
            const uint8_t* p = in.data + at;
            uint32_t length = kind == 0 ? ((p[0] & 15) << 4 | p[1] >> 4) + 0x11 : kind == 1 ? ((p[0] & 15) << 12 | p[1] << 4 | p[2] >> 4) + 0x111 : kind + 1;
            size_t distance = (kind == 0 ? (p[1] & 15) << 8 | p[2] : kind == 1 ? (p[2] & 15) << 8 | p[3] : (p[0] & 15) << 8 | p[1]) + 1;
            at += bytes;
            if (distance > out.size()) {
                return false;
            }
            for (uint32_t i = 0; i < length && out.size() < size; i++) {
                uint8_t value = out[out.size() - distance];
                out.push_back(value);
            }
        }
    }
    return true;
}

void texture(Export& job, Bytes image, uint32_t width, uint32_t height, uint32_t format, Bytes palette, uint32_t palette_format) {
    render::Layout layout = render::layout_for(format);
    if (!layout.supported || width == 0 || height == 0 || width > kLargestSide || height > kLargestSide) {
        return;
    }
    size_t size = static_cast<size_t>((width + layout.block_width - 1) / layout.block_width) * ((height + layout.block_height - 1) / layout.block_height) *
                  render::block_bytes(format);
    if (size > image.size) {
        return;
    }
    std::vector<uint8_t> tlut(kTlutBytes);
    std::copy_n(palette.data, std::min(palette.size, tlut.size()), tlut.begin());
    std::string name = custom_textures::name_texture(image.data, size, width, height, format, false, tlut.data()).full();
    if (!job.names.insert(name).second) {
        return;
    }
    std::filesystem::path path = job.folder / (name + ".png");
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        video::save_png_rgba(path.string().c_str(), render::decode_texture(image.data, width, height, format, tlut.data(), palette_format), width, height);
    }
}

void tpl(Export& job, Bytes file) {
    uint32_t count = file.u32(4);
    size_t table = file.u32(8);
    for (size_t i = 0; i < count && table + (i + 1) * 8 <= file.size; i++) {
        size_t image = file.u32(table + i * 8);
        size_t palette = file.u32(table + i * 8 + 4);
        if (image + 0x24 > file.size || file.u32(image + 0x14) >= 2) {
            continue;
        }
        Bytes colors = palette ? file.from(file.u32(palette + 8), file.u16(palette) * 2) : Bytes{};
        texture(job, file.from(file.u32(image + 8)), file.u16(image + 2), file.u16(image), file.u32(image + 4), colors, palette ? file.u32(palette + 4) : 0);
    }
}

void brres(Export& job, Bytes file) {
    std::unordered_map<std::string, std::pair<Bytes, uint32_t>> palettes;
    for (const char* magic : {"PLT0", "TEX0"}) {
        for (size_t at = 0; at + 0x40 <= file.size; at += 4) {
            if (!file.magic(at, magic) || static_cast<int64_t>(static_cast<int32_t>(file.u32(at + 12))) != -static_cast<int64_t>(at)) {
                continue;
            }
            Bytes section = file.from(at);
            std::string name = section.text(section.u32(0x14));
            if (magic[0] == 'P') {
                palettes[name] = {section.from(section.u32(0x10), section.u16(0x1C) * 2), section.u32(0x18)};
            } else if (section.u32(0x24) <= 1) {
                auto found = palettes.find(name);
                bool paletted = found != palettes.end();
                texture(job, section.from(section.u32(0x10)), section.u16(0x1C), section.u16(0x1E), section.u32(0x20), paletted ? found->second.first : Bytes{},
                        paletted ? found->second.second : 0);
            }
        }
    }
}

void font(Export& job, Bytes file) {
    for (size_t at = file.u16(12); at + 32 <= file.size;) {
        if (file.magic(at, "TGLP")) {
            size_t sheet = file.u32(at + 12);
            for (uint32_t i = 0; i < file.u16(at + 16); i++) {
                texture(job, file.from(file.u32(at + 28) + i * sheet), file.u16(at + 24), file.u16(at + 26), file.u16(at + 18) & 0x7FFF, {}, 0);
            }
        }
        uint32_t size = file.u32(at + 4);
        if (size == 0) {
            break;
        }
        at += size;
    }
}

bool known(Bytes file) {
    return file.magic(0, kArchive) || file.magic(0, kTpl) || file.magic(0, "bres") || file.magic(0, "RFNT");
}

void walk(Export& job, Bytes file, int depth) {
    std::vector<uint8_t> unpacked;
    if (file.size >= 4 && file.data[0] == 0x11) {
        if (!unpack_lz11(file, unpacked)) {
            return;
        }
        file = {unpacked.data(), unpacked.size()};
    }
    if (file.magic(0, kArchive)) {
        size_t first = file.u32(4);
        uint32_t count = file.u32(first + 8);
        for (size_t i = 1; depth < kDeepest && i < count && first + (i + 1) * 12 <= file.size; i++) {
            size_t node = first + i * 12;
            if (file.data[node] == 0) {
                walk(job, file.from(file.u32(node + 4), file.u32(node + 8)), depth + 1);
            }
        }
    } else if (file.magic(0, kTpl)) {
        tpl(job, file);
    } else if (file.magic(0, "bres")) {
        brres(job, file);
    } else if (file.magic(0, "RFNT")) {
        font(job, file);
    }
}

}

size_t export_flat(const std::filesystem::path& files, const std::filesystem::path& folder) {
    Export job{folder, {}};
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    for (std::filesystem::recursive_directory_iterator it(files, error), end; !error && it != end; it.increment(error)) {
        std::error_code unreadable;
        uintmax_t size = it->is_regular_file(unreadable) ? it->file_size(unreadable) : 0;
        if (unreadable || size < 8 || size > kLargestFile) {
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        std::vector<uint8_t> data(4);
        if (!in.read(reinterpret_cast<char*>(data.data()), 4) || (data[0] != 0x11 && !known({data.data(), 4}))) {
            continue;
        }
        data.resize(size);
        if (in.read(reinterpret_cast<char*>(data.data()) + 4, static_cast<std::streamsize>(size - 4))) {
            walk(job, {data.data(), data.size()}, 0);
        }
    }
    return job.names.size();
}

}
