#include "wp/gx_render.h"
#include "wp/gx.h"

#include "gx_render_common.h"
#include "gx_render_metal_shaders.h"
#include "../platform/gl_window.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "wp/color.h"
#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/hint.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/options.h"
#include "wp/perf_overlay.h"
#include "wp/settings.h"
#include "wp/titlebar.h"
#include "wp/touch.h"
#include "wp/video.h"

namespace wp::gx::render {

void finish_write_backs();
bool write_back_overlaps(uint32_t address, size_t size);

namespace {

constexpr uint32_t kVertexCapacity = 1 << 16;
constexpr uint32_t kScreenRingBytes = 32u << 20;
constexpr uint32_t kRunVertexBytes = 16u << 20;
constexpr uint32_t kRunIndexBytes = 4u << 20;
constexpr uint32_t kRunRowCapacity = 1u << 16;
constexpr uint32_t kKeptVertexBytes = 64u << 20;
constexpr uint32_t kKeptIndexBytes = 16u << 20;
constexpr uint32_t kNoPalette = ~0u;
constexpr uint64_t kWriteBackFrames = 2;
constexpr size_t kMaxPendingWriteBacks = 32;
constexpr uint32_t kLowMemoryEnd = 0x4000;
constexpr MTLPixelFormat kColorFormat = MTLPixelFormatRGBA8Unorm;
constexpr MTLPixelFormat kDepthFormat = MTLPixelFormatDepth32Float;
constexpr MTLPixelFormat kScreenFormat = MTLPixelFormatBGRA8Unorm;

struct Target {
    id<MTLTexture> texture = nil;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CachedTexture {
    id<MTLTexture> texture = nil;
    bool owned = true;
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t levels = 0;
    uint64_t verified_epoch = ~0ull;
    bool mipmaps = false;
    bool custom = false;
};

struct TexturePack {
    bool dump = false;
    bool load = false;
    std::filesystem::path dump_directory;
    custom_textures::Index index;
    std::unordered_map<std::string, id<MTLTexture>> loaded;
};

struct CopiedTexture {
    Target converted;
    bool has_view = false;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes = 0;
    uint64_t guest_hash = 0;
    uint32_t pending_writes = 0;
    uint32_t copied_frame = 0;
    uint32_t copied_format = 0;
};

struct PendingWriteBack {
    id<MTLBuffer> buffer = nil;
    uint64_t serial = 0;
    uint32_t address = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t row_bytes = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t texture_format = 0;
    uint64_t frame = 0;
};

struct Sampler {
    id<MTLSamplerState> state = nil;
    float bias = 1.0f;
};

struct Ring {
    id<MTLBuffer> buffer = nil;
    uint8_t* memory = nullptr;
    uint32_t capacity = 0;
    uint32_t cursor = 0;
    uint32_t segment = 0;
    uint64_t used_by[2] = {};
};

struct Image {
    id<MTLTexture> texture = nil;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Device {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    CAMetalLayer* layer = nil;
    id<MTLLibrary> library = nil;
    id<MTLFunction> tev_vertex = nil;
    id<MTLFunction> run_vertex = nil;
    id<MTLFunction> tev_fragment = nil;
    id<MTLFunction> tev_fragment_early = nil;
    id<MTLFunction> fullscreen_vertex = nil;
    id<MTLRenderPipelineState> copy = nil;
    id<MTLRenderPipelineState> present = nil;
    id<MTLRenderPipelineState> present_smooth = nil;
    id<MTLRenderPipelineState> overlay = nil;
    id<MTLRenderPipelineState> overlay_blend = nil;
    std::map<uint32_t, id<MTLRenderPipelineState>> clears;
    std::map<uint32_t, id<MTLDepthStencilState>> depth_states;
    id<MTLDepthStencilState> no_depth = nil;
    id<MTLSamplerState> linear = nil;
    id<MTLSamplerState> point = nil;
    id<MTLTexture> empty = nil;
    id<MTLTexture> efb_color = nil;
    id<MTLTexture> efb_depth = nil;
    Target frame;
    Target write_back;
    std::vector<id<MTLBuffer>> free_buffers;
    std::vector<PendingWriteBack> write_backs;
    Ring screen;
    Ring run_vertices;
    Ring run_indices;
    Ring run_rows;
    id<MTLBuffer> kept_vertices = nil;
    id<MTLBuffer> kept_indices = nil;
    Image overlay_image;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    Image touch_image;
    uint64_t touch_version = 0;
    bool touch_visible = false;
    Image knob_image;
    Image hud_image;
    uint64_t hud_version = 0;
    bool hud_visible = false;
    perf::Corner hud_corner = perf::Corner::TopLeft;
    Image hint_image;
    uint64_t hint_version = 0;
    bool hint_visible = false;
    Image bar_image;
    uint64_t bar_version = 0;
    std::map<uint64_t, Sampler> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    bool failed = false;
    bool ready = false;
};

struct PendingBatch {
    std::vector<ScreenVertex> vertices;
    Constants constants;
    void* views[kTextureMaps] = {};
    const Sampler* samplers[kTextureMaps] = {};
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
    uint32_t scissor_offset = 0;
};

Device g_device;
PendingBatch g_pending;
TexturePack g_pack;
uint32_t g_custom_maps = 0;

bool g_log_gx = std::getenv("WP_LOG_GX") != nullptr;
bool g_logged_palette = false;
bool g_logged_copy_format = false;
uint64_t g_frame = 0;
uint64_t g_texture_epoch = 0;
int g_scale = 1;
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;
uint32_t g_write_backs = 0;
double g_write_back_seconds = 0.0;
uint64_t g_frame_serials[2] = {};

id<MTLCommandBuffer> g_command = nil;
uint64_t g_command_serial = 0;
uint64_t g_next_serial = 1;
std::mutex g_done_lock;
std::condition_variable g_done_changed;
uint64_t g_done = 0;

id<MTLRenderCommandEncoder> g_encoder = nil;
const void* g_encoder_target = nullptr;
const void* g_bound_pipeline = nullptr;
const void* g_bound_depth = nullptr;
int g_bound_cull = -1;

void flush_pending();

int scaled(int value) {
    return value * g_scale;
}

void ensure_command() {
    if (g_command) {
        return;
    }
    @autoreleasepool {
        g_command = [g_device.queue commandBuffer];
    }
    g_command_serial = g_next_serial++;
    const uint64_t serial = g_command_serial;
    [g_command addCompletedHandler:^(id<MTLCommandBuffer>) {
        std::lock_guard<std::mutex> hold(g_done_lock);
        g_done = std::max(g_done, serial);
        g_done_changed.notify_all();
    }];
}

uint64_t current_serial() {
    ensure_command();
    return g_command_serial;
}

void end_encoder() {
    if (g_encoder) {
        [g_encoder endEncoding];
        g_encoder = nil;
    }
    g_encoder_target = nullptr;
    g_bound_pipeline = nullptr;
    g_bound_depth = nullptr;
    g_bound_cull = -1;
}

void submit() {
    end_encoder();
    if (g_command) {
        [g_command commit];
        g_command = nil;
    }
}

bool serial_done(uint64_t serial) {
    std::lock_guard<std::mutex> hold(g_done_lock);
    return g_done >= serial;
}

void wait_serial(uint64_t serial) {
    if (serial == 0) {
        return;
    }
    if (g_command && serial >= g_command_serial) {
        submit();
    }
    std::unique_lock<std::mutex> hold(g_done_lock);
    g_done_changed.wait(hold, [serial] { return g_done >= serial; });
}

void wait_idle() {
    uint64_t last = g_command ? g_command_serial : g_next_serial - 1;
    wait_serial(last);
}

id<MTLRenderCommandEncoder> begin_pass(id<MTLTexture> color, id<MTLTexture> depth, MTLLoadAction load, MTLClearColor clear_color) {
    end_encoder();
    ensure_command();
    @autoreleasepool {
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = color;
        pass.colorAttachments[0].loadAction = load;
        pass.colorAttachments[0].clearColor = clear_color;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        if (depth) {
            pass.depthAttachment.texture = depth;
            pass.depthAttachment.loadAction = load == MTLLoadActionClear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.clearDepth = 1.0;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
        }
        g_encoder = [g_command renderCommandEncoderWithDescriptor:pass];
    }
    g_encoder_target = (__bridge const void*)color;
    return g_encoder;
}

void set_viewport(double x, double y, double width, double height) {
    MTLViewport viewport{x, y, width, height, 0.0, 1.0};
    [g_encoder setViewport:viewport];
}

id<MTLRenderCommandEncoder> efb_encoder() {
    if (g_encoder && g_encoder_target == (__bridge const void*)g_device.efb_color) {
        return g_encoder;
    }
    begin_pass(g_device.efb_color, g_device.efb_depth, MTLLoadActionLoad, MTLClearColorMake(0, 0, 0, 0));
    set_viewport(0, 0, scaled(kEfbWidth), scaled(kEfbHeight));
    [g_encoder setDepthClipMode:MTLDepthClipModeClamp];
    return g_encoder;
}

void bind_pipeline(id<MTLRenderPipelineState> pipeline) {
    if (g_bound_pipeline != (__bridge const void*)pipeline) {
        [g_encoder setRenderPipelineState:pipeline];
        g_bound_pipeline = (__bridge const void*)pipeline;
    }
}

void bind_depth(id<MTLDepthStencilState> state) {
    if (g_bound_depth != (__bridge const void*)state) {
        [g_encoder setDepthStencilState:state];
        g_bound_depth = (__bridge const void*)state;
    }
}

void bind_cull(int cull) {
    if (g_bound_cull != cull) {
        [g_encoder setCullMode:cull == 1 ? MTLCullModeBack : (cull == 2 ? MTLCullModeFront : MTLCullModeNone)];
        g_bound_cull = cull;
    }
}

bool make_ring(Ring& ring, uint32_t bytes) {
    ring.buffer = [g_device.device newBufferWithLength:bytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    if (!ring.buffer) {
        return false;
    }
    ring.memory = static_cast<uint8_t*>([ring.buffer contents]);
    ring.capacity = bytes;
    ring.cursor = 0;
    ring.segment = 0;
    return true;
}

bool reserve(Ring& ring, uint32_t size, uint32_t& start) {
    uint32_t half = ring.capacity / 2;
    if (size > half) {
        return false;
    }
    uint32_t at = (ring.cursor + 15u) & ~15u;
    if (at >= ring.capacity) {
        at = 0;
    }
    uint32_t segment = at >= half ? 1 : 0;
    if (at + size > (segment + 1) * half) {
        segment ^= 1;
        at = segment * half;
    }
    if (segment != ring.segment) {
        ring.used_by[ring.segment] = current_serial();
        wait_serial(ring.used_by[segment]);
        ring.used_by[segment] = 0;
        ring.segment = segment;
    }
    start = at;
    ring.cursor = at + size;
    return true;
}

bool upload(Ring& ring, const void* data, uint32_t size, uint32_t& start) {
    if (size == 0) {
        start = 0;
        return true;
    }
    if (!reserve(ring, size, start)) {
        return false;
    }
    std::memcpy(ring.memory + start, data, size);
    return true;
}

id<MTLLibrary> compile_library(const std::string& source, NSString** problem) {
    @autoreleasepool {
        MTLCompileOptions* options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        options.preserveInvariance = YES;
        NSError* error = nil;
        id<MTLLibrary> library = [g_device.device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if (!library && problem) {
            *problem = error ? [error localizedDescription] : @"unknown error";
        }
        return library;
    }
}

MTLBlendFactor metal_factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return MTLBlendFactorZero;
    case BlendFactor::One:
        return MTLBlendFactorOne;
    case BlendFactor::SourceColor:
        return MTLBlendFactorSourceColor;
    case BlendFactor::InverseSourceColor:
        return MTLBlendFactorOneMinusSourceColor;
    case BlendFactor::SourceAlpha:
        return MTLBlendFactorSourceAlpha;
    case BlendFactor::InverseSourceAlpha:
        return MTLBlendFactorOneMinusSourceAlpha;
    case BlendFactor::DestinationColor:
        return MTLBlendFactorDestinationColor;
    case BlendFactor::InverseDestinationColor:
        return MTLBlendFactorOneMinusDestinationColor;
    case BlendFactor::DestinationAlpha:
        return MTLBlendFactorDestinationAlpha;
    case BlendFactor::InverseDestinationAlpha:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case BlendFactor::TevColor:
        return MTLBlendFactorSource1Color;
    case BlendFactor::TevAlpha:
        return MTLBlendFactorSource1Alpha;
    case BlendFactor::InverseTevAlpha:
        return MTLBlendFactorOneMinusSource1Alpha;
    }
    return MTLBlendFactorOne;
}

MTLBlendOperation metal_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return MTLBlendOperationSubtract;
    case BlendOperation::ReverseSubtract:
        return MTLBlendOperationReverseSubtract;
    default:
        return MTLBlendOperationAdd;
    }
}

const BlendState& blend_for(uint32_t word) {
    static std::map<uint32_t, BlendState> cache;
    auto found = cache.find(word);
    if (found == cache.end()) {
        BlendState state = blend_state_for(word);
        if (state.logic_op && g_log_gx) {
            std::fprintf(stderr, "GX logic op %s (%u), %s, blend mode %06x\n", logic_op_name(state.logic_mode), state.logic_mode,
                         logic_op_exact(state.logic_mode) ? "exact" : "approximated with blending", word);
        }
        found = cache.emplace(word, state).first;
    }
    return found->second;
}

MTLRenderPipelineDescriptor* tev_descriptor(int kind, id<MTLFunction> fragment, uint32_t blend_word) {
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = kind ? g_device.run_vertex : g_device.tev_vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
    color.pixelFormat = kColorFormat;
    const BlendState& blend = blend_for(blend_word);
    color.blendingEnabled = blend.enable ? YES : NO;
    color.rgbBlendOperation = metal_operation(blend.operation);
    color.alphaBlendOperation = metal_operation(blend.alpha_operation);
    color.sourceRGBBlendFactor = metal_factor(blend.source);
    color.destinationRGBBlendFactor = metal_factor(blend.destination);
    color.sourceAlphaBlendFactor = metal_factor(blend.source_alpha);
    color.destinationAlphaBlendFactor = metal_factor(blend.destination_alpha);
    color.writeMask = (blend.color_update ? MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue : MTLColorWriteMaskNone) |
                      (blend.alpha_update ? MTLColorWriteMaskAlpha : MTLColorWriteMaskNone);
    return descriptor;
}

id<MTLRenderPipelineState> make_pipeline(MTLRenderPipelineDescriptor* descriptor, const char* what) {
    NSError* error = nil;
    id<MTLRenderPipelineState> state = [g_device.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!state) {
        log::write("video", "the %s pipeline did not build: %s", what, error ? [[error localizedDescription] UTF8String] : "unknown error");
    }
    return state;
}

struct Pipe {
    id<MTLRenderPipelineState> state = nil;
    bool pending = false;
    bool failed = false;
};

struct Variant {
    id<MTLFunction> function = nil;
    bool compiling = false;
    bool failed = false;
    std::vector<uint64_t> waiting;
    std::unordered_map<uint64_t, Pipe> pipes;
};

std::recursive_mutex g_compile_lock;
std::unordered_map<std::string, Variant> g_variants;
std::unordered_map<uint64_t, Pipe> g_generic;
uint32_t g_compiling = 0;
constexpr uint32_t kMaxCompiling = 12;
uint32_t g_variants_ready = 0;
uint32_t g_variants_failed = 0;
uint32_t g_reported_ready = 0;

uint64_t pipe_key(int kind, bool early, uint32_t blend) {
    return (static_cast<uint64_t>(blend) << 2) | (static_cast<uint64_t>(kind) << 1) | (early ? 1u : 0u);
}

struct Records {
    std::string path;
    bool opened = false;
    std::vector<std::pair<uint64_t, std::string>> loaded;
    std::unordered_map<std::string, bool> written;
};

Records g_records;

std::string record_name(uint64_t key, const std::string& variant) {
    return std::string(reinterpret_cast<const char*>(&key), sizeof key) + variant;
}

std::string records_identity() {
    uint64_t hash = 1469598103934665603ull;
    for (const char* source : {metal_shaders::kCommon, metal_shaders::kVertices, metal_shaders::kTev}) {
        for (const char* at = source; *at; at++) {
            hash = (hash ^ static_cast<uint8_t>(*at)) * 1099511628211ull;
        }
    }
    std::string name = [[g_device.device name] UTF8String];
    return "WPMTL1|" + name + "|" + std::to_string(hash) + "\n";
}

void open_records() {
    g_records.opened = true;
    g_records.path = (std::filesystem::path(game::description().settings_file).parent_path() / "pipelines_metal.bin").string();
    std::string identity = records_identity();
    std::string bytes;
    if (FILE* file = std::fopen(g_records.path.c_str(), "rb")) {
        char buffer[1 << 16];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
            bytes.append(buffer, read);
        }
        std::fclose(file);
    }
    if (bytes.compare(0, identity.size(), identity) != 0) {
        if (FILE* file = std::fopen(g_records.path.c_str(), "wb")) {
            std::fwrite(identity.data(), 1, identity.size(), file);
            std::fclose(file);
        }
        return;
    }
    size_t at = identity.size();
    while (bytes.size() - at >= 12) {
        uint64_t key = 0;
        uint32_t length = 0;
        std::memcpy(&key, bytes.data() + at, 8);
        std::memcpy(&length, bytes.data() + at + 8, 4);
        if (at + 12 + length > bytes.size()) {
            break;
        }
        std::string variant = bytes.substr(at + 12, length);
        g_records.written[record_name(key, variant)] = true;
        g_records.loaded.emplace_back(key, variant);
        at += 12 + length;
    }
}

void note_record(uint64_t key, const std::string& variant) {
    if (g_records.path.empty()) {
        return;
    }
    std::string name = record_name(key, variant);
    if (g_records.written[name]) {
        return;
    }
    g_records.written[name] = true;
    uint32_t length = static_cast<uint32_t>(variant.size());
    if (FILE* file = std::fopen(g_records.path.c_str(), "ab")) {
        std::fwrite(&key, 1, 8, file);
        std::fwrite(&length, 1, 4, file);
        std::fwrite(variant.data(), 1, variant.size(), file);
        std::fclose(file);
    }
}

std::string variant_source(const std::string& key) {
    std::vector<uint32_t> words(key.size() / sizeof(uint32_t));
    std::memcpy(words.data(), key.data(), key.size());
    uint32_t count = words[0];
    auto vector = [](const uint32_t* v) {
        char text[96];
        std::snprintf(text, sizeof text, "uint4(%uu, %uu, %uu, %uu)", v[0], v[1], v[2], v[3]);
        return std::string(text);
    };
    const uint32_t zero[4] = {};
    std::string stages = "constant uint4 kStage[16] = {";
    std::string indirects = "constant uint4 kTevind[16] = {";
    for (uint32_t i = 0; i < kMaxStages; i++) {
        const uint32_t* stage = i < count ? &words[kVariantHeader + i * 5] : zero;
        const uint32_t indirect[4] = {i < count ? words[kVariantHeader + i * 5 + 4] : 0, 0, 0, 0};
        stages += vector(stage) + (i + 1 < kMaxStages ? ", " : "};\n");
        indirects += vector(indirect) + (i + 1 < kMaxStages ? ", " : "};\n");
    }
    std::string source = metal_shaders::kCommon;
    source += "#define WP_VARIANT\n";
    source += words[1] ? "#define WP_SKIP_LATE\n" : "#define WP_SKIP_EARLY\n";
    source += stages + indirects;
    source += "constant uint4 kSwaps = " + vector(&words[2]) + ";\n";
    source += "constant uint4 kIndirect = " + vector(&words[6]) + ";\n";
    source += "#define STAGE(i) kStage[i]\n#define TEVIND(i) kTevind[i]\n#define SWAPS kSwaps\n#define INDIRECT kIndirect\n";
    source += "#define COUNT " + std::to_string(count) + "u\n";
    source += metal_shaders::kTev;
    return source;
}

void start_pipe(Variant& variant, uint64_t key);

void start_variant(const std::string& name, Variant& variant) {
    variant.compiling = true;
    g_compiling++;
    std::string source = variant_source(name);
    bool early = name.size() >= 8 && reinterpret_cast<const uint32_t*>(name.data())[1] != 0;
    Variant* place = &variant;
    @autoreleasepool {
        MTLCompileOptions* options = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        options.fastMathEnabled = NO;
#pragma clang diagnostic pop
        options.preserveInvariance = YES;
        [g_device.device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                      options:options
                            completionHandler:^(id<MTLLibrary> library, NSError* error) {
                                id<MTLFunction> function = library ? [library newFunctionWithName:early ? @"tev_fragment_early" : @"tev_fragment"] : nil;
                                std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
                                g_compiling--;
                                place->compiling = false;
                                if (!function) {
                                    place->failed = true;
                                    g_variants_failed++;
                                    log::write("video", "a pixel shader variant did not build, the generic shader keeps drawing it: %s",
                                               error ? [[error localizedDescription] UTF8String] : "no function");
                                    return;
                                }
                                place->function = function;
                                g_variants_ready++;
                                std::vector<uint64_t> waiting;
                                waiting.swap(place->waiting);
                                for (uint64_t key : waiting) {
                                    start_pipe(*place, key);
                                }
                            }];
    }
}

void start_pipe(Variant& variant, uint64_t key) {
    Pipe& pipe = variant.pipes[key];
    if (pipe.state || pipe.pending || pipe.failed) {
        return;
    }
    pipe.pending = true;
    Pipe* place = &pipe;
    @autoreleasepool {
        MTLRenderPipelineDescriptor* descriptor = tev_descriptor(static_cast<int>((key >> 1) & 1), variant.function, static_cast<uint32_t>(key >> 2));
        [g_device.device newRenderPipelineStateWithDescriptor:descriptor
                                            completionHandler:^(id<MTLRenderPipelineState> state, NSError*) {
                                                std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
                                                place->pending = false;
                                                place->state = state;
                                                place->failed = state == nil;
                                            }];
    }
}

void start_generic(uint64_t key) {
    Pipe& pipe = g_generic[key];
    if (pipe.state || pipe.pending || pipe.failed) {
        return;
    }
    pipe.pending = true;
    Pipe* place = &pipe;
    @autoreleasepool {
        MTLRenderPipelineDescriptor* descriptor =
            tev_descriptor(static_cast<int>((key >> 1) & 1), (key & 1) ? g_device.tev_fragment_early : g_device.tev_fragment, static_cast<uint32_t>(key >> 2));
        [g_device.device newRenderPipelineStateWithDescriptor:descriptor
                                            completionHandler:^(id<MTLRenderPipelineState> state, NSError*) {
                                                std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
                                                place->pending = false;
                                                place->state = state;
                                                place->failed = state == nil;
                                            }];
    }
}

void warm_pipelines() {
    open_records();
    std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
    size_t count = 0;
    for (const auto& [key, name] : g_records.loaded) {
        if (name.empty()) {
            start_generic(key);
        } else {
            Variant& variant = g_variants[name];
            if (variant.function) {
                start_pipe(variant, key);
            } else {
                variant.waiting.push_back(key);
                if (!variant.compiling && !variant.failed) {
                    start_variant(name, variant);
                }
            }
        }
        count++;
    }
    if (count) {
        log::write("video", "%zu pipelines from earlier games are being prepared in the background", count);
    }
}

id<MTLRenderPipelineState> generic_pipeline(int kind, bool early, uint32_t blend) {
    uint64_t key = pipe_key(kind, early, blend);
    {
        std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
        Pipe& pipe = g_generic[key];
        if (pipe.state) {
            return pipe.state;
        }
    }
    @autoreleasepool {
        MTLRenderPipelineDescriptor* descriptor = tev_descriptor(kind, early ? g_device.tev_fragment_early : g_device.tev_fragment, blend);
        id<MTLRenderPipelineState> state = make_pipeline(descriptor, "TEV");
        std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
        Pipe& pipe = g_generic[key];
        if (!pipe.state) {
            pipe.state = state;
        }
        note_record(key, std::string());
        return pipe.state;
    }
}

id<MTLRenderPipelineState> tev_pipeline(const Constants& constants, bool early, int kind, uint32_t blend) {
    static const bool generic_only = std::getenv("WP_GENERIC_SHADER") != nullptr;
    uint64_t key = pipe_key(kind, early, blend);
    if (!generic_only) {
        std::string name = variant_key(constants, early);
        std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
        Variant& variant = g_variants[name];
        if (variant.function) {
            Pipe& pipe = variant.pipes[key];
            if (pipe.state) {
                return pipe.state;
            }
            if (!pipe.pending && !pipe.failed) {
                start_pipe(variant, key);
                note_record(key, name);
            }
        } else if (!variant.failed) {
            if (std::find(variant.waiting.begin(), variant.waiting.end(), key) == variant.waiting.end()) {
                variant.waiting.push_back(key);
                note_record(key, name);
            }
            if (!variant.compiling && g_compiling < kMaxCompiling) {
                start_variant(name, variant);
            }
        }
    }
    return generic_pipeline(kind, early, blend);
}

id<MTLDepthStencilState> depth_state(uint32_t word) {
    uint32_t key = (word & 1) ? (word & 0x1F) : 0;
    auto found = g_device.depth_states.find(key);
    if (found != g_device.depth_states.end()) {
        return found->second;
    }
    MTLDepthStencilDescriptor* descriptor = [MTLDepthStencilDescriptor new];
    if (key & 1) {
        descriptor.depthCompareFunction = static_cast<MTLCompareFunction>((key >> 1) & 7);
        descriptor.depthWriteEnabled = (key & (1u << 4)) ? YES : NO;
    } else {
        descriptor.depthCompareFunction = MTLCompareFunctionAlways;
        descriptor.depthWriteEnabled = NO;
    }
    id<MTLDepthStencilState> state = [g_device.device newDepthStencilStateWithDescriptor:descriptor];
    g_device.depth_states[key] = state;
    return state;
}

id<MTLRenderPipelineState> clear_pipeline(bool color, bool alpha) {
    uint32_t key = (color ? 1u : 0u) | (alpha ? 2u : 0u);
    auto found = g_device.clears.find(key);
    if (found != g_device.clears.end()) {
        return found->second;
    }
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = g_device.fullscreen_vertex;
    descriptor.fragmentFunction = [g_device.library newFunctionWithName:@"clear_fragment"];
    descriptor.colorAttachments[0].pixelFormat = kColorFormat;
    descriptor.colorAttachments[0].writeMask = (color ? MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue : MTLColorWriteMaskNone) |
                                               (alpha ? MTLColorWriteMaskAlpha : MTLColorWriteMaskNone);
    descriptor.depthAttachmentPixelFormat = kDepthFormat;
    id<MTLRenderPipelineState> state = make_pipeline(descriptor, "clear");
    g_device.clears[key] = state;
    return state;
}

id<MTLRenderPipelineState> utility_pipeline(const char* fragment, MTLPixelFormat format, bool blend) {
    MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = g_device.fullscreen_vertex;
    descriptor.fragmentFunction = [g_device.library newFunctionWithName:[NSString stringWithUTF8String:fragment]];
    descriptor.colorAttachments[0].pixelFormat = format;
    if (blend) {
        MTLRenderPipelineColorAttachmentDescriptor* color = descriptor.colorAttachments[0];
        color.blendingEnabled = YES;
        color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        color.sourceAlphaBlendFactor = MTLBlendFactorZero;
        color.destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    return make_pipeline(descriptor, fragment);
}

id<MTLSamplerState> make_sampler(MTLSamplerMinMagFilter filter) {
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = filter;
    descriptor.magFilter = filter;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    return [g_device.device newSamplerStateWithDescriptor:descriptor];
}

id<MTLTexture> make_texture(MTLPixelFormat format, uint32_t width, uint32_t height, uint32_t levels, MTLTextureUsage usage, bool private_storage) {
    MTLTextureDescriptor* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:std::max(width, 1u) height:std::max(height, 1u) mipmapped:NO];
    descriptor.mipmapLevelCount = std::max(levels, 1u);
    descriptor.usage = usage;
    if (private_storage) {
        descriptor.storageMode = MTLStorageModePrivate;
    }
    return [g_device.device newTextureWithDescriptor:descriptor];
}

bool create_render_texture(Target& target, uint32_t width, uint32_t height) {
    target.texture = make_texture(kColorFormat, width, height, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, true);
    target.width = width;
    target.height = height;
    return target.texture != nil;
}

void release_target(Target& target) {
    target = Target{};
}

bool create_efb() {
    g_device.efb_color = make_texture(kColorFormat, scaled(kEfbWidth), scaled(kEfbHeight), 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, true);
    g_device.efb_depth = make_texture(kDepthFormat, scaled(kEfbWidth), scaled(kEfbHeight), 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, true);
    if (!g_device.efb_color || !g_device.efb_depth) {
        return false;
    }
    begin_pass(g_device.efb_color, g_device.efb_depth, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 0));
    end_encoder();
    return true;
}

