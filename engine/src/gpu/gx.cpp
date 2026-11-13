#include "wp/gx.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <map>
#include <string>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "gx_render_common.h"
#include "wp/gx_lighting.h"
#include "wp/gx_render.h"
#include "wp/gx_state.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/perf.h"
#include "wp/profile.h"
#include "wp/settings.h"
#include "wp/video.h"

#ifdef _WIN32
#define PSAPI_VERSION 2
#include <psapi.h>
#endif

namespace wp::gx {

namespace {

constexpr size_t kFlushThreshold = 1u << 20;
constexpr size_t kMaxQueuedBytes = size_t{64} << 20;
std::atomic<size_t> g_queued_bytes{0};
std::atomic<bool> g_screen_hidden{false};

void publish_performance(double fps, double game_fps, double seconds, double game_thread, double gpu_thread) {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    static const settings::LiveFlag vsync("video.vsync", nullptr);
    perf::Sample sample;
    sample.fps = fps;
    sample.frame_ms = fps > 0.0 ? 1000.0 / fps : -1.0;
    sample.game_fps = game_fps;
    uint32_t late = render::take_late_frames();
    sample.late_per_second = render::interpolating() ? late / seconds : -1.0;
    sample.game_thread_percent = game_thread;
    sample.gpu_thread_percent = gpu_thread;
    perf::SystemFigures system = perf::system_figures();
    sample.cpu_percent = system.cpu_percent;
    sample.gpu_percent = system.gpu_percent;
    sample.gpu_ms = system.gpu_process_percent >= 0.0 && fps > 0.0 ? system.gpu_process_percent * 10.0 / fps : -1.0;
    sample.ram_used_gb = system.ram_used_gb;
    sample.ram_total_gb = system.ram_total_gb;
    render::GpuFigures gpu = render::gpu_figures();
    sample.vram_used_gb = gpu.vram_used_gb;
    sample.vram_total_gb = gpu.vram_total_gb;
    sample.gpu_name = gpu.adapter;
    sample.render_width = gpu.width;
    sample.render_height = gpu.height;
    sample.refresh_hz = gpu.refresh_hz;
    sample.cpu_name = perf::cpu_name();
    sample.api = render::api_name();
    sample.scale = scale();
    sample.vsync = vsync();
    perf::publish(sample);
}

size_t private_memory_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters)) {
        return counters.PrivateUsage >> 20;
    }
    return 0;
#else
    std::FILE* file = std::fopen("/proc/self/statm", "r");
    unsigned long long pages = 0;
    unsigned long long resident = 0;
    if (file) {
        if (std::fscanf(file, "%llu %llu", &pages, &resident) != 2) {
            resident = 0;
        }
        std::fclose(file);
    }
    return static_cast<size_t>(resident * 4096ull >> 20);
#endif
}
constexpr uint8_t kCommandNop = 0x00;
constexpr uint8_t kCommandLoadCp = 0x08;
constexpr uint8_t kCommandLoadXf = 0x10;
constexpr uint8_t kCommandCallList = 0x40;
constexpr uint8_t kCommandMetrics = 0x44;
constexpr uint8_t kCommandInvalidate = 0x48;
constexpr uint8_t kCommandLoadBp = 0x61;
constexpr uint8_t kCommandDrawMask = 0x80;
constexpr uint32_t kBpMask = 0xFE;
constexpr uint32_t kBpCopyExecute = 0x52;
constexpr uint32_t kBpLoadTlut = 0x65;
constexpr uint32_t kBpInvalidateTexture = 0x66;
constexpr uint32_t kCpFifoBaseLow = 0xCC000020;
constexpr uint32_t kCpFifoBaseHigh = 0xCC000022;
constexpr uint32_t kPiFifoBase = 0xCC00300C;
constexpr uint32_t kPiFifoEnd = 0xCC003010;
constexpr uint32_t kPiFifoWritePointer = 0xCC003014;
constexpr uint32_t kCpControl = 0xCC000002;
constexpr uint32_t kPeControl = 0xCC00100A;
constexpr uint16_t kPeTokenEnable = 0x0001;
constexpr uint16_t kPeFinishEnable = 0x0002;
constexpr uint16_t kPeTokenClear = 0x0004;
constexpr uint16_t kPeFinishClear = 0x0008;
constexpr uint32_t kPeToken = 0xCC00100E;
constexpr uint32_t kCpFifoEndLow = 0xCC000024;
constexpr uint32_t kCpDistanceLow = 0xCC000030;
constexpr uint32_t kCpWritePointerLow = 0xCC000034;
constexpr uint32_t kCpReadPointerLow = 0xCC000038;
constexpr uint16_t kCpReadEnable = 0x0001;
constexpr uint16_t kCpLinkEnable = 0x0010;
constexpr unsigned kGatherPipeSize = 32;
constexpr uint32_t kFifoPointerMask = 0x03FFFFE0;
constexpr uint32_t kFifoPhysicalMask = 0x03FFFFFF;
constexpr uint32_t kCopyToFramebuffer = 1u << 14;
constexpr uint32_t kBpDrawDone = 0x45;
constexpr uint32_t kBpToken = 0x47;
constexpr uint32_t kBpTokenInterrupt = 0x48;
constexpr uint32_t kCopyClear = 1u << 11;
constexpr uint32_t kPixelFormatRgba6Z24 = 1;
constexpr uint32_t kPixelFormatZ24 = 3;
constexpr uint32_t kXfSize = 0x1100;
constexpr uint32_t kArrayCount = 16;
constexpr uint32_t kRamBase = 0x80000000;
constexpr uint32_t kArrayAddressMask = 0x1FFFFFFF;
constexpr float kViewportOffset = 342.0f;
constexpr float kEfbWidth = 640.0f;
constexpr float kEfbHeight = 528.0f;
constexpr float kDepthRange = 16777215.0f;
constexpr int kMaxLoggedCopies = 60;
constexpr uint32_t kTexCoordCount = 8;
constexpr size_t kArrayReadMargin = 256;
constexpr double kSlowFrameMs = 40.0;

enum Attribute : uint32_t { kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };

struct Statistics {
    uint32_t draws = 0;
    uint32_t vertices = 0;
    uint32_t bp_writes = 0;
    uint32_t cp_writes = 0;
    uint32_t xf_words = 0;
    uint32_t lists = 0;
    uint32_t unknown = 0;
};

struct Element {
    uint32_t mode = kNone;
    uint32_t components = 0;
    uint32_t format = 0;
    uint32_t shift = 0;
    uint32_t direct_size = 0;
};

struct Layout {
    bool position_matrix = false;
    uint32_t texture_matrices = 0;
    Element position;
    Element normal;
    Element color[2];
    Element texture[kTexCoordCount];
    uint32_t size = 0;
};

struct Vertex {
    float position[3];
    float normal[3];
    uint8_t color[2][4];
    float texture[kTexCoordCount][2];
    uint32_t position_matrix;
    uint32_t texture_matrix[kTexCoordCount];
};

std::vector<uint8_t> g_fifo;
uint32_t g_vcd_low = 0;
uint32_t g_vcd_high = 0;
uint32_t g_vat[3][8] = {};
uint32_t g_array_base[kArrayCount] = {};
uint32_t g_array_stride[kArrayCount] = {};
uint32_t g_bp[256] = {};
uint32_t g_konst[8] = {};
uint32_t g_bp_mask = 0xFFFFFF;
uint32_t g_xf[kXfSize] = {};
Statistics g_stats;
int g_logged_copies = 0;
int g_copy_total = 0;
std::atomic<uint64_t> g_frames_drawn{0};
int g_log_from = std::getenv("WP_LOG_FROM") ? std::atoi(std::getenv("WP_LOG_FROM")) : 0;
const char* g_log_level = std::getenv("WP_LOG_GX");
bool g_log = g_log_level != nullptr;
bool g_seen_register[3][0x1100] = {};

void note_register(int kind, uint32_t index, uint32_t value) {
    if (g_log && index < 0x1100 && !g_seen_register[kind][index]) {
        g_seen_register[kind][index] = true;
        log::write("gx", "register %s %04x first value %08x", kind == 0 ? "bp" : (kind == 1 ? "cp" : "xf"), index, value);
    }
}

bool g_log_draws = g_log_level != nullptr && g_log_level[0] == '2';
int g_logged_draws = 0;
int g_logged_prepared = 0;
bool g_render_enabled = std::getenv("WP_NO_RENDER") == nullptr;
bool g_log_fps = std::getenv("WP_LOG_FPS") != nullptr;
int g_frames = 0;
std::chrono::steady_clock::time_point g_fps_start = std::chrono::steady_clock::now();
std::atomic<bool> g_capture_requested{false};
FILE* g_capture = nullptr;
int g_capture_index = 0;
int g_capture_draws = 0;
bool g_capture_first = true;
uint32_t g_captured_bp[256] = {};
uint32_t g_captured_konst[8] = {};
uint32_t g_captured_xf[kXfSize] = {};
char g_capture_name[64] = {};

uint32_t be32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint32_t be16(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 8) | data[1];
}

float bits_to_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

float xf_float(uint32_t index) {
    return bits_to_float(g_xf[index]);
}

uint32_t component_size(uint32_t format) {
    switch (format) {
    case 0:
    case 1:
        return 1;
    case 2:
    case 3:
        return 2;
    default:
        return 4;
    }
}

uint32_t color_size(uint32_t format) {
    static const uint32_t kSizes[] = {2, 3, 4, 2, 3, 4, 4, 4};
    return kSizes[format & 7];
}

uint32_t element_size(const Element& element) {
    switch (element.mode) {
    case kDirect:
        return element.direct_size;
    case kIndex8:
        return 1;
    case kIndex16:
        return 2;
    default:
        return 0;
    }
}

