#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace wp::gx {

void push(uint64_t value, unsigned bytes);
void process();
bool take_finish_interrupt();
bool take_token_interrupt();
// Keeps the last shown image (black at boot) instead of the game's new frames.
void set_screen_hidden(bool hidden);
void write_pixel_engine_control(uint16_t value);
void request_capture();
uint64_t frames_drawn();
size_t gpu_backlog();
bool frame_owed();
bool take_frame_held();
void run_frame_task(std::function<void()> task);
bool wait_for_token(std::chrono::milliseconds limit);
void wait_for_interrupt(std::chrono::microseconds limit);
double facing_area(const float a[3], const float b[3], const float c[3]);

}