int requested_scale() {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    int wanted = scale();
    return std::clamp(wanted == 0 ? 4 : wanted, 1, 6);
}

void prepare_texture_pack() {
    const char* directory = game::description().textures_directory;
    if (!directory || !*directory) {
        return;
    }
    std::filesystem::path base = std::filesystem::path(directory);
    g_pack.dump = settings::flag("video.dump_textures", "WP_DUMP_TEXTURES");
    g_pack.dump_directory = base / "dump";
    if (settings::flag("video.custom_textures", "WP_CUSTOM_TEXTURES")) {
        size_t count = g_pack.index.scan(base / "load");
        g_pack.load = count > 0;
        if (count > 0) {
            std::fprintf(stderr, "custom textures: %zu files in %s/load\n", count, directory);
        }
    }
    if (g_pack.dump) {
        std::fprintf(stderr, "dumping textures to %s/dump\n", directory);
    }
}

bool create_device() {
    SDL_MetalView view = static_cast<SDL_MetalView>(video::metal_view());
    if (!view) {
        return false;
    }
    g_device.layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
    g_device.device = MTLCreateSystemDefaultDevice();
    if (!g_device.layer || !g_device.device) {
        return false;
    }
    g_device.layer.device = g_device.device;
    g_device.layer.pixelFormat = kScreenFormat;
    g_device.layer.framebufferOnly = YES;
    g_device.layer.displaySyncEnabled = NO;
    g_device.queue = [g_device.device newCommandQueue];
    std::string source = std::string(metal_shaders::kCommon) + metal_shaders::kVertices + metal_shaders::kTev + metal_shaders::kUtility;
    auto started = std::chrono::steady_clock::now();
    NSString* problem = nil;
    g_device.library = compile_library(source, &problem);
    if (!g_device.library) {
        log::write("video", "the Metal shaders did not build: %s", problem ? [problem UTF8String] : "unknown error");
        return false;
    }
    log::write("video", "Metal shaders built in %.0f ms",
               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    g_device.tev_vertex = [g_device.library newFunctionWithName:@"tev_vertex"];
    g_device.run_vertex = [g_device.library newFunctionWithName:@"run_vertex"];
    g_device.tev_fragment = [g_device.library newFunctionWithName:@"tev_fragment"];
    g_device.tev_fragment_early = [g_device.library newFunctionWithName:@"tev_fragment_early"];
    g_device.fullscreen_vertex = [g_device.library newFunctionWithName:@"fullscreen_vertex"];
    g_device.copy = utility_pipeline("copy_fragment", kColorFormat, false);
    g_device.present = utility_pipeline("present_fragment", kScreenFormat, false);
    g_device.present_smooth = utility_pipeline("present_smooth_fragment", kScreenFormat, false);
    g_device.overlay = utility_pipeline("overlay_fragment", kScreenFormat, false);
    g_device.overlay_blend = utility_pipeline("overlay_fragment", kScreenFormat, true);
    if (!g_device.tev_vertex || !g_device.run_vertex || !g_device.tev_fragment || !g_device.tev_fragment_early || !g_device.copy || !g_device.present ||
        !g_device.present_smooth || !g_device.overlay || !g_device.overlay_blend) {
        return false;
    }
    MTLDepthStencilDescriptor* plain = [MTLDepthStencilDescriptor new];
    plain.depthCompareFunction = MTLCompareFunctionAlways;
    plain.depthWriteEnabled = NO;
    g_device.no_depth = [g_device.device newDepthStencilStateWithDescriptor:plain];
    g_device.linear = make_sampler(MTLSamplerMinMagFilterLinear);
    g_device.point = make_sampler(MTLSamplerMinMagFilterNearest);
    g_device.empty = make_texture(kColorFormat, 1, 1, 1, MTLTextureUsageShaderRead, false);
    const uint32_t black = 0xFF000000u;
    [g_device.empty replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&black bytesPerRow:4];
    g_device.kept_vertices = [g_device.device newBufferWithLength:kKeptVertexBytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    g_device.kept_indices = [g_device.device newBufferWithLength:kKeptIndexBytes options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
    return make_ring(g_device.screen, kScreenRingBytes) && make_ring(g_device.run_vertices, kRunVertexBytes) && make_ring(g_device.run_indices, kRunIndexBytes) &&
           make_ring(g_device.run_rows, kRunRowCapacity * sizeof(float[4])) && g_device.kept_vertices && g_device.kept_indices;
}

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    @autoreleasepool {
        g_scale = requested_scale();
        if (!create_device() || !create_efb()) {
            std::fputs("cannot initialise the Metal renderer\n", stderr);
            log::write("video", "the Metal renderer could not start");
            g_device.failed = true;
            return false;
        }
        const char* name = [[g_device.device name] UTF8String];
        std::fprintf(stderr, "Metal renderer: %s\n", name);
        log::write("video", "Metal renderer: %s", name);
        log::write("video", "the graphics card places the vertices");
        prepare_texture_pack();
        warm_pipelines();
        g_device.ready = true;
    }
    return true;
}

std::string g_copy_dump_prefix;
int g_copy_dump_index = 0;
FILE* g_texture_notes = nullptr;
std::map<uint64_t, bool> g_noted_textures;

void note_texture(uint32_t map, uint32_t address, uint32_t width, uint32_t height, uint32_t format, const char* source) {
    if (!g_texture_notes) {
        return;
    }
    uint64_t key = (static_cast<uint64_t>(address) << 24) ^ (static_cast<uint64_t>(width) << 12) ^ height ^ (static_cast<uint64_t>(format) << 56) ^
                   (static_cast<uint64_t>(map) << 60);
    if (g_noted_textures[key]) {
        return;
    }
    g_noted_textures[key] = true;
    std::fprintf(g_texture_notes, "map %u address %08x %ux%u format %u: %s\n", map, address, width, height, format, source);
}

id<MTLTexture> upload_levels(const std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate) {
    uint32_t levels = static_cast<uint32_t>(images.size());
    if (generate) {
        levels = 1;
        while ((width >> levels) != 0 || (height >> levels) != 0) {
            levels++;
        }
    }
    MTLTextureUsage usage = MTLTextureUsageShaderRead | (generate ? MTLTextureUsageRenderTarget : 0);
    id<MTLTexture> texture = make_texture(kColorFormat, width, height, levels, usage, false);
    for (size_t level = 0; level < images.size(); level++) {
        uint32_t w = std::max(width >> level, 1u);
        uint32_t h = std::max(height >> level, 1u);
        if (images[level].size() < static_cast<size_t>(w) * h) {
            continue;
        }
        [texture replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:level withBytes:images[level].data() bytesPerRow:w * 4];
    }
    if (generate && levels > 1) {
        end_encoder();
        ensure_command();
        @autoreleasepool {
            id<MTLBlitCommandEncoder> blit = [g_command blitCommandEncoder];
            [blit generateMipmapsForTexture:texture];
            [blit endEncoding];
        }
    }
    return texture;
}

id<MTLTexture> custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key, nil);
    if (!inserted) {
        return found->second;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return nil;
    }
    if (mipmaps) {
        for (uint32_t level = 1; (width >> level) != 0 || (height >> level) != 0; level++) {
            const std::filesystem::path* level_path = g_pack.index.find(custom_textures::level_name(key, level));
            std::vector<uint32_t> pixels;
            uint32_t level_width = 0;
            uint32_t level_height = 0;
            if (!level_path || !custom_textures::decode_png(*level_path, pixels, level_width, level_height) ||
                level_width != std::max(width >> level, 1u) || level_height != std::max(height >> level, 1u)) {
                break;
            }
            images.push_back(std::move(pixels));
        }
    }
    bool generate = mipmaps && images.size() == 1 && (width > 1 || height > 1);
    found->second = upload_levels(images, width, height, generate);
    return found->second;
}

