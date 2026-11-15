#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace wp::hd_textures {

enum class State { Idle, Reading, Downloading, Enlarging, Retrying, Checking, Done, Failed };

struct Progress {
    State state = State::Idle;
    uint32_t done = 0;
    uint32_t total = 0;
    uint32_t found = 0;
    uint32_t again = 0;
    uint32_t still_empty = 0;
    uint32_t kept = 0;
    uint32_t wrong = 0;
    uint32_t made = 0;
    uint32_t seconds_left = 0;
    uint64_t version = 0;
    std::string error;
};

bool available();
void start();
Progress progress();
std::vector<std::string> describe(const Progress& progress);
bool matches(const std::vector<uint32_t>& original, uint32_t width, uint32_t height, const std::vector<uint32_t>& larger, uint32_t larger_width,
             uint32_t larger_height);
struct Range {
    bool matched = false;
    bool changed = false;
    double strayed = 0.0;
};

Range keep_in_range(const std::vector<uint32_t>& original, uint32_t width, uint32_t height, std::vector<uint32_t>& larger, uint32_t larger_width,
                    uint32_t larger_height);

}