Layout build_layout(uint32_t vat) {
    Layout layout;
    uint32_t a = g_vat[0][vat];
    uint32_t b = g_vat[1][vat];
    uint32_t c = g_vat[2][vat];
    layout.position_matrix = (g_vcd_low & 1) != 0;
    layout.texture_matrices = (g_vcd_low >> 1) & 0xFF;
    layout.position.mode = (g_vcd_low >> 9) & 3;
    layout.position.components = (a & 1) ? 3 : 2;
    layout.position.format = (a >> 1) & 7;
    layout.position.shift = (a >> 4) & 31;
    layout.position.direct_size = layout.position.components * component_size(layout.position.format);
    layout.normal.mode = (g_vcd_low >> 11) & 3;
    layout.normal.components = (a & 0x200) ? 9 : 3;
    layout.normal.format = (a >> 10) & 7;
    layout.normal.direct_size = layout.normal.components * component_size(layout.normal.format);
    layout.color[0].mode = (g_vcd_low >> 13) & 3;
    layout.color[0].components = ((a >> 13) & 1) ? 4 : 3;
    layout.color[0].format = (a >> 14) & 7;
    layout.color[0].direct_size = color_size(layout.color[0].format);
    layout.color[1].mode = (g_vcd_low >> 15) & 3;
    layout.color[1].components = ((a >> 17) & 1) ? 4 : 3;
    layout.color[1].format = (a >> 18) & 7;
    layout.color[1].direct_size = color_size(layout.color[1].format);
    uint32_t counts[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    uint32_t formats[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    uint32_t shifts[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (uint32_t i = 0; i < kTexCoordCount; i++) {
        Element& element = layout.texture[i];
        element.mode = (g_vcd_high >> (2 * i)) & 3;
        element.components = counts[i] + 1;
        element.format = formats[i];
        element.shift = shifts[i];
        element.direct_size = element.components * component_size(element.format);
    }
    uint32_t size = layout.position_matrix ? 1 : 0;
    size += __builtin_popcount(layout.texture_matrices);
    size += element_size(layout.position);
    size += element_size(layout.normal);
    size += element_size(layout.color[0]);
    size += element_size(layout.color[1]);
    for (const Element& element : layout.texture) {
        size += element_size(element);
    }
    layout.size = size;
    return layout;
}

const std::array<float, 32> kShiftScale = [] {
    std::array<float, 32> scale{};
    for (int i = 0; i < 32; i++) {
        scale[i] = std::ldexp(1.0f, -i);
    }
    return scale;
}();

struct CachedLayout {
    uint32_t key[5] = {};
    bool valid = false;
    Layout layout;
};

const Layout& make_layout(uint32_t vat) {
    static CachedLayout cache[8];
    CachedLayout& entry = cache[vat];
    uint32_t key[5] = {g_vat[0][vat], g_vat[1][vat], g_vat[2][vat], g_vcd_low, g_vcd_high};
    if (!entry.valid || std::memcmp(entry.key, key, sizeof key) != 0) {
        std::memcpy(entry.key, key, sizeof key);
        entry.layout = build_layout(vat);
        entry.valid = true;
    }
    return entry.layout;
}

float read_number(const uint8_t* p, uint32_t format, uint32_t shift) {
    float value;
    switch (format) {
    case 0:
        value = static_cast<float>(p[0]);
        break;
    case 1:
        value = static_cast<float>(static_cast<int8_t>(p[0]));
        break;
    case 2:
        value = static_cast<float>(be16(p));
        break;
    case 3:
        value = static_cast<float>(static_cast<int16_t>(be16(p)));
        break;
    default:
        return bits_to_float(be32(p));
    }
    return value * kShiftScale[shift];
}

float read_normal_component(const uint8_t* p, uint32_t format) {
    switch (format) {
    case 0:
        return static_cast<float>(p[0]) * kShiftScale[7];
    case 1:
        return static_cast<float>(static_cast<int8_t>(p[0])) * kShiftScale[6];
    case 2:
        return static_cast<float>(be16(p)) * kShiftScale[15];
    case 3:
        return static_cast<float>(static_cast<int16_t>(be16(p))) * kShiftScale[14];
    default:
        return bits_to_float(be32(p));
    }
}

uint8_t color_byte(float value) {
    return static_cast<uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

template <int Bits>
const std::array<uint8_t, 1 << Bits> kExpand = [] {
    std::array<uint8_t, 1 << Bits> table{};
    for (int i = 0; i < (1 << Bits); i++) {
        table[i] = color_byte(i / static_cast<float>((1 << Bits) - 1));
    }
    return table;
}();

const std::array<float, 256> kUnit = [] {
    std::array<float, 256> table{};
    for (int i = 0; i < 256; i++) {
        table[i] = static_cast<float>(i) / 255.0f;
    }
    return table;
}();

void read_color(const uint8_t* p, uint32_t format, uint8_t* out) {
    switch (format) {
    case 0: {
        uint32_t v = be16(p);
        out[0] = kExpand<5>[(v >> 11) & 31];
        out[1] = kExpand<6>[(v >> 5) & 63];
        out[2] = kExpand<5>[v & 31];
        out[3] = 255;
        break;
    }
    case 1:
    case 2:
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = 255;
        break;
    case 3: {
        uint32_t v = be16(p);
        out[0] = kExpand<4>[(v >> 12) & 15];
        out[1] = kExpand<4>[(v >> 8) & 15];
        out[2] = kExpand<4>[(v >> 4) & 15];
        out[3] = kExpand<4>[v & 15];
        break;
    }
    case 4: {
        uint32_t v = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
        out[0] = kExpand<6>[(v >> 18) & 63];
        out[1] = kExpand<6>[(v >> 12) & 63];
        out[2] = kExpand<6>[(v >> 6) & 63];
        out[3] = kExpand<6>[v & 63];
        break;
    }
    default:
        std::memcpy(out, p, 4);
        break;
    }
}

struct ArrayState {
    uint32_t address;
    size_t stride;
    const uint8_t* base;
};

ArrayState g_arrays[kArrayCount];
uint32_t g_default_texture_matrix[kTexCoordCount];

void begin_vertex_arrays() {
    for (uint32_t i = 0; i < kArrayCount; i++) {
        uint32_t address = kRamBase | (g_array_base[i] & kArrayAddressMask);
        g_arrays[i].address = address;
        g_arrays[i].stride = g_array_stride[i];
        g_arrays[i].base = host(address);
    }
    for (uint32_t i = 0; i < kTexCoordCount; i++) {
        g_default_texture_matrix[i] = i < 4 ? (g_xf[0x1018] >> (6 + 6 * i)) & 0x3F : (g_xf[0x1019] >> (6 * (i - 4))) & 0x3F;
    }
}

const uint8_t* array_element_live(uint32_t array, uint32_t index) {
    uint32_t address = kRamBase | (g_array_base[array] & kArrayAddressMask);
    size_t offset = static_cast<size_t>(index) * g_array_stride[array];
    if (!render::guest_range_valid(address + offset, kArrayReadMargin)) {
        static const uint8_t zeros[kArrayReadMargin] = {};
        return zeros;
    }
    return host(address) + offset;
}

const uint8_t* array_element(uint32_t array, uint32_t index) {
    const ArrayState& state = g_arrays[array];
    size_t offset = static_cast<size_t>(index) * state.stride;
    if (static_cast<size_t>((state.address + offset) & kAddressMask) + kArrayReadMargin > kPhysicalSize) {
        static const uint8_t zeros[kArrayReadMargin] = {};
        return zeros;
    }
    return state.base + offset;
}

const uint8_t* fetch(const Element& element, uint32_t array, const uint8_t*& stream) {
    if (element.mode == kDirect) {
        const uint8_t* p = stream;
        stream += element.direct_size;
        return p;
    }
    uint32_t index;
    if (element.mode == kIndex8) {
        index = stream[0];
        stream += 1;
    } else {
        index = be16(stream);
        stream += 2;
    }
    return array_element(array, index);
}

void decode_vertex(const Layout& layout, const uint8_t*& stream, Vertex& vertex) {
    if (layout.position_matrix) {
        vertex.position_matrix = stream[0] & 0x3F;
        stream++;
    }
    for (uint32_t i = 0; i < kTexCoordCount; i++) {
        if (layout.texture_matrices & (1u << i)) {
            vertex.texture_matrix[i] = stream[0] & 0x3F;
            stream++;
        }
    }
    if (layout.position.mode != kNone) {
        const uint8_t* p = fetch(layout.position, 0, stream);
        uint32_t step = component_size(layout.position.format);
        for (uint32_t i = 0; i < layout.position.components; i++) {
            vertex.position[i] = read_number(p + i * step, layout.position.format, layout.position.shift);
        }
    }
    if (layout.normal.mode != kNone) {
        const uint8_t* p = fetch(layout.normal, 1, stream);
        uint32_t step = component_size(layout.normal.format);
        vertex.normal[0] = read_normal_component(p, layout.normal.format);
        vertex.normal[1] = read_normal_component(p + step, layout.normal.format);
        vertex.normal[2] = read_normal_component(p + 2 * step, layout.normal.format);
    }
    for (uint32_t k = 0; k < 2; k++) {
        if (layout.color[k].mode != kNone) {
            read_color(fetch(layout.color[k], 2 + k, stream), layout.color[k].format, vertex.color[k]);
        }
    }
    for (uint32_t k = 0; k < kTexCoordCount; k++) {
        const Element& element = layout.texture[k];
        if (element.mode != kNone) {
            const uint8_t* p = fetch(element, 4 + k, stream);
            uint32_t step = component_size(element.format);
            for (uint32_t i = 0; i < element.components && i < 2; i++) {
                vertex.texture[k][i] = read_number(p + i * step, element.format, element.shift);
            }
        }
    }
}

void begin_vertex(Vertex& vertex) {
    std::memset(&vertex, 0, sizeof vertex);
    vertex.normal[2] = 1.0f;
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t material = g_xf[0x100C + k];
        for (uint32_t i = 0; i < 4; i++) {
            vertex.color[k][i] = static_cast<uint8_t>(material >> (24 - 8 * i));
        }
    }
    vertex.position_matrix = g_xf[0x1018] & 0x3F;
    std::memcpy(vertex.texture_matrix, g_default_texture_matrix, sizeof vertex.texture_matrix);
}


struct ColorState {
    bool lit;
    bool from_vertex[2][2];
    uint8_t material[2][4];
};

ColorState g_colors;

void begin_colors() {
    g_colors.lit = ((g_xf[0x100E] | g_xf[0x100F] | g_xf[0x1010] | g_xf[0x1011]) & 2) != 0;
    for (uint32_t k = 0; k < 2; k++) {
        g_colors.from_vertex[k][0] = (g_xf[0x100E + k] & 1) != 0;
        g_colors.from_vertex[k][1] = (g_xf[0x1010 + k] & 1) != 0;
        for (uint32_t i = 0; i < 4; i++) {
            g_colors.material[k][i] = static_cast<uint8_t>(g_xf[0x100C + k] >> (24 - 8 * i));
        }
    }
}

void color_bytes(const Vertex& vertex, const float* eye, uint8_t result[2][4]) {
    if (g_colors.lit) {
        float normal[3];
        lighting::transform_normal(g_xf, vertex.position_matrix, vertex.normal, normal);
        lighting::light_channels(eye, normal, vertex.color, result);
    } else {
        for (uint32_t k = 0; k < 2; k++) {
            const uint8_t* color = g_colors.from_vertex[k][0] ? vertex.color[k] : g_colors.material[k];
            result[k][0] = color[0];
            result[k][1] = color[1];
            result[k][2] = color[2];
            result[k][3] = g_colors.from_vertex[k][1] ? vertex.color[k][3] : g_colors.material[k][3];
        }
    }
}

void rasterize_colors(const Vertex& vertex, const float* eye, float out[2][4]) {
    uint8_t result[2][4];
    color_bytes(vertex, eye, result);
    for (uint32_t k = 0; k < 2; k++) {
        for (uint32_t i = 0; i < 4; i++) {
            out[k][i] = kUnit[result[k][i]];
        }
    }
}


void transform_position(const Vertex& vertex, float* eye) {
    uint32_t base = vertex.position_matrix * 4;
    const float x = vertex.position[0];
    const float y = vertex.position[1];
    const float z = vertex.position[2];
    for (uint32_t row = 0; row < 3; row++) {
        eye[row] = xf_float(base + row * 4) * x + xf_float(base + row * 4 + 1) * y + xf_float(base + row * 4 + 2) * z +
                   xf_float(base + row * 4 + 3);
    }
}

struct TexGenUnit {
    uint32_t source;
    bool projected;
    bool abc1;
    uint32_t post;
};

struct TexGenState {
    uint32_t count;
    bool dual;
    TexGenUnit unit[kTexCoordCount];
};

TexGenState g_texgen;

void begin_texture_coordinates() {
    g_texgen.count = std::min<uint32_t>(g_bp[0x00] & 15, kTexCoordCount);
    g_texgen.dual = (g_xf[0x1012] & 1) != 0;
    for (uint32_t i = 0; i < g_texgen.count; i++) {
        uint32_t info = g_xf[0x1040 + i];
        g_texgen.unit[i].source = (info >> 7) & 31;
        g_texgen.unit[i].projected = ((info >> 1) & 1) != 0;
        g_texgen.unit[i].abc1 = ((info >> 2) & 1) != 0;
        g_texgen.unit[i].post = g_xf[0x1050 + i];
    }
}

void generate_texture_coordinates(const Vertex& vertex, float out[8][3]) {
    const bool dual = g_texgen.dual;
    for (uint32_t i = 0; i < g_texgen.count; i++) {
        const TexGenUnit& unit = g_texgen.unit[i];
        uint32_t source = unit.source;
        float input[3] = {0, 0, 1};
        if (source == 0) {
            input[0] = vertex.position[0];
            input[1] = vertex.position[1];
            input[2] = vertex.position[2];
        } else if (source == 1) {
            input[0] = vertex.normal[0];
            input[1] = vertex.normal[1];
            input[2] = vertex.normal[2];
        } else if (source >= 5 && source < 13) {
            input[0] = vertex.texture[source - 5][0];
            input[1] = vertex.texture[source - 5][1];
        }
        if (!unit.abc1) {
            input[2] = 1.0f;
        }
        uint32_t base = vertex.texture_matrix[i] * 4;
        float coord[3];
        for (uint32_t row = 0; row < 3; row++) {
            uint32_t r = base + row * 4;
            coord[row] = xf_float(r) * input[0] + xf_float(r + 1) * input[1] + xf_float(r + 2) * input[2] + xf_float(r + 3);
        }
        if (!unit.projected) {
            coord[2] = 1.0f;
        }
        if (dual) {
            uint32_t post = unit.post;
            if ((post >> 8) & 1) {
                float length = std::sqrt(coord[0] * coord[0] + coord[1] * coord[1] + coord[2] * coord[2]);
                if (length > 0.0f) {
                    coord[0] /= length;
                    coord[1] /= length;
                    coord[2] /= length;
                }
            }
            uint32_t post_base = 0x500 + (post & 0x3F) * 4;
            float result[3];
            for (uint32_t row = 0; row < 3; row++) {
                uint32_t r = post_base + row * 4;
                result[row] = xf_float(r) * coord[0] + xf_float(r + 1) * coord[1] + xf_float(r + 2) * coord[2] + xf_float(r + 3);
            }
            coord[0] = result[0];
            coord[1] = result[1];
            coord[2] = result[2];
        }
        if (coord[2] == 0.0f) {
            coord[0] = std::clamp(coord[0] / 2.0f, -1.0f, 1.0f);
            coord[1] = std::clamp(coord[1] / 2.0f, -1.0f, 1.0f);
            coord[2] = 1.0f;
        }
        out[i][0] = coord[0];
        out[i][1] = coord[1];
        out[i][2] = coord[2];
    }
}

struct ProjectionState {
    float a, b, c, d, e, f;
    bool orthographic;
    float x_scale, x_offset, y_scale, y_offset, z_scale, z_offset;
};

ProjectionState g_projection;

void begin_projection() {
    g_projection.a = xf_float(0x1020);
    g_projection.b = xf_float(0x1021);
    g_projection.c = xf_float(0x1022);
    g_projection.d = xf_float(0x1023);
    g_projection.e = xf_float(0x1024);
    g_projection.f = xf_float(0x1025);
    g_projection.orthographic = g_xf[0x1026] != 0;
    g_projection.x_scale = 2.0f * xf_float(0x101A) / kEfbWidth;
    g_projection.x_offset = 2.0f * (xf_float(0x101D) - kViewportOffset) / kEfbWidth - 1.0f;
    g_projection.y_scale = -2.0f * xf_float(0x101B) / kEfbHeight;
    g_projection.y_offset = 1.0f - 2.0f * (xf_float(0x101E) - kViewportOffset) / kEfbHeight;
    g_projection.z_scale = xf_float(0x101C);
    g_projection.z_offset = xf_float(0x101F);
}

bool project(const float* eye, float* screen) {
    const ProjectionState& p = g_projection;
    float x, y, z, w;
    if (p.orthographic) {
        x = p.a * eye[0] + p.b;
        y = p.c * eye[1] + p.d;
        z = p.e * eye[2] + p.f;
        w = 1.0f;
    } else {
        x = p.a * eye[0] + p.b * eye[2];
        y = p.c * eye[1] + p.d * eye[2];
        z = p.e * eye[2] + p.f;
        w = -eye[2];
    }
    screen[0] = x * p.x_scale + w * p.x_offset;
    screen[1] = y * p.y_scale + w * p.y_offset;
#ifdef _WIN32
    screen[2] = (w * (kDepthRange - p.z_offset) - z * p.z_scale) / kDepthRange;
#else
    screen[2] = (z * p.z_scale + w * p.z_offset) / kDepthRange;
#endif
    screen[3] = w;
    return std::isfinite(screen[0]) && std::isfinite(screen[1]) && std::isfinite(screen[2]) && std::isfinite(w);
}

struct Prepared {
    ScreenVertex vertex;
    bool valid;
};

void prepare(const Vertex& vertex, Prepared& prepared) {
    std::memset(&prepared.vertex, 0, sizeof prepared.vertex);
    float eye[3];
    transform_position(vertex, eye);
    float screen[4] = {0, 0, 0, 1};
    prepared.valid = project(eye, screen);
    prepared.vertex.x = screen[0];
    prepared.vertex.y = screen[1];
    prepared.vertex.z = screen[2];
    prepared.vertex.w = screen[3];
    rasterize_colors(vertex, eye, prepared.vertex.color);
    generate_texture_coordinates(vertex, prepared.vertex.uv);
}


bool culled(const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    uint32_t mode = (g_bp[0x00] >> 14) & 3;
    if (mode == 0) {
        return false;
    }
    if (mode == 3) {
        return true;
    }
    const float pa[3] = {a.x, a.y, a.w};
    const float pb[3] = {b.x, b.y, b.w};
    const float pc[3] = {c.x, c.y, c.w};
    bool front = facing_area(pa, pb, pc) < 0.0;
    return mode == 1 ? !front : front;
}

bool g_recording = false;
bool g_in_list = false;

struct alignas(64) DrawOutput {
    std::vector<ScreenVertex> triangles;
    std::vector<uint16_t> source_of;
    std::vector<std::array<float, 4>> positions;
    render::DrawSource source;
    const Prepared* prepared = nullptr;
    bool recorded = false;
};

void emit_triangle(DrawOutput& out, const Prepared& a, const Prepared& b, const Prepared& c) {
    if (!a.valid || !b.valid || !c.valid || culled(a.vertex, b.vertex, c.vertex)) {
        return;
    }
    out.triangles.push_back(a.vertex);
    out.triangles.push_back(b.vertex);
    out.triangles.push_back(c.vertex);
    if (g_recording) {
        out.source_of.push_back(static_cast<uint16_t>(&a - out.prepared));
        out.source_of.push_back(static_cast<uint16_t>(&b - out.prepared));
        out.source_of.push_back(static_cast<uint16_t>(&c - out.prepared));
    }
}

uint64_t draw_key(uint8_t command, uint32_t count, const Layout& layout, const uint8_t* data) {
    uint32_t vat = command & 7;
    uint64_t words[8] = {command | (static_cast<uint64_t>(count) << 8), g_vcd_low | (static_cast<uint64_t>(g_vcd_high) << 32), g_vat[0][vat],
                         g_vat[1][vat] | (static_cast<uint64_t>(g_vat[2][vat]) << 32), g_bp[0x94], g_bp[0x88], 0, 0};
    uint64_t hash = 0x9E3779B97F4A7C15ull;
    auto mix = [&hash](uint64_t value) {
        hash = (hash ^ value) * 0xFF51AFD7ED558CCDull;
        hash ^= hash >> 32;
    };
    for (uint64_t word : words) {
        mix(word);
    }
    if (layout.position.mode == kIndex8 || layout.position.mode == kIndex16) {
        size_t bytes = static_cast<size_t>(count) * layout.size;
        size_t i = 0;
        for (; i + 8 <= bytes; i += 8) {
            uint64_t word;
            std::memcpy(&word, data + i, 8);
            mix(word);
        }
        for (; i < bytes; i++) {
            mix(data[i] | 0x100);
        }
    }
    return hash | 1;
}

uint32_t g_indirect_draws = 0;
uint32_t g_coordinate_draws = 0;
std::atomic<uint32_t> g_skipped_presents{0};
#ifdef _WIN32
using ThreadClock = HANDLE;

ThreadClock current_thread_clock() {
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread, THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
    return thread;
}

double thread_seconds(ThreadClock thread) {
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        return 0.0;
    }
    auto ticks = [](const FILETIME& time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
    return static_cast<double>(ticks(kernel) + ticks(user)) * 1e-7;
}
#else
using ThreadClock = clockid_t;

ThreadClock current_thread_clock() {
    clockid_t clock = CLOCK_THREAD_CPUTIME_ID;
    pthread_getcpuclockid(pthread_self(), &clock);
    return clock;
}

double thread_seconds(ThreadClock clock) {
    timespec time{};
    if (clock_gettime(clock, &time) != 0) {
        return 0.0;
    }
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_nsec) * 1e-9;
}
#endif

std::atomic<bool> g_cpu_clock_known{false};
ThreadClock g_cpu_clock{};

void count_indirect(uint32_t draws) {
    uint32_t stages = ((g_bp[0x00] >> 10) & 15) + 1;
    bool indirect = false;
    bool coordinates = false;
    for (uint32_t i = 0; i < stages; i++) {
        uint32_t stage = g_bp[0x10 + i];
        indirect |= (stage & (3u << 7)) != 0 || (stage & (3u << 9)) != 0;
        coordinates |= (stage & ((63u << 13) | (1u << 20))) != 0;
    }
    g_indirect_draws += indirect ? draws : 0;
    g_coordinate_draws += coordinates && !indirect ? draws : 0;
}

void emit_unculled(DrawOutput& out, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c) {
    out.triangles.push_back(a);
    out.triangles.push_back(b);
    out.triangles.push_back(c);
    if (g_recording) {
        out.source_of.insert(out.source_of.end(), 3, render::kNoSource);
    }
}

ScreenVertex shifted(const ScreenVertex& vertex, float dx, float dy) {
    ScreenVertex result = vertex;
    result.x += dx * vertex.w;
    result.y += dy * vertex.w;
    return result;
}

void emit_line(DrawOutput& out, const Prepared& a, const Prepared& b) {
    if (!a.valid || !b.valid || a.vertex.w <= 0.0f || b.vertex.w <= 0.0f) {
        return;
    }
    float width = static_cast<float>(g_bp[0x22] & 0xFF) / 6.0f;
    float sx = b.vertex.x / b.vertex.w - a.vertex.x / a.vertex.w;
    float sy = b.vertex.y / b.vertex.w - a.vertex.y / a.vertex.w;
    float dx = std::fabs(sx) * kEfbWidth;
    float dy = std::fabs(sy) * kEfbHeight;
    float ox = 0.0f;
    float oy = 0.0f;
    if (dy > dx) {
        ox = width / kEfbWidth;
    } else {
        oy = width / kEfbHeight;
    }
    ScreenVertex a0 = shifted(a.vertex, -ox, -oy);
    ScreenVertex a1 = shifted(a.vertex, ox, oy);
    ScreenVertex b0 = shifted(b.vertex, -ox, -oy);
    ScreenVertex b1 = shifted(b.vertex, ox, oy);
    LinePointOffsets offsets = line_point_offsets(g_bp);
    if (offsets.line_coordinates != 0) {
        bool negative = line_offset_negative_side(sx, sy, dy > dx);
        offset_texture_coordinates(negative ? a0 : a1, offsets.line_coordinates, offsets.line, 0.0f);
        offset_texture_coordinates(negative ? b0 : b1, offsets.line_coordinates, offsets.line, 0.0f);
    }
    emit_unculled(out, a0, a1, b1);
    emit_unculled(out, a0, b1, b0);
}

void emit_point(DrawOutput& out, const Prepared& a) {
    if (!a.valid || a.vertex.w <= 0.0f) {
        return;
    }
    float size = static_cast<float>((g_bp[0x22] >> 8) & 0xFF) / 6.0f;
    float ox = size / kEfbWidth;
    float oy = size / kEfbHeight;
    ScreenVertex v0 = shifted(a.vertex, -ox, -oy);
    ScreenVertex v1 = shifted(a.vertex, ox, -oy);
    ScreenVertex v2 = shifted(a.vertex, ox, oy);
    ScreenVertex v3 = shifted(a.vertex, -ox, oy);
    LinePointOffsets offsets = line_point_offsets(g_bp);
    if (offsets.point_coordinates != 0) {
        offset_texture_coordinates(v0, offsets.point_coordinates, 0.0f, offsets.point);
        offset_texture_coordinates(v1, offsets.point_coordinates, offsets.point, offsets.point);
        offset_texture_coordinates(v2, offsets.point_coordinates, offsets.point, 0.0f);
    }
    emit_unculled(out, v0, v1, v2);
    emit_unculled(out, v0, v2, v3);
}

void capture_registers() {
    int written = 0;
    auto field = [&](const char* format, uint32_t index, uint32_t value) {
        std::fprintf(g_capture, format, index, value);
        if (++written % 8 == 0) {
            std::fputc(10, g_capture);
        }
    };
    for (uint32_t i = 0; i < 256; i++) {
        if (g_capture_first || g_bp[i] != g_captured_bp[i]) {
            field(" bp%02x=%06x", i, g_bp[i]);
            g_captured_bp[i] = g_bp[i];
        }
    }
    for (uint32_t i = 0; i < 8; i++) {
        if (g_capture_first || g_konst[i] != g_captured_konst[i]) {
            field(" konst%u=%06x", i, g_konst[i]);
            g_captured_konst[i] = g_konst[i];
        }
    }
    for (uint32_t i = 0; i < kXfSize; i++) {
        if ((g_capture_first && g_xf[i] != 0) || g_xf[i] != g_captured_xf[i]) {
            field(" xf%04x=%08x", i, g_xf[i]);
            g_captured_xf[i] = g_xf[i];
        }
    }
    g_capture_first = false;
    std::fputc(10, g_capture);
}

void capture_draw(uint8_t command, uint32_t count, size_t triangles, const Layout& layout, const Vertex* raw, const std::vector<Prepared>& vertices) {
    uint32_t vat = command & 7;
    std::fprintf(g_capture, "draw %d primitive=%u vat=%u vertices=%u triangles=%zu vcd=%08x %08x vat=%08x %08x %08x size=%u\n", g_capture_draws++,
                 (command >> 3) & 7, vat, count, triangles, g_vcd_low, g_vcd_high, g_vat[0][vat], g_vat[1][vat], g_vat[2][vat], layout.size);
    std::fprintf(g_capture, " arrays");
    for (uint32_t i = 0; i < kArrayCount; i++) {
        if (g_array_base[i] != 0) {
            std::fprintf(g_capture, " %u=%08x/%u", i, g_array_base[i], g_array_stride[i]);
        }
    }
    std::fputc(10, g_capture);
    capture_registers();
    uint32_t texgens = g_bp[0x00] & 15;
    for (uint32_t i = 0; i < count && i < 3; i++) {
        const Vertex& v = raw[i];
        const ScreenVertex& s = vertices[i].vertex;
        std::fprintf(g_capture, "  v%u pos=(%g %g %g) normal=(%g %g %g) pnmtx=%u screen=(%g %g %g %g)%s", i, v.position[0], v.position[1], v.position[2], v.normal[0],
                     v.normal[1], v.normal[2], v.position_matrix, s.x, s.y, s.z, s.w, vertices[i].valid ? "" : " invalid");
        for (uint32_t t = 0; t < kTexCoordCount; t++) {
            if (layout.texture[t].mode != 0) {
                std::fprintf(g_capture, " tex%u=(%g %g)", t, v.texture[t][0], v.texture[t][1]);
            }
        }
        for (uint32_t t = 0; t < texgens && t < kTexCoordCount; t++) {
            std::fprintf(g_capture, " uv%u=(%g %g %g) mtx%u=%u", t, s.uv[t][0], s.uv[t][1], s.uv[t][2], t, v.texture_matrix[t]);
        }
        std::fputc(10, g_capture);
    }
}

struct DrawJob {
    const uint8_t* data;
    const Layout* layout;
    uint32_t count;
    uint8_t command;
    bool in_list;
};

void build_draw(const DrawJob& job, std::vector<Prepared>& vertices, DrawOutput& out) {
    const Layout& layout = *job.layout;
    const uint8_t command = job.command;
    const uint8_t* data = job.data;
    const uint32_t count = job.count;
    vertices.resize(count);
    const uint8_t* stream = data;
    Vertex raw[3];
    Vertex vertex;
    begin_vertex(vertex);
    for (uint32_t i = 0; i < count; i++) {
        decode_vertex(layout, stream, vertex);
        if (i < 3) {
            raw[i] = vertex;
        }
        prepare(vertex, vertices[i]);
    }
    std::vector<ScreenVertex>& triangles = out.triangles;
    triangles.clear();
    out.prepared = vertices.data();
    out.source_of.clear();
    uint32_t primitive = (command >> 3) & 7;
    switch (primitive) {
    case 0:
    case 1:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            emit_triangle(out, vertices[i], vertices[i + 1], vertices[i + 2]);
            emit_triangle(out, vertices[i], vertices[i + 2], vertices[i + 3]);
        }
        break;
    case 2:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            emit_triangle(out, vertices[i], vertices[i + 1], vertices[i + 2]);
        }
        break;
    case 3:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                emit_triangle(out, vertices[i + 1], vertices[i], vertices[i + 2]);
            } else {
                emit_triangle(out, vertices[i], vertices[i + 1], vertices[i + 2]);
            }
        }
        break;
    case 4:
        for (uint32_t i = 1; i + 1 < count; i++) {
            emit_triangle(out, vertices[0], vertices[i], vertices[i + 1]);
        }
        break;
    case 5:
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            emit_line(out, vertices[i], vertices[i + 1]);
        }
        break;
    case 6:
        for (uint32_t i = 0; i + 1 < count; i++) {
            emit_line(out, vertices[i], vertices[i + 1]);
        }
        break;
    default:
        for (uint32_t i = 0; i < count; i++) {
            emit_point(out, vertices[i]);
        }
        break;
    }
    if (g_log_draws && g_copy_total >= g_log_from && g_logged_prepared < 4000) {
        g_logged_prepared++;
        uint32_t image0 = g_bp[0x88];
        const ScreenVertex& v0 = vertices[0].vertex;
        std::fprintf(stderr, "P copy=%d cmd=%02x n=%u tri=%zu stg=%u tg=%u cull=%u blend=%06x z=%06x map0=%u/%ux%u v0=(%.2f %.2f %.2f)%s tev0=%06x/%06x\n",
                     g_copy_total, command, count, triangles.size() / 3, ((g_bp[0x00] >> 10) & 15) + 1, g_bp[0x00] & 15, (g_bp[0x00] >> 14) & 3,
                     g_bp[0x41], g_bp[0x40], (image0 >> 20) & 15, (image0 & 0x3FF) + 1, ((image0 >> 10) & 0x3FF) + 1, v0.x, v0.y, v0.z,
                     vertices[0].valid ? "" : " INVALID", g_bp[0xC0], g_bp[0xC1]);
        std::fprintf(stderr, "  regs=%06x %06x %06x %06x %06x %06x %06x %06x konst=%06x %06x %06x %06x order=%06x ksel=%06x c0=(%.2f %.2f %.2f %.2f) mat=%08x ctl=%08x\n",
                     g_bp[0xE0], g_bp[0xE1], g_bp[0xE2], g_bp[0xE3], g_bp[0xE4], g_bp[0xE5], g_bp[0xE6], g_bp[0xE7], g_konst[0], g_konst[1], g_konst[2],
                     g_konst[3], g_bp[0x28], g_bp[0xF6], vertices[0].vertex.color[0][0], vertices[0].vertex.color[0][1], vertices[0].vertex.color[0][2],
                     vertices[0].vertex.color[0][3], g_xf[0x100C], g_xf[0x100E]);
    }
    if (g_capture) {
        capture_draw(command, count, triangles.size() / 3, layout, raw, vertices);
    }
    out.recorded = g_recording && count <= render::kNoSource;
    if (out.recorded) {
        std::vector<std::array<float, 4>>& positions = out.positions;
        positions.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            const ScreenVertex& v = vertices[i].vertex;
            positions[i] = {vertices[i].valid ? v.x : NAN, v.y, v.z, v.w};
        }
        render::DrawSource source;
        source.key = primitive < 5 ? draw_key(command, count, layout, data) : 0;
        if (source.key != 0) {
            bool indexed = layout.position.mode == kIndex8 || layout.position.mode == kIndex16;
            if (indexed || job.in_list) {
                uint64_t origin = indexed ? g_array_base[0] : reinterpret_cast<uintptr_t>(data);
                source.exact = ((source.key ^ origin) * 0xFF51AFD7ED558CCDull) | 1;
            }
        }
        for (uint32_t reg = 0x101A; reg <= 0x1026; reg++) {
            source.camera = (source.camera ^ g_xf[reg]) * 0x9E3779B1u;
        }
        source.positions = reinterpret_cast<const float(*)[4]>(positions.data());
        source.count = count;
        source.source_of = out.source_of.data();
        out.source = source;
    }
}