void* texture_for(uint32_t map) {
    g_custom_maps &= ~(1u << map);
    TextureRequest request = describe_texture(map);
    if (!request.supported && !g_logged_palette) {
        g_logged_palette = true;
        std::fprintf(stderr, "unsupported texture format %u\n", request.format);
    }
    if (!request.valid) {
        return nullptr;
    }
    uint32_t address = request.address;
    uint32_t width = request.width;
    uint32_t height = request.height;
    uint32_t format = request.format;
    uint32_t levels = request.levels;
    const uint8_t* source = request.source;
    auto copied = g_device.copies.find(address);
    if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && copied->second.logical_width == width &&
        copied->second.logical_height == height && format <= 6) {
        note_texture(map, address, width, height, format, "EFB copy");
        return (__bridge void*)copied->second.converted.texture;
    }
    if (!g_device.write_backs.empty() && write_back_overlaps(address, request.size)) {
        finish_write_backs();
    }
    if (copied != g_device.copies.end() && copied->second.logical_width == width && copied->second.logical_height == height && format <= 6 &&
        copied->second.guest_hash == sample_hash(source, copied->second.bytes)) {
        note_texture(map, address, width, height, format, "EFB copy");
        return (__bridge void*)copied->second.converted.texture;
    }
    if (copied != g_device.copies.end()) {
        char reason[160];
        std::snprintf(reason, sizeof reason, "EFB copy rejected (copy %ux%u, hash %s), read from RAM", copied->second.logical_width, copied->second.logical_height,
                      copied->second.guest_hash == sample_hash(source, copied->second.bytes) ? "same" : "changed");
        note_texture(map, address, width, height, format, reason);
    } else {
        note_texture(map, address, width, height, format, "RAM");
    }
    bool mipmaps = request.mipmaps;
    CachedTexture& entry = g_device.textures[request.key];
    auto use = [&](CachedTexture& texture) -> void* {
        if (texture.custom) {
            g_custom_maps |= 1u << map;
        }
        return (__bridge void*)texture.texture;
    };
    auto matches = [&]() {
        return entry.texture && entry.width == width && entry.height == height && entry.format == format && entry.levels == levels && entry.mipmaps == mipmaps;
    };
    if (matches() && entry.verified_epoch == g_texture_epoch) {
        return use(entry);
    }
    uint64_t hash = texture_hash(request);
    if (matches() && entry.hash == hash) {
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    }
    flush_pending();
    entry.texture = nil;
    entry.custom = false;
    entry.owned = true;
    auto finish = [&]() {
        entry.hash = hash;
        entry.width = width;
        entry.height = height;
        entry.format = format;
        entry.levels = levels;
        entry.mipmaps = mipmaps;
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    };
    bool named = request.supported && copied == g_device.copies.end() && (g_pack.dump || g_pack.load);
    custom_textures::TextureName name;
    id<MTLTexture> custom = nil;
    if (named) {
        name = custom_textures::name_texture(source, request.size, width, height, format, mipmaps, request.tlut);
        if (g_pack.load) {
            std::string resolved = g_pack.index.resolve(name);
            if (!resolved.empty()) {
                custom = custom_texture(resolved, mipmaps);
            }
        }
    }
    if (custom && !g_pack.dump) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    std::vector<std::vector<uint32_t>> pixels = decode_levels(request);
    if (named && g_pack.dump) {
        custom_textures::dump(g_pack.dump_directory, name, pixels, width, height);
    }
    if (custom) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    pixels.resize(levels);
    entry.texture = upload_levels(pixels, width, height, false);
    return finish();
}

