#pragma once

#include <cstddef>
#include <cstdint>

#include "wp/hle.h"

namespace wp::game {

struct Description {
    const char* title;
    const char* short_name;
    const char* data_directory;
    const char* nand_directory;
    const char* settings_file;
    const char* textures_directory;
    bool sideways_grip;
};

const Description& description();

// The game's own handler for a function listed in hle_functions.csv, or nullptr.
HleFunction replacement(const char* name);

}
