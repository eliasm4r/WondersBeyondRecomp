#pragma once

#include <cstddef>
#include <cstdint>

#include "wp/hle.h"

namespace wp::game {

struct Description {
    const char* title;
    // The name of this PC version, shown in the window title.
    const char* app_name;
    const char* short_name;
    const char* data_directory;
    const char* nand_directory;
    const char* settings_file;
    const char* textures_directory;
    bool sideways_grip;
    // The Application ID from Discord's developer portal for the "playing" status, or "" for none.
    const char* discord_application;
};

const Description& description();

// The game's own handler for a function listed in hle_functions.csv, or nullptr.
HleFunction replacement(const char* name);

// Called on the CPU thread once per video retrace, after its interrupt handler.
void retrace();

}