MTLSamplerAddressMode metal_wrap(uint32_t wrap) {
    return wrap == 1 ? MTLSamplerAddressModeRepeat : wrap == 2 ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge;
}

const Sampler* sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return &found->second;
    }
    MTLSamplerDescriptor* descriptor = [MTLSamplerDescriptor new];
    descriptor.minFilter = params.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = params.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    descriptor.mipFilter = params.mip_mode == 2 ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
    descriptor.sAddressMode = metal_wrap(params.wrap_s);
    descriptor.tAddressMode = metal_wrap(params.wrap_t);
    descriptor.lodMinClamp = params.min_lod;
    descriptor.lodMaxClamp = params.unlimited_lod ? 1000.0f : params.max_lod;
    Sampler& sampler = g_device.samplers[params.key];
    sampler.state = [g_device.device newSamplerStateWithDescriptor:descriptor];
    sampler.bias = std::exp2(params.lod_bias);
    return &sampler;
}

struct RunDraw {
    uint32_t layout = 0;
    uint32_t stride = 0;
    uint32_t cull = 0;
    uint32_t vertex_offset = 0;
    uint32_t first_index = 0;
    uint32_t indices = 0;
    uint32_t row = 0;
    bool kept = false;
};

struct RunConstants {
    uint32_t base;
    uint32_t stride;
    uint32_t layout;
    uint32_t row;
};

