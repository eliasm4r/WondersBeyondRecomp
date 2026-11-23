#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/options.h"

namespace wp::ghosts {

struct Projection {
    float a = 0.0f;
    float b = 0.0f;
    float c = 0.0f;
    float d = 0.0f;
    float x_scale = 0.0f;
    float x_offset = 0.0f;
    float y_scale = 0.0f;
    float y_offset = 0.0f;
    int margin_x = 0;
    int margin_y = 0;
};

struct Marker {
    std::array<float, 3> position{};
    std::string name;
};

bool to_picture(const Projection& projection, const std::array<float, 3>& eye, const std::array<float, 3>& target, const std::array<float, 3>& point, float& u, float& v);
bool active();
void set_projection(const Projection& projection);
void set_view(const std::array<float, 3>& eye, const std::array<float, 3>& target, std::vector<Marker> markers);
void set_picture(const options::Rect& picture);
bool take(uint64_t& version, options::Image& image, bool& visible);
options::Rect place();

}
