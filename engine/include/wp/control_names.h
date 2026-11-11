#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wp/padmap.h"

namespace wp::control_names {

std::string pad_button(int physical, padmap::Family family);
std::string pad_binding(const padmap::Binding& binding, padmap::Family family);
std::string key(int code, const std::function<uint32_t(int code)>& character);
std::string key(int code);
std::string keys(const std::vector<int>& codes);

}
