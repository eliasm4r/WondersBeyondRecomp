#include "gx_render_common.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "wp/memory.h"
#include "wp/settings.h"

namespace wp::gx::render {

uint8_t g_tlut[kTlutSize];

namespace {

uint16_t be16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

uint32_t expand5(uint32_t v) {
    return (v << 3) | (v >> 2);
}

uint32_t expand6(uint32_t v) {
    return (v << 2) | (v >> 4);
}

uint32_t expand4(uint32_t v) {
    return v * 17;
}

uint32_t expand3(uint32_t v) {
    return (v << 5) | (v << 2) | (v >> 1);
}

uint32_t rgb565(uint16_t v) {
    return rgba(expand5((v >> 11) & 31), expand6((v >> 5) & 63), expand5(v & 31), 255);
}

uint32_t rgb5a3(uint16_t v) {
    if (v & 0x8000) {
        return rgba(expand5((v >> 10) & 31), expand5((v >> 5) & 31), expand5(v & 31), 255);
    }
    return rgba(expand4((v >> 8) & 15), expand4((v >> 4) & 15), expand4(v & 15), expand3((v >> 12) & 7));
}

void decode_cmpr_block(const uint8_t* source, uint32_t* out, uint32_t stride) {
    uint16_t c0 = be16(source);
    uint16_t c1 = be16(source + 2);
    uint32_t palette[4];
    auto split = [](uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b) {
        r = expand5((c >> 11) & 31);
        g = expand6((c >> 5) & 63);
        b = expand5(c & 31);
    };
    uint32_t r0, g0, b0, r1, g1, b1;
    split(c0, r0, g0, b0);
    split(c1, r1, g1, b1);
    palette[0] = rgba(r0, g0, b0, 255);
    palette[1] = rgba(r1, g1, b1, 255);
    if (c0 > c1) {
        palette[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        palette[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        palette[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        palette[3] = rgba(0, 0, 0, 0);
    }
    for (uint32_t row = 0; row < 4; row++) {
        uint8_t bits = source[4 + row];
        for (uint32_t column = 0; column < 4; column++) {
            out[row * stride + column] = palette[(bits >> (6 - 2 * column)) & 3];
        }
    }
}

uint32_t palette_entry(const uint8_t* tlut, uint32_t tlut_format, uint32_t index) {
    uint16_t entry = be16(tlut + index * 2);
    switch (tlut_format) {
    case 0:
        return rgba(entry & 0xFF, entry & 0xFF, entry & 0xFF, entry >> 8);
    case 1:
        return rgb565(entry);
    default:
        return rgb5a3(entry);
    }
}

float signed11(uint32_t value) {
    int32_t v = static_cast<int32_t>(value & 0x7FF);
    if (v & 0x400) {
        v -= 0x800;
    }
    return static_cast<float>(v) / 255.0f;
}

}

Layout layout_for(uint32_t format) {
    switch (format) {
    case 0:
    case 8:
        return {8, 8, true};
    case 1:
    case 2:
    case 9:
        return {8, 4, true};
    case 3:
    case 4:
    case 5:
    case 6:
    case 10:
        return {4, 4, true};
    case 14:
        return {8, 8, true};
    default:
        return {4, 4, false};
    }
}

uint32_t block_bytes(uint32_t format) {
    return format == 6 ? 64 : 32;
}

namespace {

template <typename Pixel>
void decode_blocks(const uint8_t* data, uint32_t* pixels, uint32_t padded_width, uint32_t padded_height, uint32_t block_width, uint32_t block_height,
                   uint32_t bytes, Pixel pixel) {
    for (uint32_t by = 0; by < padded_height; by += block_height) {
        for (uint32_t bx = 0; bx < padded_width; bx += block_width) {
            uint32_t* out = pixels + static_cast<size_t>(by) * padded_width + bx;
            for (uint32_t y = 0; y < block_height; y++) {
                for (uint32_t x = 0; x < block_width; x++) {
                    out[y * padded_width + x] = pixel(data, x, y);
                }
            }
            data += bytes;
        }
    }
}

std::vector<uint32_t> palette_for(const uint8_t* tlut, uint32_t tlut_format, uint32_t entries) {
    std::vector<uint32_t> palette(entries);
    for (uint32_t i = 0; i < entries; i++) {
        palette[i] = palette_entry(tlut, tlut_format, i);
    }
    return palette;
}

}

std::vector<uint32_t> decode_texture(const uint8_t* data, uint32_t width, uint32_t height, uint32_t format, const uint8_t* tlut,
                                     uint32_t tlut_format) {
    Layout layout = layout_for(format);
    uint32_t padded_width = (width + layout.block_width - 1) / layout.block_width * layout.block_width;
    uint32_t padded_height = (height + layout.block_height - 1) / layout.block_height * layout.block_height;
    std::vector<uint32_t> pixels(static_cast<size_t>(padded_width) * padded_height, rgba(255, 0, 255, 255));
    if (!layout.supported) {
        return pixels;
    }
    uint32_t bytes = block_bytes(format);
    uint32_t bw = layout.block_width;
    uint32_t bh = layout.block_height;
    uint32_t* out = pixels.data();
    switch (format) {
    case 0:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t* block, uint32_t x, uint32_t y) {
            uint32_t v = expand4((block[y * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15);
            return rgba(v, v, v, v);
        });
        break;
    case 1:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t* block, uint32_t x, uint32_t y) {
            uint32_t v = block[y * 8 + x];
            return rgba(v, v, v, v);
        });
        break;
    case 2:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t* block, uint32_t x, uint32_t y) {
            uint32_t v = block[y * 8 + x];
            uint32_t i = expand4(v & 15);
            return rgba(i, i, i, expand4(v >> 4));
        });
        break;
    case 3:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t* block, uint32_t x, uint32_t y) {
            uint32_t i = block[(y * 4 + x) * 2 + 1];
            return rgba(i, i, i, block[(y * 4 + x) * 2]);
        });
        break;
    case 4:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes,
                      [](const uint8_t* block, uint32_t x, uint32_t y) { return rgb565(be16(block + (y * 4 + x) * 2)); });
        break;
    case 5:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes,
                      [](const uint8_t* block, uint32_t x, uint32_t y) { return rgb5a3(be16(block + (y * 4 + x) * 2)); });
        break;
    case 6:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t* block, uint32_t x, uint32_t y) {
            uint32_t index = (y * 4 + x) * 2;
            return rgba(block[index + 1], block[32 + index], block[32 + index + 1], block[index]);
        });
        break;
    case 8: {
        std::vector<uint32_t> palette = palette_for(tlut, tlut_format, 16);
        const uint32_t* colors = palette.data();
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes,
                      [colors](const uint8_t* block, uint32_t x, uint32_t y) { return colors[(block[y * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15]; });
        break;
    }
    case 9: {
        std::vector<uint32_t> palette = palette_for(tlut, tlut_format, 256);
        const uint32_t* colors = palette.data();
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [colors](const uint8_t* block, uint32_t x, uint32_t y) { return colors[block[y * 8 + x]]; });
        break;
    }
    case 10:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [tlut, tlut_format](const uint8_t* block, uint32_t x, uint32_t y) {
            return palette_entry(tlut, tlut_format, be16(block + (y * 4 + x) * 2) & 0x3FFF);
        });
        break;
    case 14:
        for (uint32_t by = 0; by < padded_height; by += 8) {
            for (uint32_t bx = 0; bx < padded_width; bx += 8) {
                uint32_t* corner = out + static_cast<size_t>(by) * padded_width + bx;
                for (uint32_t sub = 0; sub < 4; sub++) {
                    decode_cmpr_block(data + sub * 8, corner + (sub / 2) * 4 * padded_width + (sub % 2) * 4, padded_width);
                }
                data += 32;
            }
        }
        break;
    default:
        decode_blocks(data, out, padded_width, padded_height, bw, bh, bytes, [](const uint8_t*, uint32_t, uint32_t) { return 0u; });
        break;
    }
    if (padded_width == width) {
        pixels.resize(static_cast<size_t>(width) * height);
        return pixels;
    }
    std::vector<uint32_t> cropped(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; y++) {
        std::memcpy(&cropped[static_cast<size_t>(y) * width], &pixels[static_cast<size_t>(y) * padded_width], width * sizeof(uint32_t));
    }
    return cropped;
}