void submit_draw(const DrawOutput& out) {
    render::draw(out.triangles.data(), static_cast<uint32_t>(out.triangles.size()), out.recorded ? &out.source : nullptr);
}

constexpr uint32_t kRunLimit = 512;
constexpr uint32_t kParallelRun = 16;
constexpr uint32_t kRunChunk = 4;
constexpr uint32_t kRunHelpers = 3;
constexpr uint32_t kIdleSpins = 4000;

struct RunPool {
    std::atomic<uint64_t> state{0};
    std::atomic<uint32_t> done{0};
    std::atomic<uint32_t> sleepers{0};
    std::mutex mutex;
    std::condition_variable wake;
    uint32_t generation = 0;
};

std::vector<DrawJob> g_run;
std::vector<DrawOutput> g_outputs(kRunLimit + 1);
std::vector<Prepared> g_scratch[kRunHelpers + 1];
RunPool g_pool;

void relax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

bool run_waiting() {
    uint64_t state = g_pool.state.load();
    return (state & 0xFFFF) < ((state >> 16) & 0xFFFF);
}

struct RunFormat {
    uint32_t stride;
    uint32_t inputs;
    bool normal;
    bool indices;
    bool lit;
};

struct RunPlace {
    uint32_t vertex;
    uint32_t index;
};