std::vector<RunDraw> g_run_draws;
std::vector<uint8_t> g_run_vertex_bytes;
std::vector<uint16_t> g_run_index_words;
std::vector<std::array<float, 4>> g_run_row_values;
uint32_t g_run_vertex_used = 0;
uint32_t g_run_index_used = 0;
uint32_t g_run_row_used = 0;
uint32_t g_run_palette = kNoPalette;
uint64_t g_run_palette_serial = 0;
uint32_t g_run_kept = 0;
uint32_t g_run_kept_indices = 0;
uint32_t g_kept_vertex_used = 0;
uint32_t g_kept_index_used = 0;
uint32_t g_kept_generation = 1;

bool batch_empty() {
    return g_pending.vertices.empty() && g_run_draws.empty();
}

template <typename T>
void reserve_run(std::vector<T>& values, size_t wanted) {
    if (values.size() < wanted) {
        values.resize(std::max(wanted, values.size() * 2));
    }
}

bool set_batch_state(const PendingBatch& pending) {
    int left = static_cast<int>(((pending.top_left >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset));
    int top = static_cast<int>((pending.top_left & 0x7FF) - scissor_offset_y(pending.scissor_offset));
    int right = static_cast<int>(((pending.bottom_right >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset) + 1);
    int bottom = static_cast<int>((pending.bottom_right & 0x7FF) - scissor_offset_y(pending.scissor_offset) + 1);
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, kEfbWidth);
    bottom = std::min(bottom, kEfbHeight);
    if (right <= left || bottom <= top) {
        return false;
    }
    efb_encoder();
    MTLScissorRect scissor{static_cast<NSUInteger>(scaled(left)), static_cast<NSUInteger>(scaled(top)), static_cast<NSUInteger>(scaled(right - left)),
                           static_cast<NSUInteger>(scaled(bottom - top))};
    [g_encoder setScissorRect:scissor];
    [g_encoder setFragmentBytes:&pending.constants length:sizeof(Constants) atIndex:0];
    float biases[8];
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        id<MTLTexture> view = pending.views[map] ? (__bridge id<MTLTexture>)pending.views[map] : g_device.empty;
        const Sampler* sampler = pending.samplers[map];
        [g_encoder setFragmentTexture:view atIndex:map];
        [g_encoder setFragmentSamplerState:sampler ? sampler->state : g_device.point atIndex:map];
        biases[map] = sampler ? sampler->bias : 1.0f;
    }
    [g_encoder setFragmentBytes:biases length:sizeof biases atIndex:1];
    bind_depth(depth_state(pending.depth));
    return true;
}

void draw_runs() {
    uint32_t vertex_start = 0;
    uint32_t index_start = 0;
    uint32_t row_start = 0;
    bool ok = upload(g_device.run_rows, g_run_row_values.data(), g_run_row_used * sizeof(float[4]), row_start) &&
              upload(g_device.run_vertices, g_run_vertex_bytes.data(), g_run_vertex_used, vertex_start) &&
              upload(g_device.run_indices, g_run_index_words.data(), g_run_index_used * sizeof(uint16_t), index_start);
    id<MTLRenderPipelineState> pipeline = ok ? tev_pipeline(g_pending.constants, g_pending.early_depth, 1, g_pending.blend) : nil;
    if (pipeline && set_batch_state(g_pending)) {
        bind_pipeline(pipeline);
        [g_encoder setVertexBuffer:g_device.run_rows.buffer offset:0 atIndex:1];
        uint32_t first_row = row_start / static_cast<uint32_t>(sizeof(float[4]));
        uint32_t first_index_word = index_start / static_cast<uint32_t>(sizeof(uint16_t));
        int bound = -1;
        for (const RunDraw& draw : g_run_draws) {
            int wanted = draw.kept ? 1 : 0;
            if (bound != wanted) {
                [g_encoder setVertexBuffer:draw.kept ? g_device.kept_vertices : g_device.run_vertices.buffer offset:0 atIndex:0];
                bound = wanted;
            }
            bind_cull(static_cast<int>(draw.cull));
            RunConstants constants;
            constants.base = (draw.kept ? draw.vertex_offset : vertex_start + draw.vertex_offset) / 4;
            constants.stride = draw.stride / 4;
            constants.layout = draw.layout;
            constants.row = first_row + draw.row;
            [g_encoder setVertexBytes:&constants length:sizeof constants atIndex:2];
            uint32_t first_index = draw.kept ? draw.first_index : first_index_word + draw.first_index;
            [g_encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                  indexCount:draw.indices
                                   indexType:MTLIndexTypeUInt16
                                 indexBuffer:draw.kept ? g_device.kept_indices : g_device.run_indices.buffer
                           indexBufferOffset:static_cast<NSUInteger>(first_index) * sizeof(uint16_t)];
        }
        bind_cull(0);
    }
    g_run_draws.clear();
    g_run_kept = 0;
    g_run_kept_indices = 0;
    g_run_vertex_used = 0;
    g_run_index_used = 0;
    g_run_row_used = 0;
    g_run_palette = kNoPalette;
}

