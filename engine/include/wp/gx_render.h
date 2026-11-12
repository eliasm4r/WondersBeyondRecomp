#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::gx {

struct ScreenVertex {
    float x;
    float y;
    float z;
    float w;
    float color[2][4];
    float uv[8][3];
};

const uint32_t* bp_registers();
const uint32_t* konst_registers();
const uint32_t* xf_registers();
// Changes whenever a BP, konst or XF control register is written: matrices alone leave it unchanged.
uint64_t register_serial();

}

namespace wp::gx::render {

struct CopyFilter {
    uint32_t coefficients[3] = {0, 64, 0};
    float gamma = 1.0f;
    bool clamp_top = true;
    bool clamp_bottom = true;
};

bool guest_range_valid(uint32_t address, size_t size);
void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds);
void take_copy_statistics(uint32_t& write_backs, double& wait_seconds);

struct DrawSource {
    uint64_t key = 0;
    uint64_t exact = 0;
    uint32_t camera = 0;
    const float (*positions)[4] = nullptr;
    uint32_t count = 0;
    const uint16_t* source_of = nullptr;
};

constexpr uint16_t kNoSource = 0xFFFF;

const char* api_name();
bool interpolating();
void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource* source = nullptr);
void finish_copies();
void finish_frame();
void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter);
bool present_frame(void* window, double aspect);
bool service_presents();
uint32_t take_interpolated_frames();
uint32_t take_late_frames();

struct GpuFigures {
    double vram_used_gb = -1;
    double vram_total_gb = -1;
    std::string adapter;
    int width = 0;
    int height = 0;
    int refresh_hz = 0;
};

GpuFigures gpu_figures();
bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);

struct RecentImage {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    bool between = false;
};

bool read_recent(std::vector<RecentImage>& images);
void dump_copies(const char* prefix);
void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter);
void invalidate_textures();
void load_tlut(uint32_t address, uint32_t tmem_offset, uint32_t bytes);
void clear(int x, int y, int width, int height);

}