RunFormat g_format;
RunPlace g_places[kRunLimit];
uint32_t g_pool_first = 0;
uint8_t* g_run_vertices = nullptr;
uint16_t* g_run_indices = nullptr;
float g_run_rows[render::kRunMaxRows][4];
uint64_t g_palette_serial = 1;
bool g_pool_packs = false;

uint32_t index_count(const DrawJob& job) {
    switch ((job.command >> 3) & 7) {
    case 0:
    case 1:
        return job.count / 4 * 6;
    case 2:
        return job.count / 3 * 3;
    default:
        return job.count >= 3 ? (job.count - 2) * 3 : 0;
    }
}

void pack_draw(uint32_t at) {
    const DrawJob& job = g_run[at];
    const Layout& layout = *job.layout;
    const RunFormat format = g_format;
    const uint32_t count = job.count;
    const uint32_t first = g_places[at].vertex;
    uint8_t* out = g_run_vertices + static_cast<size_t>(first) * format.stride;
    const uint8_t* stream = job.data;
    Vertex vertex;
    begin_vertex(vertex);
    for (uint32_t i = 0; i < count; i++, out += format.stride) {
        decode_vertex(layout, stream, vertex);
        std::memcpy(out, vertex.position, 12);
        if (format.lit) {
            std::memcpy(out + 12, vertex.color, 8);
        } else {
            uint8_t colors[2][4];
            float eye[3] = {};
            if (g_colors.lit) {
                transform_position(vertex, eye);
            }
            color_bytes(vertex, eye, colors);
            std::memcpy(out + 12, colors, 8);
        }
        uint8_t* p = out + 20;
        if (format.normal) {
            std::memcpy(p, vertex.normal, 12);
            p += 12;
        }
        if (format.indices) {
            p[0] = static_cast<uint8_t>(vertex.position_matrix);
            for (uint32_t k = 0; k < kTexCoordCount; k++) {
                p[1 + k] = static_cast<uint8_t>(vertex.texture_matrix[k]);
            }
            p[9] = p[10] = p[11] = 0;
            p += 12;
        }
        std::memcpy(p, vertex.texture, format.inputs * sizeof(float[2]));
    }
    uint16_t* index = g_run_indices + g_places[at].index;
    auto triangle = [&index, first](uint32_t a, uint32_t b, uint32_t c) {
        index[0] = static_cast<uint16_t>(first + a);
        index[1] = static_cast<uint16_t>(first + b);
        index[2] = static_cast<uint16_t>(first + c);
        index += 3;
    };
    switch ((job.command >> 3) & 7) {
    case 0:
    case 1:
        for (uint32_t i = 0; i + 3 < count; i += 4) {
            triangle(i, i + 1, i + 2);
            triangle(i, i + 2, i + 3);
        }
        break;
    case 2:
        for (uint32_t i = 0; i + 2 < count; i += 3) {
            triangle(i, i + 1, i + 2);
        }
        break;
    case 3:
        for (uint32_t i = 0; i + 2 < count; i++) {
            if (i & 1) {
                triangle(i + 1, i, i + 2);
            } else {
                triangle(i, i + 1, i + 2);
            }
        }
        break;
    default:
        for (uint32_t i = 1; i + 1 < count; i++) {
            triangle(0, i, i + 1);
        }
        break;
    }
}

bool build_some(uint32_t thread) {
    uint64_t state = g_pool.state.load();
    uint32_t index = state & 0xFFFF;
    uint32_t count = (state >> 16) & 0xFFFF;
    if (index >= count) {
        return false;
    }
    if (!g_pool.state.compare_exchange_strong(state, state + kRunChunk)) {
        return true;
    }
    uint32_t end = std::min(count, index + kRunChunk);
    for (uint32_t i = index; i < end; i++) {
        if (g_pool_packs) {
            pack_draw(g_pool_first + i);
        } else {
            build_draw(g_run[i], g_scratch[thread], g_outputs[i]);
        }
    }
    g_pool.done.fetch_add(end - index);
    return true;
}

void run_helper(uint32_t thread) {
    uint32_t idle = 0;
    for (;;) {
        if (build_some(thread)) {
            idle = 0;
        } else if (++idle < kIdleSpins) {
            relax();
        } else {
            std::unique_lock<std::mutex> lock(g_pool.mutex);
            g_pool.sleepers++;
            g_pool.wake.wait(lock, run_waiting);
            g_pool.sleepers--;
            idle = 0;
        }
    }
}