void flush_pending() {
    PendingBatch& pending = g_pending;
    if (!g_run_draws.empty()) {
        auto started = std::chrono::steady_clock::now();
        g_batches++;
        g_batch_vertices += g_run_index_used + g_run_kept_indices;
        @autoreleasepool {
            draw_runs();
        }
        g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return;
    }
    if (pending.vertices.empty()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    g_batches++;
    g_batch_vertices += static_cast<uint32_t>(pending.vertices.size());
    @autoreleasepool {
        id<MTLRenderPipelineState> pipeline = tev_pipeline(pending.constants, pending.early_depth, 0, pending.blend);
        if (pipeline && set_batch_state(pending)) {
            bind_pipeline(pipeline);
            bind_cull(0);
            size_t total = pending.vertices.size();
            size_t offset = 0;
            while (offset < total) {
                uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexCapacity));
                uint32_t start = 0;
                if (!upload(g_device.screen, pending.vertices.data() + offset, batch * sizeof(ScreenVertex), start)) {
                    break;
                }
                [g_encoder setVertexBuffer:g_device.screen.buffer offset:start atIndex:0];
                [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:batch];
                offset += batch;
            }
        }
    }
    pending.vertices.clear();
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void select_batch() {
    static uint64_t batched_serial = ~0ull;
    static uint64_t batched_epoch = 0;
    static int batched_scale = 0;
    PendingBatch& pending = g_pending;
    if (!batch_empty() && batched_serial == register_serial() && batched_epoch == g_texture_epoch && batched_scale == g_scale) {
        return;
    }
    const uint32_t* bp = bp_registers();
    Constants constants;
    fill_constants(constants, g_scale);
    void* views[kTextureMaps] = {};
    const Sampler* samplers[kTextureMaps] = {};
    uint32_t used = used_maps(constants);
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        if (used & (1u << map)) {
            views[map] = texture_for(map);
            samplers[map] = sampler_for(map);
        }
    }
    uint32_t pixel_format = bp[0x43] & 7;
    AlphaTestResult alpha_test = alpha_test_result(bp[0xF3]);
    uint32_t blend = (bp[0x41] & 0xFFFF) | (static_cast<uint32_t>(pixel_format == 1) << 16) | (((bp[0x42] >> 8) & 1) << 17) |
                     (static_cast<uint32_t>(!alpha_test.can_pass) << 18);
    uint32_t depth = bp[0x40] & 0x1F;
    bool early_depth = (depth & 1) != 0 && (bp[0x43] & (1u << 6)) != 0 && alpha_test.can_pass && alpha_test.can_fail;
    bool same = !batch_empty() && pending.blend == blend && pending.depth == depth && pending.early_depth == early_depth && pending.top_left == bp[0x20] &&
                pending.bottom_right == bp[0x21] && pending.scissor_offset == bp[kBpScissorOffset] && std::memcmp(&pending.constants, &constants, sizeof constants) == 0 &&
                std::memcmp(pending.views, views, sizeof views) == 0 && std::memcmp(pending.samplers, samplers, sizeof samplers) == 0;
    if (!same) {
        flush_pending();
        pending.constants = constants;
        std::memcpy(pending.views, views, sizeof views);
        std::memcpy(pending.samplers, samplers, sizeof samplers);
        pending.blend = blend;
        pending.depth = depth;
        pending.early_depth = early_depth;
        pending.top_left = bp[0x20];
        pending.bottom_right = bp[0x21];
        pending.scissor_offset = bp[kBpScissorOffset];
    }
    batched_serial = register_serial();
    batched_epoch = g_texture_epoch;
    batched_scale = g_scale;
}

bool add_run(const VertexRun& run, uint32_t bytes, uint32_t index_count) {
    uint32_t rows = run.row_count + (run.palette ? kPaletteRows + kNormalRows : 0);
    if (!g_pending.vertices.empty() || g_run_vertex_used + bytes > kRunVertexBytes / 2 || (g_run_index_used + index_count + 1) * sizeof(uint16_t) > kRunIndexBytes / 2 ||
        g_run_row_used + rows > kRunRowCapacity / 2) {
        flush_pending();
    }
    select_batch();
    g_run_index_used = (g_run_index_used + 1u) & ~1u;
    reserve_run(g_run_vertex_bytes, g_run_vertex_used + bytes);
    reserve_run(g_run_index_words, g_run_index_used + index_count);
    reserve_run(g_run_row_values, g_run_row_used + rows);
    if (run.palette && (g_run_palette == kNoPalette || g_run_palette_serial != run.palette_serial)) {
        std::memcpy(g_run_row_values.data() + g_run_row_used, run.palette, kPaletteRows * sizeof(float[4]));
        g_run_palette = g_run_row_used;
        g_run_palette_serial = run.palette_serial;
        g_run_row_used += kPaletteRows;
        std::memcpy(g_run_row_values.data() + g_run_row_used, run.palette + kNormalWords, kNormalRows * sizeof(float[4]));
        g_run_row_used += kNormalRows;
    }
    std::memcpy(g_run_row_values.data() + g_run_row_used, run.rows, run.row_count * sizeof(float[4]));
    if (run.palette) {
        g_run_row_values[g_run_row_used + 3][2] = static_cast<float>(static_cast<int>(g_run_palette) - static_cast<int>(g_run_row_used));
    }
    RunDraw draw;
    draw.layout = (run.normal ? 1u : 0u) | (run.matrix_indices ? 2u : 0u) | (run.texture_inputs << 2);
    draw.stride = run.stride;
    draw.cull = run.cull;
    draw.vertex_offset = g_run_vertex_used;
    draw.first_index = g_run_index_used;
    draw.indices = run.indices;
    draw.row = g_run_row_used;
    g_run_draws.push_back(draw);
    g_run_row_used += run.row_count;
    return true;
}

bool run_copy(const Target& target, uint32_t target_width, uint32_t target_height, int x, int y, int width, int height, uint32_t format, bool intensity,
              bool alpha, bool depth, const CopyFilter& filter) {
    struct CopyConstants {
        uint32_t params[4];
        float region[4];
        uint32_t filter[4];
        float rows[4];
        float extent[4];
    } constants{};
    uint32_t sum = filter.coefficients[0] + filter.coefficients[1] + filter.coefficients[2];
    int efb_height = scaled(kEfbHeight);
    int top = filter.clamp_top ? scaled(y) : 0;
    int bottom = (filter.clamp_bottom ? scaled(y + height) : efb_height) - 1;
    constants.params[0] = format;
    constants.params[1] = intensity ? 1u : 0u;
    constants.params[2] = alpha ? 1u : 0u;
    constants.params[3] = depth ? 1u : 0u;
    constants.region[0] = static_cast<float>(scaled(x));
    constants.region[1] = static_cast<float>(scaled(y));
    constants.region[2] = static_cast<float>(scaled(x + width) - scaled(x));
    constants.region[3] = static_cast<float>(scaled(y + height) - scaled(y));
    constants.filter[0] = filter.coefficients[0];
    constants.filter[1] = filter.coefficients[1];
    constants.filter[2] = filter.coefficients[2];
    constants.filter[3] = sum >= 128 ? 1u : 0u;
    constants.rows[0] = 1.0f / filter.gamma;
    constants.rows[1] = static_cast<float>(top) + 0.5f;
    constants.rows[2] = static_cast<float>(bottom) + 0.5f;
    constants.extent[0] = static_cast<float>(scaled(kEfbWidth));
    constants.extent[1] = static_cast<float>(efb_height);
    @autoreleasepool {
        begin_pass(target.texture, nil, MTLLoadActionDontCare, MTLClearColorMake(0, 0, 0, 0));
        set_viewport(0, 0, target_width, target_height);
        bind_pipeline(g_device.copy);
        [g_encoder setFragmentBytes:&constants length:sizeof constants atIndex:0];
        [g_encoder setFragmentTexture:g_device.efb_color atIndex:0];
        [g_encoder setFragmentTexture:g_device.efb_depth atIndex:1];
        [g_encoder setFragmentSamplerState:g_device.linear atIndex:0];
        [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        end_encoder();
    }
    return true;
}

id<MTLBuffer> take_buffer() {
    if (!g_device.free_buffers.empty()) {
        id<MTLBuffer> buffer = g_device.free_buffers.back();
        g_device.free_buffers.pop_back();
        return buffer;
    }
    return [g_device.device newBufferWithLength:static_cast<NSUInteger>(kEfbWidth) * kEfbHeight * 4 options:MTLResourceStorageModeShared];
}

void read_back(id<MTLTexture> texture, uint32_t width, uint32_t height, id<MTLBuffer> buffer) {
    end_encoder();
    ensure_command();
    @autoreleasepool {
        id<MTLBlitCommandEncoder> blit = [g_command blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:width * 4
            destinationBytesPerImage:static_cast<NSUInteger>(width) * height * 4];
        [blit endEncoding];
    }
}

void complete_write_backs(bool wait);

bool queue_write_back(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                      bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back.texture && !create_render_texture(g_device.write_back, kEfbWidth, kEfbHeight)) {
        return false;
    }
    if (g_device.write_backs.size() >= kMaxPendingWriteBacks) {
        complete_write_backs(true);
    }
    uint32_t texture_format = copy_texture_format(format);
    Layout layout = layout_for(texture_format);
    uint32_t blocks_x = (logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t row_bytes = stride * 32;
    uint32_t bytes = block_bytes(texture_format);
    size_t span = static_cast<size_t>(blocks_y - 1) * row_bytes + blocks_x * bytes;
    if (blocks_x * bytes > row_bytes || !guest_range_valid(address, span) || (address & kAddressMask) < kLowMemoryEnd) {
        return false;
    }
    if (!run_copy(g_device.write_back, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter)) {
        return false;
    }
    PendingWriteBack pending;
    pending.buffer = take_buffer();
    read_back(g_device.write_back.texture, logical_width, logical_height, pending.buffer);
    pending.serial = current_serial();
    pending.address = address;
    pending.start = address & kAddressMask;
    pending.end = pending.start + static_cast<uint32_t>(span);
    pending.row_bytes = row_bytes;
    pending.logical_width = logical_width;
    pending.logical_height = logical_height;
    pending.texture_format = texture_format;
    pending.frame = g_frame;
    g_device.write_backs.push_back(pending);
    return true;
}

bool write_back(const PendingWriteBack& pending, bool wait) {
    auto wait_start = std::chrono::steady_clock::now();
    if (!serial_done(pending.serial)) {
        if (!wait) {
            return false;
        }
        wait_serial(pending.serial);
    }
    Layout layout = layout_for(pending.texture_format);
    uint32_t blocks_x = (pending.logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (pending.logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t bytes = block_bytes(pending.texture_format);
    size_t pitch = static_cast<size_t>(pending.logical_width) * 4;
    const uint8_t* pixels = static_cast<const uint8_t*>([pending.buffer contents]);
    g_write_back_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_start).count();
    g_write_backs++;
    uint8_t block[64];
    for (uint32_t by = 0; by < blocks_y; by++) {
        uint8_t* row = host(pending.address + by * pending.row_bytes);
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            std::memset(block, 0, sizeof block);
            for (uint32_t ty = 0; ty < layout.block_height; ty++) {
                uint32_t py = std::min(by * layout.block_height + ty, pending.logical_height - 1);
                for (uint32_t tx = 0; tx < layout.block_width; tx++) {
                    uint32_t px = std::min(bx * layout.block_width + tx, pending.logical_width - 1);
                    encode_texel(block, pending.texture_format, ty * layout.block_width + tx, pixels + py * pitch + px * 4);
                }
            }
            std::memcpy(row + bx * bytes, block, bytes);
        }
    }
    return true;
}

void complete_write_backs(bool wait) {
    size_t done = 0;
    for (const PendingWriteBack& pending : g_device.write_backs) {
        if (!write_back(pending, wait || pending.frame + kWriteBackFrames <= g_frame)) {
            break;
        }
        done++;
        g_device.free_buffers.push_back(pending.buffer);
        auto copied = g_device.copies.find(pending.address);
        if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && --copied->second.pending_writes == 0) {
            CopiedTexture& entry = copied->second;
            entry.guest_hash = guest_range_valid(pending.address, entry.bytes) ? sample_hash(host(pending.address), entry.bytes) : 0;
        }
    }
    g_device.write_backs.erase(g_device.write_backs.begin(), g_device.write_backs.begin() + static_cast<std::ptrdiff_t>(done));
}

