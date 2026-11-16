#include "wp/gx_render.h"
#include "wp/gx.h"

#include "gx_render_common.h"
#include "../platform/gl_window.h"

#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "wp/color.h"
#include "wp/custom_textures.h"
#include "wp/frame_pacing.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/hint.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/options.h"
#include "wp/perf_overlay.h"
#include "wp/screenshot.h"
#include "wp/settings.h"
#include "wp/titlebar.h"
#include "wp/touch.h"
#include "wp/video.h"

namespace wp::gx::render::vulkan {

void finish_write_backs();
bool write_back_overlaps(uint32_t address, size_t size);
void finish_frame();

namespace {

const uint32_t kFullscreenVertexCode[] =
#include "vk_fullscreen_vert.inc"
    ;
const uint32_t kPresentCode[] =
#include "vk_present_frag.inc"
    ;
const uint32_t kPresentSmoothCode[] =
#include "vk_present_smooth_frag.inc"
    ;
const uint32_t kOverlayCode[] =
#include "vk_overlay_frag.inc"
    ;
const uint32_t kClearCode[] =
#include "vk_clear_frag.inc"
    ;
const uint32_t kCopyCode[] =
#include "vk_copy_frag.inc"
    ;
const uint32_t kTevVertexCode[] =
#include "vk_tev_vert.inc"
    ;
const uint32_t kRunVertexCode[] =
#include "vk_run_vert.inc"
    ;
const uint32_t kTevCode[] =
#include "vk_tev_frag.inc"
    ;
const uint32_t kTevEarlyCode[] =
#include "vk_tev_early_frag.inc"
    ;
const uint32_t kTevSingleCode[] =
#include "vk_tev_single_frag.inc"
    ;
const uint32_t kTevEarlySingleCode[] =
#include "vk_tev_early_single_frag.inc"
    ;

struct ShaderCode {
    const uint32_t* code;
    size_t bytes;
};

#include "vk_sized_shaders.inc"

constexpr uint32_t kVertexCapacity = 1 << 16;
constexpr uint32_t kRingSegments = 4;
constexpr uint32_t kScreenRingBytes = 64u << 20;
constexpr uint32_t kRunVertexBytes = 32u << 20;
constexpr uint32_t kRunIndexBytes = 8u << 20;
constexpr uint32_t kRunRowCapacity = 1u << 17;
constexpr uint32_t kRunRowBatch = kRunRowCapacity / kRingSegments;
constexpr uint32_t kUniformRingBytes = 16u << 20;
constexpr uint32_t kStagingRingBytes = 64u << 20;
constexpr uint32_t kKeptVertexBytes = 64u << 20;
constexpr uint32_t kKeptIndexBytes = 16u << 20;
constexpr uint32_t kNoPalette = ~0u;
constexpr uint64_t kWriteBackFrames = 2;
constexpr size_t kMaxPendingWriteBacks = 32;
constexpr uint32_t kLowMemoryEnd = 0x4000;
constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkDeviceSize kBlockBytes = 64ull << 20;
constexpr size_t kMaxSubmissions = 8;
constexpr uint32_t kSetsPerPool = 1024;
constexpr uint32_t kSpecConstants = 91;
constexpr uint32_t kScreenWords = sizeof(ScreenVertex) / sizeof(float);
constexpr uint32_t kMovedRingBytes = 64u << 20;
constexpr size_t kMaxBetween = 3;
constexpr size_t kMaxCameras = 8;
constexpr uint32_t kMotionRows = kMaxCameras * 4;

struct Context {
    bool probed = false;
    bool usable = false;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    bool timestamps = false;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    bool dual_source = false;
    bool depth_clamp = false;
    bool load_none = false;
    bool anisotropy = false;
    bool depth_copies = false;
    VkDeviceSize uniform_alignment = 256;
    VkDeviceSize storage_alignment = 256;
};

Context g_vk;

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    int block = -1;
};

struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize size = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;
};

std::vector<Block> g_blocks;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation memory;
    VkFormat format = kColorFormat;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
};

struct Target {
    Image* image = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CachedTexture {
    Image* texture = nullptr;
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
    std::unordered_map<std::string, Image*> loaded;
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
    bool whole_canvas = false;
};

struct PendingWriteBack {
    Buffer* buffer = nullptr;
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

struct Ring {
    Buffer buffer;
    uint32_t capacity = 0;
    uint32_t cursor = 0;
    uint32_t segment = 0;
    uint64_t used_by[kRingSegments] = {};
    uint32_t switches = 0;
};

struct Overlay {
    Image* image = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct Submission {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE;
    VkCommandBuffer draw = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool times = VK_NULL_HANDLE;
    bool timed = false;
    uint32_t next_query = 2;
    std::vector<uint8_t> pass_kinds;
    uint64_t serial = 0;
    bool busy = false;
    bool uploads = false;
    std::vector<VkDescriptorPool> pools;
    size_t pool_used = 0;
    std::unordered_map<uint64_t, VkDescriptorSet> map_sets;
};

struct Swapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> rendered;
    std::vector<VkSemaphore> acquired;
    uint32_t next_acquire = 0;
    bool stale = false;
    bool vsync = false;
};

struct Device {
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkDescriptorSetLayout draw_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout maps_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout copy_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout single_layout = VK_NULL_HANDLE;
    VkPipelineLayout tev_layout = VK_NULL_HANDLE;
    VkPipelineLayout copy_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout single_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout clear_layout = VK_NULL_HANDLE;
    VkDescriptorPool static_pool = VK_NULL_HANDLE;
    VkDescriptorSet draw_sets[4] = {};
    VkDescriptorSet copy_set = VK_NULL_HANDLE;
    VkShaderModule fullscreen_vertex = VK_NULL_HANDLE;
    VkShaderModule present_fragment = VK_NULL_HANDLE;
    VkShaderModule present_smooth_fragment = VK_NULL_HANDLE;
    VkShaderModule overlay_fragment = VK_NULL_HANDLE;
    VkShaderModule clear_fragment = VK_NULL_HANDLE;
    VkShaderModule copy_fragment = VK_NULL_HANDLE;
    VkShaderModule tev_vertex = VK_NULL_HANDLE;
    VkShaderModule run_vertex = VK_NULL_HANDLE;
    VkShaderModule tev_fragment[2] = {};
    VkShaderModule tev_vertex_sized[8] = {};
    VkShaderModule run_vertex_sized[8] = {};
    VkShaderModule tev_fragment_sized[2][8] = {};
    VkRenderPass efb_passes[2][3] = {};
    VkRenderPass target_pass = VK_NULL_HANDLE;
    VkPipeline copy = VK_NULL_HANDLE;
    std::map<uint32_t, VkPipeline> clears;
    std::map<uint64_t, VkPipeline> screen_pipelines;
    VkSampler linear = VK_NULL_HANDLE;
    VkSampler point = VK_NULL_HANDLE;
    Image* empty = nullptr;
    Image* efb_color = nullptr;
    Image* efb_depth = nullptr;
    VkFramebuffer efb_framebuffer = VK_NULL_HANDLE;
    Target frame;
    Target interpolated[kMaxBetween];
    Target write_back;
    std::vector<Buffer*> free_buffers;
    std::vector<PendingWriteBack> write_backs;
    Ring screen;
    Ring run_vertices;
    Ring run_indices;
    Ring run_rows;
    Ring moved;
    Ring uniforms;
    Ring staging;
    Buffer kept_vertices;
    Buffer kept_indices;
    Overlay overlay_image;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    Overlay touch_image;
    uint64_t touch_version = 0;
    bool touch_visible = false;
    Overlay knob_image;
    Overlay pressed_image;
    Overlay base_image;
    uint64_t extras_version = 0;
    Overlay hud_image;
    uint64_t hud_version = 0;
    bool hud_visible = false;
    perf::Corner hud_corner = perf::Corner::TopLeft;
    Overlay hint_image;
    uint64_t hint_version = 0;
    bool hint_visible = false;
    Overlay bar_image;
    uint64_t bar_version = 0;
    std::map<uint64_t, VkSampler> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    Swapchain chain;
    bool failed = false;
    bool ready = false;
};

struct BatchState {
    Constants constants;
    Image* views[kTextureMaps] = {};
    VkSampler samplers[kTextureMaps] = {};
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    bool keeps_all = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
    uint32_t scissor_offset = 0;
    int canvas_kind = kInPicture;
};

struct PendingBatch : BatchState {
    std::vector<ScreenVertex> vertices;
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
uint32_t g_efb_passes = 0;
uint32_t g_cleared_passes = 0;
uint32_t g_submits = 0;
double g_fence_wait = 0.0;
double g_frame_wait = 0.0;
double g_ring_wait = 0.0;
double g_present_wait = 0.0;
double g_idle_wait = 0.0;
uint32_t g_idle_waits = 0;

double g_gpu_time = 0.0;
constexpr uint32_t kTimeQueries = 512;
double g_pass_time[4] = {};
uint32_t g_pass_count[4] = {};

void read_gpu_time(struct Submission& submission);

double seconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}
int g_shown_height = kEfbHeight;
int g_anisotropy = 1;
bool g_recording = false;
bool g_record_broken = false;
bool g_efb_before_valid = false;
std::vector<std::function<void()>> g_held_retired;

std::vector<std::unique_ptr<Submission>> g_submissions;
Submission* g_current = nullptr;
uint64_t g_next_serial = 1;
uint64_t g_done = 0;

struct Retired {
    uint64_t serial;
    std::function<void()> destroy;
};

std::deque<Retired> g_retired;

enum class Pass { None, Efb, Target, Screen };
Pass g_pass = Pass::None;
bool g_pass_depth = false;
bool g_clear_color_pending = false;
bool g_clear_depth_pending = false;
VkClearColorValue g_clear_color{};
float g_clear_depth = 1.0f;
VkPipeline g_bound_pipeline = VK_NULL_HANDLE;
VkDescriptorSet g_bound_draw_set = VK_NULL_HANDLE;
uint32_t g_bound_uniform = ~0u;
VkDescriptorSet g_bound_maps = VK_NULL_HANDLE;

Canvas g_canvas;
int g_canvas_kind = kInPicture;
std::atomic<uint32_t> g_shared_margins{0};
bool g_world_drawn = false;
bool g_world_shown = false;
int g_world_change_frames = 0;
constexpr int kSteadyFrames = 8;
double g_window_aspect = 0.0;
double g_picture_aspect = 4.0 / 3.0;

void flush_pending();

int scaled(int value) {
    return value * g_scale;
}

int canvas_width() {
    return kEfbWidth + 2 * g_canvas.margin_x;
}

int canvas_height() {
    return kEfbHeight + 2 * g_canvas.margin_y;
}

bool canvas_grown() {
    return g_canvas.margin_x != 0 || g_canvas.margin_y != 0;
}

void place_on_canvas(int& x, int& y, int& width, int& height, bool grow = true) {
    if (grow && canvas_grown() && covers_picture(x, y, width, height)) {
        width += x + 2 * g_canvas.margin_x;
        height += y + 2 * g_canvas.margin_y;
        x = 0;
        y = 0;
        return;
    }
    x += g_canvas.margin_x;
    y += g_canvas.margin_y;
}

VkDevice device() {
    return g_vk.device;
}

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& extension : list) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags preferred) {
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags flags = pass == 0 ? wanted | preferred : wanted;
        for (uint32_t i = 0; i < g_vk.memory.memoryTypeCount; i++) {
            if ((bits & (1u << i)) && (g_vk.memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool carve(Block& block, const VkMemoryRequirements& needs, VkDeviceSize& offset) {
    for (auto it = block.free.begin(); it != block.free.end(); ++it) {
        VkDeviceSize start = it->first;
        VkDeviceSize length = it->second;
        VkDeviceSize aligned = (start + needs.alignment - 1) / needs.alignment * needs.alignment;
        if (aligned + needs.size > start + length) {
            continue;
        }
        block.free.erase(it);
        if (aligned > start) {
            block.free[start] = aligned - start;
        }
        VkDeviceSize end = aligned + needs.size;
        if (end < start + length) {
            block.free[end] = start + length - end;
        }
        offset = aligned;
        return true;
    }
    return false;
}

bool allocate_image_memory(const VkMemoryRequirements& needs, Allocation& out) {
    int type = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type < 0) {
        type = memory_type(needs.memoryTypeBits, 0, 0);
    }
    if (type < 0) {
        return false;
    }
    if (needs.size > kBlockBytes / 4) {
        VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        info.allocationSize = needs.size;
        info.memoryTypeIndex = static_cast<uint32_t>(type);
        if (vkAllocateMemory(device(), &info, nullptr, &out.memory) != VK_SUCCESS) {
            return false;
        }
        out.offset = 0;
        out.size = needs.size;
        out.block = -1;
        return true;
    }
    for (size_t i = 0; i < g_blocks.size(); i++) {
        Block& block = g_blocks[i];
        if (block.type == static_cast<uint32_t>(type) && carve(block, needs, out.offset)) {
            out.memory = block.memory;
            out.size = needs.size;
            out.block = static_cast<int>(i);
            return true;
        }
    }
    Block block;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = kBlockBytes;
    info.memoryTypeIndex = static_cast<uint32_t>(type);
    if (vkAllocateMemory(device(), &info, nullptr, &block.memory) != VK_SUCCESS) {
        return false;
    }
    block.type = static_cast<uint32_t>(type);
    block.size = kBlockBytes;
    block.free[0] = kBlockBytes;
    g_blocks.push_back(std::move(block));
    Block& added = g_blocks.back();
    if (!carve(added, needs, out.offset)) {
        return false;
    }
    out.memory = added.memory;
    out.size = needs.size;
    out.block = static_cast<int>(g_blocks.size() - 1);
    return true;
}

void free_image_memory(Allocation& allocation) {
    if (!allocation.memory) {
        return;
    }
    if (allocation.block < 0) {
        vkFreeMemory(device(), allocation.memory, nullptr);
    } else {
        Block& block = g_blocks[static_cast<size_t>(allocation.block)];
        auto inserted = block.free.emplace(allocation.offset, allocation.size).first;
        auto next = std::next(inserted);
        if (next != block.free.end() && inserted->first + inserted->second == next->first) {
            inserted->second += next->second;
            block.free.erase(next);
        }
        if (inserted != block.free.begin()) {
            auto previous = std::prev(inserted);
            if (previous->first + previous->second == inserted->first) {
                previous->second += inserted->second;
                block.free.erase(inserted);
            }
        }
    }
    allocation = Allocation{};
}

bool make_buffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage, bool readback) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device(), &info, nullptr, &buffer.buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(device(), buffer.buffer, &needs);
    int type = memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           readback ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) {
        vkDestroyBuffer(device(), buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = static_cast<uint32_t>(type);
    void* mapped = nullptr;
    if (vkAllocateMemory(device(), &allocate, nullptr, &buffer.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device(), buffer.buffer, buffer.memory, 0) != VK_SUCCESS ||
        vkMapMemory(device(), buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        if (buffer.memory) {
            vkFreeMemory(device(), buffer.memory, nullptr);
        }
        vkDestroyBuffer(device(), buffer.buffer, nullptr);
        buffer = Buffer{};
        return false;
    }
    buffer.mapped = static_cast<uint8_t*>(mapped);
    buffer.size = size;
    return true;
}

void drop_buffer(Buffer& buffer) {
    if (buffer.buffer) {
        vkDestroyBuffer(device(), buffer.buffer, nullptr);
    }
    if (buffer.memory) {
        vkFreeMemory(device(), buffer.memory, nullptr);
    }
    buffer = Buffer{};
}

uint64_t recording_serial() {
    return g_current ? g_current->serial : g_next_serial - 1;
}

void retire(std::function<void()> destroy) {
    if (g_recording) {
        g_held_retired.push_back(std::move(destroy));
        return;
    }
    g_retired.push_back({recording_serial(), std::move(destroy)});
}

void release_held() {
    for (std::function<void()>& destroy : g_held_retired) {
        g_retired.push_back({recording_serial(), std::move(destroy)});
    }
    g_held_retired.clear();
}

void destroy_image(Image* image) {
    if (!image) {
        return;
    }
    if (image->framebuffer) {
        vkDestroyFramebuffer(device(), image->framebuffer, nullptr);
    }
    if (image->view) {
        vkDestroyImageView(device(), image->view, nullptr);
    }
    if (image->image) {
        vkDestroyImage(device(), image->image, nullptr);
    }
    free_image_memory(image->memory);
    delete image;
}

void retire_image(Image* image) {
    if (image) {
        retire([image] { destroy_image(image); });
    }
}

Image* make_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageUsageFlags usage) {
    Image* image = new Image;
    image->format = format;
    image->width = std::max(width, 1u);
    image->height = std::max(height, 1u);
    image->levels = std::max(levels, 1u);
    bool depth = format == g_vk.depth_format;
    image->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {image->width, image->height, 1};
    info.mipLevels = image->levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device(), &info, nullptr, &image->image) != VK_SUCCESS) {
        delete image;
        return nullptr;
    }
    VkMemoryRequirements needs{};
    vkGetImageMemoryRequirements(device(), image->image, &needs);
    if (!allocate_image_memory(needs, image->memory) || vkBindImageMemory(device(), image->image, image->memory.memory, image->memory.offset) != VK_SUCCESS) {
        destroy_image(image);
        return nullptr;
    }
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {image->aspect, 0, image->levels, 0, 1};
    if (vkCreateImageView(device(), &view, nullptr, &image->view) != VK_SUCCESS) {
        destroy_image(image);
        return nullptr;
    }
    return image;
}

void layout_scope(VkImageLayout layout, bool destination, VkPipelineStageFlags& stage, VkAccessFlags& access) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = destination ? VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        access = destination ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
        stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        access = destination ? VK_ACCESS_SHADER_READ_BIT : 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = destination ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    default:
        stage = destination ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        access = 0;
        break;
    }
}

