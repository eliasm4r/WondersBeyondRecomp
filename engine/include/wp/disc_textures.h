#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace wp::gx::disc_textures {

size_t export_flat(const std::filesystem::path& files, const std::filesystem::path& folder);
bool unpack_lz11(const std::vector<uint8_t>& in, std::vector<uint8_t>& out);

}
