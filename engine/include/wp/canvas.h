#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wp::canvas {

enum class Align { Left, Right, Center };

enum class FontId { Body, Heavy, Mono, System, SystemBold };

struct Face {
    const void* font = nullptr;
    float scale = 0.0f;
    int ascent = 0;
    int descent = 0;
    float spacing = 0.0f;
};

std::string_view embedded_font(const char* name);
bool font_ok(FontId font);
Face make_face(FontId font, int pixel_height, float spacing_em = 0.0f);
std::u32string decode(const std::string& text);
float text_width(const Face& face, const std::u32string& text);
float text_width(const Face& face, const std::string& utf8);

struct Canvas {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;

    void blend(int x, int y, uint32_t color, unsigned coverage);
    void fill(int left, int top, int right, int bottom, uint32_t color);
    void fill_round(int left, int top, int right, int bottom, int radius, uint32_t color);
    void fill_polygon(const std::vector<std::array<float, 2>>& points, uint32_t color);
    void dot(int x, int y, int stroke, uint32_t color);
    void cross(int cx, int cy, int half, int stroke, uint32_t color);
    void chevron(int cx, int cy, int half, int direction, int stroke, uint32_t color);
    void text(const Face& face, uint32_t color, const std::string& value, int left, int top, int right, int bottom, Align align, bool ellipsis);
};

}