uint32_t start_helpers() {
    uint32_t cores = std::thread::hardware_concurrency();
    uint32_t helpers = cores > 4 && std::getenv("WP_SERIAL_DRAWS") == nullptr ? std::min(kRunHelpers, cores - 3) : 0;
    for (uint32_t i = 0; i < helpers; i++) {
        std::thread(run_helper, i + 1).detach();
    }
    return helpers;
}

void build_run(uint32_t count) {
    g_pool.done = 0;
    g_pool.state = (static_cast<uint64_t>(++g_pool.generation) << 32) | (static_cast<uint64_t>(count) << 16);
    if (g_pool.sleepers.load() != 0) {
        g_pool.mutex.lock();
        g_pool.mutex.unlock();
        g_pool.wake.notify_all();
    }
    while (build_some(0)) {
    }
    while (g_pool.done.load() != count) {
        relax();
    }
}

void check_draw(uint32_t index) {
    static uint32_t checked = 0;
    static uint32_t different = 0;
    const DrawOutput& built = g_outputs[index];
    DrawOutput& again = g_outputs[kRunLimit];
    build_draw(g_run[index], g_scratch[0], again);
    bool same = built.recorded == again.recorded && built.triangles.size() == again.triangles.size() && built.source_of == again.source_of &&
                std::memcmp(built.triangles.data(), again.triangles.data(), built.triangles.size() * sizeof(ScreenVertex)) == 0;
    if (same && built.recorded) {
        same = built.source.key == again.source.key && built.source.exact == again.source.exact && built.source.camera == again.source.camera &&
               built.source.count == again.source.count &&
               std::memcmp(built.positions.data(), again.positions.data(), built.source.count * sizeof(float[4])) == 0;
    }
    different += !same;
    if ((++checked & 0xFFFFF) == 0) {
        log::write("draws", "%u draws built on helper threads checked, %u differ", checked, different);
    }
}

void copy_rows(float (*to)[4], uint32_t address) {
    std::memcpy(to, &g_xf[address], sizeof(float[3][4]));
}

uint32_t begin_packed_run() {
    const TexGenState& texgen = g_texgen;
    const ProjectionState& p = g_projection;
    RunFormat format{};
    for (uint32_t i = 0; i < texgen.count; i++) {
        uint32_t source = texgen.unit[i].source;
        format.normal |= source == 1;
        if (source >= 5 && source < 13) {
            format.inputs = std::max(format.inputs, source - 4);
        }
    }
    format.indices = (g_vcd_low & 0x1FF) != 0;
    static const bool processor_lights = std::getenv("WP_CPU_LIGHTS") != nullptr;
    format.lit = g_colors.lit && !processor_lights;
    format.normal |= format.lit;
    format.stride = 20 + (format.normal ? 12 : 0) + (format.indices ? 12 : 0) + format.inputs * 8;
    g_format = format;
    uint32_t rows = render::kRunHeaderRows + render::kRunUnitRows * texgen.count;
    std::memset(g_run_rows, 0, sizeof(float[4]) * rows);
    float header[4][4] = {{p.a, p.b, p.c, p.d},
                          {p.e, p.f, p.orthographic ? 1.0f : 0.0f, format.indices ? 0.0f : 4.0f},
                          {p.x_scale, p.x_offset, p.y_scale, p.y_offset},
                          {p.z_scale, kDepthRange - p.z_offset, 0.0f, static_cast<float>(texgen.count + (texgen.dual ? 16 : 0))}};
    std::memcpy(g_run_rows, header, sizeof header);
    if (!format.indices) {
        copy_rows(g_run_rows + 4, (g_xf[0x1018] & 0x3F) * 4);
    }
    for (uint32_t i = 0; i < texgen.count; i++) {
        const TexGenUnit& unit = texgen.unit[i];
        uint32_t at = render::kRunHeaderRows + render::kRunUnitRows * i;
        g_run_rows[at][0] = static_cast<float>(unit.source);
        g_run_rows[at][1] = static_cast<float>((unit.projected ? 1 : 0) | (unit.abc1 ? 2 : 0) | (((unit.post >> 8) & 1) ? 4 : 0));
        g_run_rows[at][2] = format.indices ? 0.0f : static_cast<float>(at + 1);
        if (!format.indices) {
            copy_rows(g_run_rows + at + 1, g_default_texture_matrix[i] * 4);
        }
        copy_rows(g_run_rows + at + 4, 0x500 + (unit.post & 0x3F) * 4);
    }
    if (format.lit) {
        float (*light)[4] = g_run_rows + rows;
        std::memset(light, 0, sizeof(float[4]) * render::kRunLightRows);
        g_run_rows[3][3] += 32.0f;
        uint32_t used = 0;
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t color = g_xf[0x100E + k] & 0x7FFF;
            uint32_t alpha = g_xf[0x1010 + k] & 0x7FFF;
            light[0][k] = static_cast<float>(color);
            light[0][2 + k] = static_cast<float>(alpha);
            used |= (color & 2) ? ((color >> 2) & 15) | (((color >> 11) & 15) << 4) : 0;
            used |= (alpha & 2) ? ((alpha >> 2) & 15) | (((alpha >> 11) & 15) << 4) : 0;
            for (uint32_t i = 0; i < 4; i++) {
                light[1 + k][i] = static_cast<float>((g_xf[0x100C + k] >> (24 - 8 * i)) & 0xFF);
                light[3 + k][i] = static_cast<float>((g_xf[0x100A + k] >> (24 - 8 * i)) & 0xFF);
            }
        }
        if (!format.indices) {
            copy_rows(light + 5, render::kNormalWords + (g_xf[0x1018] & 31) * 3);
        }
        for (uint32_t n = 0; n < 8; n++) {
            if (used & (1u << n)) {
                uint32_t base = 0x600 + n * 16;
                for (uint32_t i = 0; i < 4; i++) {
                    light[8 + n * 4][i] = static_cast<float>((g_xf[base + 3] >> (24 - 8 * i)) & 0xFF);
                }
                copy_rows(light + 9 + n * 4, base + 4);
            }
        }
        rows += render::kRunLightRows;
    }
    return rows;
}

constexpr uint32_t kKeptMinVertices = 32;
constexpr uint32_t kKeptCheckBytes = 64;
constexpr uint32_t kKeptChanges = 4;
constexpr uint64_t kKeptSleepFrames = 64;
constexpr size_t kKeptLimit = 1u << 15;
constexpr uint32_t kKeptArrays = 12;

struct KeptRange {
    uint32_t first;
    uint32_t last;
    uint32_t size;
};

struct KeptList {
    uint64_t spans = 0;
    uint64_t content = 0;
    uint64_t wake = 0;
    uint64_t drawn = ~0ull;
    uint32_t changes = 0;
    uint32_t arrays = 0;
    bool scanned = false;
    bool seen = false;
    bool placed = false;
    bool stored = false;
    bool lacks_color[2] = {};
    KeptRange ranges[kKeptArrays] = {};
    render::KeptRun kept;
    std::vector<uint8_t> copy;
};

std::unordered_map<uint64_t, KeptList> g_kept;
uint64_t g_kept_frame = 0;
uint32_t g_kept_drawn = 0;
uint32_t g_kept_decoded = 0;
uint32_t g_kept_stored = 0;
uint64_t g_kept_checked = 0;
uint32_t g_kept_held = 0;
bool g_kept_refused = false;
uint32_t g_kept_verified = 0;
uint32_t g_kept_different = 0;

uint64_t mix(uint64_t hash, uint64_t value) {
    hash = (hash ^ value) * 1099511628211ull;
    return hash ^ (hash >> 29);
}

KeptList* find_kept(uint32_t first, uint32_t end, uint32_t vertices, uint32_t indices) {
    if (vertices < kKeptMinVertices || (g_colors.lit && !g_format.lit)) {
        return nullptr;
    }
    for (uint32_t i = first; i < end; i++) {
        if (!g_run[i].in_list) {
            return nullptr;
        }
    }
    uint64_t words[8] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_run[first].data)),
                         static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_run[end - 1].data)),
                         end - first,
                         vertices,
                         indices,
                         g_format.stride | (g_format.inputs << 8) | (g_format.normal ? 1u << 16 : 0) | (g_format.indices ? 1u << 17 : 0) | (g_format.lit ? 1u << 18 : 0),
                         g_vcd_low,
                         g_vcd_high};
    uint64_t key = mix(render::hash_bytes(reinterpret_cast<const uint8_t*>(words), sizeof words), render::hash_bytes(reinterpret_cast<const uint8_t*>(g_vat), sizeof g_vat));
    KeptList& list = g_kept[key];
    return g_kept_frame < list.wake ? nullptr : &list;
}

uint64_t hash_spans(uint32_t first, uint32_t end) {
    uint64_t hash = 0;
    const uint8_t* begin = g_run[first].data - 3;
    const uint8_t* stop = begin;
    for (uint32_t i = first; i < end; i++) {
        const DrawJob& job = g_run[i];
        if (job.data - 3 != stop) {
            hash = mix(hash, render::hash_bytes(begin, static_cast<size_t>(stop - begin)));
            begin = job.data - 3;
        }
        stop = job.data + static_cast<size_t>(job.count) * job.layout->size;
    }
    g_kept_checked += static_cast<uint64_t>(stop - begin);
    return mix(hash, render::hash_bytes(begin, static_cast<size_t>(stop - begin)));
}

void scan_ranges(KeptList& list, uint32_t first, uint32_t end) {
    list.arrays = 0;
    list.lacks_color[0] = false;
    list.lacks_color[1] = false;
    for (KeptRange& range : list.ranges) {
        range = {~0u, 0, 0};
    }
    for (uint32_t i = first; i < end; i++) {
        const DrawJob& job = g_run[i];
        const Layout& layout = *job.layout;
        const Element* elements[kKeptArrays] = {&layout.position, &layout.normal, &layout.color[0], &layout.color[1]};
        for (uint32_t k = 0; k < kTexCoordCount; k++) {
            elements[4 + k] = &layout.texture[k];
        }
        list.lacks_color[0] |= layout.color[0].mode == kNone;
        list.lacks_color[1] |= layout.color[1].mode == kNone;
        uint32_t skip = (layout.position_matrix ? 1 : 0) + static_cast<uint32_t>(__builtin_popcount(layout.texture_matrices));
        uint32_t steps[kKeptArrays];
        uint32_t used = 0;
        for (uint32_t a = 0; a < kKeptArrays; a++) {
            const Element& element = *elements[a];
            steps[a] = element.mode == kDirect ? element.direct_size : (element.mode == kIndex8 ? 1 : (element.mode == kIndex16 ? 2 : 0));
            if (element.mode == kIndex8 || element.mode == kIndex16) {
                used |= 1u << a;
                list.ranges[a].size = std::max(list.ranges[a].size, element.direct_size);
            }
        }
        list.arrays |= used;
        const uint8_t* stream = job.data;
        for (uint32_t v = 0; v < job.count; v++) {
            stream += skip;
            for (uint32_t a = 0; a < kKeptArrays; a++) {
                if (used & (1u << a)) {
                    uint32_t index = steps[a] == 1 ? stream[0] : be16(stream);
                    KeptRange& range = list.ranges[a];
                    range.first = std::min(range.first, index);
                    range.last = std::max(range.last, index);
                }
                stream += steps[a];
            }
        }
    }
}

bool kept_content(KeptList& list, uint32_t first, uint32_t end, uint32_t vertices, uint64_t& content) {
    uint64_t hash = hash_spans(first, end);
    if (!list.scanned || list.spans != hash) {
        scan_ranges(list, first, end);
        list.spans = hash;
        list.scanned = true;
    }
    size_t budget = static_cast<size_t>(vertices) * kKeptCheckBytes;
    for (uint32_t a = 0; a < kKeptArrays; a++) {
        if (!(list.arrays & (1u << a))) {
            continue;
        }
        const ArrayState& state = g_arrays[a];
        const KeptRange& range = list.ranges[a];
        size_t offset = static_cast<size_t>(range.first) * state.stride;
        if (range.first > range.last) {
            continue;
        }
        size_t bytes = static_cast<size_t>(range.last - range.first) * state.stride + range.size;
        if (bytes > budget || static_cast<size_t>(state.address & kAddressMask) + offset + bytes + kArrayReadMargin > kPhysicalSize) {
            return false;
        }
        budget -= bytes;
        g_kept_checked += bytes;
        hash = mix(hash, (static_cast<uint64_t>(state.stride) << 32) | state.address);
        hash = mix(hash, render::hash_bytes(state.base + offset, bytes));
    }
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t controls = g_xf[0x100E + k] | g_xf[0x1010 + k];
        uint32_t sources = (g_colors.from_vertex[k][0] ? 1u : 0u) | (g_colors.from_vertex[k][1] ? 2u : 0u);
        bool material = g_format.lit ? list.lacks_color[k] && (controls & 0x41) != 0 : list.lacks_color[k] || sources != 3;
        hash = mix(hash, g_format.lit ? 4 : sources);
        hash = mix(hash, material ? (1ull << 32) | g_xf[0x100C + k] : 0);
    }
    if (g_format.indices) {
        hash = mix(hash, (static_cast<uint64_t>(g_xf[0x1019]) << 32) | g_xf[0x1018]);
    }
    content = hash;
    return true;
}

