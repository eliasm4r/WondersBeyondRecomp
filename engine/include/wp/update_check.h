#pragma once

#include <string>

namespace wp::update {

enum class State { Off, Checking, Current, Newer, Failed };

const char* version();
bool available();
void start();
State state();
std::string latest();
void open_page();
bool parse_version(const std::string& text, int out[3]);
std::string tag_of(const std::string& json);
bool newer(const std::string& theirs, const std::string& mine);

}
