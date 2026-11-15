#pragma once

#include <cstddef>
#include <filesystem>

namespace wp::gx::disc_textures {

size_t export_flat(const std::filesystem::path& files, const std::filesystem::path& folder);

}