void forget_kept() {
    g_kept.clear();
    g_kept_held = 0;
    render::reset_kept();
}

void pack_run(uint32_t count, uint32_t helpers) {
    uint32_t cull = (g_bp[0x00] >> 14) & 3;
    if (cull == 3) {
        return;
    }
    static const settings::LiveFlag keeps("video.keep_models", "WP_KEEP_MODELS");
    const bool keeping = keeps() && !g_kept_refused;
    uint32_t rows = begin_packed_run();
    uint32_t first = 0;
    while (first < count) {
        uint32_t end = first;
        uint32_t vertices = 0;
        uint32_t indices = 0;
        while (end < count && vertices + g_run[end].count <= 0xFFFF) {
            g_places[end] = {vertices, indices};
            vertices += g_run[end].count;
            indices += index_count(g_run[end]);
            end++;
        }
        if (indices != 0) {
            render::VertexRun run;
            run.stride = g_format.stride;
            run.texture_inputs = g_format.inputs;
            run.normal = g_format.normal;
            run.matrix_indices = g_format.indices;
            run.cull = cull;
            run.vertices = vertices;
            run.indices = indices;
            run.rows = g_run_rows;
            run.row_count = rows;
            run.palette = g_format.indices ? g_xf : nullptr;
            run.palette_serial = g_palette_serial;
            if (g_recording) {
                uint64_t words[6] = {static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_run[first].data)),
                                     end - first,
                                     vertices,
                                     indices,
                                     g_format.stride | (static_cast<uint64_t>(rows) << 16) | (static_cast<uint64_t>(cull) << 32),
                                     (static_cast<uint64_t>(g_bp[0x94]) << 32) | g_bp[0x88]};
                run.key = render::hash_bytes(reinterpret_cast<const uint8_t*>(words), sizeof words) | 1;
                for (uint32_t reg = 0x101A; reg <= 0x1026; reg++) {
                    run.camera = (run.camera ^ g_xf[reg]) * 0x9E3779B1u;
                }
                run.matrix = g_xf[0x1018] & 0x3F;
            }
            KeptList* list = keeping ? find_kept(first, end, vertices, indices) : nullptr;
            uint64_t content = 0;
            if (list && !kept_content(*list, first, end, vertices, content)) {
                list->wake = g_kept_frame + kKeptSleepFrames;
                list = nullptr;
            }
            static const bool verify = std::getenv("WP_CHECK_KEPT") != nullptr;
            const bool held = list && list->stored && list->content == content;
            if (held && !verify && render::draw_kept(run, list->kept)) {
                g_kept_drawn++;
                list->drawn = g_kept_frame;
                first = end;
                continue;
            }
            if (!render::begin_run(run, &g_run_vertices, &g_run_indices)) {
                return;
            }
            uint32_t jobs = end - first;
            if (jobs >= kParallelRun && helpers != 0) {
                g_pool_packs = true;
                g_pool_first = first;
                build_run(jobs);
                g_pool_packs = false;
            } else {
                for (uint32_t i = first; i < end; i++) {
                    pack_draw(i);
                }
            }
            const size_t vertex_bytes = static_cast<size_t>(vertices) * g_format.stride;
            const size_t index_bytes = static_cast<size_t>(indices) * sizeof(uint16_t);
            if (held && !verify) {
                list->stored = false;
            } else if (held) {
                g_kept_verified++;
                if (list->copy.size() != vertex_bytes + index_bytes || std::memcmp(list->copy.data(), g_run_vertices, vertex_bytes) != 0 ||
                    std::memcmp(list->copy.data() + vertex_bytes, g_run_indices, index_bytes) != 0) {
                    g_kept_different++;
                }
            } else if (list) {
                g_kept_decoded++;
                uint64_t after = 0;
                if (list->drawn != g_kept_frame && list->seen && list->content == content && kept_content(*list, first, end, vertices, after) && after == content) {
                    list->changes = 0;
                    list->stored = render::keep_run(run, g_run_vertices, g_run_indices, list->kept, list->placed);
                    list->placed = list->stored;
                    if (list->stored) {
                        g_kept_stored++;
                        g_kept_held++;
                        if (verify) {
                            list->copy.assign(g_run_vertices, g_run_vertices + vertex_bytes);
                            list->copy.insert(list->copy.end(), reinterpret_cast<const uint8_t*>(g_run_indices), reinterpret_cast<const uint8_t*>(g_run_indices) + index_bytes);
                        }
                    } else {
                        g_kept_refused = g_kept_held == 0;
                    }
                    if (!list->stored || g_kept.size() > kKeptLimit) {
                        forget_kept();
                    }
                } else {
                    if (list->seen && ++list->changes >= kKeptChanges) {
                        list->changes = kKeptChanges - 1;
                        list->wake = g_kept_frame + kKeptSleepFrames;
                    }
                    list->content = content;
                    list->seen = true;
                    list->stored = false;
                }
            }
        }
        first = end;
    }
}

bool run_of_triangles() {
    for (const DrawJob& job : g_run) {
        if (((job.command >> 3) & 7) >= 5) {
            return false;
        }
    }
    return true;
}

bool run_in_lists() {
    for (const DrawJob& job : g_run) {
        if (!job.in_list) {
            return false;
        }
    }
    return true;
}

void flush_run() {
    uint32_t count = static_cast<uint32_t>(g_run.size());
    if (count == 0) {
        return;
    }
    static const uint32_t helpers = start_helpers();
    static const bool check = std::getenv("WP_CHECK_DRAWS") != nullptr;
    render::finish_frame();
    count_indirect(count);
    begin_vertex_arrays();
    lighting::begin(g_xf);
    begin_texture_coordinates();
    begin_projection();
    begin_colors();
    g_recording = render::interpolating();
    if (!g_log_draws && !g_capture && run_of_triangles() && (!g_recording || run_in_lists()) && render::transforms_vertices()) {
        pack_run(count, helpers);
    } else if (count >= kParallelRun && helpers != 0 && !g_log_draws && !g_capture) {
        build_run(count);
        for (uint32_t i = 0; i < count; i++) {
            if (check) {
                check_draw(i);
            }
            submit_draw(g_outputs[i]);
        }
    } else {
        for (uint32_t i = 0; i < count; i++) {
            build_draw(g_run[i], g_scratch[0], g_outputs[kRunLimit]);
            submit_draw(g_outputs[kRunLimit]);
        }
    }
    g_run.clear();
}

void draw_primitive(uint8_t command, const uint8_t* data, uint32_t count, const Layout& layout) {
    if (!g_render_enabled) {
        return;
    }
    g_run.push_back({data, &layout, count, command, g_in_list});
    if (g_run.size() == kRunLimit) {
        flush_run();
    }
}

void report_copy() {
    if (!g_log || g_logged_copies >= kMaxLoggedCopies) {
        return;
    }
    g_logged_copies++;
    std::fprintf(stderr, "GX copy: draws=%u vertices=%u bp=%u cp=%u xf=%u lists=%u unknown=%u\n", g_stats.draws,
                 g_stats.vertices, g_stats.bp_writes, g_stats.cp_writes, g_stats.xf_words, g_stats.lists,
                 g_stats.unknown);
    g_stats = Statistics{};
}

void capture_frame_boundary() {
    if (g_capture) {
        std::fprintf(g_capture, "end of frame, %d draws\n", g_capture_draws);
        std::fclose(g_capture);
        g_capture = nullptr;
        render::dump_copies(nullptr);
        char image[80];
        std::snprintf(image, sizeof image, "%s.png", g_capture_name);
        video::save_next_frame(image);
        std::fprintf(stderr, "GX capture saved to %s.txt and %s.png", g_capture_name, g_capture_name);
        std::fputc(10, stderr);
        log::write("gx", "capture saved to %s.txt and %s.png", g_capture_name, g_capture_name);
        return;
    }
    static const char* scheduled = std::getenv("WP_CAPTURE_AT");
    static const auto start = std::chrono::steady_clock::now();
    bool due = false;
    if (scheduled) {
        static const char* next = scheduled;
        if (*next && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= std::atof(next)) {
            due = true;
            const char* comma = std::strchr(next, ',');
            next = comma ? comma + 1 : next + std::strlen(next);
        }
    }
    if (!g_capture_requested.exchange(false) && !due) {
        return;
    }
    std::error_code created;
    std::filesystem::create_directories("captures", created);
    char path[80];
    while (true) {
        std::snprintf(g_capture_name, sizeof g_capture_name, "captures/gx_capture_%03d", ++g_capture_index);
        std::snprintf(path, sizeof path, "%s.txt", g_capture_name);
        FILE* existing = std::fopen(path, "rb");
        if (!existing) {
            break;
        }
        std::fclose(existing);
    }
    g_capture = std::fopen(path, "w");
    render::dump_copies(g_capture_name);
    g_capture_draws = 0;
    g_capture_first = true;
}

render::CopyFilter copy_filter(uint32_t control) {
    static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
    uint64_t bits = g_bp[0x53] | (static_cast<uint64_t>(g_bp[0x54]) << 24);
    uint32_t weights[7];
    for (int i = 0; i < 7; i++) {
        weights[i] = static_cast<uint32_t>((bits >> (6 * i)) & 0x3F);
    }
    render::CopyFilter filter;
    filter.coefficients[0] = weights[0] + weights[1];
    filter.coefficients[1] = weights[2] + weights[3] + weights[4];
    filter.coefficients[2] = weights[5] + weights[6];
    static const settings::LiveFlag filter_enabled("video.copy_filter", "WP_COPY_FILTER");
    if (!filter_enabled()) {
        filter.coefficients[1] += filter.coefficients[0] + filter.coefficients[2];
        filter.coefficients[0] = 0;
        filter.coefficients[2] = 0;
    }
    filter.gamma = kGamma[(control >> 7) & 3];
    filter.clamp_top = (control & 1) != 0;
    filter.clamp_bottom = (control & 2) != 0;
    return filter;
}

std::atomic<uint32_t> g_frame_holds{0};
std::atomic<uint32_t> g_long_holds{0};
std::atomic<int64_t> g_gpu_asleep_ns{0};
std::atomic<bool> g_frame_late{false};

