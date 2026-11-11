#include "wp/canvas.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "wp/log.h"

namespace wp::canvas {

namespace {

constexpr char32_t kEllipsis = 0x2026;

struct Font {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

std::string matched_font(const char* pattern) {
#ifdef _WIN32
    (void)pattern;
    return "";
#else
    std::string command = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return "";
    }
    std::string path;
    char buffer[512];
    while (std::fgets(buffer, sizeof buffer, pipe)) {
        path += buffer;
    }
    pclose(pipe);
    return path;
#endif
}

bool adopt(Font& font, const char* source) {
    int offset = stbtt_GetFontOffsetForIndex(font.data.data(), 0);
    if (offset >= 0 && stbtt_InitFont(&font.info, font.data.data(), offset)) {
        font.ok = true;
        log::write("options", "font %s", source);
        return true;
    }
    font.data.clear();
    return false;
}

Font load_file(const std::vector<std::string>& candidates) {
    Font font;
    for (const std::string& path : candidates) {
        std::error_code error;
        if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        font.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (adopt(font, path.c_str())) {
            return font;
        }
    }
    return font;
}

std::string windows_font(const char* name) {
    const char* windows = std::getenv("WINDIR");
    return windows ? std::string(windows) + "/Fonts/" + name : "";
}

Font load_system(bool bold) {
    std::vector<std::string> candidates = {matched_font(bold ? "sans-serif:weight=180" : "sans-serif")};
    const char* fallbacks[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",   "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/System/Library/Fonts/Supplemental/Arial.ttf",      "/Library/Fonts/Arial.ttf",
    };
    candidates.insert(candidates.end(), std::begin(fallbacks), std::end(fallbacks));
    if (std::getenv("WINDIR")) {
        candidates.insert(candidates.begin(), windows_font(bold ? "seguisb.ttf" : "segoeui.ttf"));
        candidates.push_back(windows_font("segoeui.ttf"));
        candidates.push_back(windows_font("arial.ttf"));
    }
    Font font = load_file(candidates);
    if (!font.ok) {
        log::write("options", "no usable system font");
    }
    return font;
}

Font load_embedded(const char* name) {
    Font font;
    std::string_view blob = embedded_font(name);
    font.data.assign(blob.begin(), blob.end());
    if (!font.data.empty()) {
        adopt(font, name);
    }
    return font;
}

Font load_mono() {
    std::vector<std::string> candidates = {windows_font("consolab.ttf"), windows_font("consola.ttf"), matched_font("monospace:weight=200"),
                                           "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
                                           "/System/Library/Fonts/Menlo.ttc"};
    return load_file(candidates);
}

const Font& font(FontId id) {
    static const Font system = load_system(false);
    static const Font system_bold = load_system(true);
    static const Font body = load_embedded("nunito_extrabold");
    static const Font heavy = load_embedded("nunito_black");
    static const Font mono = load_mono();
    const Font& bold = system_bold.ok ? system_bold : system;
    switch (id) {
    case FontId::Body:
        return body.ok ? body : bold;
    case FontId::Heavy:
        return heavy.ok ? heavy : bold;
    case FontId::Mono:
        return mono.ok ? mono : system;
    case FontId::SystemBold:
        return bold;
    default:
        return system;
    }
}

const Font& font_of(const Face& face) {
    return *static_cast<const Font*>(face.font);
}

float advance(const Face& face, char32_t codepoint, char32_t next) {
    const Font& chosen = font_of(face);
    int width = 0;
    int bearing = 0;
    stbtt_GetCodepointHMetrics(&chosen.info, static_cast<int>(codepoint), &width, &bearing);
    int kern = next ? stbtt_GetCodepointKernAdvance(&chosen.info, static_cast<int>(codepoint), static_cast<int>(next)) : 0;
    return (width + kern) * face.scale + (next ? face.spacing : 0.0f);
}

uint32_t opaque(uint32_t color) {
    return (color >> 24) == 0 ? color | 0xFF000000u : color;
}

}

bool font_ok(FontId id) {
    return font(id).ok;
}

Face make_face(FontId id, int pixel_height, float spacing_em) {
    const Font& chosen = font(id);
    Face face;
    face.font = &chosen;
    if (!chosen.ok) {
        return face;
    }
    face.scale = stbtt_ScaleForMappingEmToPixels(&chosen.info, static_cast<float>(pixel_height));
    int ascent = 0;
    int descent = 0;
    int gap = 0;
    stbtt_GetFontVMetrics(&chosen.info, &ascent, &descent, &gap);
    face.ascent = static_cast<int>(std::lround(ascent * face.scale));
    face.descent = static_cast<int>(std::lround(descent * face.scale));
    face.spacing = spacing_em * static_cast<float>(pixel_height);
    return face;
}

std::u32string decode(const std::string& text) {
    std::u32string result;
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int length = lead < 0x80 ? 1 : (lead >> 5) == 6 ? 2 : (lead >> 4) == 14 ? 3 : (lead >> 3) == 30 ? 4 : 1;
        char32_t value = length == 1 ? lead : lead & (0x7F >> length);
        for (int k = 1; k < length && i + k < text.size(); k++) {
            value = (value << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        }
        result.push_back(value);
        i += static_cast<size_t>(length);
    }
    return result;
}

float text_width(const Face& face, const std::u32string& text) {
    if (!face.font || !font_of(face).ok) {
        return 0.0f;
    }
    float width = 0.0f;
    for (size_t i = 0; i < text.size(); i++) {
        width += advance(face, text[i], i + 1 < text.size() ? text[i + 1] : 0);
    }
    return width;
}

float text_width(const Face& face, const std::string& utf8) {
    return text_width(face, decode(utf8));
}

void Canvas::blend(int x, int y, uint32_t color, unsigned coverage) {
    if (x < 0 || y < 0 || x >= width || y >= height || coverage == 0) {
        return;
    }
    color = opaque(color);
    unsigned alpha = ((color >> 24) * std::min(coverage, 255u) + 127) / 255;
    if (alpha == 0) {
        return;
    }
    uint32_t& target = pixels[static_cast<size_t>(y) * width + x];
    if (alpha == 255) {
        target = color;
        return;
    }
    unsigned below_alpha = target >> 24;
    unsigned kept = below_alpha * (255 - alpha);
    unsigned total = alpha * 255 + kept;
    uint32_t result = static_cast<uint32_t>((total + 127) / 255) << 24;
    for (int shift = 0; shift < 24; shift += 8) {
        unsigned below = (target >> shift) & 0xFF;
        unsigned above = (color >> shift) & 0xFF;
        unsigned mixed = (above * alpha * 255 + below * kept + total / 2) / total;
        result |= std::min(mixed, 255u) << shift;
    }
    target = result;
}

void Canvas::fill(int left, int top, int right, int bottom, uint32_t color) {
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, width);
    bottom = std::min(bottom, height);
    color = opaque(color);
    for (int y = top; y < bottom; y++) {
        if ((color >> 24) == 0xFF) {
            std::fill(pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + left, pixels.begin() + static_cast<std::ptrdiff_t>(y) * width + right, color);
            continue;
        }
        for (int x = left; x < right; x++) {
            blend(x, y, color, 255);
        }
    }
}