std::vector<uint8_t> cmpr_to_bc1(const uint8_t* data, uint32_t width, uint32_t height) {
    static const auto reversed = [] {
        std::array<uint8_t, 256> table{};
        for (uint32_t value = 0; value < 256; value++) {
            table[value] = static_cast<uint8_t>((value >> 6 & 3) | (value >> 2 & 12) | (value << 2 & 48) | (value << 6 & 192));
        }
        return table;
    }();
    uint32_t across = (width + 3) / 4;
    uint32_t down = (height + 3) / 4;
    uint32_t large_across = (width + 7) / 8;
    std::vector<uint8_t> blocks(static_cast<size_t>(across) * down * 8);
    uint8_t* out = blocks.data();
    for (uint32_t by = 0; by < down; by++) {
        for (uint32_t bx = 0; bx < across; bx++) {
            const uint8_t* in = data + (static_cast<size_t>(by / 2) * large_across + bx / 2) * 32 + ((by & 1) * 2 + (bx & 1)) * 8;
            out[0] = in[1];
            out[1] = in[0];
            out[2] = in[3];
            out[3] = in[2];
            out[4] = reversed[in[4]];
            out[5] = reversed[in[5]];
            out[6] = reversed[in[6]];
            out[7] = reversed[in[7]];
            out += 8;
        }
    }
    return blocks;
}

