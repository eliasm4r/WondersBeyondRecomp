#pragma once

#include <cstdint>
#include <string>

namespace wp::disc {

bool mount(const std::string& extracted_directory);
void read(uint64_t offset, uint32_t length, uint32_t destination);
// Seconds since the game last read a file whose path contains this text. The first call starts the watch.
double seconds_since_read(const char* path_part);

}