void transition(VkCommandBuffer command, Image& image, VkImageLayout to) {
    if (image.layout == to) {
        return;
    }
    VkPipelineStageFlags source_stage = 0;
    VkPipelineStageFlags destination_stage = 0;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    layout_scope(image.layout, false, source_stage, barrier.srcAccessMask);
    layout_scope(to, true, destination_stage, barrier.dstAccessMask);
    barrier.oldLayout = image.layout;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.image;
    barrier.subresourceRange = {image.aspect, 0, image.levels, 0, 1};
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    image.layout = to;
}

void reset_submission(Submission& submission) {
    vkResetCommandPool(device(), submission.pool, 0);
    for (size_t i = 0; i < submission.pool_used && i < submission.pools.size(); i++) {
        vkResetDescriptorPool(device(), submission.pools[i], 0);
    }
    submission.pool_used = 0;
    submission.map_sets.clear();
    submission.uploads = false;
}

void release_retired() {
    while (!g_retired.empty() && g_retired.front().serial <= g_done) {
        std::function<void()> destroy = std::move(g_retired.front().destroy);
        g_retired.pop_front();
        destroy();
    }
}

void poll() {
    for (auto& submission : g_submissions) {
        if (submission->busy && vkGetFenceStatus(device(), submission->fence) == VK_SUCCESS) {
            read_gpu_time(*submission);
            submission->busy = false;
            g_done = std::max(g_done, submission->serial);
        }
    }
    release_retired();
}

void wait_for(Submission& submission) {
    if (!submission.busy) {
        return;
    }
    auto start = std::chrono::steady_clock::now();
    vkWaitForFences(device(), 1, &submission.fence, VK_TRUE, UINT64_MAX);
    g_fence_wait += seconds_since(start);
    read_gpu_time(submission);
    submission.busy = false;
    g_done = std::max(g_done, submission.serial);
}

void read_gpu_time(Submission& submission) {
    if (!submission.timed) {
        return;
    }
    submission.timed = false;
    uint32_t used = 2 + static_cast<uint32_t>(submission.pass_kinds.size()) * 2;
    std::vector<uint64_t> stamps(used);
    if (vkGetQueryPoolResults(device(), submission.times, 0, used, used * sizeof(uint64_t), stamps.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
        return;
    }
    double tick = g_vk.properties.limits.timestampPeriod * 1e-9;
    if (stamps[1] > stamps[0]) {
        g_gpu_time += static_cast<double>(stamps[1] - stamps[0]) * tick;
    }
    for (size_t i = 0; i < submission.pass_kinds.size(); i++) {
        uint64_t begin = stamps[2 + i * 2];
        uint64_t end = stamps[3 + i * 2];
        if (end > begin) {
            g_pass_time[submission.pass_kinds[i]] += static_cast<double>(end - begin) * tick;
        }
        g_pass_count[submission.pass_kinds[i]]++;
    }
}

Submission* take_submission() {
    poll();
    for (auto& submission : g_submissions) {
        if (!submission->busy) {
            return submission.get();
        }
    }
    if (g_submissions.size() < kMaxSubmissions) {
        auto submission = std::make_unique<Submission>();
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pool.queueFamilyIndex = g_vk.family;
        if (vkCreateCommandPool(device(), &pool, nullptr, &submission->pool) == VK_SUCCESS) {
            VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            allocate.commandPool = submission->pool;
            allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocate.commandBufferCount = 2;
            VkCommandBuffer buffers[2] = {};
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (vkAllocateCommandBuffers(device(), &allocate, buffers) == VK_SUCCESS &&
                vkCreateFence(device(), &fence, nullptr, &submission->fence) == VK_SUCCESS) {
                submission->upload = buffers[0];
                submission->draw = buffers[1];
                g_submissions.push_back(std::move(submission));
                return g_submissions.back().get();
            }
        }
    }
    Submission* oldest = nullptr;
    for (auto& submission : g_submissions) {
        if (!oldest || submission->serial < oldest->serial) {
            oldest = submission.get();
        }
    }
    wait_for(*oldest);
    release_retired();
    return oldest;
}

void ensure_command() {
    if (g_current) {
        return;
    }
    Submission* submission = take_submission();
    reset_submission(*submission);
    vkResetFences(device(), 1, &submission->fence);
    submission->serial = g_next_serial++;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(submission->upload, &begin);
    vkBeginCommandBuffer(submission->draw, &begin);
    if (g_vk.timestamps && !submission->times) {
        VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        query.queryType = VK_QUERY_TYPE_TIMESTAMP;
        query.queryCount = kTimeQueries;
        vkCreateQueryPool(device(), &query, nullptr, &submission->times);
    }
    submission->timed = submission->times != VK_NULL_HANDLE;
    if (submission->timed) {
        vkCmdResetQueryPool(submission->draw, submission->times, 0, kTimeQueries);
        submission->next_query = 2;
        submission->pass_kinds.clear();
        vkCmdWriteTimestamp(submission->draw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, submission->times, 0);
    }
    g_current = submission;
}

VkCommandBuffer command() {
    ensure_command();
    return g_current->draw;
}

VkCommandBuffer upload_command() {
    ensure_command();
    g_current->uploads = true;
    return g_current->upload;
}

uint64_t current_serial() {
    ensure_command();
    return g_current->serial;
}

void end_pass() {
    if (g_pass != Pass::None) {
        vkCmdEndRenderPass(g_current->draw);
        if (g_current->timed && g_current->next_query + 2 <= kTimeQueries) {
            vkCmdWriteTimestamp(g_current->draw, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_current->times, g_current->next_query + 1);
            g_current->next_query += 2;
            g_current->pass_kinds.push_back(static_cast<uint8_t>(g_pass));
        }
        g_pass = Pass::None;
    }
    g_bound_pipeline = VK_NULL_HANDLE;
    g_bound_draw_set = VK_NULL_HANDLE;
    g_bound_uniform = ~0u;
    g_bound_maps = VK_NULL_HANDLE;
}

void submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE) {
    if (!g_current) {
        return;
    }
    end_pass();
    Submission& submission = *g_current;
    if (submission.timed) {
        vkCmdWriteTimestamp(submission.draw, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, submission.times, 1);
    }
    vkEndCommandBuffer(submission.upload);
    vkEndCommandBuffer(submission.draw);
    VkCommandBuffer buffers[2];
    uint32_t count = 0;
    if (submission.uploads) {
        buffers[count++] = submission.upload;
    }
    buffers[count++] = submission.draw;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = count;
    info.pCommandBuffers = buffers;
    if (wait) {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.pWaitDstStageMask = &stage;
    }
    if (signal) {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &signal;
    }
    VkResult result = vkQueueSubmit(g_vk.queue, 1, &info, submission.fence);
    if (result != VK_SUCCESS) {
        log::write("video", "Vulkan refused a submission (%d)", static_cast<int>(result));
        vkQueueWaitIdle(g_vk.queue);
        g_done = std::max(g_done, submission.serial);
        submission.busy = false;
    } else {
        submission.busy = true;
    }
    g_submits++;
    g_current = nullptr;
}

bool serial_done(uint64_t serial) {
    if (g_done >= serial) {
        return true;
    }
    poll();
    return g_done >= serial;
}

void wait_serial(uint64_t serial) {
    if (serial == 0 || g_done >= serial) {
        return;
    }
    if (g_current && serial >= g_current->serial) {
        submit();
    }
    for (auto& submission : g_submissions) {
        if (submission->busy && submission->serial <= serial) {
            wait_for(*submission);
        }
    }
    release_retired();
}

void wait_idle() {
    auto start = std::chrono::steady_clock::now();
    g_idle_waits++;
    struct Count {
        std::chrono::steady_clock::time_point start;
        ~Count() { g_idle_wait += seconds_since(start); }
    } count{start};
    if (g_current) {
        submit();
    }
    for (auto& submission : g_submissions) {
        wait_for(*submission);
    }
    release_retired();
}

VkDescriptorPool make_descriptor_pool(uint32_t sets, bool freeable) {
    VkDescriptorPoolSize sizes[4] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * kTextureMaps},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, sets},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4},
    };
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = freeable ? VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT : 0;
    info.maxSets = sets;
    info.poolSizeCount = 4;
    info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(device(), &info, nullptr, &pool);
    return pool;
}

VkDescriptorSet allocate_set(VkDescriptorSetLayout layout) {
    ensure_command();
    Submission& submission = *g_current;
    while (true) {
        if (submission.pool_used == submission.pools.size()) {
            VkDescriptorPool pool = make_descriptor_pool(kSetsPerPool, false);
            if (!pool) {
                return VK_NULL_HANDLE;
            }
            submission.pools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        info.descriptorPool = submission.pools[submission.pool_used];
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        VkResult result = vkAllocateDescriptorSets(device(), &info, &set);
        if (result == VK_SUCCESS) {
            return set;
        }
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            return VK_NULL_HANDLE;
        }
        submission.pool_used++;
        if (submission.pool_used < submission.pools.size()) {
            vkResetDescriptorPool(device(), submission.pools[submission.pool_used], 0);
        }
    }
}

VkDescriptorSet single_set(const Image& image, VkSampler sampler) {
    VkDescriptorSet set = allocate_set(g_device.single_layout);
    if (!set) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo info{sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device(), 1, &write, 0, nullptr);
    return set;
}

bool make_ring(Ring& ring, uint32_t bytes, VkBufferUsageFlags usage) {
    if (!make_buffer(ring.buffer, bytes, usage, false)) {
        return false;
    }
    ring.capacity = bytes;
    ring.cursor = 0;
    ring.segment = 0;
    return true;
}

bool reserve(Ring& ring, uint32_t size, uint32_t alignment, uint32_t& start) {
    uint32_t part = ring.capacity / kRingSegments;
    if (size > part) {
        return false;
    }
    uint32_t at = (ring.cursor + alignment - 1) / alignment * alignment;
    if (at >= part * kRingSegments) {
        at = 0;
    }
    uint32_t segment = at / part;
    if (at + size > (segment + 1) * part) {
        segment = (segment + 1) % kRingSegments;
        at = segment * part;
    }
    if (segment != ring.segment) {
        ring.used_by[ring.segment] = current_serial();
        auto start = std::chrono::steady_clock::now();
        wait_serial(ring.used_by[segment]);
        g_ring_wait += seconds_since(start);
        ring.used_by[segment] = 0;
        ring.segment = segment;
        ring.switches++;
    }
    start = at;
    ring.cursor = at + size;
    return true;
}

bool upload(Ring& ring, const void* data, uint32_t size, uint32_t alignment, uint32_t& start) {
    if (size == 0) {
        start = 0;
        return true;
    }
    if (!reserve(ring, size, alignment, start)) {
        return false;
    }
    std::memcpy(ring.buffer.mapped + start, data, size);
    return true;
}

VkShaderModule make_module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device(), &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

VkRenderPass make_render_pass(VkFormat color, VkAttachmentLoadOp color_load, VkImageLayout color_initial, VkImageLayout color_final, bool depth,
                              VkAttachmentLoadOp depth_load, VkAttachmentStoreOp depth_store) {
    VkAttachmentDescription attachments[2]{};
    attachments[0].format = color;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = color_load;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = color_initial;
    attachments[0].finalLayout = color_final;
    attachments[1].format = g_vk.depth_format;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = depth_load;
    attachments[1].storeOp = depth_store;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = depth ? &depth_reference : nullptr;
    VkSubpassDependency dependencies[2]{};
    const VkPipelineStageFlags attachment_stages =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attachment_writes = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags attachment_access = attachment_writes | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = attachment_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = attachment_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = attachment_writes | VK_ACCESS_TRANSFER_WRITE_BIT;
    dependencies[0].dstAccessMask = attachment_access | VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = attachment_stages;
    dependencies[1].dstStageMask = attachment_stages | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = attachment_writes;
    dependencies[1].dstAccessMask = attachment_access | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = depth ? 2 : 1;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device(), &info, nullptr, &pass) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pass;
}

bool make_framebuffer(VkRenderPass pass, const VkImageView* views, uint32_t count, uint32_t width, uint32_t height, VkFramebuffer& framebuffer) {
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    return vkCreateFramebuffer(device(), &info, nullptr, &framebuffer) == VK_SUCCESS;
}

VkBlendFactor vk_factor(BlendFactor factor) {
    bool dual = g_vk.dual_source;
    switch (factor) {
    case BlendFactor::Zero:
        return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:
        return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SourceColor:
        return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::TevColor:
        return dual ? VK_BLEND_FACTOR_SRC1_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::TevAlpha:
        return dual ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InverseTevAlpha:
        return dual ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

VkBlendOp vk_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return VK_BLEND_OP_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:
        return VK_BLEND_OP_ADD;
    }
}

std::mutex g_blend_lock;

BlendState blend_for(uint32_t word) {
    static std::map<uint32_t, BlendState> cache;
    std::lock_guard<std::mutex> hold(g_blend_lock);
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

uint32_t canonical_blend(uint32_t word) {
    static std::unordered_map<uint32_t, uint32_t> words;
    static std::unordered_map<uint32_t, uint32_t> states;
    auto known = words.find(word);
    if (known != words.end()) {
        return known->second;
    }
    BlendState blend = blend_for(word);
    uint32_t packed = (blend.color_update ? 1u : 0u) | (blend.alpha_update ? 2u : 0u);
    if (blend.enable) {
        packed |= 4u | (static_cast<uint32_t>(blend.operation) << 3) | (static_cast<uint32_t>(blend.alpha_operation) << 5) |
                  (static_cast<uint32_t>(blend.source) << 7) | (static_cast<uint32_t>(blend.destination) << 11) |
                  (static_cast<uint32_t>(blend.source_alpha) << 15) | (static_cast<uint32_t>(blend.destination_alpha) << 19);
    }
    uint32_t chosen = states.emplace(packed, word).first->second;
    words.emplace(word, chosen);
    return chosen;
}

uint32_t depth_key(uint32_t word) {
    return (word & 1) ? (word & 0x1F) : 0;
}

uint64_t pipe_key(int kind, bool early, bool keeps_all, uint32_t blend, uint32_t depth, uint32_t cull) {
    return (static_cast<uint64_t>(blend) << 16) | (static_cast<uint64_t>(depth) << 8) | (static_cast<uint64_t>(cull) << 4) | (keeps_all ? 4u : 0u) |
           (static_cast<uint64_t>(kind) << 1) | (early ? 1u : 0u);
}

VkPipelineColorBlendAttachmentState blend_attachment(uint32_t word) {
    BlendState blend = blend_for(word);
    VkPipelineColorBlendAttachmentState state{};
    state.blendEnable = blend.enable ? VK_TRUE : VK_FALSE;
    state.srcColorBlendFactor = vk_factor(blend.source);
    state.dstColorBlendFactor = vk_factor(blend.destination);
    state.colorBlendOp = vk_operation(blend.operation);
    state.srcAlphaBlendFactor = vk_factor(blend.source_alpha);
    state.dstAlphaBlendFactor = vk_factor(blend.destination_alpha);
    state.alphaBlendOp = vk_operation(blend.alpha_operation);
    state.colorWriteMask = (blend.color_update ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 0u) |
                           (blend.alpha_update ? VK_COLOR_COMPONENT_A_BIT : 0u);
    return state;
}

struct PipelineShape {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    const VkSpecializationInfo* specialization = nullptr;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkPipelineColorBlendAttachmentState blend{};
    bool depth_test = false;
    bool depth_write = false;
    VkCompareOp depth_compare = VK_COMPARE_OP_ALWAYS;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    bool depth_attachment = false;
};

VkPipeline build_pipeline(const PipelineShape& shape) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = shape.vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = shape.fragment;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = shape.specialization;
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = shape.depth_attachment && g_vk.depth_clamp ? VK_TRUE : VK_FALSE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = shape.cull;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = shape.depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = shape.depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = shape.depth_compare;
    depth.minDepthBounds = 0.0f;
    depth.maxDepthBounds = 1.0f;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &shape.blend;
    VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = shape.depth_attachment ? &depth : nullptr;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = shape.layout;
    info.renderPass = shape.pass;
    info.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(device(), g_device.cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

const VkPipelineColorBlendAttachmentState kOpaque = {VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                                                     VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xF};
const VkPipelineColorBlendAttachmentState kOver = {VK_TRUE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ZERO,
                                                   VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF};

struct Specialization {
    uint32_t values[kSpecConstants] = {};
    VkSpecializationMapEntry entries[kSpecConstants];
    VkSpecializationInfo info{};

    Specialization() {
        for (uint32_t i = 0; i < kSpecConstants; i++) {
            entries[i] = {i, i * 4u, 4u};
        }
        info.mapEntryCount = kSpecConstants;
        info.pMapEntries = entries;
        info.dataSize = sizeof values;
        info.pData = values;
    }
};

void fill_specialization(Specialization& specialization, const std::string& name, bool keeps_all) {
    std::memset(specialization.values, 0, sizeof specialization.values);
    specialization.values[90] = keeps_all ? 1u : 0u;
    if (name.size() < kVariantHeader * sizeof(uint32_t)) {
        return;
    }
    std::vector<uint32_t> words(name.size() / sizeof(uint32_t));
    std::memcpy(words.data(), name.data(), words.size() * sizeof(uint32_t));
    uint32_t count = std::min<uint32_t>(words[0], kMaxStages);
    specialization.values[0] = 1;
    specialization.values[1] = count;
    for (uint32_t i = 0; i < 8; i++) {
        specialization.values[2 + i] = words[2 + i];
    }
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < 5; j++) {
            size_t at = kVariantHeader + i * 5 + j;
            specialization.values[10 + i * 5 + j] = at < words.size() ? words[at] : 0;
        }
    }
}