void execute_copy(uint32_t value) {
    g_copy_total++;
    report_copy();
    if (!g_render_enabled) {
        return;
    }
    if (g_capture && !(value & kCopyToFramebuffer)) {
        std::fprintf(g_capture, "copy to texture at %06x, source %06x size %06x, control %06x, pixel format %06x\n", g_bp[0x4B] << 5, g_bp[0x49], g_bp[0x4A], value, g_bp[0x43]);
    }
    if (value & kCopyToFramebuffer) {
        g_kept_frame++;
        capture_frame_boundary();
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        int x = static_cast<int>(source & 0x3FF);
        int y = static_cast<int>((source >> 10) & 0x3FF);
        int width = static_cast<int>(size & 0x3FF) + 1;
        int height = static_cast<int>((size >> 10) & 0x3FF) + 1;
        {
            g_frames++;
            g_frames_drawn++;
            auto now = std::chrono::steady_clock::now();
            static auto previous_frame = now;
            double frame_ms = std::chrono::duration<double>(now - previous_frame).count() * 1000.0;
            previous_frame = now;
            if (frame_ms > kSlowFrameMs) {
                log::write("frame", "slow frame: %.0f ms", frame_ms);
            }
            double seconds = std::chrono::duration<double>(now - g_fps_start).count();
            if (seconds >= 1.0) {
                int real_frames = g_frames;
                g_frames += static_cast<int>(render::take_interpolated_frames());
                uint32_t batches = 0;
                uint32_t vertices = 0;
                double batch_seconds = 0.0;
                render::take_statistics(batches, vertices, batch_seconds);
                uint32_t write_backs = 0;
                double write_back_seconds = 0.0;
                render::take_copy_statistics(write_backs, write_back_seconds);
                if (g_log_fps) {
                    std::fprintf(stderr, "fps %.1f batches %u vertices %u cpu-side draw time %.0f ms", g_frames / seconds, batches, vertices, batch_seconds * 1000.0);
                    std::fputc(10, stderr);
                }
                log::write("fps", "%.1f fps, %u draw batches, %u vertices, %.0f ms drawing, %u draws with indirect texturing, %u with wrapped coordinates, %u presents skipped", g_frames / seconds, batches,
                           vertices, batch_seconds * 1000.0, g_indirect_draws, g_coordinate_draws, g_skipped_presents.exchange(0));
                if (g_kept_drawn + g_kept_decoded + g_kept_verified != 0) {
                    log::write("models", "%u runs drawn from video memory, %u decoded, %u stored, %u KB compared, %zu lists known, %u verified, %u differ", g_kept_drawn, g_kept_decoded,
                               g_kept_stored, static_cast<unsigned>(g_kept_checked >> 10), g_kept.size(), g_kept_verified, g_kept_different);
                }
                g_kept_verified = 0;
                g_kept_drawn = 0;
                g_kept_decoded = 0;
                g_kept_stored = 0;
                g_kept_checked = 0;
                static double previous_cpu = 0.0;
                static double previous_gpu = 0.0;
                double cpu = g_cpu_clock_known.load() ? thread_seconds(g_cpu_clock) : 0.0;
                static const ThreadClock gpu_clock = current_thread_clock();
                double gpu = thread_seconds(gpu_clock);
                log::write("copies", "%u EFB copies written back to RAM, %.1f ms waiting for the GPU", write_backs, write_back_seconds * 1000.0);
                log::write("memory", "%zu MB private memory, %zu KB of graphics commands queued, timers held %u times for a token, %u for over 4 ms", private_memory_mb(),
                           g_queued_bytes.load() >> 10, g_frame_holds.exchange(0), g_long_holds.exchange(0));
                double idle = idle_share();
                double asleep = static_cast<double>(g_gpu_asleep_ns.exchange(0)) / 1e7 / seconds;
                if (idle >= 0.0) {
                    log::write("threads", "CPU thread busy %.0f%% (guest idle %.0f%%), GPU thread busy %.0f%%, without work %.0f%%", (cpu - previous_cpu) * 100.0 / seconds, idle * 100.0,
                               (gpu - previous_gpu) * 100.0 / seconds, asleep);
                } else {
                    log::write("threads", "CPU thread busy %.0f%%, GPU thread busy %.0f%%, without work %.0f%%", (cpu - previous_cpu) * 100.0 / seconds, (gpu - previous_gpu) * 100.0 / seconds, asleep);
                }
                publish_performance(g_frames / seconds, real_frames / seconds, seconds, idle >= 0.0 ? (1.0 - idle) * 100.0 : -1.0, std::clamp(100.0 - asleep, 0.0, 100.0));
                previous_cpu = cpu;
                previous_gpu = gpu;
                g_indirect_draws = 0;
                g_coordinate_draws = 0;
                video::update_statistics(g_frames / seconds);
                g_frames = 0;
                g_fps_start = now;
            }
        }
        if (!g_screen_hidden.load()) {
            render::copy_to_framebuffer(x, y, width, height, (g_bp[0x43] & 7) == kPixelFormatZ24, copy_filter(value));
        }
    } else {
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        int x = static_cast<int>(source & 0x3FF);
        int y = static_cast<int>((source >> 10) & 0x3FF);
        int width = static_cast<int>(size & 0x3FF) + 1;
        int height = static_cast<int>((size >> 10) & 0x3FF) + 1;
        uint32_t address = kRamBase | ((g_bp[0x4B] & 0xFFFFFF) << 5);
        uint32_t coded = (value >> 3) & 15;
        uint32_t format = coded / 2 + (coded & 1) * 8;
        render::copy_to_texture(address, g_bp[0x4D] & 0x3FF, x, y, width, height, (value & (1u << 9)) != 0, format, (value & (1u << 15)) != 0, (g_bp[0x43] & 7) == kPixelFormatZ24,
                                (g_bp[0x43] & 7) == kPixelFormatRgba6Z24, copy_filter(value));
    }
    if (value & kCopyClear) {
        uint32_t source = g_bp[0x49];
        uint32_t size = g_bp[0x4A];
        render::clear(static_cast<int>(source & 0x3FF), static_cast<int>((source >> 10) & 0x3FF), static_cast<int>(size & 0x3FF) + 1,
                      static_cast<int>((size >> 10) & 0x3FF) + 1);
    }
}

std::atomic<bool> g_finish_pending{false};
std::atomic<bool> g_token_pending{false};
std::atomic<uint32_t> g_token{0};
std::atomic<uint32_t> g_token_sent{0};
std::atomic<uint32_t> g_token_done{0};
std::mutex g_interrupt_lock;
std::condition_variable g_interrupt_raised;

#ifdef _WIN32
const HANDLE g_interrupt_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
#endif

void raise_interrupt() {
    g_interrupt_lock.lock();
    g_interrupt_lock.unlock();
    g_interrupt_raised.notify_all();
#ifdef _WIN32
    SetEvent(g_interrupt_event);
#endif
}

uint64_t g_register_serial = 0;

void load_bp(uint32_t word) {
    flush_run();
    g_register_serial++;
    uint32_t reg = word >> 24;
    uint32_t value = word & 0xFFFFFF;
    g_stats.bp_writes++;
    note_register(0, reg, value);
    if (reg == kBpMask) {
        g_bp_mask = value;
        return;
    }
    uint32_t mask = g_bp_mask;
    g_bp_mask = 0xFFFFFF;
    if (reg >= 0xE0 && reg <= 0xE7 && (value & (1u << 23))) {
        g_konst[reg - 0xE0] = (g_konst[reg - 0xE0] & ~mask) | (value & mask);
        return;
    }
    g_bp[reg] = (g_bp[reg] & ~mask) | (value & mask);
    if (reg == kBpDrawDone && (value & 2)) {
        render::finish_copies();
        g_finish_pending = true;
        raise_interrupt();
        render::finish_frame();
    } else if (reg == kBpToken || reg == kBpTokenInterrupt) {
        render::finish_copies();
        g_token = value & 0xFFFF;
        if (reg == kBpTokenInterrupt) {
            g_token_pending = true;
            g_token_done = value & 0xFFFF;
            raise_interrupt();
        }
        render::finish_frame();
    } else if (reg == kBpCopyExecute) {
        execute_copy(g_bp[reg]);
    } else if (reg == kBpInvalidateTexture) {
        render::invalidate_textures();
    } else if (reg == kBpLoadTlut) {
        uint32_t address = kRamBase | ((g_bp[0x64] & 0xFFFFFF) << 5);
        render::load_tlut(address, (g_bp[reg] & 0x3FF) << 9, ((g_bp[reg] >> 10) & 0x7FF) << 5);
    }
}

void load_cp(uint8_t reg, uint32_t value) {
    flush_run();
    g_stats.cp_writes++;
    note_register(1, reg, value);
    if (reg == 0x50) {
        g_vcd_low = value;
    } else if (reg == 0x60) {
        g_vcd_high = value;
    } else if (reg >= 0x70 && reg < 0x98) {
        g_vat[(reg >> 4) - 7][reg & 7] = value;
    } else if (reg >= 0xA0 && reg < 0xB0) {
        g_array_base[reg - 0xA0] = value;
    } else if (reg >= 0xB0 && reg < 0xC0) {
        g_array_stride[reg - 0xB0] = value & 0xFF;
    }
}

void write_xf(uint32_t address, uint32_t value) {
    if (address < kXfSize) {
        flush_run();
        if (address >= 0x1000) {
            note_register(2, address, value);
            g_register_serial++;
        } else if (address < render::kPaletteRows * 4 || (address >= render::kNormalWords && address < render::kNormalWords + render::kNormalRows * 4)) {
            g_palette_serial++;
        }
        g_xf[address] = value;
    }
}

void indexed_xf_load(uint8_t command, uint32_t word) {
    uint32_t array = 12 + ((command >> 3) & 3);
    uint32_t index = word >> 16;
    uint32_t count = ((word >> 12) & 15) + 1;
    uint32_t address = word & 0xFFF;
    const uint8_t* source = array_element_live(array, index);
    for (uint32_t i = 0; i < count; i++) {
        write_xf(address + i, be32(source + 4 * i));
    }
}

size_t parse(const uint8_t* data, size_t size, bool list);

size_t parse_one(const uint8_t* data, size_t size, bool list) {
    uint8_t command = data[0];
    if (command == kCommandNop || command == kCommandMetrics || command == kCommandInvalidate) {
        return 1;
    }
    if (command == kCommandLoadCp) {
        if (size < 6) {
            return 0;
        }
        load_cp(data[1], be32(data + 2));
        return 6;
    }
    if (command == kCommandLoadXf) {
        if (size < 5) {
            return 0;
        }
        uint32_t header = be32(data + 1);
        uint32_t count = (header >> 16) + 1;
        if (size < 5 + 4 * static_cast<size_t>(count)) {
            return 0;
        }
        for (uint32_t i = 0; i < count; i++) {
            write_xf((header & 0xFFFF) + i, be32(data + 5 + 4 * i));
        }
        g_stats.xf_words += count;
        return 5 + 4 * static_cast<size_t>(count);
    }
    if ((command & 0xE7) == 0x20) {
        if (size < 5) {
            return 0;
        }
        indexed_xf_load(command, be32(data + 1));
        g_stats.xf_words++;
        return 5;
    }
    if (command == kCommandCallList) {
        if (size < 9) {
            return 0;
        }
        uint32_t address = be32(data + 1);
        uint32_t length = be32(data + 5);
        g_stats.lists++;
        if (static_cast<uint64_t>(address & 0x1FFFFFFF) + length > kPhysicalSize) {
            std::fprintf(stderr, "GX display list out of range: address=%08x length=%08x list=%d\n", address, length, list);
            g_stats.unknown++;
            return 9;
        }
        if (!list) {
            g_in_list = true;
            parse(host(address), length, true);
            g_in_list = false;
        }
        return 9;
    }
    if (command == kCommandLoadBp) {
        if (size < 5) {
            return 0;
        }
        load_bp(be32(data + 1));
        return 5;
    }
    if (command & kCommandDrawMask) {
        if (size < 3) {
            return 0;
        }
        uint32_t count = be16(data + 1);
        const Layout& layout = make_layout(command & 7);
        size_t total = 3 + static_cast<size_t>(count) * layout.size;
        if (size < total) {
            return 0;
        }
        if (g_log_draws && g_copy_total >= g_log_from && g_logged_draws < 60) {
            g_logged_draws++;
            std::fprintf(stderr, "GX draw %02x count=%u size=%u vcd=%08x/%08x\n", command, count, layout.size, g_vcd_low, g_vcd_high);
        }
        draw_primitive(command, data + 3, count, layout);
        g_stats.draws++;
        g_stats.vertices += count;
        return total;
    }
    g_stats.unknown++;
    return 1;
}

size_t parse(const uint8_t* data, size_t size, bool list) {
    size_t offset = 0;
    while (offset < size) {
        size_t used = parse_one(data + offset, size - offset, list);
        if (used == 0) {
            break;
        }
        offset += used;
    }
    if (!list) {
        flush_run();
    }
    return offset;
}

}

const uint32_t* bp_registers() {
    return g_bp;
}

const uint32_t* konst_registers() {
    return g_konst;
}

uint64_t register_serial() {
    return g_register_serial;
}

const uint32_t* xf_registers() {
    return g_xf;
}

uint32_t register32(uint32_t address) {
    uint32_t value;
    std::memcpy(&value, host(address), sizeof value);
    return __builtin_bswap32(value);
}

uint16_t register16(uint32_t address) {
    uint16_t value;
    std::memcpy(&value, host(address), sizeof value);
    return __builtin_bswap16(value);
}

bool record_display_list(const uint8_t* data, unsigned bytes) {
    uint32_t pi_base = register32(kPiFifoBase) & kFifoPhysicalMask;
    uint32_t cp_base = (register16(kCpFifoBaseLow) | (static_cast<uint32_t>(register16(kCpFifoBaseHigh)) << 16)) & kFifoPhysicalMask;
    if (cp_base == 0 || pi_base == cp_base) {
        return false;
    }
    uint32_t pointer = register32(kPiFifoWritePointer);
    uint32_t address = pointer & kFifoPhysicalMask;
    for (unsigned i = 0; i < bytes; i++) {
        *host(kRamBase | (address + i)) = data[i];
    }
    uint32_t written = __builtin_bswap32((pointer & ~kFifoPhysicalMask) | (address + bytes));
    std::memcpy(host(kPiFifoWritePointer), &written, sizeof written);
    return true;
}

