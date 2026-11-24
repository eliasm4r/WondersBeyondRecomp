#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wp::disc {

bool mount(const std::string& extracted_directory);
void read(uint64_t offset, uint32_t length, uint32_t destination);
// Seconds since the game last read a file whose path contains this text. The first call starts the watch.
double seconds_since_read(const char* path_part);
uint32_t field_in_path(const std::string& path);
uint32_t current_field();
// Language of the message file the game loaded ("en", "fr", "de", "it", "es"), empty before it does.
std::string game_language();
// The game's own name of a field in its language, empty when it has none.
std::string field_name(uint32_t field);
std::map<std::string, std::string> message_texts(const std::vector<uint8_t>& data);

}