void Canvas::fill_round(int left, int top, int right, int bottom, int radius, uint32_t color) {
    radius = std::max(0, std::min({radius, (right - left) / 2, (bottom - top) / 2}));
    for (int y = std::max(top, 0); y < std::min(bottom, height); y++) {
        for (int x = std::max(left, 0); x < std::min(right, width); x++) {
            double cx = x + 0.5;
            double cy = y + 0.5;
            double ox = cx < left + radius ? left + radius - cx : cx > right - radius ? cx - (right - radius) : 0.0;
            double oy = cy < top + radius ? top + radius - cy : cy > bottom - radius ? cy - (bottom - radius) : 0.0;
            double coverage = 1.0;
            if (ox > 0.0 && oy > 0.0) {
                coverage = std::clamp(radius - std::sqrt(ox * ox + oy * oy) + 0.5, 0.0, 1.0);
            }
            blend(x, y, color, static_cast<unsigned>(coverage * 255.0 + 0.5));
        }
    }
}

void Canvas::fill_polygon(const std::vector<std::array<float, 2>>& points, uint32_t color) {
    constexpr int kSamples = 4;
    if (points.size() < 3 || width <= 0 || height <= 0) {
        return;
    }
    float min_x = points[0][0];
    float max_x = points[0][0];
    float min_y = points[0][1];
    float max_y = points[0][1];
    for (const std::array<float, 2>& point : points) {
        min_x = std::min(min_x, point[0]);
        max_x = std::max(max_x, point[0]);
        min_y = std::min(min_y, point[1]);
        max_y = std::max(max_y, point[1]);
    }
    int left = std::max(0, static_cast<int>(std::floor(min_x)));
    int right = std::min(width, static_cast<int>(std::ceil(max_x)));
    int top = std::max(0, static_cast<int>(std::floor(min_y)));
    int bottom = std::min(height, static_cast<int>(std::ceil(max_y)));
    if (left >= right || top >= bottom) {
        return;
    }
    std::vector<int> coverage(static_cast<size_t>(right - left));
    std::vector<std::pair<float, int>> crossings;
    for (int y = top; y < bottom; y++) {
        std::fill(coverage.begin(), coverage.end(), 0);
        for (int row = 0; row < kSamples; row++) {
            float sample_y = y + (row + 0.5f) / kSamples;
            crossings.clear();
            for (size_t i = 0; i < points.size(); i++) {
                const std::array<float, 2>& a = points[i];
                const std::array<float, 2>& b = points[(i + 1) % points.size()];
                if (a[1] == b[1] || sample_y < std::min(a[1], b[1]) || sample_y >= std::max(a[1], b[1])) {
                    continue;
                }
                float x = a[0] + (sample_y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]);
                crossings.push_back({x, b[1] > a[1] ? 1 : -1});
            }
            std::sort(crossings.begin(), crossings.end());
            int winding = 0;
            for (size_t i = 0; i + 1 < crossings.size(); i++) {
                winding += crossings[i].second;
                if (winding == 0) {
                    continue;
                }
                int first = static_cast<int>(std::ceil((crossings[i].first - left) * kSamples - 0.5f));
                int last = static_cast<int>(std::ceil((crossings[i + 1].first - left) * kSamples - 0.5f));
                first = std::max(first, 0);
                last = std::min(last, (right - left) * kSamples);
                while (first < last && (first % kSamples) != 0) {
                    coverage[static_cast<size_t>(first / kSamples)]++;
                    first++;
                }
                while (last - first >= kSamples) {
                    coverage[static_cast<size_t>(first / kSamples)] += kSamples;
                    first += kSamples;
                }
                while (first < last) {
                    coverage[static_cast<size_t>(first / kSamples)]++;
                    first++;
                }
            }
        }
        for (int x = left; x < right; x++) {
            int covered = coverage[static_cast<size_t>(x - left)];
            if (covered > 0) {
                blend(x, y, color, static_cast<unsigned>(covered * 255 / (kSamples * kSamples)));
            }
        }
    }
}