uint64_t hash_bytes(const uint8_t* data, size_t size) {
    constexpr uint64_t kPrime = 1099511628211ull;
    uint64_t hash = 1469598103934665603ull;
    size_t words = size / 8;
    const uint64_t* p = reinterpret_cast<const uint64_t*>(data);
    uint64_t lanes[4] = {hash, hash ^ 1, hash ^ 2, hash ^ 3};
    size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        lanes[0] = (lanes[0] ^ p[i]) * kPrime;
        lanes[1] = (lanes[1] ^ p[i + 1]) * kPrime;
        lanes[2] = (lanes[2] ^ p[i + 2]) * kPrime;
        lanes[3] = (lanes[3] ^ p[i + 3]) * kPrime;
    }
    for (uint64_t lane : lanes) {
        hash = (hash ^ lane) * kPrime;
    }
    for (; i < words; i++) {
        hash = (hash ^ p[i]) * kPrime;
    }
    for (size_t tail = words * 8; tail < size; tail++) {
        hash = (hash ^ data[tail]) * kPrime;
    }
    return hash;
}

uint64_t sample_hash(const uint8_t* data, size_t size) {
    constexpr size_t kSamples = 32;
    uint64_t hash = 1469598103934665603ull;
    size_t step = size / kSamples;
    for (size_t i = 0; i < kSamples; i++) {
        uint64_t word = 0;
        std::memcpy(&word, data + std::min(i * step, size > 8 ? size - 8 : 0), std::min<size_t>(size, 8));
        hash = (hash ^ word) * 1099511628211ull;
    }
    return hash;
}

BlendState blend_state_for(uint32_t word) {
    return blend_state(word & 0xFFFF, ((word >> 16) & 1) != 0, ((word >> 17) & 1) != 0, ((word >> 18) & 1) == 0);
}

uint32_t used_maps(const Constants& constants) {
    uint32_t maps = 0;
    for (uint32_t i = 0; i < constants.header[0] && i < kMaxStages; i++) {
        if (constants.stage[i][2] & 0x40) {
            maps |= 1u << (constants.stage[i][2] & 7);
        }
    }
    // An indirect map is often sampled by no TEV stage (a distortion's normal map): left unbound it reads as zero,
    // the largest offset, and the dash shockwave showed the scene shifted by 128 texels.
    for (uint32_t j = 0; j < (constants.indirect[0] & 7) && j < 4; j++) {
        maps |= 1u << ((constants.indirect[1] >> (6 * j)) & 7);
    }
    return maps;
}

