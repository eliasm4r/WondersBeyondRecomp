#include "wp/gx_render.h"
#include "wp/gx.h"

#include "gx_render_common.h"
#include "../platform/gl_window.h"

#include <atomic>
#include <string>

#include "wp/log.h"
#include "wp/settings.h"

namespace wp::gx::render {

namespace opengl {
#include "gx_render_backend.inc"
}

namespace vulkan {
#include "gx_render_backend.inc"
bool probe();
}

namespace {

std::atomic<int> g_chosen{-1};

bool use_vulkan() {
    int chosen = g_chosen.load(std::memory_order_acquire);
    if (chosen < 0) {
        chosen = video::vulkan_window();
        if (chosen < 0) {
            return false;
        }
        g_chosen.store(chosen, std::memory_order_release);
    }
    return chosen != 0;
}

}

bool choose_vulkan() {
    std::string wanted = settings::text("video.renderer", "WP_RENDERER");
    if (wanted == "opengl") {
        log::write("video", "OpenGL ES chosen in the settings");
        return false;
    }
    bool usable = vulkan::probe();
    if (!usable) {
        log::write("video", "Vulkan cannot draw the game on this phone, OpenGL ES draws it");
    }
    return usable;
}

#define WP_BACKEND(call) (use_vulkan() ? vulkan::call : opengl::call)

const char* api_name() {
    return WP_BACKEND(api_name());
}

int auto_scale() {
    return WP_BACKEND(auto_scale());
}

bool interpolating() {
    return WP_BACKEND(interpolating());
}

void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource* source) {
    WP_BACKEND(draw(vertices, count, source));
}

bool transforms_vertices() {
    return WP_BACKEND(transforms_vertices());
}

bool begin_run(const VertexRun& run, uint8_t** vertices, uint16_t** indices) {
    return WP_BACKEND(begin_run(run, vertices, indices));
}

bool keep_run(const VertexRun& run, const uint8_t* vertices, const uint16_t* indices, KeptRun& kept, bool placed) {
    return WP_BACKEND(keep_run(run, vertices, indices, kept, placed));
}

bool draw_kept(const VertexRun& run, const KeptRun& kept) {
    return WP_BACKEND(draw_kept(run, kept));
}

void reset_kept() {
    WP_BACKEND(reset_kept());
}

void finish_copies() {
    WP_BACKEND(finish_copies());
}

void finish_frame() {
    WP_BACKEND(finish_frame());
}

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    WP_BACKEND(copy_to_framebuffer(x, y, width, height, depth, filter));
}

bool present_frame(void* window, double aspect) {
    return WP_BACKEND(present_frame(window, aspect));
}

Canvas canvas() {
    return WP_BACKEND(canvas());
}

Canvas shared_canvas() {
    return WP_BACKEND(shared_canvas());
}

void set_canvas_kind(int kind) {
    WP_BACKEND(set_canvas_kind(kind));
}

bool canvas_copy(uint32_t address) {
    return WP_BACKEND(canvas_copy(address));
}

int64_t service_presents() {
    return WP_BACKEND(service_presents());
}

uint32_t take_interpolated_frames() {
    return WP_BACKEND(take_interpolated_frames());
}

uint32_t take_late_frames() {
    return WP_BACKEND(take_late_frames());
}

GpuFigures gpu_figures() {
    return WP_BACKEND(gpu_figures());
}

bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    return WP_BACKEND(read_frame(pixels, width, height));
}

bool read_recent(std::vector<RecentImage>& images) {
    return WP_BACKEND(read_recent(images));
}

void dump_copies(const char* prefix) {
    WP_BACKEND(dump_copies(prefix));
}

void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter) {
    WP_BACKEND(copy_to_texture(address, stride, x, y, width, height, half, format, intensity, depth, alpha, filter));
}

void invalidate_textures() {
    WP_BACKEND(invalidate_textures());
}

void clear(int x, int y, int width, int height) {
    WP_BACKEND(clear(x, y, width, height));
}

bool guest_range_valid(uint32_t address, size_t size) {
    return WP_BACKEND(guest_range_valid(address, size));
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    WP_BACKEND(take_statistics(batches, vertices, seconds));
}

void take_copy_statistics(uint32_t& write_backs, double& wait_seconds) {
    WP_BACKEND(take_copy_statistics(write_backs, wait_seconds));
}

}
