#pragma once

#include <cstdint>

namespace wp {
struct Cpu;
}

namespace wp::hle_audio {

struct Layout {
    uint32_t voices = 0;
    uint32_t voice_count = 0;
    uint32_t resume_callback = 0;
    int32_t aux_return[3] = {};
};

bool wanted();
bool active();
void configure(const Layout& layout);
void set_dma_callback(uint32_t callback);
void set_dma_bytes(uint32_t bytes);
void start_dma();
void stop_dma();
void set_aux(int bus, bool output, uint32_t address, uint32_t small_data);
void run_due_frames(Cpu& c);

}