void fill_constants(Constants& constants, int scale, int margin_x) {
    const uint32_t* bp = bp_registers();
    std::memset(&constants, 0, sizeof constants);
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ra = bp[0xE0 + 2 * i];
        uint32_t bg = bp[0xE1 + 2 * i];
        constants.initial[i][0] = signed11(ra);
        constants.initial[i][3] = signed11(ra >> 12);
        constants.initial[i][2] = signed11(bg);
        constants.initial[i][1] = signed11(bg >> 12);
    }
    const uint32_t* konst = konst_registers();
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t ra = konst[2 * i];
        uint32_t bg = konst[2 * i + 1];
        constants.konst[i][0] = signed11(ra);
        constants.konst[i][3] = signed11(ra >> 12);
        constants.konst[i][2] = signed11(bg);
        constants.konst[i][1] = signed11(bg >> 12);
    }
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t image0 = bp[i < 4 ? 0x88 + i : 0xA8 + (i - 4)];
        constants.texdims[i][0] = static_cast<float>((image0 & 0x3FF) + 1);
        constants.texdims[i][1] = static_cast<float>(((image0 >> 10) & 0x3FF) + 1);
        constants.texdims[i][2] = static_cast<float>((bp[0x30 + 2 * i] & 0xFFFF) + 1);
        constants.texdims[i][3] = static_cast<float>((bp[0x31 + 2 * i] & 0xFFFF) + 1);
    }
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t a = bp[0x06 + 3 * i];
        uint32_t b = bp[0x07 + 3 * i];
        uint32_t c = bp[0x08 + 3 * i];
        auto field = [](uint32_t value) {
            int32_t v = static_cast<int32_t>(value & 0x7FF);
            return (v & 0x400) ? v - 0x800 : v;
        };
        int32_t scale = static_cast<int32_t>(((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 3) << 4));
        constants.indmtx[2 * i][0] = field(a);
        constants.indmtx[2 * i][1] = field(b);
        constants.indmtx[2 * i][2] = field(c);
        constants.indmtx[2 * i][3] = 17 - scale;
        constants.indmtx[2 * i + 1][0] = field(a >> 11);
        constants.indmtx[2 * i + 1][1] = field(b >> 11);
        constants.indmtx[2 * i + 1][2] = field(c >> 11);
        constants.indmtx[2 * i + 1][3] = 17 - scale;
    }
    constants.indirect[0] = ((bp[0x00] >> 16) & 7) | ((bp[0x00] & 15) << 8);
    constants.indirect[1] = bp[0x27];
    constants.indirect[2] = bp[0x25];
    constants.indirect[3] = bp[0x26];
    uint32_t stages = ((bp[0x00] >> 10) & 15) + 1;
    for (uint32_t i = 0; i < stages && i < kMaxStages; i++) {
        constants.tevind[i][0] = bp[0x10 + i];
    }
    constants.header[0] = stages;
    constants.header[1] = bp[0xF3];
    constants.header[2] = bp[0x42];
    bool rgba6 = (bp[0x43] & 7) == 1;
    constants.header[3] = (rgba6 ? 1u : 0u) | ((rgba6 && (bp[0x41] & 4) != 0) ? 2u : 0u) |
                          (static_cast<uint32_t>(blend_state(bp[0x41], true, false, true).output) << 2);
    auto fog_float = [](uint32_t value) {
        uint32_t bits = (((value >> 19) & 1) << 31) | (((value >> 11) & 0xFF) << 23) | ((value & 0x7FF) << 12);
        float result;
        std::memcpy(&result, &bits, sizeof result);
        return result;
    };
    uint32_t fog_a = bp[0xEE];
    uint32_t fog_c = bp[0xF1];
    bool nan_case = ((fog_a >> 11) & 0xFF) == 255 && ((fog_c >> 11) & 0xFF) == 255;
    constants.fog[0] = nan_case ? 0.0f : fog_float(fog_a);
    constants.fog[1] = nan_case ? ((!((fog_a >> 19) & 1) && !((fog_c >> 19) & 1)) ? -INFINITY : INFINITY) : fog_float(fog_c);
    constants.fog[2] = 0.0f;
    constants.fog[3] = 1.0f;
    constants.fog_integer[0] = static_cast<int32_t>((fog_c >> 20) & 15);
    constants.fog_integer[1] = static_cast<int32_t>(bp[0xEF] & 0xFFFFFF);
    constants.fog_integer[2] = static_cast<int32_t>(bp[0xF2] & 0xFFFFFF);
    constants.fog_integer[3] = static_cast<int32_t>(bp[0xF0] & 0x1F);
    if (bp[0xE8] & (1u << 10)) {
        const uint32_t* xf = xf_registers();
        float width = 0.0f;
        std::memcpy(&width, &xf[0x101A], sizeof width);
        int center = static_cast<int>(bp[0xE8] & 0x3FF) - 342 + margin_x;
        constants.fog[2] = (center / (2.0f * width)) * 2.0f - 1.0f;
        constants.fog[3] = 2.0f * width * static_cast<float>(scale);
        for (uint32_t i = 0; i < 5; i++) {
            uint32_t k = bp[0xE9 + i];
            uint32_t low = 2 * i;
            uint32_t high = 2 * i + 1;
            constants.fog_range[low / 4][low % 4] = ((k >> 12) & 0xFFF) / 256.0f * 4.0f;
            constants.fog_range[high / 4][high % 4] = (k & 0xFFF) / 256.0f * 4.0f;
        }
        constants.fog_integer[0] |= 16;
    }
    for (uint32_t table = 0; table < 4; table++) {
        uint32_t low = bp[0xF6 + 2 * table];
        uint32_t high = bp[0xF7 + 2 * table];
        constants.swaps[table] = (low & 3) | (((low >> 2) & 3) << 2) | ((high & 3) << 4) | (((high >> 2) & 3) << 6);
    }
    for (uint32_t i = 0; i < stages && i < kMaxStages; i++) {
        uint32_t order_word = bp[0x28 + i / 2];
        uint32_t order = (i & 1) ? (order_word >> 12) : order_word;
        uint32_t map = order & 7;
        uint32_t coord = (order >> 3) & 7;
        uint32_t enable = (order >> 6) & 1;
        uint32_t channel = (order >> 7) & 7;
        uint32_t ksel_word = bp[0xF6 + i / 2];
        uint32_t ksel = (i & 1) ? ((ksel_word >> 14) & 0x3FF) : ((ksel_word >> 4) & 0x3FF);
        constants.stage[i][0] = bp[0xC0 + 2 * i];
        constants.stage[i][1] = bp[0xC1 + 2 * i];
        constants.stage[i][2] = map | (coord << 3) | (enable << 6) | (channel << 7);
        constants.stage[i][3] = (ksel & 31) | (((ksel >> 5) & 31) << 5);
    }
}