void Canvas::dot(int x, int y, int stroke, uint32_t color) {
    fill(x - stroke / 2, y - stroke / 2, x - stroke / 2 + stroke, y - stroke / 2 + stroke, color);
}

void Canvas::cross(int cx, int cy, int half, int stroke, uint32_t color) {
    for (int k = -half; k <= half; k++) {
        dot(cx + k, cy + k, stroke, color);
        dot(cx + k, cy - k, stroke, color);
    }
}

void Canvas::chevron(int cx, int cy, int half, int direction, int stroke, uint32_t color) {
    for (int k = 0; k <= half; k++) {
        int x = cx + direction * (half / 2 - k);
        dot(x, cy - half + k, stroke, color);
        dot(x, cy + half - k, stroke, color);
    }
}

void Canvas::text(const Face& face, uint32_t color, const std::string& value, int left, int top, int right, int bottom, Align align, bool ellipsis) {
    if (!face.font || !font_of(face).ok) {
        return;
    }
    const Font& chosen = font_of(face);
    std::u32string glyphs = decode(value);
    if (ellipsis && text_width(face, glyphs) > right - left) {
        while (!glyphs.empty() && text_width(face, glyphs + kEllipsis) > right - left) {
            glyphs.pop_back();
        }
        glyphs.push_back(kEllipsis);
    }
    float total = text_width(face, glyphs);
    float x = align == Align::Left ? static_cast<float>(left) : align == Align::Right ? right - total : left + (right - left - total) / 2.0f;
    int baseline = top + (bottom - top - (face.ascent - face.descent)) / 2 + face.ascent;
    std::vector<unsigned char> bitmap;
    for (size_t i = 0; i < glyphs.size(); i++) {
        int codepoint = static_cast<int>(glyphs[i]);
        float origin = std::floor(x);
        float shift = x - origin;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        stbtt_GetCodepointBitmapBoxSubpixel(&chosen.info, codepoint, face.scale, face.scale, shift, 0.0f, &x0, &y0, &x1, &y1);
        int glyph_width = x1 - x0;
        int glyph_height = y1 - y0;
        if (glyph_width > 0 && glyph_height > 0) {
            bitmap.assign(static_cast<size_t>(glyph_width) * glyph_height, 0);
            stbtt_MakeCodepointBitmapSubpixel(&chosen.info, bitmap.data(), glyph_width, glyph_height, glyph_width, face.scale, face.scale, shift, 0.0f, codepoint);
            for (int gy = 0; gy < glyph_height; gy++) {
                for (int gx = 0; gx < glyph_width; gx++) {
                    blend(static_cast<int>(origin) + x0 + gx, baseline + y0 + gy, color, bitmap[static_cast<size_t>(gy) * glyph_width + gx]);
                }
            }
        }
        x += advance(face, glyphs[i], i + 1 < glyphs.size() ? glyphs[i + 1] : 0);
    }
}

}