bool read_target(id<MTLTexture> texture, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels) {
    if (!texture || width == 0 || height == 0) {
        return false;
    }
    id<MTLBuffer> buffer = [g_device.device newBufferWithLength:static_cast<NSUInteger>(width) * height * 4 options:MTLResourceStorageModeShared];
    read_back(texture, width, height, buffer);
    wait_serial(current_serial());
    const uint8_t* bytes = static_cast<const uint8_t*>([buffer contents]);
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (static_cast<uint32_t>(bytes[i * 4]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | bytes[i * 4 + 2];
    }
    return true;
}

void dump_target(id<MTLTexture> texture, uint32_t width, uint32_t height, const char* suffix) {
    std::vector<uint32_t> pixels;
    if (texture && read_target(texture, width, height, pixels)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_target(g_device.efb_color, static_cast<uint32_t>(scaled(kEfbWidth)), static_cast<uint32_t>(scaled(kEfbHeight)), "_efb");
    dump_target(entry.converted.texture, entry.converted.width, entry.converted.height, "");
}

void apply_scale() {
    int scale = requested_scale();
    if (scale == g_scale) {
        return;
    }
    flush_pending();
    end_encoder();
    int previous = g_scale;
    g_scale = scale;
    if (!create_efb()) {
        g_scale = previous;
        if (!create_efb()) {
            std::fputs("cannot recreate the EFB after a scale change\n", stderr);
            g_device.ready = false;
            g_device.failed = true;
        }
    }
}

void upload_image(Image& target, const options::Image& image) {
    if (image.pixels.size() != static_cast<size_t>(image.width) * image.height || image.width == 0 || image.height == 0) {
        return;
    }
    id<MTLTexture> texture = make_texture(kScreenFormat, image.width, image.height, 1, MTLTextureUsageShaderRead, false);
    [texture replaceRegion:MTLRegionMake2D(0, 0, image.width, image.height) mipmapLevel:0 withBytes:image.pixels.data() bytesPerRow:image.width * 4];
    target.texture = texture;
    target.width = image.width;
    target.height = image.height;
}

void draw_image(id<MTLTexture> texture, const options::Rect& placed, bool blend) {
    if (!texture || placed.width <= 0 || placed.height <= 0) {
        return;
    }
    set_viewport(placed.x, placed.y, placed.width, placed.height);
    bind_pipeline(blend ? g_device.overlay_blend : g_device.overlay);
    [g_encoder setFragmentTexture:texture atIndex:0];
    [g_encoder setFragmentSamplerState:g_device.linear atIndex:0];
    [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

void draw_overlay(int client_width, int client_height, int bar) {
    options::Image image;
    if (perf::take(g_device.hud_version, image, g_device.hud_visible, g_device.hud_corner) && !image.pixels.empty()) {
        upload_image(g_device.hud_image, image);
    }
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty()) {
        upload_image(g_device.overlay_image, image);
    }
    if (hint::take(g_device.hint_version, image, g_device.hint_visible) && !image.pixels.empty()) {
        upload_image(g_device.hint_image, image);
    }
    if (g_device.hud_visible) {
        options::Rect placed = perf::corner_rect(client_width, client_height - bar, static_cast<int>(g_device.hud_image.width), static_cast<int>(g_device.hud_image.height),
                                                 g_device.hud_corner);
        placed.y += bar;
        draw_image(g_device.hud_image.texture, placed, true);
    }
    if (g_device.hint_visible) {
        options::Rect placed = hint::place(client_width, client_height - bar, static_cast<int>(g_device.hint_image.width), static_cast<int>(g_device.hint_image.height));
        placed.y += bar;
        draw_image(g_device.hint_image.texture, placed, true);
    }
    options::Image knob;
    if (touch::take(g_device.touch_version, image, knob, g_device.touch_visible, client_width, client_height) && g_device.touch_visible && !image.pixels.empty()) {
        upload_image(g_device.touch_image, image);
        upload_image(g_device.knob_image, knob);
    }
    if (g_device.touch_visible && !g_device.overlay_visible) {
        draw_image(g_device.touch_image.texture, options::Rect{0, 0, client_width, client_height}, true);
        draw_image(g_device.knob_image.texture, touch::knob_rect(client_height), true);
    }
    if (g_device.overlay_visible) {
        draw_image(g_device.overlay_image.texture,
                   options::place(client_width, client_height, static_cast<int>(g_device.overlay_image.width), static_cast<int>(g_device.overlay_image.height)), true);
    }
}

void draw_titlebar(int client_width, int bar) {
    options::Image image;
    if (titlebar::take(g_device.bar_version, image)) {
        upload_image(g_device.bar_image, image);
    }
    if (bar <= 0 || !g_device.bar_image.texture) {
        return;
    }
    draw_image(g_device.bar_image.texture, options::Rect{0, 0, client_width, bar}, false);
}

}

const char* api_name() {
    return "Metal";
}

int auto_scale() {
    return 4;
}

bool interpolating() {
    return false;
}

bool transforms_vertices() {
    static const settings::LiveFlag enabled("video.gpu_vertices", "WP_GPU_VERTICES");
    return enabled() && initialize();
}

bool begin_run(const VertexRun& run, uint8_t** vertices, uint16_t** indices) {
    uint32_t bytes = run.vertices * run.stride;
    if (!add_run(run, bytes, run.indices)) {
        return false;
    }
    *vertices = g_run_vertex_bytes.data() + g_run_vertex_used;
    *indices = g_run_index_words.data() + g_run_index_used;
    g_run_vertex_used += bytes;
    g_run_index_used += run.indices;
    return true;
}

bool draw_kept(const VertexRun& run, const KeptRun& kept) {
    if (kept.generation != g_kept_generation || !add_run(run, 0, 0)) {
        return false;
    }
    RunDraw& draw = g_run_draws.back();
    draw.kept = true;
    draw.vertex_offset = kept.vertex;
    draw.first_index = kept.index;
    g_run_kept++;
    g_run_kept_indices += run.indices;
    return true;
}

bool keep_run(const VertexRun& run, const uint8_t* vertices, const uint16_t* indices, KeptRun& kept, bool) {
    if (!initialize()) {
        return false;
    }
    uint32_t bytes = run.vertices * run.stride;
    uint32_t vertex = (g_kept_vertex_used + run.stride - 1) / run.stride * run.stride;
    uint32_t index = (g_kept_index_used + 1u) & ~1u;
    if (vertex + bytes > kKeptVertexBytes || (index + run.indices) * sizeof(uint16_t) > kKeptIndexBytes) {
        return false;
    }
    kept.vertex = vertex;
    kept.index = index;
    kept.generation = g_kept_generation;
    g_kept_vertex_used = vertex + bytes;
    g_kept_index_used = index + run.indices;
    std::memcpy(static_cast<uint8_t*>([g_device.kept_vertices contents]) + kept.vertex, vertices, bytes);
    std::memcpy(static_cast<uint8_t*>([g_device.kept_indices contents]) + static_cast<size_t>(kept.index) * sizeof(uint16_t), indices,
                static_cast<size_t>(run.indices) * sizeof(uint16_t));
    return true;
}

void reset_kept() {
    if (!g_device.ready) {
        return;
    }
    if (g_run_kept != 0) {
        flush_pending();
    }
    wait_idle();
    g_kept_generation++;
    g_kept_vertex_used = 0;
    g_kept_index_used = 0;
}

void finish_frame() {}

Canvas canvas() {
    return {};
}

Canvas shared_canvas() {
    return {};
}

void set_canvas_kind(int) {}

bool canvas_copy(uint32_t) {
    return false;
}

int64_t service_presents() {
    return -1;
}

uint32_t take_interpolated_frames() {
    return 0;
}

uint32_t take_late_frames() {
    return 0;
}

GpuFigures gpu_figures() {
    return {};
}

void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource*) {
    if (count == 0 || !initialize()) {
        return;
    }
    if (!g_run_draws.empty()) {
        flush_pending();
    }
    select_batch();
    g_pending.vertices.insert(g_pending.vertices.end(), vertices, vertices + count);
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    batches = g_batches;
    vertices = g_batch_vertices;
    seconds = g_batch_seconds;
    g_batches = 0;
    g_batch_vertices = 0;
    g_batch_seconds = 0.0;
}

void take_copy_statistics(uint32_t& write_backs, double& wait_seconds) {
    write_backs = g_write_backs;
    wait_seconds = g_write_back_seconds;
    g_write_backs = 0;
    g_write_back_seconds = 0.0;
}

bool guest_range_valid(uint32_t address, size_t size) {
    return static_cast<size_t>(address & kAddressMask) + size <= kPhysicalSize;
}

void invalidate_textures() {
    g_texture_epoch++;
}

void finish_write_backs() {
    complete_write_backs(true);
}

bool write_back_overlaps(uint32_t address, size_t size) {
    uint32_t start = address & kAddressMask;
    uint32_t end = start + static_cast<uint32_t>(size);
    for (const PendingWriteBack& pending : g_device.write_backs) {
        if (start < pending.end && pending.start < end) {
            return true;
        }
    }
    return false;
}

void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    if (format > 12) {
        if (!g_logged_copy_format) {
            g_logged_copy_format = true;
            std::fprintf(stderr, "unsupported EFB copy format %u\n", format);
        }
        flush_pending();
        g_device.copies.erase(address);
        return;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    flush_pending();
    CopiedTexture& entry = g_device.copies[address];
    constexpr uint32_t kRecurringFrames = 2;
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    uint32_t logical_width = half ? std::max(1u, w / 2) : w;
    uint32_t logical_height = half ? std::max(1u, h / 2) : h;
    bool recurring = entry.has_view && g_frame - entry.copied_frame <= kRecurringFrames && entry.copied_format == format && entry.logical_width == logical_width &&
                     entry.logical_height == logical_height;
    entry.copied_frame = static_cast<uint32_t>(g_frame);
    entry.copied_format = format;
    entry.logical_width = logical_width;
    entry.logical_height = logical_height;
    uint32_t target_width = std::max(1u, entry.logical_width * g_scale);
    uint32_t target_height = std::max(1u, entry.logical_height * g_scale);
    @autoreleasepool {
        if (!entry.converted.texture || entry.converted.width != target_width || entry.converted.height != target_height) {
            release_target(entry.converted);
            entry.has_view = false;
            if (!create_render_texture(entry.converted, target_width, target_height)) {
                g_device.copies.erase(address);
                return;
            }
        }
        run_copy(entry.converted, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter);
        entry.has_view = true;
        entry.bytes = entry.logical_width * entry.logical_height * copy_bits(format) / 8;
        if (!recurring &&
            queue_write_back(address, stride, x, y, width, height, entry.logical_width, entry.logical_height, format, intensity, alpha || depth, depth, filter)) {
            entry.pending_writes++;
        } else if (entry.pending_writes == 0) {
            entry.guest_hash = guest_range_valid(address, entry.bytes) ? sample_hash(host(address), entry.bytes) : 0;
        }
        dump_copy(entry);
    }
}

void finish_copies() {
    if (g_device.ready) {
        complete_write_backs(true);
    }
}

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    @autoreleasepool {
        flush_pending();
        complete_write_backs(false);
        uint32_t slot = static_cast<uint32_t>(g_frame % 2);
        uint64_t finished = current_serial();
        submit();
        wait_serial(g_frame_serials[slot]);
        g_frame_serials[slot] = finished;
        {
            std::lock_guard<std::recursive_mutex> hold(g_compile_lock);
            static auto logged = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (g_variants_ready != g_reported_ready && now - logged >= std::chrono::seconds(1)) {
                logged = now;
                g_reported_ready = g_variants_ready;
                log::write("video", "%u pixel shader variants ready, %u refused, %u building in the background", g_variants_ready, g_variants_failed, g_compiling);
            }
        }
        g_frame++;
        g_texture_epoch++;
        x = std::max(0, x);
        y = std::max(0, y);
        width = std::min(width, kEfbWidth - x);
        height = std::min(height, kEfbHeight - y);
        if (width <= 0 || height <= 0) {
            return;
        }
        uint32_t physical_width = static_cast<uint32_t>(scaled(width));
        uint32_t physical_height = static_cast<uint32_t>(scaled(height));
        if (!g_device.frame.texture || g_device.frame.width != physical_width || g_device.frame.height != physical_height) {
            release_target(g_device.frame);
            if (!create_render_texture(g_device.frame, physical_width, physical_height)) {
                release_target(g_device.frame);
                return;
            }
        }
        run_copy(g_device.frame, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
    }
}

bool present_frame(void* window_handle, double aspect) {
    if (!g_device.ready || !g_device.frame.texture || !window_handle) {
        return false;
    }
    SDL_Window* window = static_cast<SDL_Window*>(window_handle);
    int client_width = 0;
    int client_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &client_width, &client_height) || client_width <= 0 || client_height <= 0) {
        return false;
    }
    @autoreleasepool {
        flush_pending();
        CGSize size = CGSizeMake(client_width, client_height);
        if (!CGSizeEqualToSize(g_device.layer.drawableSize, size)) {
            g_device.layer.drawableSize = size;
        }
        static const settings::LiveFlag vsync("video.vsync", "WP_VSYNC");
        BOOL synced = vsync() ? YES : NO;
        if (g_device.layer.displaySyncEnabled != synced) {
            g_device.layer.displaySyncEnabled = synced;
        }
        id<CAMetalDrawable> drawable = [g_device.layer nextDrawable];
        if (drawable) {
            begin_pass(drawable.texture, nil, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 1));
            int bar = titlebar::height(window);
            int area = std::max(1, client_height - bar);
            double width = client_width;
            double height = width / aspect;
            if (height > area) {
                height = area;
                width = height * aspect;
            }
            set_viewport(std::floor((client_width - width) / 2), std::floor(bar + (area - height) / 2), std::floor(width), std::floor(height));
            static const settings::LiveFlag smooth("video.smooth", "WP_SMOOTH");
            bool reduce = smooth() && (g_device.frame.width > width * 1.25 || g_device.frame.height > height * 1.25);
            bind_pipeline(reduce ? g_device.present_smooth : g_device.present);
            color::Adjust colors = color::current();
            float veil = gx::veil_level();
            colors.brightness *= 1.0f - veil;
            colors.saturation *= 1.0f - veil;
            float adjust[4] = {colors.brightness, colors.contrast, colors.saturation, 1.0f / colors.gamma};
            [g_encoder setFragmentBytes:adjust length:sizeof adjust atIndex:0];
            [g_encoder setFragmentTexture:g_device.frame.texture atIndex:0];
            [g_encoder setFragmentSamplerState:g_device.linear atIndex:0];
            [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            draw_overlay(client_width, client_height, bar);
            draw_titlebar(client_width, bar);
            end_encoder();
            [g_command presentDrawable:drawable];
        }
        submit();
    }
    video::park_while_background();
    @autoreleasepool {
        apply_scale();
    }
    return true;
}