AlphaTestResult alpha_test_result(uint32_t test) {
    static uint32_t cached_test = 0xFFFFFFFF;
    static AlphaTestResult cached_result{};
    if (test == cached_test) {
        return cached_result;
    }
    auto compare = [](uint32_t mode, uint32_t value, uint32_t reference) {
        switch (mode) {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value == reference;
        case 3: return value <= reference;
        case 4: return value > reference;
        case 5: return value != reference;
        case 6: return value >= reference;
        default: return true;
        }
    };
    AlphaTestResult result{false, false};
    for (uint32_t alpha = 0; alpha < 256; alpha++) {
        bool first = compare((test >> 16) & 7, alpha, test & 255);
        bool second = compare((test >> 19) & 7, alpha, (test >> 8) & 255);
        uint32_t logic = (test >> 22) & 3;
        bool accepted = logic == 0 ? (first && second) : (logic == 1 ? (first || second) : (logic == 2 ? (first != second) : (first == second)));
        result.can_pass |= accepted;
        result.can_fail |= !accepted;
    }
    cached_test = test;
    cached_result = result;
    return result;
}


uint32_t copy_bits(uint32_t format) {
    switch (format) {
    case 0:
        return 4;
    case 1:
    case 2:
    case 7:
    case 8:
    case 9:
    case 10:
        return 8;
    case 6:
        return 32;
    default:
        return 16;
    }
}

uint32_t copy_texture_format(uint32_t format) {
    static constexpr uint32_t kFormats[13] = {0, 1, 2, 3, 4, 5, 6, 1, 1, 1, 1, 3, 3};
    return kFormats[format];
}