VkPipeline build_tev(uint64_t key, const std::string& name) {
    bool early = (key & 1) != 0;
    int kind = static_cast<int>((key >> 1) & 1);
    bool keeps_all = (key & 4) != 0;
    uint32_t cull = static_cast<uint32_t>((key >> 4) & 3);
    uint32_t depth = static_cast<uint32_t>((key >> 8) & 0xFF);
    uint32_t blend = static_cast<uint32_t>(key >> 16);
    Specialization specialization;
    fill_specialization(specialization, name, keeps_all);
    PipelineShape shape;
    shape.vertex = kind ? g_device.run_vertex : g_device.tev_vertex;
    shape.fragment = g_device.tev_fragment[early ? 1 : 0];
    if (name.size() >= kVariantHeader * sizeof(uint32_t)) {
        uint32_t indirect = 0;
        std::memcpy(&indirect, name.data() + 6 * sizeof(uint32_t), sizeof indirect);
        uint32_t coords = (indirect >> 8) & 15;
        if (coords < 8 && g_device.tev_fragment_sized[early ? 1 : 0][coords]) {
            shape.vertex = kind ? g_device.run_vertex_sized[coords] : g_device.tev_vertex_sized[coords];
            shape.fragment = g_device.tev_fragment_sized[early ? 1 : 0][coords];
        }
    }
    shape.specialization = &specialization.info;
    shape.layout = g_device.tev_layout;
    shape.pass = g_device.efb_passes[0][0];
    shape.blend = blend_attachment(blend);
    shape.depth_attachment = true;
    shape.depth_test = (depth & 1) != 0;
    shape.depth_write = shape.depth_test && (depth & (1u << 4)) != 0;
    shape.depth_compare = static_cast<VkCompareOp>((depth >> 1) & 7);
    shape.cull = cull == 1 ? VK_CULL_MODE_BACK_BIT : (cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    return build_pipeline(shape);
}

struct Pipe {
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool pending = false;
    bool failed = false;
};

struct Job {
    uint64_t key;
    std::string name;
};

std::mutex g_compile_lock;
std::condition_variable g_compile_changed;
std::deque<Job> g_jobs;
std::unordered_map<std::string, std::unordered_map<uint64_t, Pipe>> g_variants;
std::unordered_map<uint64_t, Pipe> g_generic;
std::vector<std::thread> g_workers;
uint32_t g_variants_ready = 0;
uint32_t g_variants_failed = 0;
uint32_t g_reported_ready = 0;
uint32_t g_saved_ready = 0;
size_t g_queued = 0;

struct Records {
    std::string path;
    std::vector<std::pair<uint64_t, std::string>> loaded;
    std::unordered_map<std::string, bool> written;
};

Records g_records;

std::string record_name(uint64_t key, const std::string& variant) {
    return std::string(reinterpret_cast<const char*>(&key), sizeof key) + variant;
}

std::string cache_directory() {
    return std::filesystem::path(game::description().settings_file).parent_path().string();
}

std::string records_identity() {
    uint64_t hash = 1469598103934665603ull;
    for (const uint32_t* code : {kTevCode, kTevEarlyCode, kTevVertexCode, kRunVertexCode}) {
        for (size_t i = 0; i < 64; i++) {
            hash = (hash ^ code[i]) * 1099511628211ull;
        }
    }
    hash = (hash ^ sizeof kTevCode) * 1099511628211ull;
    char text[512];
    std::snprintf(text, sizeof text, "WPVK1|%s|%u|%u|%llu\n", g_vk.properties.deviceName, g_vk.properties.vendorID, g_vk.properties.driverVersion,
                  static_cast<unsigned long long>(hash));
    return text;
}

void open_records() {
    g_records.path = (std::filesystem::path(cache_directory()) / "pipelines_vk.bin").string();
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

std::string cache_path() {
    return (std::filesystem::path(cache_directory()) / "pipeline_cache_vk.bin").string();
}

void save_pipeline_cache() {
    size_t size = 0;
    if (!g_device.cache || vkGetPipelineCacheData(device(), g_device.cache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<uint8_t> data(size);
    if (vkGetPipelineCacheData(device(), g_device.cache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    std::string path = cache_path();
    std::string temporary = path + ".new";
    if (FILE* file = std::fopen(temporary.c_str(), "wb")) {
        bool ok = std::fwrite(data.data(), 1, size, file) == size;
        ok = std::fclose(file) == 0 && ok;
        std::error_code ignored;
        if (ok) {
            std::filesystem::rename(temporary, path, ignored);
        } else {
            std::filesystem::remove(temporary, ignored);
        }
    }
}

void work() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> hold(g_compile_lock);
            g_compile_changed.wait(hold, [] { return !g_jobs.empty(); });
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        VkPipeline pipeline = build_tev(job.key, job.name);
        std::lock_guard<std::mutex> hold(g_compile_lock);
        Pipe& pipe = job.name.empty() ? g_generic[job.key] : g_variants[job.name][job.key];
        pipe.pending = false;
        if (pipe.pipeline) {
            if (pipeline) {
                vkDestroyPipeline(device(), pipeline, nullptr);
            }
            continue;
        }
        pipe.pipeline = pipeline;
        pipe.failed = pipeline == VK_NULL_HANDLE;
        if (!job.name.empty()) {
            if (pipeline) {
                g_variants_ready++;
            } else {
                g_variants_failed++;
            }
        }
    }
}

void queue_job(uint64_t key, const std::string& name) {
    g_jobs.push_back({key, name});
    g_queued++;
    g_compile_changed.notify_one();
}

void start_workers() {
    unsigned count = std::clamp(std::thread::hardware_concurrency() / 3, 1u, 3u);
    for (unsigned i = 0; i < count; i++) {
        g_workers.emplace_back(work);
        g_workers.back().detach();
    }
}

void warm_pipelines() {
    open_records();
    std::lock_guard<std::mutex> hold(g_compile_lock);
    for (const auto& [key, name] : g_records.loaded) {
        Pipe& pipe = name.empty() ? g_generic[key] : g_variants[name][key];
        if (!pipe.pipeline && !pipe.pending) {
            pipe.pending = true;
            queue_job(key, name);
        }
    }
    if (!g_records.loaded.empty()) {
        log::write("video", "%zu pipelines from earlier games are being prepared in the background", g_records.loaded.size());
    }
}

VkPipeline generic_pipeline(uint64_t key) {
    {
        std::lock_guard<std::mutex> hold(g_compile_lock);
        auto found = g_generic.find(key);
        if (found != g_generic.end() && found->second.pipeline) {
            return found->second.pipeline;
        }
    }
    VkPipeline pipeline = build_tev(key, std::string());
    std::lock_guard<std::mutex> hold(g_compile_lock);
    Pipe& pipe = g_generic[key];
    if (pipe.pipeline) {
        if (pipeline) {
            vkDestroyPipeline(device(), pipeline, nullptr);
        }
        return pipe.pipeline;
    }
    pipe.pipeline = pipeline;
    note_record(key, std::string());
    if (!pipeline) {
        log::write("video", "a Vulkan drawing pipeline did not build");
    }
    return pipeline;
}

VkPipeline tev_pipeline(const BatchState& pending, int kind, uint32_t cull) {
    static const bool generic_only = std::getenv("WP_GENERIC_SHADER") != nullptr;
    uint32_t blend = canonical_blend(pending.blend);
    uint64_t key = pipe_key(kind, pending.early_depth, pending.keeps_all, blend, depth_key(pending.depth), cull);
    if (!generic_only) {
        std::string name = variant_key(pending.constants, pending.early_depth);
        std::lock_guard<std::mutex> hold(g_compile_lock);
        Pipe& pipe = g_variants[name][key];
        if (pipe.pipeline) {
            return pipe.pipeline;
        }
        if (!pipe.pending && !pipe.failed) {
            pipe.pending = true;
            queue_job(key, name);
            note_record(key, name);
        }
    }
    return generic_pipeline(key);
}

VkPipeline clear_pipeline(bool color, bool alpha, bool depth) {
    uint32_t key = (color ? 1u : 0u) | (alpha ? 2u : 0u) | (depth ? 4u : 0u);
    auto found = g_device.clears.find(key);
    if (found != g_device.clears.end()) {
        return found->second;
    }
    PipelineShape shape;
    shape.vertex = g_device.fullscreen_vertex;
    shape.fragment = g_device.clear_fragment;
    shape.layout = g_device.clear_layout;
    shape.pass = g_device.efb_passes[0][0];
    shape.blend = kOpaque;
    shape.blend.colorWriteMask = (color ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 0u) | (alpha ? VK_COLOR_COMPONENT_A_BIT : 0u);
    shape.depth_attachment = true;
    shape.depth_test = depth;
    shape.depth_write = depth;
    shape.depth_compare = VK_COMPARE_OP_ALWAYS;
    VkPipeline pipeline = build_pipeline(shape);
    g_device.clears[key] = pipeline;
    return pipeline;
}

VkPipeline screen_pipeline(int which, bool blend) {
    uint64_t key = (static_cast<uint64_t>(which) << 1) | (blend ? 1u : 0u) | (static_cast<uint64_t>(g_device.chain.format) << 8);
    auto found = g_device.screen_pipelines.find(key);
    if (found != g_device.screen_pipelines.end()) {
        return found->second;
    }
    PipelineShape shape;
    shape.vertex = g_device.fullscreen_vertex;
    shape.fragment = which == 0 ? g_device.present_fragment : (which == 1 ? g_device.present_smooth_fragment : g_device.overlay_fragment);
    shape.layout = g_device.single_pipeline_layout;
    shape.pass = g_device.chain.pass;
    shape.blend = blend ? kOver : kOpaque;
    VkPipeline pipeline = build_pipeline(shape);
    g_device.screen_pipelines[key] = pipeline;
    return pipeline;
}

VkSampler make_sampler(VkFilter filter) {
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = filter;
    info.minFilter = filter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(device(), &info, nullptr, &sampler);
    return sampler;
}

void begin_render_pass(VkRenderPass pass, VkFramebuffer framebuffer, uint32_t width, uint32_t height, uint32_t clears, const VkClearValue* values) {
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = pass;
    begin.framebuffer = framebuffer;
    begin.renderArea = {{0, 0}, {width, height}};
    begin.clearValueCount = clears;
    begin.pClearValues = values;
    if (g_current->timed && g_current->next_query + 2 <= kTimeQueries) {
        vkCmdWriteTimestamp(g_current->draw, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_current->times, g_current->next_query);
    }
    vkCmdBeginRenderPass(g_current->draw, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

void set_viewport(float x, float y, float width, float height) {
    VkViewport viewport{x, y, width, height, 0.0f, 1.0f};
    vkCmdSetViewport(g_current->draw, 0, 1, &viewport);
}

void set_scissor(int x, int y, int width, int height) {
    VkRect2D rect{{std::max(x, 0), std::max(y, 0)}, {static_cast<uint32_t>(std::max(width, 0)), static_cast<uint32_t>(std::max(height, 0))}};
    vkCmdSetScissor(g_current->draw, 0, 1, &rect);
}

void bind_pipeline(VkPipeline pipeline) {
    if (g_bound_pipeline != pipeline) {
        vkCmdBindPipeline(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g_bound_pipeline = pipeline;
    }
}

void begin_efb(bool depth_used) {
    if (g_pass == Pass::Efb) {
        if (!depth_used || g_pass_depth) {
            return;
        }
    }
    end_pass();
    VkCommandBuffer cb = command();
    transition(cb, *g_device.efb_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cb, *g_device.efb_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    int color = g_clear_color_pending ? 1 : 0;
    int depth = g_clear_depth_pending ? 1 : (depth_used || !g_vk.load_none ? 0 : 2);
    VkClearValue values[2]{};
    values[0].color = g_clear_color;
    values[1].depthStencil = {g_clear_depth, 0};
    begin_render_pass(g_device.efb_passes[color][depth], g_device.efb_framebuffer, static_cast<uint32_t>(scaled(canvas_width())),
                      static_cast<uint32_t>(scaled(canvas_height())), 2, values);
    g_pass = Pass::Efb;
    g_pass_depth = depth != 2;
    g_efb_passes++;
    if (g_clear_color_pending || g_clear_depth_pending) {
        g_cleared_passes++;
    }
    g_clear_color_pending = false;
    g_clear_depth_pending = false;
    set_viewport(0, 0, static_cast<float>(scaled(canvas_width())), static_cast<float>(scaled(canvas_height())));
}

void settle_efb() {
    if (g_clear_color_pending || g_clear_depth_pending) {
        begin_efb(false);
    }
    end_pass();
}

bool create_efb() {
    uint32_t width = static_cast<uint32_t>(scaled(canvas_width()));
    uint32_t height = static_cast<uint32_t>(scaled(canvas_height()));
    g_device.efb_color = make_image(width, height, 1, kColorFormat,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageUsageFlags copies = g_vk.depth_copies ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0;
    g_device.efb_depth = make_image(width, height, 1, g_vk.depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | copies);
    if (!g_device.efb_color || !g_device.efb_depth) {
        return false;
    }
    VkImageView views[2] = {g_device.efb_color->view, g_device.efb_depth->view};
    if (!make_framebuffer(g_device.efb_passes[0][0], views, 2, width, height, g_device.efb_framebuffer)) {
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_device.static_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &g_device.copy_layout;
    if (vkAllocateDescriptorSets(device(), &allocate, &g_device.copy_set) != VK_SUCCESS) {
        return false;
    }
    VkDescriptorImageInfo images[2] = {{g_device.linear, g_device.efb_color->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g_device.point, g_device.efb_depth->view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = g_device.copy_set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device(), 2, writes, 0, nullptr);
    g_clear_color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    g_clear_depth = 1.0f;
    g_clear_color_pending = true;
    g_clear_depth_pending = true;
    return true;
}

void release_efb() {
    end_pass();
    Image* color = g_device.efb_color;
    Image* depth = g_device.efb_depth;
    VkFramebuffer framebuffer = g_device.efb_framebuffer;
    VkDescriptorSet set = g_device.copy_set;
    retire([color, depth, framebuffer, set] {
        if (framebuffer) {
            vkDestroyFramebuffer(device(), framebuffer, nullptr);
        }
        if (set) {
            vkFreeDescriptorSets(device(), g_device.static_pool, 1, &set);
        }
        destroy_image(color);
        destroy_image(depth);
    });
    g_device.efb_color = nullptr;
    g_device.efb_depth = nullptr;
    g_device.efb_framebuffer = VK_NULL_HANDLE;
    g_device.copy_set = VK_NULL_HANDLE;
    g_clear_color_pending = false;
    g_clear_depth_pending = false;
}

bool create_render_texture(Target& target, uint32_t width, uint32_t height) {
    target.image = make_image(width, height, 1, kColorFormat,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (!target.image) {
        return false;
    }
    if (!make_framebuffer(g_device.target_pass, &target.image->view, 1, target.image->width, target.image->height, target.image->framebuffer)) {
        destroy_image(target.image);
        target.image = nullptr;
        return false;
    }
    target.width = width;
    target.height = height;
    return true;
}

void release_target(Target& target) {
    retire_image(target.image);
    target = Target{};
}

int requested_scale() {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    int wanted = scale();
    return std::clamp(wanted == 0 ? 2 : wanted, 1, 6);
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

Image* upload_levels(const std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate, VkFormat format = kColorFormat);

bool create_layouts() {
    VkDescriptorSetLayoutBinding draw_bindings[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
    };
    VkDescriptorSetLayoutBinding map_bindings[kTextureMaps];
    for (uint32_t i = 0; i < kTextureMaps; i++) {
        map_bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutBinding copy_bindings[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    };
    auto make_set_layout = [](const VkDescriptorSetLayoutBinding* bindings, uint32_t count, VkDescriptorSetLayout& layout) {
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        info.bindingCount = count;
        info.pBindings = bindings;
        return vkCreateDescriptorSetLayout(device(), &info, nullptr, &layout) == VK_SUCCESS;
    };
    if (!make_set_layout(draw_bindings, 3, g_device.draw_layout) || !make_set_layout(map_bindings, kTextureMaps, g_device.maps_layout) ||
        !make_set_layout(copy_bindings, 2, g_device.copy_layout) || !make_set_layout(copy_bindings, 1, g_device.single_layout)) {
        return false;
    }
    auto make_layout = [](const VkDescriptorSetLayout* sets, uint32_t count, VkShaderStageFlags stage, uint32_t push, VkPipelineLayout& layout) {
        VkPushConstantRange range{stage, 0, push};
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.setLayoutCount = count;
        info.pSetLayouts = sets;
        info.pushConstantRangeCount = push ? 1 : 0;
        info.pPushConstantRanges = &range;
        return vkCreatePipelineLayout(device(), &info, nullptr, &layout) == VK_SUCCESS;
    };
    VkDescriptorSetLayout tev_sets[2] = {g_device.draw_layout, g_device.maps_layout};
    return make_layout(tev_sets, 2, VK_SHADER_STAGE_VERTEX_BIT, 16, g_device.tev_layout) &&
           make_layout(&g_device.copy_layout, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 80, g_device.copy_pipeline_layout) &&
           make_layout(&g_device.single_layout, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 16, g_device.single_pipeline_layout) &&
           make_layout(nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, 32, g_device.clear_layout);
}

bool create_draw_sets() {
    VkDescriptorSetLayout layouts[4] = {g_device.draw_layout, g_device.draw_layout, g_device.draw_layout, g_device.draw_layout};
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_device.static_pool;
    allocate.descriptorSetCount = 4;
    allocate.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(device(), &allocate, g_device.draw_sets) != VK_SUCCESS) {
        return false;
    }
    const Buffer* vertices[4] = {&g_device.screen.buffer, &g_device.run_vertices.buffer, &g_device.kept_vertices, &g_device.screen.buffer};
    for (uint32_t i = 0; i < 4; i++) {
        VkDescriptorBufferInfo buffers[3] = {{g_device.uniforms.buffer.buffer, 0, sizeof(Constants)},
                                             {vertices[i]->buffer, 0, VK_WHOLE_SIZE},
                                             {(i == 3 ? g_device.moved : g_device.run_rows).buffer.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet writes[3]{};
        for (uint32_t b = 0; b < 3; b++) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = g_device.draw_sets[i];
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[b].pBufferInfo = &buffers[b];
        }
        vkUpdateDescriptorSets(device(), 3, writes, 0, nullptr);
    }
    return true;
}

bool create_device_objects() {
    std::string cache_bytes;
    if (FILE* file = std::fopen(cache_path().c_str(), "rb")) {
        char buffer[1 << 16];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
            cache_bytes.append(buffer, read);
        }
        std::fclose(file);
    }
    VkPipelineCacheCreateInfo cache{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    cache.initialDataSize = cache_bytes.size();
    cache.pInitialData = cache_bytes.empty() ? nullptr : cache_bytes.data();
    if (vkCreatePipelineCache(device(), &cache, nullptr, &g_device.cache) != VK_SUCCESS) {
        cache.initialDataSize = 0;
        cache.pInitialData = nullptr;
        vkCreatePipelineCache(device(), &cache, nullptr, &g_device.cache);
    }
    if (!create_layouts()) {
        return false;
    }
    g_device.static_pool = make_descriptor_pool(16, true);
    g_device.fullscreen_vertex = make_module(kFullscreenVertexCode, sizeof kFullscreenVertexCode);
    g_device.present_fragment = make_module(kPresentCode, sizeof kPresentCode);
    g_device.present_smooth_fragment = make_module(kPresentSmoothCode, sizeof kPresentSmoothCode);
    g_device.overlay_fragment = make_module(kOverlayCode, sizeof kOverlayCode);
    g_device.clear_fragment = make_module(kClearCode, sizeof kClearCode);
    g_device.copy_fragment = make_module(kCopyCode, sizeof kCopyCode);
    g_device.tev_vertex = make_module(kTevVertexCode, sizeof kTevVertexCode);
    g_device.run_vertex = make_module(kRunVertexCode, sizeof kRunVertexCode);
    if (g_vk.dual_source) {
        g_device.tev_fragment[0] = make_module(kTevCode, sizeof kTevCode);
        g_device.tev_fragment[1] = make_module(kTevEarlyCode, sizeof kTevEarlyCode);
    } else {
        g_device.tev_fragment[0] = make_module(kTevSingleCode, sizeof kTevSingleCode);
        g_device.tev_fragment[1] = make_module(kTevEarlySingleCode, sizeof kTevEarlySingleCode);
    }
    for (int size = 0; size < 8; size++) {
        const ShaderCode& plain = g_vk.dual_source ? kTevSized[size] : kTevSingleSized[size];
        const ShaderCode& early = g_vk.dual_source ? kTevEarlySized[size] : kTevEarlySingleSized[size];
        g_device.tev_vertex_sized[size] = make_module(kTevVertexSized[size].code, kTevVertexSized[size].bytes);
        g_device.run_vertex_sized[size] = make_module(kRunVertexSized[size].code, kRunVertexSized[size].bytes);
        g_device.tev_fragment_sized[0][size] = make_module(plain.code, plain.bytes);
        g_device.tev_fragment_sized[1][size] = make_module(early.code, early.bytes);
        if (!g_device.tev_vertex_sized[size] || !g_device.run_vertex_sized[size] || !g_device.tev_fragment_sized[0][size] || !g_device.tev_fragment_sized[1][size]) {
            g_device.tev_fragment_sized[0][size] = VK_NULL_HANDLE;
            g_device.tev_fragment_sized[1][size] = VK_NULL_HANDLE;
        }
    }
    if (!g_device.static_pool || !g_device.fullscreen_vertex || !g_device.present_fragment || !g_device.present_smooth_fragment || !g_device.overlay_fragment ||
        !g_device.clear_fragment || !g_device.copy_fragment || !g_device.tev_vertex || !g_device.run_vertex || !g_device.tev_fragment[0] || !g_device.tev_fragment[1]) {
        log::write("video", "Vulkan refused the shaders");
        return false;
    }
    const VkAttachmentLoadOp color_loads[2] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR};
    const VkAttachmentLoadOp depth_loads[3] = {VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_LOAD_OP_NONE};
    for (int color = 0; color < 2; color++) {
        for (int depth = 0; depth < 3; depth++) {
            if (depth == 2 && !g_vk.load_none) {
                g_device.efb_passes[color][depth] = g_device.efb_passes[color][0];
                continue;
            }
            g_device.efb_passes[color][depth] =
                make_render_pass(kColorFormat, color_loads[color], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true,
                                 depth_loads[depth], depth == 2 ? VK_ATTACHMENT_STORE_OP_NONE : VK_ATTACHMENT_STORE_OP_STORE);
            if (!g_device.efb_passes[color][depth]) {
                return false;
            }
        }
    }
    g_device.target_pass = make_render_pass(kColorFormat, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
    if (!g_device.target_pass) {
        return false;
    }
    g_device.linear = make_sampler(VK_FILTER_LINEAR);
    g_device.point = make_sampler(VK_FILTER_NEAREST);
    PipelineShape copy;
    copy.vertex = g_device.fullscreen_vertex;
    copy.fragment = g_device.copy_fragment;
    copy.layout = g_device.copy_pipeline_layout;
    copy.pass = g_device.target_pass;
    copy.blend = kOpaque;
    g_device.copy = build_pipeline(copy);
    if (!g_device.copy) {
        return false;
    }
    const VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (!make_ring(g_device.screen, kScreenRingBytes, storage) || !make_ring(g_device.run_vertices, kRunVertexBytes, storage) ||
        !make_ring(g_device.run_indices, kRunIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT) ||
        !make_ring(g_device.run_rows, kRunRowCapacity * sizeof(float[4]), storage) || !make_ring(g_device.moved, kMovedRingBytes, storage) ||
        !make_ring(g_device.uniforms, kUniformRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !make_ring(g_device.staging, kStagingRingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) ||
        !make_buffer(g_device.kept_vertices, kKeptVertexBytes, storage, false) ||
        !make_buffer(g_device.kept_indices, kKeptIndexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, false)) {
        log::write("video", "Vulkan refused the vertex buffers");
        return false;
    }
    if (!create_draw_sets()) {
        return false;
    }
    const std::vector<std::vector<uint32_t>> black = {{0xFF000000u}};
    g_device.empty = upload_levels(black, 1, 1, false);
    return g_device.empty != nullptr;
}

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    if (!g_vk.usable) {
        g_device.failed = true;
        return false;
    }
    g_scale = requested_scale();
    if (!create_efb()) {
        std::fputs("cannot initialise the Vulkan renderer\n", stderr);
        log::write("video", "the Vulkan renderer could not start");
        g_device.failed = true;
        return false;
    }
    std::fprintf(stderr, "Vulkan renderer: %s\n", g_vk.properties.deviceName);
    log::write("video", "Vulkan renderer: %s, Vulkan %u.%u.%u, driver %u, depth format %d, dual-source blending %s, depth clamp %s, load/store none %s, in-between images %s",
               g_vk.properties.deviceName, VK_VERSION_MAJOR(g_vk.properties.apiVersion), VK_VERSION_MINOR(g_vk.properties.apiVersion),
               VK_VERSION_PATCH(g_vk.properties.apiVersion), g_vk.properties.driverVersion, static_cast<int>(g_vk.depth_format), g_vk.dual_source ? "yes" : "no",
               g_vk.depth_clamp ? "yes" : "no", g_vk.load_none ? "yes" : "no", g_vk.depth_copies ? "yes" : "no");
    log::write("video", "the graphics card places the vertices");
    prepare_texture_pack();
    start_workers();
    warm_pipelines();
    g_device.ready = true;
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

Image* upload_levels(const std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate, VkFormat format) {
    uint32_t levels = static_cast<uint32_t>(images.size());
    if (generate) {
        levels = 1;
        while ((width >> levels) != 0 || (height >> levels) != 0) {
            levels++;
        }
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (generate ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    Image* image = make_image(width, height, levels, format, usage);
    if (!image) {
        return nullptr;
    }
    size_t total = 0;
    for (size_t level = 0; level < images.size(); level++) {
        total += static_cast<size_t>(std::max(width >> level, 1u)) * std::max(height >> level, 1u) * 4;
    }
    uint32_t start = 0;
    Buffer* dedicated = nullptr;
    VkBuffer source = g_device.staging.buffer.buffer;
    uint8_t* memory = nullptr;
    if (total <= kStagingRingBytes / kRingSegments && reserve(g_device.staging, static_cast<uint32_t>(total), 16, start)) {
        memory = g_device.staging.buffer.mapped + start;
    } else {
        dedicated = new Buffer;
        if (!make_buffer(*dedicated, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false)) {
            delete dedicated;
            destroy_image(image);
            return nullptr;
        }
        source = dedicated->buffer;
        memory = dedicated->mapped;
        start = 0;
    }
    std::vector<VkBufferImageCopy> regions;
    size_t offset = 0;
    for (size_t level = 0; level < images.size(); level++) {
        uint32_t w = std::max(width >> level, 1u);
        uint32_t h = std::max(height >> level, 1u);
        size_t bytes = static_cast<size_t>(w) * h * 4;
        if (images[level].size() >= static_cast<size_t>(w) * h) {
            std::memcpy(memory + offset, images[level].data(), bytes);
        } else {
            std::memset(memory + offset, 0, bytes);
        }
        VkBufferImageCopy region{};
        region.bufferOffset = start + offset;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(level), 0, 1};
        region.imageExtent = {w, h, 1};
        regions.push_back(region);
        offset += bytes;
    }
    VkCommandBuffer cb = upload_command();
    transition(cb, *image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, source, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
    if (generate && levels > 1) {
        for (uint32_t level = 1; level < levels; level++) {
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = image->image;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0, 1};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            blit.srcOffsets[1] = {static_cast<int32_t>(std::max(width >> (level - 1), 1u)), static_cast<int32_t>(std::max(height >> (level - 1), 1u)), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            blit.dstOffsets[1] = {static_cast<int32_t>(std::max(width >> level, 1u)), static_cast<int32_t>(std::max(height >> level, 1u)), 1};
            vkCmdBlitImage(cb, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        }
        VkImageMemoryBarrier barriers[2]{};
        for (int i = 0; i < 2; i++) {
            barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[i].srcAccessMask = i == 0 ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[i].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barriers[i].oldLayout = i == 0 ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barriers[i].image = image->image;
            barriers[i].subresourceRange = i == 0 ? VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, levels - 1, 0, 1}
                                                  : VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, levels - 1, 1, 0, 1};
        }
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    } else {
        transition(cb, *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (dedicated) {
        retire([dedicated] {
            drop_buffer(*dedicated);
            delete dedicated;
        });
    }
    return image;
}

bool keep_photos() {
    static const settings::LiveFlag enabled("game.save_photos", "WP_SAVE_PHOTOS");
    return enabled();
}

Image* custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key, nullptr);
    if (!inserted) {
        return found->second;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return nullptr;
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

void release_cached(CachedTexture& entry) {
    if (entry.texture && entry.owned) {
        retire_image(entry.texture);
    }
    entry.texture = nullptr;
}

Image* texture_for(uint32_t map) {
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
        return copied->second.converted.image;
    }
    if (!g_device.write_backs.empty() && write_back_overlaps(address, request.size)) {
        finish_write_backs();
    }
    if (copied != g_device.copies.end() && copied->second.logical_width == width && copied->second.logical_height == height && format <= 6 &&
        copied->second.guest_hash == sample_hash(source, copied->second.bytes)) {
        note_texture(map, address, width, height, format, "EFB copy");
        return copied->second.converted.image;
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
    auto use = [&](CachedTexture& texture) {
        if (texture.custom) {
            g_custom_maps |= 1u << map;
        }
        return texture.texture;
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
    release_cached(entry);
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
    Image* custom = nullptr;
    if (named) {
        name = custom_textures::name_texture(source, request.size, width, height, format, mipmaps, request.tlut);
        if (g_pack.load) {
            std::string resolved = g_pack.index.resolve(name);
            if (!resolved.empty()) {
                custom = custom_texture(resolved, mipmaps);
            }
        }
    }
    if (format == 6 && width == screenshot::kPhotoWidth && height == screenshot::kPhotoHeight && levels == 1 && copied == g_device.copies.end() && !custom && keep_photos()) {
        std::vector<std::vector<uint32_t>> photo = decode_levels(request);
        std::string path = screenshot::photo_matching(screenshot::photo_signature(photo[0].data(), width, height, true));
        if (!path.empty()) {
            custom = custom_texture(path, false);
            log::write("photo", "shown from %s", path.c_str());
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

VkSamplerAddressMode vk_wrap(uint32_t wrap) {
    return wrap == 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : wrap == 2 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

VkSampler sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return found->second;
    }
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = params.mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = params.min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = params.mip_mode == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = vk_wrap(params.wrap_s);
    info.addressModeV = vk_wrap(params.wrap_t);
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    float limit = g_vk.properties.limits.maxSamplerLodBias;
    info.mipLodBias = std::clamp(params.lod_bias, -limit, limit);
    info.minLod = params.min_lod;
    info.maxLod = params.unlimited_lod ? 1000.0f : std::max(params.max_lod, params.min_lod);
    if (g_vk.anisotropy && g_anisotropy > 1 && params.min_linear && params.mag_linear) {
        info.anisotropyEnable = VK_TRUE;
        info.maxAnisotropy = std::min(static_cast<float>(g_anisotropy), g_vk.properties.limits.maxSamplerAnisotropy);
    }
    VkSampler sampler = VK_NULL_HANDLE;
    vkCreateSampler(device(), &info, nullptr, &sampler);
    g_device.samplers[params.key] = sampler;
    return sampler;
}

struct RunDraw {
    uint32_t layout = 0;
    uint32_t stride = 0;
    uint32_t cull = 0;
    uint32_t vertex_offset = 0;
    uint32_t first_index = 0;
    uint32_t indices = 0;
    uint32_t row = 0;
    uint32_t rows = 0;
    uint32_t palette = kNoPalette;
    uint32_t matrix = 0;
    uint32_t camera = 0;
    uint64_t key = 0;
    uint64_t shape = 0;
    bool kept = false;
};

struct DrawConstants {
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

struct RecordedDraw {
    uint64_t key = 0;
    uint64_t exact = 0;
    uint64_t material = 0;
    uint32_t camera = 0;
    uint32_t first_position = 0;
    uint32_t positions = 0;
    size_t first_vertex = 0;
    uint32_t vertices = 0;
};

struct CopyCommand {
    Target target;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    bool intensity = false;
    bool alpha = false;
    bool depth = false;
    CopyFilter filter;
};

struct ClearCommand {
    bool color = false;
    bool alpha = false;
    bool depth = false;
    VkClearColorValue color_value{};
    float depth_value = 1.0f;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct RecordedRun {
    RunDraw draw;
    uint32_t match = ~0u;
};

struct RecordedCommand {
    enum Kind { kBatch, kCopy, kClear, kRuns } kind = kBatch;
    BatchState batch;
    size_t begin = 0;
    size_t end = 0;
    uint32_t base = 0;
    CopyCommand copy;
    ClearCommand clear;
};

struct RecordedTarget {
    Image* image = nullptr;
    bool twin = false;
};

struct ReplayTarget {
    Image* before = nullptr;
    Target texture;
    uint64_t frame = ~0ull;
};

struct MovedVertex {
    std::array<float, 4> position;
    std::array<float, 4> color[2];
};

struct FrameRecord {
    std::vector<uint16_t> source_of;
    std::vector<std::array<float, 4>> positions;
    std::vector<std::array<std::array<float, 4>, 2>> colors;
    std::vector<RecordedDraw> draws;
    std::vector<RecordedCommand> commands;
    std::vector<RecordedTarget> targets;
    std::vector<RecordedRun> runs;
    std::vector<std::array<float, 4>> rows;
};

struct CommandColors {
    bool runs = false;
    size_t begin = 0;
    size_t end = 0;
    uint64_t shape = 0;
    float initial[4][4] = {};
    float konst[4][4] = {};
};

FrameRecord g_record;
FrameRecord g_previous;
std::vector<CommandColors> g_previous_colors;
std::vector<uint32_t> g_color_partner;
size_t g_frame_vertices = 0;
uint32_t g_interpolated_ready = 0;
std::atomic<uint32_t> g_interpolated_frames{0};
uint32_t g_ring_marks[3] = {};

uint64_t command_shape(const BatchState& batch) {
    const Constants& constants = batch.constants;
    return hash_bytes(reinterpret_cast<const uint8_t*>(constants.stage), sizeof constants.stage) ^
           hash_bytes(reinterpret_cast<const uint8_t*>(constants.header), sizeof constants.header) * 31 ^
           hash_bytes(reinterpret_cast<const uint8_t*>(constants.swaps), sizeof constants.swaps) * 961 ^ batch.blend;
}

bool send_replayed();
void service_deferred();

#include "gx_between.inc"

bool batch_empty() {
    return g_pending.vertices.empty() && g_run_draws.empty();
}

template <typename T>
void reserve_run(std::vector<T>& values, size_t wanted) {
    if (values.size() < wanted) {
        values.resize(std::max(wanted, values.size() * 2));
    }
}

uint64_t maps_hash(const BatchState& pending) {
    uint64_t hash = 1469598103934665603ull;
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        hash = (hash ^ reinterpret_cast<uintptr_t>(pending.views[map])) * 1099511628211ull;
        hash = (hash ^ reinterpret_cast<uintptr_t>(pending.samplers[map])) * 1099511628211ull;
    }
    return hash;
}

VkDescriptorSet maps_set(const BatchState& pending) {
    uint64_t hash = maps_hash(pending);
    auto found = g_current->map_sets.find(hash);
    if (found != g_current->map_sets.end()) {
        return found->second;
    }
    VkDescriptorSet set = allocate_set(g_device.maps_layout);
    if (!set) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorImageInfo images[kTextureMaps];
    VkWriteDescriptorSet writes[kTextureMaps]{};
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        const Image* image = pending.views[map] ? pending.views[map] : g_device.empty;
        images[map] = {pending.samplers[map] ? pending.samplers[map] : g_device.point, image->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[map].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[map].dstSet = set;
        writes[map].dstBinding = map;
        writes[map].descriptorCount = 1;
        writes[map].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[map].pImageInfo = &images[map];
    }
    vkUpdateDescriptorSets(device(), kTextureMaps, writes, 0, nullptr);
    g_current->map_sets[hash] = set;
    return set;
}

void prepare_views(const BatchState& pending) {
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        Image* image = pending.views[map];
        if (image && image->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            if (g_pass != Pass::None) {
                end_pass();
            }
            transition(command(), *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
}

bool batch_scissor(const BatchState& pending, VkRect2D& rect) {
    int left = static_cast<int>(((pending.top_left >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset));
    int top = static_cast<int>((pending.top_left & 0x7FF) - scissor_offset_y(pending.scissor_offset));
    int right = static_cast<int>(((pending.bottom_right >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset) + 1);
    int bottom = static_cast<int>((pending.bottom_right & 0x7FF) - scissor_offset_y(pending.scissor_offset) + 1);
    if (canvas_grown() && covers_picture(left, top, right - left, bottom - top)) {
        if (pending.canvas_kind != kInPicture) {
            right += 2 * g_canvas.margin_x;
            bottom += 2 * g_canvas.margin_y;
            left = 0;
            top = 0;
        } else {
            left = g_canvas.margin_x;
            top = g_canvas.margin_y;
            right = kEfbWidth + g_canvas.margin_x;
            bottom = kPictureHeight + g_canvas.margin_y;
        }
        if (pending.canvas_kind == kWide) {
            top = g_canvas.margin_y;
            bottom = kPictureHeight + g_canvas.margin_y;
        }
    } else {
        left += g_canvas.margin_x;
        top += g_canvas.margin_y;
        right += g_canvas.margin_x;
        bottom += g_canvas.margin_y;
    }
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, canvas_width());
    bottom = std::min(bottom, canvas_height());
    if (right <= left || bottom <= top) {
        return false;
    }
    rect.offset = {scaled(left), scaled(top)};
    rect.extent = {static_cast<uint32_t>(scaled(right - left)), static_cast<uint32_t>(scaled(bottom - top))};
    return true;
}

bool set_batch_state(const BatchState& pending, uint32_t uniform, int source) {
    VkRect2D rect{};
    if (!batch_scissor(pending, rect)) {
        return false;
    }
    prepare_views(pending);
    begin_efb((pending.depth & 1) != 0);
    VkDescriptorSet maps = maps_set(pending);
    if (!maps) {
        return false;
    }
    vkCmdSetScissor(g_current->draw, 0, 1, &rect);
    VkDescriptorSet draw_set = g_device.draw_sets[source];
    if (g_bound_draw_set != draw_set || g_bound_uniform != uniform) {
        vkCmdBindDescriptorSets(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.tev_layout, 0, 1, &draw_set, 1, &uniform);
        g_bound_draw_set = draw_set;
        g_bound_uniform = uniform;
    }
    if (g_bound_maps != maps) {
        vkCmdBindDescriptorSets(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.tev_layout, 1, 1, &maps, 0, nullptr);
        g_bound_maps = maps;
    }
    return true;
}

bool upload_constants(const BatchState& pending, uint32_t& offset) {
    return upload(g_device.uniforms, &pending.constants, sizeof(Constants), static_cast<uint32_t>(g_vk.uniform_alignment), offset);
}

void issue_runs(const RunDraw* draws, size_t count, uint32_t vertex_start, uint32_t index_start, uint32_t first_row, uint32_t uniform, const BatchState& state) {
    int bound = -1;
    uint32_t bound_cull = ~0u;
    for (size_t i = 0; i < count; i++) {
        const RunDraw& draw = draws[i];
        int wanted = draw.kept ? 1 : 0;
        if (bound != wanted) {
            VkDescriptorSet set = g_device.draw_sets[draw.kept ? 2 : 1];
            vkCmdBindDescriptorSets(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.tev_layout, 0, 1, &set, 1, &uniform);
            g_bound_draw_set = set;
            g_bound_uniform = uniform;
            vkCmdBindIndexBuffer(g_current->draw, draw.kept ? g_device.kept_indices.buffer : g_device.run_indices.buffer.buffer, 0, VK_INDEX_TYPE_UINT16);
            bound = wanted;
        }
        if (bound_cull != draw.cull) {
            VkPipeline pipeline = tev_pipeline(state, 1, draw.cull);
            if (!pipeline) {
                continue;
            }
            bind_pipeline(pipeline);
            bound_cull = draw.cull;
        }
        DrawConstants constants;
        constants.base = (draw.kept ? draw.vertex_offset : vertex_start + draw.vertex_offset) / 4;
        constants.stride = draw.stride / 4;
        constants.layout = draw.layout;
        constants.row = first_row + draw.row;
        vkCmdPushConstants(g_current->draw, g_device.tev_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof constants, &constants);
        uint32_t first_index = draw.kept ? draw.first_index : index_start + draw.first_index;
        vkCmdDrawIndexed(g_current->draw, draw.indices, 1, first_index, 0, 0);
    }
}

void record_runs(const BatchState& pending, uint32_t vertex_start, uint32_t index_start) {
    RecordedCommand command;
    command.kind = RecordedCommand::kRuns;
    command.batch = pending;
    command.begin = g_record.runs.size();
    uint32_t base = static_cast<uint32_t>(g_record.rows.size());
    g_record.rows.insert(g_record.rows.end(), g_run_row_values.begin(), g_run_row_values.begin() + g_run_row_used);
    for (const RunDraw& draw : g_run_draws) {
        RecordedRun run;
        run.draw = draw;
        if (!draw.kept) {
            run.draw.vertex_offset += vertex_start;
            run.draw.first_index += index_start;
        }
        run.draw.row += base;
        run.draw.matrix += base;
        if (draw.palette != kNoPalette) {
            run.draw.palette += base;
        }
        g_record.runs.push_back(run);
    }
    command.end = g_record.runs.size();
    g_record.commands.push_back(command);
}

void draw_runs() {
    uint32_t vertex_start = 0;
    uint32_t index_start = 0;
    uint32_t row_start = 0;
    uint32_t uniform = 0;
    bool ok = upload(g_device.run_rows, g_run_row_values.data(), g_run_row_used * sizeof(float[4]), 16, row_start) &&
              upload(g_device.run_vertices, g_run_vertex_bytes.data(), g_run_vertex_used, 16, vertex_start) &&
              upload(g_device.run_indices, g_run_index_words.data(), g_run_index_used * sizeof(uint16_t), 16, index_start) && upload_constants(g_pending, uniform);
    uint32_t first_index_word = index_start / static_cast<uint32_t>(sizeof(uint16_t));
    if (ok && set_batch_state(g_pending, uniform, 1)) {
        issue_runs(g_run_draws.data(), g_run_draws.size(), vertex_start, first_index_word, row_start / static_cast<uint32_t>(sizeof(float[4])), uniform, g_pending);
    }
    if (g_recording) {
        g_record_broken = g_record_broken || !ok;
        record_runs(g_pending, vertex_start, first_index_word);
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
        draw_runs();
        g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return;
    }
    if (pending.vertices.empty()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    g_batches++;
    g_batch_vertices += static_cast<uint32_t>(pending.vertices.size());
    VkPipeline pipeline = tev_pipeline(pending, 0, 0);
    uint32_t uniform = 0;
    size_t total = pending.vertices.size();
    if (pipeline && upload_constants(pending, uniform)) {
        size_t offset = 0;
        while (offset < total) {
            uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexCapacity));
            uint32_t start = 0;
            if (!upload(g_device.screen, pending.vertices.data() + offset, batch * sizeof(ScreenVertex), 16, start)) {
                break;
            }
            bool shown = set_batch_state(pending, uniform, 0);
            if (shown) {
                bind_pipeline(pipeline);
                DrawConstants constants{start / 4, kScreenWords, 0, 0};
                vkCmdPushConstants(g_current->draw, g_device.tev_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof constants, &constants);
                vkCmdDraw(g_current->draw, batch, 1, 0, 0);
            }
            if (g_recording) {
                RecordedCommand command;
                command.kind = RecordedCommand::kBatch;
                command.batch = pending;
                command.begin = g_frame_vertices + offset;
                command.end = command.begin + batch;
                command.base = start / 4;
                g_record.commands.push_back(command);
            } else if (!shown) {
                break;
            }
            offset += batch;
        }
        if (g_recording && offset < total) {
            g_record_broken = true;
        }
    }
    if (g_recording) {
        g_frame_vertices += total;
    }
    pending.vertices.clear();
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void select_batch() {
    static uint64_t batched_serial = ~0ull;
    static uint64_t batched_epoch = 0;
    static int batched_scale = 0;
    static int batched_canvas = kInPicture;
    PendingBatch& pending = g_pending;
    if (!batch_empty() && batched_serial == register_serial() && batched_epoch == g_texture_epoch && batched_scale == g_scale && batched_canvas == g_canvas_kind) {
        return;
    }
    const uint32_t* bp = bp_registers();
    Constants constants;
    fill_constants(constants, g_scale, g_canvas.margin_x);
    Image* views[kTextureMaps] = {};
    VkSampler samplers[kTextureMaps] = {};
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
    bool keeps_all = alpha_test.can_pass && !alpha_test.can_fail;
    bool same = !batch_empty() && pending.blend == blend && pending.depth == depth && pending.early_depth == early_depth && pending.keeps_all == keeps_all &&
                pending.top_left == bp[0x20] && pending.bottom_right == bp[0x21] && pending.scissor_offset == bp[kBpScissorOffset] &&
                pending.canvas_kind == g_canvas_kind && std::memcmp(&pending.constants, &constants, sizeof constants) == 0 &&
                std::memcmp(pending.views, views, sizeof views) == 0 && std::memcmp(pending.samplers, samplers, sizeof samplers) == 0;
    if (!same) {
        flush_pending();
        pending.constants = constants;
        std::memcpy(pending.views, views, sizeof views);
        std::memcpy(pending.samplers, samplers, sizeof samplers);
        pending.blend = blend;
        pending.depth = depth;
        pending.early_depth = early_depth;
        pending.keeps_all = keeps_all;
        pending.top_left = bp[0x20];
        pending.bottom_right = bp[0x21];
        pending.scissor_offset = bp[kBpScissorOffset];
        pending.canvas_kind = g_canvas_kind;
    }
    batched_serial = register_serial();
    batched_epoch = g_texture_epoch;
    batched_scale = g_scale;
    batched_canvas = g_canvas_kind;
}

void append(const ScreenVertex* vertices, uint32_t count, const DrawSource* source) {
    size_t first = g_frame_vertices + g_pending.vertices.size();
    g_pending.vertices.insert(g_pending.vertices.end(), vertices, vertices + count);
    if (!g_recording) {
        return;
    }
    if (source && source->key != 0) {
        RecordedDraw draw;
        draw.key = source->key;
        draw.exact = source->exact;
        draw.material = command_shape(g_pending);
        draw.camera = source->camera;
        draw.first_position = static_cast<uint32_t>(g_record.positions.size());
        draw.positions = source->count;
        draw.first_vertex = first;
        draw.vertices = count;
        g_record.draws.push_back(draw);
        const auto* positions = reinterpret_cast<const std::array<float, 4>*>(source->positions);
        g_record.positions.insert(g_record.positions.end(), positions, positions + source->count);
        g_record.source_of.insert(g_record.source_of.end(), source->source_of, source->source_of + count);
        g_record.colors.resize(g_record.positions.size());
        for (uint32_t v = 0; v < count; v++) {
            if (source->source_of[v] != kNoSource) {
                auto& color = g_record.colors[draw.first_position + source->source_of[v]];
                std::copy(vertices[v].color[0], vertices[v].color[0] + 4, color[0].begin());
                std::copy(vertices[v].color[1], vertices[v].color[1] + 4, color[1].begin());
            }
        }
    } else {
        g_record.source_of.insert(g_record.source_of.end(), count, kNoSource);
    }
}

bool add_run(const VertexRun& run, uint32_t bytes, uint32_t index_count) {
    finish_frame();
    service_deferred();
    uint32_t rows = run.row_count + (run.palette ? kPaletteRows + kNormalRows : 0);
    if (!g_pending.vertices.empty() || g_run_vertex_used + bytes > kRunVertexBytes / kRingSegments || (g_run_index_used + index_count) * sizeof(uint16_t) > kRunIndexBytes / kRingSegments ||
        g_run_row_used + rows > kRunRowBatch) {
        flush_pending();
    }
    select_batch();
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
    draw.rows = run.row_count;
    draw.palette = run.palette ? g_run_palette : kNoPalette;
    draw.matrix = run.palette ? g_run_palette + run.matrix : g_run_row_used + 4;
    draw.camera = run.camera;
    draw.key = run.key;
    draw.shape = run.shape;
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
    int efb_height = scaled(canvas_height());
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
    constants.extent[0] = static_cast<float>(scaled(canvas_width()));
    constants.extent[1] = static_cast<float>(efb_height);
    if (!target.image) {
        return false;
    }
    settle_efb();
    VkCommandBuffer cb = command();
    transition(cb, *g_device.efb_color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, *g_device.efb_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    transition(cb, *target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_render_pass(g_device.target_pass, target.image->framebuffer, target.image->width, target.image->height, 0, nullptr);
    g_pass = Pass::Target;
    set_viewport(0, 0, static_cast<float>(target_width), static_cast<float>(target_height));
    set_scissor(0, 0, static_cast<int>(target.image->width), static_cast<int>(target.image->height));
    bind_pipeline(g_device.copy);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.copy_pipeline_layout, 0, 1, &g_device.copy_set, 0, nullptr);
    vkCmdPushConstants(cb, g_device.copy_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end_pass();
    transition(cb, *target.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

Buffer* take_buffer() {
    if (!g_device.free_buffers.empty()) {
        Buffer* buffer = g_device.free_buffers.back();
        g_device.free_buffers.pop_back();
        return buffer;
    }
    Buffer* buffer = new Buffer;
    if (!make_buffer(*buffer, static_cast<VkDeviceSize>(kEfbWidth) * kEfbHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        delete buffer;
        return nullptr;
    }
    return buffer;
}

void read_back(Image& image, uint32_t width, uint32_t height, VkBuffer buffer) {
    end_pass();
    VkCommandBuffer cb = command();
    VkImageLayout previous = image.layout;
    transition(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    if (previous == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        transition(cb, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void complete_write_backs(bool wait);

bool queue_write_back(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                      bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back.image && !create_render_texture(g_device.write_back, kEfbWidth, kEfbHeight)) {
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
    Buffer* buffer = take_buffer();
    if (!buffer) {
        return false;
    }
    if (!run_copy(g_device.write_back, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter)) {
        g_device.free_buffers.push_back(buffer);
        return false;
    }
    read_back(*g_device.write_back.image, logical_width, logical_height, buffer->buffer);
    PendingWriteBack pending;
    pending.buffer = buffer;
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
    const uint8_t* pixels = pending.buffer->mapped;
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

bool read_target(Image* image, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels) {
    if (!image || width == 0 || height == 0) {
        return false;
    }
    Buffer buffer;
    if (!make_buffer(buffer, static_cast<VkDeviceSize>(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
        return false;
    }
    if (image == g_device.efb_color) {
        settle_efb();
    }
    read_back(*image, width, height, buffer.buffer);
    wait_serial(current_serial());
    const uint8_t* bytes = buffer.mapped;
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (static_cast<uint32_t>(bytes[i * 4]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | bytes[i * 4 + 2];
    }
    drop_buffer(buffer);
    return true;
}

void dump_target(Image* image, uint32_t width, uint32_t height, const char* suffix) {
    std::vector<uint32_t> pixels;
    if (image && read_target(image, width, height, pixels)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_target(g_device.efb_color, static_cast<uint32_t>(scaled(canvas_width())), static_cast<uint32_t>(scaled(canvas_height())), "_efb");
    dump_target(entry.converted.image, entry.converted.width, entry.converted.height, "");
}

void apply_anisotropy() {
    static const settings::LiveNumber setting("video.anisotropy", "WP_ANISOTROPY");
    int level = std::clamp(setting(), 1, 16);
    if (level == g_anisotropy) {
        return;
    }
    flush_pending();
    for (auto& entry : g_device.samplers) {
        VkSampler sampler = entry.second;
        retire([sampler] { vkDestroySampler(device(), sampler, nullptr); });
    }
    g_device.samplers.clear();
    g_anisotropy = level;
}

void apply_scale() {
    int scale = requested_scale();
    Canvas wanted = wanted_canvas(g_picture_aspect, g_window_aspect);
    if (scale == g_scale && wanted.margin_x == g_canvas.margin_x && wanted.margin_y == g_canvas.margin_y) {
        return;
    }
    flush_pending();
    int previous = g_scale;
    Canvas previous_canvas = g_canvas;
    release_efb();
    g_scale = scale;
    g_canvas = wanted;
    if (!create_efb()) {
        release_efb();
        g_scale = previous;
        g_canvas = previous_canvas;
        if (!create_efb()) {
            std::fputs("cannot recreate the EFB after a scale change\n", stderr);
            g_device.ready = false;
            g_device.failed = true;
            return;
        }
    }
    g_shared_margins.store(static_cast<uint32_t>(g_canvas.margin_x) << 16 | static_cast<uint32_t>(g_canvas.margin_y), std::memory_order_release);
    g_efb_before_valid = false;
    g_record_broken = g_record_broken || g_recording;
    log::write("video", "picture %dx%d at scale %d", canvas_width(), kPictureHeight + 2 * g_canvas.margin_y, g_scale);
}

void upload_overlay(Overlay& target, const options::Image& image) {
    if (image.pixels.size() != static_cast<size_t>(image.width) * image.height || image.width == 0 || image.height == 0) {
        return;
    }
    const std::vector<std::vector<uint32_t>> levels = {image.pixels};
    Image* uploaded = upload_levels(levels, image.width, image.height, false, kImageFormat);
    if (!uploaded) {
        return;
    }
    retire_image(target.image);
    target.image = uploaded;
    target.width = image.width;
    target.height = image.height;
}

void draw_image(const Image* image, const options::Rect& placed, bool blend) {
    if (!image || placed.width <= 0 || placed.height <= 0) {
        return;
    }
    VkDescriptorSet set = single_set(*image, g_device.linear);
    VkPipeline pipeline = screen_pipeline(2, blend);
    if (!set || !pipeline) {
        return;
    }
    set_viewport(static_cast<float>(placed.x), static_cast<float>(placed.y), static_cast<float>(placed.width), static_cast<float>(placed.height));
    bind_pipeline(pipeline);
    vkCmdBindDescriptorSets(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.single_pipeline_layout, 0, 1, &set, 0, nullptr);
    vkCmdDraw(g_current->draw, 3, 1, 0, 0);
}

void take_overlays(int client_width, int client_height) {
    options::Image image;
    if (perf::take(g_device.hud_version, image, g_device.hud_visible, g_device.hud_corner) && !image.pixels.empty()) {
        upload_overlay(g_device.hud_image, image);
    }
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty()) {
        upload_overlay(g_device.overlay_image, image);
    }
    if (hint::take(g_device.hint_version, image, g_device.hint_visible) && !image.pixels.empty()) {
        upload_overlay(g_device.hint_image, image);
    }
    options::Image knob;
    if (touch::take(g_device.touch_version, image, knob, g_device.touch_visible, client_width, client_height) && g_device.touch_visible && !image.pixels.empty()) {
        upload_overlay(g_device.touch_image, image);
        upload_overlay(g_device.knob_image, knob);
    }
    if (g_device.touch_visible) {
        options::Image pressed;
        options::Image base;
        if (touch::take_extras(g_device.extras_version, pressed, base, client_height)) {
            upload_overlay(g_device.pressed_image, pressed);
            upload_overlay(g_device.base_image, base);
        }
    }
    if (titlebar::take(g_device.bar_version, image)) {
        upload_overlay(g_device.bar_image, image);
    }
}

void draw_overlay(int client_width, int client_height, int bar) {
    if (g_device.hud_visible) {
        options::Rect placed = perf::corner_rect(client_width, client_height - bar, static_cast<int>(g_device.hud_image.width), static_cast<int>(g_device.hud_image.height),
                                                 g_device.hud_corner);
        placed.y += bar;
        draw_image(g_device.hud_image.image, placed, true);
    }
    if (g_device.hint_visible) {
        options::Rect placed = hint::place(client_width, client_height - bar, static_cast<int>(g_device.hint_image.width), static_cast<int>(g_device.hint_image.height));
        placed.y += bar;
        draw_image(g_device.hint_image.image, placed, true);
    }
    if (g_device.touch_visible && !g_device.overlay_visible) {
        draw_image(g_device.touch_image.image, options::Rect{0, 0, client_width, client_height}, true);
        draw_image(g_device.base_image.image, touch::base_rect(client_height), true);
        static std::vector<options::Rect> pressed_rects;
        touch::pressed_rects(pressed_rects);
        for (const options::Rect& rect : pressed_rects) {
            draw_image(g_device.pressed_image.image, rect, true);
        }
        draw_image(g_device.knob_image.image, touch::knob_rect(client_height), true);
    }
    if (g_device.overlay_visible) {
        draw_image(g_device.overlay_image.image,
                   options::place(client_width, client_height, static_cast<int>(g_device.overlay_image.width), static_cast<int>(g_device.overlay_image.height)), true);
    }
    if (bar > 0 && g_device.bar_image.image) {
        draw_image(g_device.bar_image.image, options::Rect{0, 0, client_width, bar}, false);
    }
}

void drop_swapchain_images() {
    Swapchain& chain = g_device.chain;
    for (VkFramebuffer framebuffer : chain.framebuffers) {
        vkDestroyFramebuffer(device(), framebuffer, nullptr);
    }
    for (VkImageView view : chain.views) {
        vkDestroyImageView(device(), view, nullptr);
    }
    for (VkSemaphore semaphore : chain.rendered) {
        vkDestroySemaphore(device(), semaphore, nullptr);
    }
    for (VkSemaphore semaphore : chain.acquired) {
        vkDestroySemaphore(device(), semaphore, nullptr);
    }
    chain.framebuffers.clear();
    chain.views.clear();
    chain.rendered.clear();
    chain.acquired.clear();
    chain.images.clear();
}

void drop_surface() {
    Swapchain& chain = g_device.chain;
    if (!chain.surface && !chain.swapchain) {
        return;
    }
    wait_idle();
    vkDeviceWaitIdle(device());
    drop_swapchain_images();
    if (chain.swapchain) {
        vkDestroySwapchainKHR(device(), chain.swapchain, nullptr);
        chain.swapchain = VK_NULL_HANDLE;
    }
    if (chain.surface) {
        vkDestroySurfaceKHR(g_vk.instance, chain.surface, nullptr);
        chain.surface = VK_NULL_HANDLE;
    }
    chain.width = 0;
    chain.height = 0;
}

VkPresentModeKHR wanted_present_mode(bool vsync) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical, g_device.chain.surface, &count, nullptr);
    std::vector<VkPresentModeKHR> modes(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.physical, g_device.chain.surface, &count, modes.data());
    auto has = [&](VkPresentModeKHR mode) { return std::find(modes.begin(), modes.end(), mode) != modes.end(); };
    if (vsync) {
        return VK_PRESENT_MODE_FIFO_KHR;
    }
    if (has(VK_PRESENT_MODE_MAILBOX_KHR)) {
        return VK_PRESENT_MODE_MAILBOX_KHR;
    }
    if (has(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
        return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

bool ensure_swapchain(SDL_Window* window, int width, int height) {
    Swapchain& chain = g_device.chain;
    static const settings::LiveFlag vsync("video.vsync", "WP_VSYNC");
    bool synced = vsync();
    if (chain.swapchain && !chain.stale && chain.width == static_cast<uint32_t>(width) && chain.height == static_cast<uint32_t>(height) && chain.vsync == synced) {
        return true;
    }
    if (!chain.surface) {
        if (!SDL_Vulkan_CreateSurface(window, g_vk.instance, nullptr, &chain.surface)) {
            log::write("video", "no Vulkan surface for the window: %s", SDL_GetError());
            chain.surface = VK_NULL_HANDLE;
            return false;
        }
        VkBool32 supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.physical, g_vk.family, chain.surface, &supported);
        if (!supported) {
            log::write("video", "the Vulkan queue cannot show pictures on this window");
            return false;
        }
    }
    wait_idle();
    vkDeviceWaitIdle(device());
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.physical, chain.surface, &caps) != VK_SUCCESS) {
        return false;
    }
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical, chain.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.physical, chain.surface, &format_count, formats.data());
    if (formats.empty()) {
        return false;
    }
    VkSurfaceFormatKHR chosen = formats[0];
    for (const VkSurfaceFormatKHR& format : formats) {
        if ((format.format == VK_FORMAT_R8G8B8A8_UNORM || format.format == VK_FORMAT_B8G8R8A8_UNORM) && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = format;
            break;
        }
    }
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = chain.surface;
    uint32_t count = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount && count > caps.maxImageCount) {
        count = caps.maxImageCount;
    }
    info.minImageCount = count;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
#ifdef __ANDROID__
    info.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
#else
    info.imageExtent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    info.preTransform = caps.currentTransform;
#endif
    if (info.imageExtent.width == 0 || info.imageExtent.height == 0) {
        return false;
    }
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        for (uint32_t bit = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1) {
            if (caps.supportedCompositeAlpha & bit) {
                info.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
                break;
            }
        }
    }
    info.presentMode = wanted_present_mode(synced);
    info.clipped = VK_TRUE;
    info.oldSwapchain = chain.swapchain;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    VkResult result = vkCreateSwapchainKHR(device(), &info, nullptr, &created);
    drop_swapchain_images();
    if (chain.swapchain) {
        vkDestroySwapchainKHR(device(), chain.swapchain, nullptr);
    }
    chain.swapchain = VK_NULL_HANDLE;
    if (result != VK_SUCCESS) {
        log::write("video", "Vulkan refused the swapchain (%d)", static_cast<int>(result));
        return false;
    }
    chain.swapchain = created;
    if (chain.format != chosen.format || !chain.pass) {
        if (chain.pass) {
            vkDestroyRenderPass(device(), chain.pass, nullptr);
            for (auto& entry : g_device.screen_pipelines) {
                vkDestroyPipeline(device(), entry.second, nullptr);
            }
            g_device.screen_pipelines.clear();
        }
        chain.format = chosen.format;
        chain.pass = make_render_pass(chosen.format, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false,
                                      VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE);
    }
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device(), chain.swapchain, &image_count, nullptr);
    chain.images.resize(image_count);
    vkGetSwapchainImagesKHR(device(), chain.swapchain, &image_count, chain.images.data());
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < image_count; i++) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = chain.images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = chain.format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView created_view = VK_NULL_HANDLE;
        vkCreateImageView(device(), &view, nullptr, &created_view);
        chain.views.push_back(created_view);
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        make_framebuffer(chain.pass, &created_view, 1, info.imageExtent.width, info.imageExtent.height, framebuffer);
        chain.framebuffers.push_back(framebuffer);
        VkSemaphore rendered = VK_NULL_HANDLE;
        vkCreateSemaphore(device(), &semaphore, nullptr, &rendered);
        chain.rendered.push_back(rendered);
    }
    for (uint32_t i = 0; i < image_count + 1; i++) {
        VkSemaphore acquired = VK_NULL_HANDLE;
        vkCreateSemaphore(device(), &semaphore, nullptr, &acquired);
        chain.acquired.push_back(acquired);
    }
    chain.next_acquire = 0;
    chain.width = info.imageExtent.width;
    chain.height = info.imageExtent.height;
    chain.mode = info.presentMode;
    chain.vsync = synced;
    chain.stale = false;
    log::write("video", "Vulkan swapchain %ux%u, %u images, present mode %d, transform %u", chain.width, chain.height, image_count,
               static_cast<int>(chain.mode), static_cast<unsigned>(caps.currentTransform));
    return true;
}

void report_frame_figures() {
    std::lock_guard<std::mutex> hold(g_compile_lock);
    static auto logged = std::chrono::steady_clock::now();
    static uint64_t logged_frame = g_frame;
    auto now = std::chrono::steady_clock::now();
    if (now - logged < std::chrono::seconds(1)) {
        return;
    }
    double frames = static_cast<double>(std::max<uint64_t>(g_frame - logged_frame, 1));
    if (g_variants_ready != g_reported_ready) {
        g_reported_ready = g_variants_ready;
        log::write("video", "%u pixel shader variants ready, %u refused, %zu waiting", g_variants_ready, g_variants_failed, g_jobs.size());
    }
    log::write("video", "Vulkan: %.1f EFB passes per frame, %.1f started by a clear, %.1f submissions, scale %d", g_efb_passes / frames, g_cleared_passes / frames,
               g_submits / frames, g_scale);
    log::write("video", "Vulkan GPU busy %.0f ms a second: EFB %.0f ms in %u passes, copies %.0f ms in %u, screen %.0f ms in %u", g_gpu_time * 1000.0,
               g_pass_time[1] * 1000.0, g_pass_count[1], g_pass_time[2] * 1000.0, g_pass_count[2], g_pass_time[3] * 1000.0, g_pass_count[3]);
    g_gpu_time = 0.0;
    for (int i = 0; i < 4; i++) {
        g_pass_time[i] = 0.0;
        g_pass_count[i] = 0;
    }
    log::write("video", "Vulkan waits a second: %.0f ms on fences (%.0f frame, %.0f buffers, %.0f idle in %u), %.0f ms acquire and present", g_fence_wait * 1000.0,
               g_frame_wait * 1000.0, g_ring_wait * 1000.0, g_idle_wait * 1000.0, g_idle_waits, g_present_wait * 1000.0);
    g_fence_wait = g_frame_wait = g_ring_wait = g_idle_wait = g_present_wait = 0.0;
    g_idle_waits = 0;
    logged = now;
    logged_frame = g_frame;
    g_efb_passes = 0;
    g_cleared_passes = 0;
    g_submits = 0;
}

void maybe_save_cache() {
    static auto saved = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    uint32_t ready = 0;
    bool idle = false;
    {
        std::lock_guard<std::mutex> hold(g_compile_lock);
        ready = g_variants_ready + static_cast<uint32_t>(g_generic.size());
        idle = g_jobs.empty();
    }
    if (ready != g_saved_ready && (now - saved >= std::chrono::seconds(20) || (idle && now - saved >= std::chrono::seconds(5)))) {
        g_saved_ready = ready;
        saved = now;
        save_pipeline_cache();
    }
}

}

const char* api_name() {
    return "Vulkan";
}

int auto_scale() {
    return 2;
}

bool interpolating() {
    return g_recording;
}

bool probe() {
    if (g_vk.probed) {
        return g_vk.usable;
    }
    g_vk.probed = true;
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        log::write("video", "no Vulkan library: %s", SDL_GetError());
        return false;
    }
    Uint32 extension_count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!extensions) {
        log::write("video", "Vulkan cannot show pictures here: %s", SDL_GetError());
        return false;
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = game::description().app_name;
    application.pEngineName = "WondersBeyondRecomp";
    application.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo instance{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance.pApplicationInfo = &application;
    instance.enabledExtensionCount = extension_count;
    instance.ppEnabledExtensionNames = extensions;
    VkResult result = vkCreateInstance(&instance, nullptr, &g_vk.instance);
    if (result != VK_SUCCESS) {
        log::write("video", "Vulkan refused to start (%d)", static_cast<int>(result));
        return false;
    }
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g_vk.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(g_vk.instance, &count, devices.data());
    for (VkPhysicalDevice candidate : devices) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (uint32_t i = 0; i < family_count; i++) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                g_vk.physical = candidate;
                g_vk.family = i;
                g_vk.timestamps = families[i].timestampValidBits != 0;
                break;
            }
        }
        if (g_vk.physical) {
            break;
        }
    }
    if (!g_vk.physical) {
        log::write("video", "no Vulkan graphics card");
        return false;
    }
    vkGetPhysicalDeviceProperties(g_vk.physical, &g_vk.properties);
    vkGetPhysicalDeviceMemoryProperties(g_vk.physical, &g_vk.memory);
    VkPhysicalDeviceFeatures features{};
    vkGetPhysicalDeviceFeatures(g_vk.physical, &features);
    const VkFormat depth_formats[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM};
    g_vk.depth_format = VK_FORMAT_UNDEFINED;
    for (VkFormat format : depth_formats) {
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(g_vk.physical, format, &properties);
        VkFormatFeatureFlags needed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & needed) == needed) {
            g_vk.depth_format = format;
            break;
        }
    }
    if (g_vk.depth_format == VK_FORMAT_UNDEFINED) {
        log::write("video", "this Vulkan graphics card cannot read its depth buffer");
        return false;
    }
    VkFormatProperties depth_properties{};
    vkGetPhysicalDeviceFormatProperties(g_vk.physical, g_vk.depth_format, &depth_properties);
    const VkFormatFeatureFlags depth_copies = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    g_vk.depth_copies = (depth_properties.optimalTilingFeatures & depth_copies) == depth_copies || g_vk.properties.apiVersion < VK_API_VERSION_1_1;
    uint32_t device_extension_count = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.physical, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions(device_extension_count);
    vkEnumerateDeviceExtensionProperties(g_vk.physical, nullptr, &device_extension_count, device_extensions.data());
    if (!has_extension(device_extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        log::write("video", "this Vulkan graphics card cannot show pictures");
        return false;
    }
    std::vector<const char*> enabled = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    static const bool no_load_none = std::getenv("WP_VK_NO_LOAD_NONE") != nullptr;
    if (!no_load_none && has_extension(device_extensions, "VK_KHR_load_store_op_none")) {
        enabled.push_back("VK_KHR_load_store_op_none");
        g_vk.load_none = true;
    } else if (!no_load_none && has_extension(device_extensions, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME)) {
        enabled.push_back(VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME);
        g_vk.load_none = true;
    }
    VkPhysicalDeviceFeatures wanted{};
    wanted.dualSrcBlend = features.dualSrcBlend;
    wanted.depthClamp = features.depthClamp;
    wanted.samplerAnisotropy = features.samplerAnisotropy;
    g_vk.anisotropy = features.samplerAnisotropy == VK_TRUE;
    g_vk.dual_source = features.dualSrcBlend == VK_TRUE;
    g_vk.depth_clamp = features.depthClamp == VK_TRUE;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = g_vk.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    info.pEnabledFeatures = &wanted;
    result = vkCreateDevice(g_vk.physical, &info, nullptr, &g_vk.device);
    if (result != VK_SUCCESS) {
        log::write("video", "the Vulkan graphics card refused to start (%d)", static_cast<int>(result));
        return false;
    }
    vkGetDeviceQueue(g_vk.device, g_vk.family, 0, &g_vk.queue);
    g_vk.uniform_alignment = std::max<VkDeviceSize>(g_vk.properties.limits.minUniformBufferOffsetAlignment, 16);
    g_vk.storage_alignment = std::max<VkDeviceSize>(g_vk.properties.limits.minStorageBufferOffsetAlignment, 16);
    if (!create_device_objects()) {
        log::write("video", "Vulkan on %s cannot build the game's shaders", g_vk.properties.deviceName);
        return false;
    }
    g_vk.usable = true;
    log::write("video", "Vulkan found %s (Vulkan %u.%u)", g_vk.properties.deviceName, VK_VERSION_MAJOR(g_vk.properties.apiVersion),
               VK_VERSION_MINOR(g_vk.properties.apiVersion));
    return true;
}

bool transforms_vertices() {
    static const settings::LiveFlag enabled("video.gpu_vertices", "WP_GPU_VERTICES");
    static const settings::LiveFlag between("video.gpu_between", "WP_GPU_BETWEEN");
    return enabled() && (!g_recording || between()) && initialize();
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
    uint32_t index = g_kept_index_used;
    if (vertex + bytes > kKeptVertexBytes || (index + run.indices) * sizeof(uint16_t) > kKeptIndexBytes) {
        return false;
    }
    kept.vertex = vertex;
    kept.index = index;
    kept.generation = g_kept_generation;
    g_kept_vertex_used = vertex + bytes;
    g_kept_index_used = index + run.indices;
    std::memcpy(g_device.kept_vertices.mapped + kept.vertex, vertices, bytes);
    std::memcpy(g_device.kept_indices.mapped + static_cast<size_t>(kept.index) * sizeof(uint16_t), indices, static_cast<size_t>(run.indices) * sizeof(uint16_t));
    return true;
}

void reset_kept() {
    if (!g_device.ready) {
        return;
    }
    if (g_run_kept != 0) {
        flush_pending();
    }
    if (g_recording && !g_record.runs.empty()) {
        g_record_broken = true;
    }
    wait_idle();
    g_kept_generation++;
    g_kept_vertex_used = 0;
    g_kept_index_used = 0;
}

Canvas canvas() {
    Canvas current = g_canvas;
    current.world = g_world_shown;
    return current;
}

Canvas shared_canvas() {
    uint32_t margins = g_shared_margins.load(std::memory_order_acquire);
    Canvas shared;
    shared.margin_x = static_cast<int>(margins >> 16);
    shared.margin_y = static_cast<int>(margins & 0xFFFF);
    return shared;
}

void set_canvas_kind(int kind) {
    g_canvas_kind = kind;
    g_world_drawn |= kind == kWorld;
}

bool canvas_copy(uint32_t address) {
    auto copied = g_device.copies.find(address);
    return copied != g_device.copies.end() && copied->second.whole_canvas;
}

GpuFigures gpu_figures() {
    GpuFigures figures;
    if (g_device.ready) {
        figures.adapter = g_vk.properties.deviceName;
        figures.width = scaled(canvas_width());
        figures.height = scaled(canvas_height());
    }
    return figures;
}

void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource* source) {
    finish_frame();
    if (count == 0) {
        if (g_recording && source && source->key != 0) {
            append(vertices, 0, source);
        }
        return;
    }
    if (!initialize()) {
        return;
    }
    static uint32_t draws = 0;
    if ((++draws & 31) == 0) {
        service_deferred();
    }
    if (!g_run_draws.empty()) {
        flush_pending();
    }
    select_batch();
    append(vertices, count, source);
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

namespace {

void copy_image(Image& from, Image& to) {
    end_pass();
    VkCommandBuffer cb = command();
    transition(cb, from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region{};
    region.srcSubresource = {from.aspect, 0, 0, 1};
    region.dstSubresource = {to.aspect, 0, 0, 1};
    region.extent = {std::min(from.width, to.width), std::min(from.height, to.height), 1};
    vkCmdCopyImage(cb, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

struct EfbSnapshot {
    Image* color = nullptr;
    Image* depth = nullptr;
};

EfbSnapshot g_efb_before;
EfbSnapshot g_efb_after;

bool same_size(const Image& source, Image*& copy) {
    if (copy && copy->width == source.width && copy->height == source.height && copy->format == source.format) {
        return true;
    }
    retire_image(copy);
    copy = make_image(source.width, source.height, 1, source.format, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    return copy != nullptr;
}

bool save_efb(EfbSnapshot& snapshot) {
    if (!g_vk.depth_copies || !g_device.efb_color || !g_device.efb_depth) {
        return false;
    }
    settle_efb();
    if (!same_size(*g_device.efb_color, snapshot.color) || !same_size(*g_device.efb_depth, snapshot.depth)) {
        return false;
    }
    copy_image(*g_device.efb_color, *snapshot.color);
    copy_image(*g_device.efb_depth, *snapshot.depth);
    return true;
}

void load_efb(const EfbSnapshot& snapshot) {
    settle_efb();
    if (!snapshot.color || !snapshot.depth || !g_device.efb_color || !g_device.efb_depth) {
        return;
    }
    copy_image(*snapshot.color, *g_device.efb_color);
    copy_image(*snapshot.depth, *g_device.efb_depth);
}

std::unordered_map<Image*, ReplayTarget> g_replay_targets;

void release_twin(ReplayTarget& twin) {
    retire_image(twin.before);
    twin.before = nullptr;
    release_target(twin.texture);
}

void note_copy_target(const Target& target) {
    for (const RecordedTarget& seen : g_record.targets) {
        if (seen.image == target.image) {
            return;
        }
    }
    bool sampled = false;
    for (const RecordedCommand& command : g_record.commands) {
        if ((command.kind == RecordedCommand::kBatch || command.kind == RecordedCommand::kRuns) &&
            std::find(std::begin(command.batch.views), std::end(command.batch.views), target.image) != std::end(command.batch.views)) {
            sampled = true;
            break;
        }
    }
    if (!sampled) {
        g_record.targets.push_back({target.image, false});
        return;
    }
    ReplayTarget& twin = g_replay_targets[target.image];
    if (!twin.before || !twin.texture.image || twin.texture.width != target.width || twin.texture.height != target.height) {
        release_twin(twin);
        twin.before = make_image(target.width, target.height, 1, kColorFormat, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        if (!twin.before || !create_render_texture(twin.texture, target.width, target.height)) {
            release_twin(twin);
            g_replay_targets.erase(target.image);
            g_record_broken = true;
            return;
        }
    }
    twin.frame = g_frame;
    copy_image(*target.image, *twin.before);
    g_record.targets.push_back({target.image, true});
}

void prune_twins() {
    for (auto it = g_replay_targets.begin(); it != g_replay_targets.end();) {
        if (it->second.frame + 600 < g_frame) {
            release_twin(it->second);
            it = g_replay_targets.erase(it);
        } else {
            ++it;
        }
    }
}

bool interpolated_targets() {
    const Target& frame = g_device.frame;
    if (g_device.interpolated[0].image && g_device.interpolated[0].width == frame.width && g_device.interpolated[0].height == frame.height) {
        return true;
    }
    for (Target& target : g_device.interpolated) {
        release_target(target);
    }
    for (Target& target : g_device.interpolated) {
        if (!create_render_texture(target, frame.width, frame.height)) {
            for (Target& made : g_device.interpolated) {
                release_target(made);
            }
            return false;
        }
    }
    return true;
}

const BatchState* g_replay_state = nullptr;
uint32_t g_replay_uniform = 0;

bool send_replayed() {
    if (g_replay_draws.empty()) {
        return true;
    }
    uint32_t start = 0;
    bool ok = upload(g_device.run_rows, g_replay_rows.data(), static_cast<uint32_t>(g_replay_rows.size() * sizeof(float[4])), 16, start);
    if (ok && set_batch_state(*g_replay_state, g_replay_uniform, 1)) {
        issue_runs(g_replay_draws.data(), g_replay_draws.size(), 0, 0, start / static_cast<uint32_t>(sizeof(float[4])), g_replay_uniform, *g_replay_state);
    }
    g_replay_draws.clear();
    g_replay_rows.clear();
    return ok;
}

void run_clear(const ClearCommand& command) {
    bool color_enable = command.color;
    bool alpha_enable = command.alpha;
    bool depth_enable = command.depth;
    int x = command.x;
    int y = command.y;
    int width = command.width;
    int height = command.height;
    bool whole = x == 0 && y == 0 && width == canvas_width() && height >= std::min(g_shown_height, canvas_height());
    bool full_color = color_enable && alpha_enable;
    if (whole && g_pass != Pass::Efb) {
        if (depth_enable) {
            g_clear_depth_pending = true;
            g_clear_depth = command.depth_value;
            depth_enable = false;
        }
        if (full_color) {
            g_clear_color_pending = true;
            g_clear_color = command.color_value;
            color_enable = false;
            alpha_enable = false;
        }
        if (!color_enable && !alpha_enable && !depth_enable) {
            return;
        }
    }
    begin_efb(depth_enable);
    VkClearRect rect{};
    rect.rect = {{scaled(x), scaled(y)}, {static_cast<uint32_t>(scaled(x + width) - scaled(x)), static_cast<uint32_t>(scaled(y + height) - scaled(y))}};
    rect.layerCount = 1;
    if (color_enable == alpha_enable) {
        VkClearAttachment attachments[2]{};
        uint32_t count = 0;
        if (color_enable) {
            attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            attachments[count].colorAttachment = 0;
            attachments[count].clearValue.color = command.color_value;
            count++;
        }
        if (depth_enable) {
            attachments[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            attachments[count].clearValue.depthStencil = {command.depth_value, 0};
            count++;
        }
        if (count) {
            vkCmdClearAttachments(g_current->draw, count, attachments, 1, &rect);
        }
        return;
    }
    VkPipeline pipeline = clear_pipeline(color_enable, alpha_enable, depth_enable);
    if (!pipeline) {
        return;
    }
    bind_pipeline(pipeline);
    set_scissor(rect.rect.offset.x, rect.rect.offset.y, static_cast<int>(rect.rect.extent.width), static_cast<int>(rect.rect.extent.height));
    const VkClearColorValue& color_value = command.color_value;
    struct {
        float color[4];
        float depth[4];
    } values{{color_value.float32[0], color_value.float32[1], color_value.float32[2], color_value.float32[3]}, {command.depth_value, 0, 0, 0}};
    vkCmdPushConstants(g_current->draw, g_device.clear_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof values, &values);
    vkCmdDraw(g_current->draw, 3, 1, 0, 0);
    g_bound_draw_set = VK_NULL_HANDLE;
    g_bound_maps = VK_NULL_HANDLE;
}

void apply_clear(const ClearCommand& command) {
    run_clear(command);
    if (g_recording) {
        RecordedCommand recorded;
        recorded.kind = RecordedCommand::kClear;
        recorded.clear = command;
        g_record.commands.push_back(recorded);
    }
}

bool replay_between(const std::vector<std::pair<uint32_t, uint32_t>>& matches, float t, const Target& target, int x, int y, int width, int height, bool depth,
                    const CopyFilter& filter) {
    auto started = std::chrono::steady_clock::now();
    float motion[kMaxCameras][4][4];
    motion_matrices(t, motion);
    uint32_t bytes = static_cast<uint32_t>((kMotionRows + 3 * g_frame_vertices) * sizeof(float[4]));
    uint32_t start = 0;
    if (!reserve(g_device.moved, bytes, 16, start)) {
        return false;
    }
    float* moved = reinterpret_cast<float*>(g_device.moved.buffer.mapped + start);
    std::memcpy(moved, motion, sizeof motion);
    fill_moved(reinterpret_cast<MovedVertex*>(moved + kMotionRows * 4), matches, t, motion);
    uint32_t moved_row = start / static_cast<uint32_t>(sizeof(float[4]));
    auto filled = std::chrono::steady_clock::now();
    g_time_filling += std::chrono::duration<double>(filled - started).count();
    struct Sending {
        std::chrono::steady_clock::time_point from;
        ~Sending() { g_time_sending += std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(); }
    } sending{filled};
    load_efb(g_efb_before);
    static std::vector<std::pair<const RecordedTarget*, ReplayTarget*>> twins;
    twins.clear();
    for (const RecordedTarget& recorded : g_record.targets) {
        if (!recorded.twin) {
            continue;
        }
        auto found = g_replay_targets.find(recorded.image);
        if (found == g_replay_targets.end()) {
            return false;
        }
        copy_image(*found->second.before, *found->second.texture.image);
        twins.emplace_back(&recorded, &found->second);
    }
    auto twin_of = [](const Target& copied) -> const Target& {
        for (const auto& [recorded, twin] : twins) {
            if (recorded->image == copied.image) {
                return twin->texture;
            }
        }
        return copied;
    };
    const std::vector<uint32_t>& command_partner = command_partners();
    BatchState swapped;
    BatchState blended;
    for (size_t c = 0; c < g_record.commands.size(); c++) {
        const RecordedCommand& command = g_record.commands[c];
        switch (command.kind) {
        case RecordedCommand::kBatch:
        case RecordedCommand::kRuns: {
            const BatchState* state = blend_colors(command, command_partner[c], t, blended);
            for (size_t map = 0; map < kTextureMaps; map++) {
                for (const auto& [recorded, twin] : twins) {
                    if (command.batch.views[map] == recorded->image) {
                        if (state != &swapped) {
                            swapped = *state;
                            state = &swapped;
                        }
                        swapped.views[map] = twin->texture.image;
                    }
                }
            }
            uint32_t uniform = 0;
            if (!upload_constants(*state, uniform)) {
                return false;
            }
            if (command.kind == RecordedCommand::kRuns) {
                g_replay_state = state;
                g_replay_uniform = uniform;
                if (!replay_runs(command, t)) {
                    return false;
                }
                break;
            }
            VkPipeline pipeline = tev_pipeline(*state, 0, 0);
            if (pipeline && set_batch_state(*state, uniform, 3)) {
                bind_pipeline(pipeline);
                DrawConstants constants{command.base, static_cast<uint32_t>(command.begin), 1, moved_row};
                vkCmdPushConstants(g_current->draw, g_device.tev_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof constants, &constants);
                vkCmdDraw(g_current->draw, static_cast<uint32_t>(command.end - command.begin), 1, 0, 0);
            }
            break;
        }
        case RecordedCommand::kCopy: {
            const CopyCommand& copy = command.copy;
            run_copy(twin_of(copy.target), copy.target_width, copy.target_height, copy.x, copy.y, copy.width, copy.height, copy.format, copy.intensity, copy.alpha,
                     copy.depth, copy.filter);
            break;
        }
        case RecordedCommand::kClear:
            run_clear(command.clear);
            break;
        }
    }
    return run_copy(target, g_device.frame.width, g_device.frame.height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

Ring* const g_recorded_rings[3] = {&g_device.screen, &g_device.run_vertices, &g_device.run_indices};

bool rings_overrun() {
    for (size_t i = 0; i < 3; i++) {
        if (g_recorded_rings[i]->switches - g_ring_marks[i] >= kRingSegments) {
            return true;
        }
    }
    return false;
}

void hold_rings() {
    for (Ring* ring : g_recorded_rings) {
        for (uint32_t segment = 0; segment < kRingSegments; segment++) {
            if (segment != ring->segment) {
                ring->used_by[segment] = std::max(ring->used_by[segment], current_serial());
            }
        }
    }
}

uint32_t run_interpolated(int x, int y, int width, int height, bool depth, const CopyFilter& filter, const pacing::FramePlan& plan) {
    if (!can_pair() || !g_efb_before_valid || (kMotionRows + 3 * g_frame_vertices) * sizeof(float[4]) > kMovedRingBytes / kRingSegments || rings_overrun() ||
        !interpolated_targets()) {
        return 0;
    }
    if (!g_paired) {
        pair_runs();
        if (!g_previous.draws.empty() && !g_record.draws.empty()) {
            pair_frame();
        } else {
            g_matches.clear();
            g_camera_cut = false;
        }
        g_paired = true;
    }
    if (g_camera_cut || g_run_cut) {
        return 0;
    }
    uint32_t count = std::min<uint32_t>(plan.images, kMaxBetween);
    uint32_t made = 0;
    while (made < count && replay_between(g_matches, plan.fractions[made], g_device.interpolated[made], x, y, width, height, depth, filter)) {
        made++;
    }
    hold_rings();
    return made;
}

int g_display_hz = 0;
std::atomic<uint32_t> g_late_frames{0};
std::atomic<uint32_t> g_hidden_frames{0};
double g_phase = 0.0;
bool g_show_real = true;
double g_planned_slot = 1.0 / 120.0;
double g_period = 1.0 / 60.0;
double g_slot = 1.0 / 120.0;

void end_record() {
    g_previous_colors.clear();
    if (g_recording && !g_record_broken) {
        keep_colors();
        g_previous.positions.swap(g_record.positions);
        g_previous.colors.swap(g_record.colors);
        g_previous.draws.swap(g_record.draws);
        g_previous.runs.swap(g_record.runs);
        g_previous.rows.swap(g_record.rows);
    } else {
        g_previous.positions.clear();
        g_previous.colors.clear();
        g_previous.draws.clear();
        g_previous.runs.clear();
        g_previous.rows.clear();
    }
    g_record.source_of.clear();
    g_record.positions.clear();
    g_record.colors.clear();
    g_record.draws.clear();
    g_record.commands.clear();
    g_record.targets.clear();
    g_record.runs.clear();
    g_record.rows.clear();
    g_paired = false;
    if (g_frame % 600 == 0) {
        prune_twins();
    }
    g_frame_vertices = 0;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    g_recording = g_vk.depth_copies && pacing::interpolation_wanted(static_cast<int>(fps()), g_display_hz);
    g_record_broken = false;
    release_held();
    for (size_t i = 0; i < 3; i++) {
        g_ring_marks[i] = g_recorded_rings[i]->switches;
    }
}

struct FrameEnd {
    bool due = false;
    bool copied = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool depth = false;
    CopyFilter filter;
    bool has_clear = false;
    ClearCommand clear;
};

FrameEnd g_frame_end;

SDL_Window* g_present_window = nullptr;
double g_present_aspect = 4.0 / 3.0;

struct ShownImage {
    Target target;
    std::chrono::steady_clock::time_point due;
    bool interpolated = false;
};

constexpr size_t kShownImages = kMaxBetween + 3;
ShownImage g_shown[kShownImages];
std::deque<size_t> g_show_queue;
std::chrono::steady_clock::time_point g_last_shown;

bool queue_image(const Target& source, std::chrono::steady_clock::time_point due, bool interpolated) {
    size_t free = kShownImages;
    for (size_t i = 0; i < kShownImages && free == kShownImages; i++) {
        if (std::find(g_show_queue.begin(), g_show_queue.end(), i) == g_show_queue.end()) {
            free = i;
        }
    }
    if (free == kShownImages || !source.image) {
        return false;
    }
    ShownImage& image = g_shown[free];
    if (!image.target.image || image.target.width != source.width || image.target.height != source.height) {
        release_target(image.target);
        if (!create_render_texture(image.target, source.width, source.height)) {
            release_target(image.target);
            return false;
        }
    }
    copy_image(*source.image, *image.target.image);
    image.due = due;
    image.interpolated = interpolated;
    g_show_queue.push_back(free);
    return true;
}

bool present_image(SDL_Window* window, double aspect, Image* image) {
    int client_width = 0;
    int client_height = 0;
    if (!window || !image || !SDL_GetWindowSizeInPixels(window, &client_width, &client_height) || client_width <= 0 || client_height <= 0) {
        return false;
    }
    flush_pending();
    take_overlays(client_width, client_height);
    end_pass();
    transition(command(), *image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    submit();
    Swapchain& chain = g_device.chain;
    if (video::background_wanted()) {
        drop_surface();
        return false;
    }
    if (!ensure_swapchain(window, client_width, client_height)) {
        return false;
    }
    VkSemaphore acquired = chain.acquired[chain.next_acquire];
    uint32_t index = 0;
    auto acquire_start = std::chrono::steady_clock::now();
    VkResult result = vkAcquireNextImageKHR(device(), chain.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
    g_present_wait += seconds_since(acquire_start);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        chain.stale = true;
        return false;
    }
    if (result == VK_ERROR_SURFACE_LOST_KHR) {
        drop_surface();
        return false;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return false;
    }
    chain.next_acquire = (chain.next_acquire + 1) % static_cast<uint32_t>(chain.acquired.size());
    ensure_command();
    VkClearValue black{};
    black.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    begin_render_pass(chain.pass, chain.framebuffers[index], chain.width, chain.height, 1, &black);
    g_pass = Pass::Screen;
    set_scissor(0, 0, static_cast<int>(chain.width), static_cast<int>(chain.height));
    int bar = titlebar::height(window);
    int area = std::max(1, client_height - bar);
    g_picture_aspect = aspect;
    g_window_aspect = static_cast<double>(client_width) / area;
    double box_width = client_width;
    double box_height = area;
    if (canvas_grown() && image->width > 0 && image->height > 0) {
        double box_aspect = aspect * (kEfbWidth + 2.0 * g_canvas.margin_x) / (kPictureHeight + 2.0 * g_canvas.margin_y) * kPictureHeight / kEfbWidth;
        box_height = std::min<double>(area, client_width / box_aspect);
        box_width = box_height * box_aspect;
        aspect *= static_cast<double>(image->width) / image->height * kPictureHeight / kEfbWidth;
    }
    double width = box_width;
    double height = width / aspect;
    if (height > box_height) {
        height = box_height;
        width = height * aspect;
    }
    set_viewport(static_cast<float>(std::floor((client_width - width) / 2)), static_cast<float>(std::floor(bar + (area - height) / 2)),
                 static_cast<float>(std::floor(width)), static_cast<float>(std::floor(height)));
    static const settings::LiveFlag smooth("video.smooth", "WP_SMOOTH");
    bool reduce = smooth() && (image->width > width * 1.25 || image->height > height * 1.25);
    VkPipeline pipeline = screen_pipeline(reduce ? 1 : 0, false);
    VkDescriptorSet set = single_set(*image, g_device.linear);
    if (pipeline && set) {
        color::Adjust colors = color::current();
        float veil = gx::veil_level();
        colors.brightness *= 1.0f - veil;
        colors.saturation *= 1.0f - veil;
        float adjust[4] = {colors.brightness, colors.contrast, colors.saturation, 1.0f / colors.gamma};
        bind_pipeline(pipeline);
        vkCmdBindDescriptorSets(g_current->draw, VK_PIPELINE_BIND_POINT_GRAPHICS, g_device.single_pipeline_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(g_current->draw, g_device.single_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof adjust, adjust);
        vkCmdDraw(g_current->draw, 3, 1, 0, 0);
    }
    draw_overlay(client_width, client_height, bar);
    submit(acquired, chain.rendered[index]);
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &chain.rendered[index];
    present.swapchainCount = 1;
    present.pSwapchains = &chain.swapchain;
    present.pImageIndices = &index;
    auto present_start = std::chrono::steady_clock::now();
    VkResult shown = vkQueuePresentKHR(g_vk.queue, &present);
    g_present_wait += seconds_since(present_start);
    if (shown == VK_ERROR_OUT_OF_DATE_KHR) {
        chain.stale = true;
    } else if (shown == VK_ERROR_SURFACE_LOST_KHR) {
        drop_surface();
    }
    return shown == VK_SUCCESS || shown == VK_SUBOPTIMAL_KHR;
}

void service_deferred() {
    auto now = std::chrono::steady_clock::now();
    size_t ready = 0;
    while (ready < g_show_queue.size() && g_shown[g_show_queue[ready]].due <= now) {
        ready++;
    }
    if (ready == 0) {
        return;
    }
    size_t skipped = ready > 2 ? ready - 2 : 0;
    ready = skipped + 1;
    const ShownImage& image = g_shown[g_show_queue[ready - 1]];
    if (present_image(g_present_window, g_present_aspect, image.target.image) && image.interpolated) {
        g_interpolated_frames++;
    }
    g_last_shown = now - image.due < std::chrono::duration<double>(g_slot) ? image.due : now;
    g_show_queue.erase(g_show_queue.begin(), g_show_queue.begin() + static_cast<std::ptrdiff_t>(ready));
}

bool copy_frame(int& x, int& y, int& width, int& height, bool depth, const CopyFilter& filter) {
    flush_pending();
    complete_write_backs(false);
    uint32_t slot = static_cast<uint32_t>(g_frame % 2);
    uint64_t finished = current_serial();
    submit();
    auto frame_start = std::chrono::steady_clock::now();
    wait_serial(g_frame_serials[slot]);
    g_frame_wait += seconds_since(frame_start);
    g_frame_serials[slot] = finished;
    report_frame_figures();
    maybe_save_cache();
    g_frame++;
    g_texture_epoch++;
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return false;
    }
    g_world_change_frames = g_world_drawn == g_world_shown ? 0 : g_world_change_frames + 1;
    if (g_world_change_frames >= kSteadyFrames) {
        g_world_shown = g_world_drawn;
        g_world_change_frames = 0;
    }
    g_world_drawn = false;
    place_on_canvas(x, y, width, height, g_world_shown);
    g_shown_height = y + height;
    uint32_t physical_width = static_cast<uint32_t>(scaled(width));
    uint32_t physical_height = static_cast<uint32_t>(scaled(height));
    if (!g_device.frame.image || g_device.frame.width != physical_width || g_device.frame.height != physical_height) {
        release_target(g_device.frame);
        if (!create_render_texture(g_device.frame, physical_width, physical_height)) {
            release_target(g_device.frame);
            return false;
        }
    }
    return run_copy(g_device.frame, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

}

void finish_frame() {
    if (!g_frame_end.due) {
        return;
    }
    FrameEnd end = g_frame_end;
    g_frame_end.due = false;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    constexpr size_t kBehindBytes = 256 << 10;
    bool late = gx::take_frame_held();
    bool saved = end.copied && g_recording && save_efb(g_efb_after);
    auto started = std::chrono::steady_clock::now();
    pacing::FramePlan plan;
    if (saved) {
        plan = pacing::plan_frame(g_phase, g_period, pacing::output_rate(static_cast<int>(fps())), kMaxBetween);
        if (!late && gx::gpu_backlog() < kBehindBytes) {
            g_interpolated_ready = run_interpolated(end.x, end.y, end.width, end.height, end.depth, end.filter, plan);
            load_efb(g_efb_after);
        }
        std::swap(g_efb_before, g_efb_after);
    }
    bool complete = plan.images > 0 && g_interpolated_ready == plan.images;
    g_show_real = plan.show_real || !complete;
    g_planned_slot = complete ? plan.slot : g_period / (g_interpolated_ready + 1);
    if (!complete) {
        g_phase = 0.0;
    }
    if (end.copied && g_recording && g_interpolated_ready == 0) {
        g_late_frames++;
    }
    if (g_recording) {
        static auto report = started;
        static uint32_t frames = 0, with_images = 0, late_frames = 0, draws = 0;
        static double seconds = 0.0;
        auto now = std::chrono::steady_clock::now();
        frames++;
        with_images += g_interpolated_ready != 0;
        late_frames += late;
        draws += static_cast<uint32_t>(g_record.draws.size());
        seconds += std::chrono::duration<double>(now - started).count();
        if (now - report >= std::chrono::seconds(1)) {
            uint32_t n = std::max(1u, with_images);
            log::write("replay", "%u frames, %u with images, %u late, %.2f ms per frame for the images (pairing %.2f, positions %.2f, commands %.2f), %u draws per frame: %u sure pairs, %u by order, %u rejected, %u unpaired, %u unpaired inline, %u with the camera (%u cameras)",
                       frames, with_images, late_frames, seconds * 1000.0 / frames, g_time_pairing * 1000.0 / frames, g_time_filling * 1000.0 / frames, g_time_sending * 1000.0 / frames, draws / frames, g_match[0] / n, g_match[1] / n, g_match[2] / n, g_match[3] / n, g_match[4] / n,
                       g_match[5] / n, static_cast<uint32_t>(std::count_if(std::begin(g_cameras), std::end(g_cameras), [](const CameraMotion& c) { return c.known; })));
            if (g_run_counts[0] != 0) {
                log::write("runs", "%u models per frame redrawn for the images: %u paired, %u moved too far, %u texture matrices turned in all, %u look-alikes too far to pair, %u teleports", g_run_counts[0] / n, g_run_counts[1] / n, g_run_counts[2] / n, g_run_counts[3], g_run_counts[4], g_run_counts[5]);
            }
            std::fill(std::begin(g_match), std::end(g_match), 0u);
            std::fill(std::begin(g_run_counts), std::end(g_run_counts), 0u);
            report = now;
            frames = with_images = late_frames = draws = 0;
            seconds = 0.0;
            g_time_pairing = g_time_filling = g_time_sending = 0.0;
        }
    }
    g_efb_before_valid = saved;
    end_record();
    if (end.has_clear) {
        apply_clear(end.clear);
    }
}

namespace {

void keep_photo(Image* image, uint32_t full_width, uint32_t full_height, int x, int y, int width, int height, int canvas_width, int canvas_height) {
    std::vector<uint32_t> pixels;
    if (!image || canvas_width <= 0 || canvas_height <= 0 || !read_target(image, full_width, full_height, pixels)) {
        return;
    }
    uint32_t left = std::min(full_width, static_cast<uint32_t>(std::max(0, x)) * full_width / static_cast<uint32_t>(canvas_width));
    uint32_t top = std::min(full_height, static_cast<uint32_t>(std::max(0, y)) * full_height / static_cast<uint32_t>(canvas_height));
    uint32_t photo_width = std::min(full_width - left, static_cast<uint32_t>(width) * full_width / static_cast<uint32_t>(canvas_width));
    uint32_t photo_height = std::min(full_height - top, static_cast<uint32_t>(height) * full_height / static_cast<uint32_t>(canvas_height));
    if (photo_width == 0 || photo_height == 0) {
        return;
    }
    std::vector<uint32_t> photo(static_cast<size_t>(photo_width) * photo_height);
    for (uint32_t row = 0; row < photo_height; row++) {
        const uint32_t* source = pixels.data() + static_cast<size_t>(top + row) * full_width + left;
        std::copy(source, source + photo_width, photo.begin() + static_cast<size_t>(row) * photo_width);
    }
    screenshot::photo_taken(std::move(photo), photo_width, photo_height);
}

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
        auto stale = g_device.copies.find(address);
        if (stale != g_device.copies.end()) {
            release_target(stale->second.converted);
            g_device.copies.erase(stale);
        }
        return;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    finish_frame();
    flush_pending();
    int canvas_x = x;
    int canvas_y = y;
    int canvas_w = width;
    int canvas_h = height;
    place_on_canvas(canvas_x, canvas_y, canvas_w, canvas_h);
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
    uint32_t copy_width = half ? std::max(1u, static_cast<uint32_t>(canvas_w) / 2) : static_cast<uint32_t>(canvas_w);
    uint32_t copy_height = half ? std::max(1u, static_cast<uint32_t>(canvas_h) / 2) : static_cast<uint32_t>(canvas_h);
    uint32_t target_width = std::max(1u, copy_width * g_scale);
    uint32_t target_height = std::max(1u, copy_height * g_scale);
    if (!entry.converted.image || entry.converted.width != target_width || entry.converted.height != target_height) {
        release_target(entry.converted);
        entry.has_view = false;
        if (!create_render_texture(entry.converted, target_width, target_height)) {
            g_device.copies.erase(address);
            return;
        }
    }
    if (g_recording) {
        note_copy_target(entry.converted);
    }
    run_copy(entry.converted, target_width, target_height, canvas_x, canvas_y, canvas_w, canvas_h, format, intensity, alpha || depth, depth, filter);
    if (g_recording) {
        RecordedCommand command;
        command.kind = RecordedCommand::kCopy;
        command.copy = {entry.converted, target_width, target_height, canvas_x, canvas_y, canvas_w, canvas_h, format, intensity, alpha || depth, depth, filter};
        g_record.commands.push_back(command);
    }
    if (!recurring && !half && format == 6 && logical_width == screenshot::kPhotoWidth && logical_height == screenshot::kPhotoHeight && keep_photos()) {
        keep_photo(entry.converted.image, entry.converted.width, entry.converted.height, x + g_canvas.margin_x - canvas_x, y + g_canvas.margin_y - canvas_y, width, height,
                   canvas_w, canvas_h);
    }
    entry.has_view = true;
    entry.whole_canvas = canvas_w > width;
    entry.bytes = entry.logical_width * entry.logical_height * copy_bits(format) / 8;
    if (!recurring && queue_write_back(address, stride, x + g_canvas.margin_x, y + g_canvas.margin_y, width, height, entry.logical_width, entry.logical_height, format,
                                       intensity, alpha || depth, depth, filter)) {
        entry.pending_writes++;
    } else if (entry.pending_writes == 0) {
        entry.guest_hash = guest_range_valid(address, entry.bytes) ? sample_hash(host(address), entry.bytes) : 0;
    }
    dump_copy(entry);
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
    auto now = std::chrono::steady_clock::now();
    static auto previous = now;
    double period = std::chrono::duration<double>(now - previous).count();
    previous = now;
    if (period > 0.004 && period < 0.1) {
        g_period = g_period * 0.9 + std::min(period, 0.05) * 0.1;
    }
    finish_frame();
    bool copied = copy_frame(x, y, width, height, depth, filter);
    g_interpolated_ready = 0;
    g_frame_end = {true, copied, x, y, width, height, depth, filter, false, {}};
}

bool present_frame(void* window_handle, double aspect) {
    if (!g_device.ready || !g_device.frame.image || !window_handle) {
        return false;
    }
    finish_frame();
    SDL_Window* window = static_cast<SDL_Window*>(window_handle);
    g_present_window = window;
    g_present_aspect = aspect;
    static auto checked = std::chrono::steady_clock::time_point{};
    if (std::chrono::steady_clock::now() - checked > std::chrono::seconds(2)) {
        checked = std::chrono::steady_clock::now();
        static const settings::LiveNumber fps("video.fps", "WP_FPS");
        int highest = 0;
        int hz = video::display_rate(std::max(60, static_cast<int>(fps())), highest);
        pacing::set_highest_hz(highest);
        if (hz != g_display_hz) {
            log::write("video", "display refresh rate %d Hz, up to %d Hz", hz, highest);
            g_display_hz = hz;
            pacing::set_display_hz(hz);
        }
    }
    static uint64_t queued_frame = 0;
    bool fresh = queued_frame != g_frame;
    queued_frame = g_frame;
    bool presented = true;
    if (g_interpolated_ready == 0 && g_show_queue.empty()) {
        if (fresh || !g_recording || gx::veil_moving()) {
            presented = present_image(window, aspect, g_device.frame.image);
            g_last_shown = std::chrono::steady_clock::now();
        }
    } else if (!fresh) {
        service_deferred();
    } else {
        auto now = std::chrono::steady_clock::now();
        auto last = g_show_queue.empty() ? g_last_shown : g_shown[g_show_queue.back()].due;
        if (last - now > std::chrono::duration<double>(g_period / 2)) {
            g_show_queue.clear();
            last = g_last_shown;
        }
        g_slot = std::clamp(g_planned_slot, 0.002, 0.025);
        uint32_t queued = g_interpolated_ready + (g_show_real ? 1 : 0);
        if (queued > 0) {
            double lead = std::chrono::duration<double>(last - now).count();
            g_slot = std::clamp(g_slot - (lead - g_slot / 4) * 0.1 / queued, g_slot * 0.9, g_slot * 1.1);
        }
        auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(g_slot));
        auto slot = std::max(now, last + step);
        for (uint32_t i = 0; i < g_interpolated_ready; i++) {
            if (queue_image(g_device.interpolated[i], slot, true)) {
                slot += step;
            }
        }
        g_interpolated_ready = 0;
        if (g_show_real) {
            queue_image(g_device.frame, slot, false);
        } else {
            g_hidden_frames++;
        }
        service_deferred();
    }
    video::park_while_background();
    apply_scale();
    apply_anisotropy();
    return presented;
}

int64_t service_presents() {
    service_deferred();
    if (g_show_queue.empty()) {
        return -1;
    }
    auto left = g_shown[g_show_queue.front()].due - std::chrono::steady_clock::now();
    return std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::microseconds>(left).count());
}

uint32_t take_interpolated_frames() {
    uint32_t shown = g_interpolated_frames.exchange(0);
    uint32_t hidden = g_hidden_frames.exchange(0);
    return shown > hidden ? shown - hidden : 0;
}

uint32_t take_late_frames() {
    return g_late_frames.exchange(0);
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
    if (!g_device.ready || !g_device.frame.image) {
        return false;
    }
    flush_pending();
    width = g_device.frame.width;
    height = g_device.frame.height;
    return read_target(g_device.frame.image, width, height, pixels);
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
    place_on_canvas(x, y, width, height);
    ClearCommand command;
    command.color = color_enable;
    command.alpha = alpha_enable;
    command.depth = depth_enable;
    command.color_value = VkClearColorValue{{red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f}};
    command.depth_value = static_cast<float>(depth) / 16777215.0f;
    command.x = x;
    command.y = y;
    command.width = width;
    command.height = height;
    if (g_frame_end.due && !g_frame_end.has_clear) {
        g_frame_end.has_clear = true;
        g_frame_end.clear = command;
        return;
    }
    finish_frame();
    apply_clear(command);
}

}