void store16(uint32_t address, uint16_t value) {
    uint16_t stored = __builtin_bswap16(value);
    std::memcpy(host(address), &stored, sizeof stored);
}

void store32(uint32_t address, uint32_t value) {
    uint32_t stored = __builtin_bswap32(value);
    std::memcpy(host(address), &stored, sizeof stored);
}

uint32_t cp_register(uint32_t low) {
    return register16(low) | (static_cast<uint32_t>(register16(low + 2)) << 16);
}

void set_cp_register(uint32_t low, uint32_t value) {
    store16(low, static_cast<uint16_t>(value));
    store16(low + 2, static_cast<uint16_t>(value >> 16));
}

uint8_t g_gather_pipe[kGatherPipeSize];
unsigned g_gather_count = 0;

void gather_pipe_burst() {
    uint32_t pi_write = register32(kPiFifoWritePointer);
    uint32_t pi_end = register32(kPiFifoEnd) & kFifoPointerMask;
    uint32_t pi_base = register32(kPiFifoBase) & kFifoPointerMask;
    uint32_t address = pi_write & kFifoPointerMask;
    if (address != 0 && render::guest_range_valid(kRamBase | address, kGatherPipeSize)) {
        std::memcpy(host(kRamBase | address), g_gather_pipe, kGatherPipeSize);
    }
    uint32_t next = address == pi_end ? pi_base : address + kGatherPipeSize;
    store32(kPiFifoWritePointer, (pi_write & ~kFifoPhysicalMask) | next);
    uint32_t cp_end = cp_register(kCpFifoEndLow) & kFifoPointerMask;
    uint32_t cp_base = cp_register(kCpFifoBaseLow) & kFifoPointerMask;
    uint32_t cp_write = cp_register(kCpWritePointerLow) & kFifoPointerMask;
    cp_write = cp_write == cp_end ? cp_base : cp_write + kGatherPipeSize;
    set_cp_register(kCpWritePointerLow, cp_write);
    set_cp_register(kCpReadPointerLow, cp_write);
    set_cp_register(kCpDistanceLow, 0);
}

bool linked_mode() {
    uint16_t control = register16(kCpControl);
    if ((control & (kCpReadEnable | kCpLinkEnable)) != (kCpReadEnable | kCpLinkEnable)) {
        return false;
    }
    uint32_t pi_base = register32(kPiFifoBase) & kFifoPhysicalMask;
    uint32_t cp_base = cp_register(kCpFifoBaseLow) & kFifoPhysicalMask;
    return cp_base != 0 && pi_base == cp_base;
}

void push(uint64_t value, unsigned bytes) {
    uint64_t swapped = __builtin_bswap64(value << (8 * (8 - bytes)));
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&swapped);
    if (record_display_list(data, bytes)) {
        return;
    }
    static bool bp_next = false;
    if (bytes == 4 && bp_next && (value >> 24) == kBpTokenInterrupt) {
        g_token_sent = static_cast<uint32_t>(value) & 0xFFFF;
    }
    bp_next = bytes == 1 && value == 0x61;
    if (linked_mode()) {
        for (unsigned i = 0; i < bytes; i++) {
            g_gather_pipe[g_gather_count++] = data[i];
            if (g_gather_count == kGatherPipeSize) {
                g_gather_count = 0;
                gather_pipe_burst();
            }
        }
    }
    g_fifo.insert(g_fifo.end(), data, data + bytes);
    if (g_fifo.size() >= kFlushThreshold) {
        process();
    }
}

bool take_finish_interrupt() {
    if (!(register16(kPeControl) & kPeFinishEnable)) {
        return false;
    }
    return g_finish_pending.exchange(false);
}

bool take_token_interrupt() {
    bool pending = (register16(kPeControl) & kPeTokenEnable) && g_token_pending.exchange(false);
    store16(kPeToken, static_cast<uint16_t>(g_token.load()));
    return pending;
}

bool frame_owed() {
    static bool holding = false;
    static std::chrono::steady_clock::time_point since;
    bool unreached = g_token_sent.load() != g_token_done.load();
    bool owed = unreached || (g_token_pending.load() && (register16(kPeControl) & kPeTokenEnable));
    if (!owed) {
        holding = false;
        return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (!holding) {
        holding = true;
        since = now;
        g_frame_holds++;
    }
    if (now - since > std::chrono::milliseconds(100)) {
        return false;
    }
    if (unreached && now - since > std::chrono::milliseconds(4) && !g_frame_late.exchange(true)) {
        g_long_holds++;
    }
    return true;
}

bool take_frame_held() {
    return g_frame_late.exchange(false);
}

void write_pixel_engine_control(uint16_t value) {
    store16(kPeControl, value & (kPeTokenEnable | kPeFinishEnable));
    if (value & kPeTokenClear) {
        g_token_pending = false;
    }
    if (value & kPeFinishClear) {
        g_finish_pending = false;
    }
}

void set_screen_hidden(bool hidden) {
    g_screen_hidden = hidden;
}

uint64_t frames_drawn() {
    return g_frames_drawn.load();
}

void request_capture() {
    g_capture_requested = true;
}

namespace {

constexpr int kMaxFramesAhead = 2;

struct GpuItem {
    std::vector<uint8_t> bytes;
    std::function<void()> task;
    bool frame = false;
};

struct GpuQueue {
    std::mutex lock;
    std::condition_variable work;
    std::condition_variable space;
    std::deque<GpuItem> items;
    int frames = 0;
    bool started = false;
};

GpuQueue g_gpu;
const bool g_threaded = [] {
    const char* setting = std::getenv("WP_GPU_THREAD");
    return !setting || std::atoi(setting) != 0;
}();



#ifdef _WIN32
constexpr auto kProfileWarmUp = std::chrono::seconds(40);
constexpr auto kProfileWritePeriod = std::chrono::seconds(5);

void sample_thread(HANDLE thread, std::string path) {
    struct Sample {
        uint64_t leaf;
        uint64_t frames[3];
    };
    uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandle(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    uint64_t end = base + nt->OptionalHeader.SizeOfImage;
    std::map<std::string, uint32_t> counts;
    auto written = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(kProfileWarmUp);
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Sample sample{};
        SuspendThread(thread);
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            sample.leaf = context.Rip;
            int found = 0;
            for (int depth = 0; depth < 64 && found < 3 && context.Rip != 0; depth++) {
                if (context.Rip >= base && context.Rip < end) {
                    sample.frames[found++] = context.Rip - base;
                }
                DWORD64 image = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image, nullptr);
                if (!function) {
                    if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), 8)) {
                        break;
                    }
                    context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                    continue;
                }
                void* handler_data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, function, &context, &handler_data, &frame, nullptr);
            }
        }
        ResumeThread(thread);
        char key[256];
        char module[MAX_PATH] = "exe";
        if (sample.leaf < base || sample.leaf >= end) {
            HMODULE handle = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(sample.leaf),
                                   &handle) &&
                GetModuleFileNameA(handle, module, sizeof module)) {
                const char* name = std::strrchr(module, 92);
                std::memmove(module, name ? name + 1 : module, std::strlen(name ? name + 1 : module) + 1);
            } else {
                std::strcpy(module, "unknown");
            }
        }
        std::snprintf(key, sizeof key, "%s %llx %llx %llx", module, static_cast<unsigned long long>(sample.frames[0]),
                      static_cast<unsigned long long>(sample.frames[1]), static_cast<unsigned long long>(sample.frames[2]));
        counts[key]++;
        auto now = std::chrono::steady_clock::now();
        if (now - written > kProfileWritePeriod) {
            written = now;
            if (std::FILE* file = std::fopen(path.c_str(), "w")) {
                for (const auto& entry : counts) {
                    std::fprintf(file, "%u %s", entry.second, entry.first.c_str());
                    std::fputc(10, file);
                }
                std::fclose(file);
            }
        }
    }
}
#endif

void gpu_thread() {
#ifdef _WIN32
    if (const char* path = std::getenv("WP_PROFILE_GPU")) {
        HANDLE self = nullptr;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS);
        std::thread(sample_thread, self, std::string(path)).detach();
    }
#endif
    std::vector<uint8_t> buffer;
    while (true) {
        std::unique_lock<std::mutex> hold(g_gpu.lock);
        if (g_gpu.items.empty()) {
            hold.unlock();
            render::finish_copies();
            render::finish_frame();
            hold.lock();
        }
        while (g_gpu.items.empty()) {
            hold.unlock();
            bool waiting = render::service_presents();
            hold.lock();
            auto asleep = std::chrono::steady_clock::now();
            if (waiting) {
                g_gpu.work.wait_for(hold, std::chrono::milliseconds(1), [] { return !g_gpu.items.empty(); });
            } else {
                g_gpu.work.wait(hold, [] { return !g_gpu.items.empty(); });
            }
            g_gpu_asleep_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - asleep).count();
        }
        GpuItem item = std::move(g_gpu.items.front());
        g_gpu.items.pop_front();
        g_queued_bytes -= item.bytes.size();
        hold.unlock();
        g_gpu.space.notify_all();
        render::service_presents();
        if (!item.bytes.empty()) {
            buffer.insert(buffer.end(), item.bytes.begin(), item.bytes.end());
            size_t used = parse(buffer.data(), buffer.size(), false);
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(used));
        }
        if (item.task) {
            item.task();
        }
        if (item.frame) {
            std::lock_guard<std::mutex> hold(g_gpu.lock);
            g_gpu.frames--;
        }
    }
}

void enqueue(GpuItem item) {
    std::unique_lock<std::mutex> hold(g_gpu.lock);
    if (!g_gpu.started) {
        g_gpu.started = true;
        std::thread(gpu_thread).detach();
    }
    if (item.frame) {
        if (g_gpu.frames >= kMaxFramesAhead) {
            hold.unlock();
            g_skipped_presents++;
            return;
        }
        g_gpu.frames++;
    }
    g_gpu.space.wait(hold, [] { return g_queued_bytes.load() < kMaxQueuedBytes; });
    g_queued_bytes += item.bytes.size();
    g_gpu.items.push_back(std::move(item));
    hold.unlock();
    g_gpu.work.notify_one();
}

}

size_t gpu_backlog() {
    return g_queued_bytes.load();
}

void process() {
    if (!g_cpu_clock_known.load()) {
        g_cpu_clock = current_thread_clock();
        g_cpu_clock_known = true;
    }
    if (g_fifo.empty()) {
        return;
    }
    if (!g_threaded) {
        size_t used = parse(g_fifo.data(), g_fifo.size(), false);
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + static_cast<std::ptrdiff_t>(used));
        return;
    }
    GpuItem item;
    item.bytes.swap(g_fifo);
    g_fifo.reserve(item.bytes.capacity());
    enqueue(std::move(item));
}

void wait_for_interrupt(std::chrono::microseconds limit) {
#ifdef _WIN32
    static const HANDLE timer = [] {
        HANDLE precise = CreateWaitableTimerExW(nullptr, nullptr, 2, TIMER_ALL_ACCESS);
        return precise ? precise : CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }();
    LARGE_INTEGER due;
    due.QuadPart = -limit.count() * 10;
    if (timer && g_interrupt_event && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        HANDLE handles[2] = {g_interrupt_event, timer};
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        return;
    }
#endif
    std::unique_lock<std::mutex> hold(g_interrupt_lock);
    g_interrupt_raised.wait_for(hold, limit, [] { return g_finish_pending.load() || g_token_pending.load(); });
}

bool wait_for_token(std::chrono::milliseconds limit) {
    process();
    auto end = std::chrono::steady_clock::now() + limit;
    while (g_token_sent.load() != g_token_done.load()) {
        if (std::chrono::steady_clock::now() >= end) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

double facing_area(const float a[3], const float b[3], const float c[3]) {
    const double ax = a[0], ay = a[1], aw = a[2];
    const double bx = b[0], by = b[1], bw = b[2];
    const double cx = c[0], cy = c[1], cw = c[2];
    return ax * (by * cw - cy * bw) - ay * (bx * cw - cx * bw) + aw * (bx * cy - cx * by);
}

void run_frame_task(std::function<void()> task) {
    process();
    if (!g_threaded) {
        task();
        return;
    }
    GpuItem item;
    item.task = std::move(task);
    item.frame = true;
    enqueue(std::move(item));
}

}