uint32_t expand(uint32_t value, int bits) {
    return (value << (8 - bits)) | (value >> (2 * bits - 8));
}

uint32_t quantize(uint32_t value, int bits) {
    return expand(value >> (8 - bits), bits);
}

void load_tlut(uint32_t address, uint32_t tmem_offset, uint32_t bytes) {
    tmem_offset &= kTlutMask;
    bytes = std::min(bytes, kTlutSize - tmem_offset);
    if (!guest_range_valid(address, bytes)) {
        return;
    }
    std::memcpy(g_tlut + tmem_offset, host(address), bytes);
}

TextureRequest describe_texture(uint32_t map) {
    TextureRequest request;
    const uint32_t* bp = bp_registers();
    uint32_t image0 = bp[map < 4 ? 0x88 + map : 0xA8 + (map - 4)];
    uint32_t image3 = bp[map < 4 ? 0x94 + map : 0xB4 + (map - 4)];
    request.width = (image0 & 0x3FF) + 1;
    request.height = ((image0 >> 10) & 0x3FF) + 1;
    request.format = (image0 >> 20) & 15;
    request.address = 0x80000000u | ((image3 & 0x00FFFFFF) << 5);
    Layout layout = layout_for(request.format);
    request.supported = layout.supported;
    uint32_t mode0 = bp[map < 4 ? 0x80 + map : 0xA0 + (map - 4)];
    uint32_t mode1 = bp[map < 4 ? 0x84 + map : 0xA4 + (map - 4)];
    request.mipmaps = ((mode0 >> 5) & 3) != 0;
    if (request.mipmaps) {
        uint32_t requested = (((mode1 >> 8) & 0xFF) + 15) / 16 + 1;
        uint32_t largest = std::max(request.width, request.height);
        uint32_t available = 1;
        while ((largest >> available) != 0) {
            available++;
        }
        request.levels = std::min({requested, available, 12u});
    }
    auto level_size = [&](uint32_t level) {
        uint32_t level_width = std::max(request.width >> level, 1u);
        uint32_t level_height = std::max(request.height >> level, 1u);
        uint32_t padded_width = (level_width + layout.block_width - 1) / layout.block_width * layout.block_width;
        uint32_t padded_height = (level_height + layout.block_height - 1) / layout.block_height * layout.block_height;
        return static_cast<size_t>(padded_width / layout.block_width) * (padded_height / layout.block_height) * block_bytes(request.format);
    };
    request.size = level_size(0);
    if (!guest_range_valid(request.address, request.size)) {
        return request;
    }
    request.total = request.size;
    for (uint32_t level = 1; level < request.levels; level++) {
        size_t next = request.total + level_size(level);
        if (!guest_range_valid(request.address, next)) {
            request.levels = level;
            break;
        }
        request.offsets[level] = request.total;
        request.total = next;
    }
    request.source = host(request.address);
    request.key = gx::texture_key(image0, image3, request.mipmaps ? request.levels : 0, 0);
    if (request.format >= 8 && request.format <= 10) {
        uint32_t tlut_register = bp[map < 4 ? 0x98 + map : 0xB8 + (map - 4)];
        uint32_t tlut_offset = (tlut_register & 0x3FF) << 9;
        request.tlut_format = (tlut_register >> 10) & 3;
        request.tlut = g_tlut + (tlut_offset & kTlutMask);
        request.entries = request.format == 8 ? 16 : request.format == 9 ? 256 : 16384;
        request.key = gx::texture_key(image0, image3, request.mipmaps ? request.levels : 0, tlut_register);
    }
    request.valid = true;
    return request;
}

uint64_t texture_hash(const TextureRequest& request) {
    uint64_t hash = hash_bytes(request.source, request.total);
    if (request.tlut) {
        hash = (hash ^ hash_bytes(request.tlut, request.entries * 2)) * 1099511628211ull ^ request.tlut_format;
    }
    return hash;
}

