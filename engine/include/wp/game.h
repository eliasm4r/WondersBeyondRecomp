#pragma once

#include <cstddef>
#include <cstdint>

namespace wp::game {

struct Description {
    const char* title;
    const char* short_name;
    const char* data_directory;
    const char* nand_directory;
    const char* settings_file;
    const char* textures_directory;
    bool sideways_grip;
    uint32_t notice_function;
};

const Description& description();

}