void dump_copies(const char* prefix) {
    g_copy_dump_prefix = prefix ? prefix : "";
    g_copy_dump_index = 0;
    if (g_texture_notes) {
        std::fclose(g_texture_notes);
        g_texture_notes = nullptr;
    }
    g_noted_textures.clear();
    if (prefix) {
        g_texture_notes = std::fopen((g_copy_dump_prefix + "_textures.txt").c_str(), "w");
    }
}

bool read_recent(std::vector<RecentImage>&) {
    return false;
}

bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    if (!g_device.ready || !g_device.frame.texture) {
        return false;
    }
    @autoreleasepool {
        flush_pending();
        width = g_device.frame.width;
        height = g_device.frame.height;
        return read_target(g_device.frame.texture, width, height, pixels);
    }
}

void clear(int x, int y, int width, int height) {
    if (!initialize()) {
        return;
    }
    flush_pending();
    const uint32_t* bp = bp_registers();
    uint32_t pixel_format = bp[0x43] & 7;
    bool color_enable = (bp[0x41] & (1u << 3)) != 0;
    bool has_alpha = pixel_format != 0 && pixel_format != 2 && pixel_format != 3;
    bool alpha_enable = (bp[0x41] & (1u << 4)) != 0 && has_alpha;
    bool depth_enable = (bp[0x40] & (1u << 4)) != 0;
    if (!color_enable && !alpha_enable && !depth_enable) {
        return;
    }
    uint32_t red = bp[0x4F] & 0xFF;
    uint32_t alpha = (bp[0x4F] >> 8) & 0xFF;
    uint32_t green = (bp[0x50] >> 8) & 0xFF;
    uint32_t blue = bp[0x50] & 0xFF;
    uint32_t depth = bp[0x51] & 0xFFFFFF;
    if (pixel_format == 1) {
        red = quantize(red, 6);
        green = quantize(green, 6);
        blue = quantize(blue, 6);
        alpha = quantize(alpha, 6);
    } else if (pixel_format == 2) {
        red = quantize(red, 5);
        green = quantize(green, 6);
        blue = quantize(blue, 5);
        depth = (depth & 0xFFFF00) | (depth >> 16);
    }
    if (!has_alpha) {
        alpha_enable = true;
        alpha = 0;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    @autoreleasepool {
        efb_encoder();
        MTLScissorRect scissor{static_cast<NSUInteger>(scaled(x)), static_cast<NSUInteger>(scaled(y)), static_cast<NSUInteger>(scaled(x + width) - scaled(x)),
                               static_cast<NSUInteger>(scaled(y + height) - scaled(y))};
        [g_encoder setScissorRect:scissor];
        id<MTLRenderPipelineState> pipeline = clear_pipeline(color_enable, alpha_enable);
        if (!pipeline) {
            return;
        }
        bind_pipeline(pipeline);
        bind_depth(depth_enable ? depth_state(1u | (7u << 1) | (1u << 4)) : g_device.no_depth);
        bind_cull(0);
        struct {
            float color[4];
            float depth;
            float padding[3];
        } values{{red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f}, static_cast<float>(depth) / 16777215.0f, {}};
        [g_encoder setFragmentBytes:&values length:sizeof values atIndex:0];
        [g_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
}

}