std::vector<std::vector<uint32_t>> decode_levels(const TextureRequest& request) {
    std::vector<std::vector<uint32_t>> pixels(request.levels);
    for (uint32_t level = 0; level < request.levels; level++) {
        uint32_t level_width = std::max(request.width >> level, 1u);
        uint32_t level_height = std::max(request.height >> level, 1u);
        pixels[level] = decode_texture(request.source + request.offsets[level], level_width, level_height, request.format, request.tlut, request.tlut_format);
    }
    return pixels;
}

SamplerParams sampler_params(uint32_t map, bool custom) {
    const uint32_t* bp = bp_registers();
    uint32_t mode = bp[map < 4 ? 0x80 + map : 0xA0 + (map - 4)];
    uint32_t lod = bp[map < 4 ? 0x84 + map : 0xA4 + (map - 4)];
    SamplerParams params;
    params.key = (mode & 0x3FFFFF) | (static_cast<uint64_t>(lod & 0xFFFF) << 32) | (static_cast<uint64_t>(custom) << 48);
    params.wrap_s = mode & 3;
    params.wrap_t = (mode >> 2) & 3;
    params.mag_linear = ((mode >> 4) & 1) != 0;
    params.mip_mode = (mode >> 5) & 3;
    params.min_linear = ((mode >> 7) & 1) != 0;
    if (params.mip_mode != 0) {
        uint32_t max_lod = (lod >> 8) & 0xFF;
        uint32_t min_lod = std::min(lod & 0xFF, max_lod);
        params.min_lod = static_cast<float>(min_lod) / 16.0f;
        params.max_lod = static_cast<float>(max_lod) / 16.0f;
        params.unlimited_lod = custom;
        params.lod_bias = static_cast<float>(static_cast<int8_t>((mode >> 9) & 0xFF)) / 32.0f;
    }
    return params;
}

}

namespace wp::gx {

uint64_t texture_key(uint32_t image0, uint32_t image3, uint32_t mip_levels, uint32_t tlut_register) {
    // Each field in its own bits, so two different textures never share a cache entry: address 24 bits, width and height
    // 10 each, format 4, mip levels 4 (0 without mipmaps), palette offset 10 and palette format 2.
    return (image3 & 0x00FFFFFF) | (static_cast<uint64_t>(image0 & 0xFFFFFF) << 24) | (static_cast<uint64_t>(mip_levels & 15) << 48) |
           (static_cast<uint64_t>(tlut_register & 0xFFF) << 52);
}

bool fit_motion(const float (*from)[4], const float (*to)[4], size_t count, float matrix[4][4]) {
    constexpr size_t kMinimum = 12;
    if (count < kMinimum) {
        return false;
    }
    double scale[4] = {};
    for (size_t i = 0; i < count; i++) {
        for (int j = 0; j < 4; j++) {
            scale[j] += static_cast<double>(from[i][j]) * from[i][j];
        }
    }
    for (double& s : scale) {
        s = s > 0.0 ? 1.0 / std::sqrt(s / static_cast<double>(count)) : 1.0;
    }
    std::vector<uint8_t> used(count, 1);
    double solved[4][4] = {};
    constexpr int kRounds = 5;
    constexpr double kLimits[kRounds] = {0.0, 0.05, 0.01, 0.0, 0.0};
    size_t kept = count;
    std::vector<double> error(count);
    std::vector<double> sorted;
    for (int round = 0; round < kRounds; round++) {
        if (round > 0) {
            for (size_t i = 0; i < count; i++) {
                double out[4];
                for (int r = 0; r < 4; r++) {
                    out[r] = 0.0;
                    for (int c = 0; c < 4; c++) {
                        out[r] += solved[r][c] * from[i][c];
                    }
                }
                double w = std::max(std::fabs(static_cast<double>(to[i][3])), 1e-6);
                error[i] = std::max({std::fabs(out[0] - to[i][0]), std::fabs(out[1] - to[i][1]), std::fabs(out[3] - to[i][3])}) / w;
            }
            double limit = kLimits[round];
            if (round == 1) {
                sorted = error;
                std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count / 2), sorted.end());
                limit = std::max(limit, 3.0 * sorted[count / 2]);
            } else if (round >= 3) {
                sorted.clear();
                for (size_t i = 0; i < count; i++) {
                    if (used[i]) {
                        sorted.push_back(error[i]);
                    }
                }
                std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
                limit = std::min(0.01, std::max(1e-4, 4.0 * sorted[sorted.size() / 2]));
            }
            kept = 0;
            for (size_t i = 0; i < count; i++) {
                used[i] = error[i] <= limit;
                kept += used[i];
            }
            if (kept < kMinimum || kept * 10 < count * 6) {
                if (round >= 3) {
                    break;
                }
                return false;
            }
        }
        double system[4][8] = {};
        for (size_t i = 0; i < count; i++) {
            if (!used[i]) {
                continue;
            }
            double f[4];
            for (int j = 0; j < 4; j++) {
                f[j] = from[i][j] * scale[j];
            }
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    system[r][c] += f[r] * f[c];
                    system[r][4 + c] += f[r] * (static_cast<double>(to[i][c]) - from[i][c]);
                }
            }
        }
        double trace = system[0][0] + system[1][1] + system[2][2] + system[3][3];
        for (int j = 0; j < 4; j++) {
            system[j][j] += trace * 1e-12 + 1e-30;
        }
        for (int column = 0; column < 4; column++) {
            int pivot = column;
            for (int r = column + 1; r < 4; r++) {
                if (std::fabs(system[r][column]) > std::fabs(system[pivot][column])) {
                    pivot = r;
                }
            }
            if (std::fabs(system[pivot][column]) < 1e-300) {
                return false;
            }
            std::swap_ranges(system[column], system[column] + 8, system[pivot]);
            for (int r = 0; r < 4; r++) {
                if (r == column) {
                    continue;
                }
                double factor = system[r][column] / system[column][column];
                for (int c = column; c < 8; c++) {
                    system[r][c] -= factor * system[column][c];
                }
            }
        }
        for (int r = 0; r < 4; r++) {
            for (int j = 0; j < 4; j++) {
                solved[r][j] = system[j][4 + r] / system[j][j] * scale[j] + (r == j ? 1.0 : 0.0);
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            matrix[r][c] = static_cast<float>(solved[r][c]);
            if (!std::isfinite(matrix[r][c])) {
                return false;
            }
        }
    }
    return true;
}

