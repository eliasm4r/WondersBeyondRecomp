#pragma once

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
void run_frame_task(std::function<void()> task);

}