namespace render {

namespace {

constexpr double kWidePicture = 16.0 / 9.0;
constexpr double kNarrowestCanvas = 4.0 / 3.0;
constexpr double kWidestCanvas = 32.0 / 9.0;
constexpr double kUltrawide = 64.0 / 27.0;

double chosen_aspect(double window_aspect) {
    static uint32_t seen = ~0u;
    static std::string choice;
    if (settings::revision() != seen) {
        seen = settings::revision();
        choice = settings::text("video.aspect", "WP_ASPECT");
    }
    if (choice == "21:9") {
        return kUltrawide;
    }
    if (choice == "auto") {
        return window_aspect > 0.0 ? std::clamp(window_aspect, kNarrowestCanvas, kWidestCanvas) : kWidePicture;
    }
    size_t colon = choice.find(':');
    double width = std::atof(choice.c_str());
    double height = colon == std::string::npos ? 0.0 : std::atof(choice.c_str() + colon + 1);
    if (width <= 0.0 || height <= 0.0) {
        return kWidePicture;
    }
    return std::clamp(width / height, kNarrowestCanvas, kWidestCanvas);
}

}

Canvas wanted_canvas(double picture_aspect, double window_aspect) {
    Canvas canvas;
    if (picture_aspect < kWidePicture - 0.01) {
        return canvas;
    }
    double aspect = chosen_aspect(window_aspect);
    if (aspect > kWidePicture + 0.01) {
        canvas.margin_x = static_cast<int>(std::lround(kEfbWidth / 2.0 * (aspect / kWidePicture - 1.0)));
    } else if (aspect < kWidePicture - 0.01) {
        canvas.margin_y = static_cast<int>(std::lround(kPictureHeight / 2.0 * (kWidePicture / aspect - 1.0)));
    }
    return canvas;
}

bool covers_picture(int x, int y, int width, int height) {
    return x <= 1 && y <= 1 && x + width >= kEfbWidth - 1 && y + height >= kPictureHeight - 1;
}

}

}
