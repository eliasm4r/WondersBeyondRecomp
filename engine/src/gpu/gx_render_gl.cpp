#include "wp/gx_render.h"

#include "gx_render_common.h"
#include "../platform/gl_window.h"

#define GL_GLEXT_PROTOTYPES
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wp/color.h"
#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/titlebar.h"
#include "wp/options.h"
#include "wp/perf_overlay.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx::render {

void finish_write_backs();
bool write_back_overlaps(uint32_t address, size_t size);

namespace {

#define WP_GL_FUNCTIONS(X)                                                                                                                                  \
    X(ActiveTexture) X(AttachShader) X(BindBuffer) X(BindBufferBase) X(BindFramebuffer) X(BindSampler) X(BindTexture) X(BindVertexArray)              \
    X(BlendEquationSeparate) X(BlendFuncSeparate) X(BufferData) X(BufferSubData) X(CheckFramebufferStatus) X(Clear) X(ClearColor) X(ClearDepth)      \
    X(ColorMask) X(CompileShader) X(CreateProgram) X(CreateShader) X(DeleteFramebuffers) X(DeleteShader) X(DeleteTextures) X(DepthFunc)              \
    X(DepthMask) X(Disable) X(DrawArrays) X(Enable) X(EnableVertexAttribArray) X(FramebufferTexture2D) X(GenBuffers) X(GenFramebuffers)             \
    X(GenSamplers) X(GenTextures) X(GenVertexArrays) X(GenerateMipmap) X(GetIntegerv) X(GetProgramInfoLog) X(GetProgramiv) X(GetShaderInfoLog)      \
    X(GetShaderiv) X(GetString) X(GetStringi) X(GetUniformBlockIndex) X(GetUniformLocation) X(LinkProgram) X(PixelStorei) X(ReadPixels)             \
    X(SamplerParameterf) X(SamplerParameteri) X(Scissor) X(ShaderSource) X(TexImage2D) X(TexParameteri) X(TexSubImage2D) X(Uniform1i)              \
    X(Uniform1f) X(Uniform4f) X(UniformBlockBinding) X(UseProgram) X(VertexAttribPointer) X(Viewport) X(MapBufferRange) X(UnmapBuffer)    \
    X(FenceSync) X(ClientWaitSync) X(DeleteSync) X(DeleteProgram)

#define WP_GL_RUN_FUNCTIONS(X)                                                                                                                              \
    X(CullFace) X(DeleteBuffers) X(DisableVertexAttribArray) X(DrawElementsBaseVertex) X(GetError) X(TexBuffer) X(VertexAttrib4f) X(VertexAttribI4ui)  \
    X(VertexAttribIPointer)

struct Gl {
#define WP_GL_MEMBER(name) decltype(&::gl##name) name = nullptr;
    WP_GL_FUNCTIONS(WP_GL_MEMBER)
    WP_GL_RUN_FUNCTIONS(WP_GL_MEMBER)
#undef WP_GL_MEMBER
    decltype(&::glClipControl) ClipControl = nullptr;
};

Gl gl;

bool load_functions() {
    bool ok = true;
#define WP_GL_LOAD(name)                                                                                   \
    gl.name = reinterpret_cast<decltype(gl.name)>(SDL_GL_GetProcAddress("gl" #name));                     \
    if (!gl.name) {                                                                                        \
        std::fprintf(stderr, "OpenGL function gl%s is missing\n", #name);                                  \
        ok = false;                                                                                        \
    }
    WP_GL_FUNCTIONS(WP_GL_LOAD)
#undef WP_GL_LOAD
    return ok;
}

bool load_run_functions() {
    bool ok = true;
#define WP_GL_LOAD_RUN(name)                                                                \
    gl.name = reinterpret_cast<decltype(gl.name)>(SDL_GL_GetProcAddress("gl" #name));       \
    ok = ok && gl.name != nullptr;
    WP_GL_RUN_FUNCTIONS(WP_GL_LOAD_RUN)
#undef WP_GL_LOAD_RUN
    return ok;
}

constexpr uint32_t kVertexCapacity = 1 << 16;

const char* kFullscreenVertex = R"GLSL(#version 410 core
uniform float flip;
out vec2 uv;
void main() {
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    uv = corner;
    gl_Position = vec4(corner.x * 2.0 - 1.0, (1.0 - corner.y * 2.0) * flip, 0.0, 1.0);
}
)GLSL";

const char* kPresentFragment = R"GLSL(#version 410 core
uniform sampler2D frame;
uniform vec4 adjust;
in vec2 uv;
out vec4 color;
void main() {
    vec3 c = ((texture(frame, uv).rgb - 0.5) * adjust.y + 0.5) * adjust.x;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(luma + (c - luma) * adjust.z, 0.0, 1.0);
    color = vec4(pow(c, vec3(adjust.w)), 1.0);
}
)GLSL";

const char* kOverlayFragment = R"GLSL(#version 410 core
uniform sampler2D overlay;
in vec2 uv;
out vec4 color;
void main() {
    color = texture(overlay, uv);
}
)GLSL";

const char* kClearFragment = R"GLSL(#version 410 core
uniform vec4 clear_color;
uniform float clear_depth;
in vec2 uv;
out vec4 color;
void main() {
    color = clear_color;
    gl_FragDepth = clear_depth;
}
)GLSL";

const char* kCopyFragment = R"GLSL(#version 410 core
uniform sampler2D color_source;
uniform sampler2D depth_source;
layout(std140) uniform CopyConstants {
    uvec4 params;
    vec4 region;
    uvec4 filter_taps;
    vec4 rows;
    vec4 extent;
};
in vec2 uv;
out vec4 out_color;

vec4 convert(uvec4 raw) {
    if (params.z == 0u) {
        raw.a = 255u;
    }
    if (params.y != 0u) {
        const vec4 y_const = vec4(66.0, 129.0, 25.0, 16.0);
        const vec4 u_const = vec4(-38.0, -74.0, 112.0, 128.0);
        const vec4 v_const = vec4(112.0, -94.0, -18.0, 128.0);
        vec4 source = vec4(vec3(raw.rgb), 256.0);
        uvec3 yuv = uvec3(dot(y_const, source), dot(u_const, source), dot(v_const, source));
        raw.rgb = min((yuv >> 8u) + ((yuv >> 7u) & 1u), uvec3(255u));
    }
    vec4 value = vec4(raw) / 255.0;
    switch (params.x) {
    case 0u: {
        float red = float(raw.r & 0xF0u) / 240.0;
        return vec4(red);
    }
    case 1u:
    case 8u: return value.rrrr;
    case 2u: {
        vec2 red_alpha = vec2(uvec2(raw.r, raw.a) & 0xF0u) / 240.0;
        return red_alpha.rrrg;
    }
    case 3u: return value.rrra;
    case 4u: {
        vec2 red_blue = vec2(uvec2(raw.r, raw.b) & 0xF8u) / 248.0;
        float green = float(raw.g & 0xFCu) / 252.0;
        return vec4(red_blue.r, green, red_blue.g, 1.0);
    }
    case 5u: return vec4(vec3(raw.rgb & 0xF8u) / 248.0, float(raw.a & 0xE0u) / 224.0);
    case 7u: return value.aaaa;
    case 9u: return value.gggg;
    case 10u: return value.bbbb;
    case 11u: return value.rrrg;
    case 12u: return value.gggb;
    case 13u: return vec4(value.rgb, 1.0);
    default: return value;
    }
}

uvec4 sample_efb(vec2 position, float offset) {
    float y = clamp(position.y + offset, rows.y, rows.z);
    if (params.w != 0u) {
        uint depth = min(uint(clamp(texelFetch(depth_source, ivec2(int(position.x), int(y)), 0).r, 0.0, 1.0) * 16777216.0), 0xFFFFFFu);
        return uvec4((depth >> 16u) & 255u, (depth >> 8u) & 255u, depth & 255u, 255u);
    }
    return uvec4(roundEven(clamp(textureLod(color_source, vec2(position.x, y) / extent.xy, 0.0), 0.0, 1.0) * 255.0));
}

void main() {
    vec2 source = region.xy + uv * region.zw;
    uvec4 current = sample_efb(source, 0.0);
    uvec3 combined = current.rgb * filter_taps.y;
    if (filter_taps.x != 0u || filter_taps.z != 0u) {
        combined += sample_efb(source, -1.0).rgb * filter_taps.x + sample_efb(source, 1.0).rgb * filter_taps.z;
    }
    uvec4 raw = uvec4(combined >> 6u, current.a);
    if (filter_taps.w != 0u) {
        raw &= 0x1FFu;
    }
    raw = min(raw, uvec4(255u));
    if (rows.x != 1.0) {
        raw.rgb = uvec3(roundEven(pow(vec3(raw.rgb) / 255.0, vec3(rows.x)) * 255.0));
    }
    out_color = convert(raw);
}
)GLSL";

const char* kTevVertex = R"GLSL(#version 410 core
layout(location = 0) in vec4 a_position;
layout(location = 1) in vec4 a_c0;
layout(location = 2) in vec4 a_c1;
layout(location = 3) in vec3 a_uv[8];
uniform int depth_zero_to_one;
out vec4 v_c0;
out vec4 v_c1;
out vec3 v_uv[8];
void main() {
    float z = depth_zero_to_one != 0 ? a_position.z : 2.0 * a_position.z - a_position.w;
    gl_Position = vec4(a_position.x, -a_position.y, z, a_position.w);
    v_c0 = a_c0;
    v_c1 = a_c1;
    for (int i = 0; i < 8; i++) {
        v_uv[i] = a_uv[i];
    }
}
)GLSL";

const char* kRunVertex = R"GLSL(#version 410 core
uniform samplerBuffer rows;
uniform int run_row;
uniform int depth_zero_to_one;
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_c0;
layout(location = 2) in vec4 a_c1;
layout(location = 3) in vec3 a_normal;
layout(location = 4) in uvec4 a_m0;
layout(location = 5) in uvec4 a_m1;
layout(location = 6) in uvec4 a_m2;
layout(location = 7) in vec2 a_t0;
layout(location = 8) in vec2 a_t1;
layout(location = 9) in vec2 a_t2;
layout(location = 10) in vec2 a_t3;
layout(location = 11) in vec2 a_t4;
layout(location = 12) in vec2 a_t5;
layout(location = 13) in vec2 a_t6;
layout(location = 14) in vec2 a_t7;
out vec4 v_c0;
out vec4 v_c1;
out vec3 v_uv[8];

vec4 row(int at) {
    return texelFetch(rows, at);
}

vec3 transformed(int at, vec3 v) {
    vec4 r0 = row(at);
    vec4 r1 = row(at + 1);
    vec4 r2 = row(at + 2);
    precise vec3 result;
    result.x = r0.x * v.x + r0.y * v.y + r0.z * v.z + r0.w;
    result.y = r1.x * v.x + r1.y * v.y + r1.z * v.z + r1.w;
    result.z = r2.x * v.x + r2.y * v.y + r2.z * v.z + r2.w;
    return result;
}

vec3 unit(vec3 v) {
    float extent = sqrt(dot(v, v));
    return extent == 0.0 ? v : v / extent;
}

float ratio(float n, float d) {
    return d == 0.0 ? (n > 0.0 ? 1.0 : 0.0) : n / d;
}

float light_factor(int at, uint control, vec3 eye, vec3 normal) {
    vec4 a = row(at + 1);
    vec4 b = row(at + 2);
    vec4 c = row(at + 3);
    vec3 angle = a.xyz;
    vec3 range = vec3(a.w, b.x, b.y);
    vec3 direction = c.yzw;
    uint diffuse = (control >> 7) & 3u;
    uint attenuation = (control >> 9) & 3u;
    vec3 toward = vec3(b.z, b.w, c.x) - eye;
    float amount = 1.0;
    if (attenuation == 1u) {
        toward = unit(toward);
        amount = dot(toward, normal) >= 0.0 ? max(0.0, dot(direction, normal)) : 0.0;
        vec3 powers = vec3(1.0, amount, amount * amount);
        if (diffuse != 0u) {
            range = unit(range);
        }
        amount = ratio(max(0.0, dot(powers, angle)), dot(powers, range));
    } else if (attenuation == 3u) {
        float square = dot(toward, toward);
        float span = sqrt(square);
        toward = toward / span;
        amount = max(0.0, dot(toward, direction));
        float cone = angle.x + angle.y * amount + angle.z * amount * amount;
        float fade = range.x + range.y * span + range.z * square;
        amount = ratio(max(0.0, cone), fade);
    } else {
        toward = unit(toward);
        if (toward.x == 0.0 && toward.y == 0.0 && toward.z == 0.0) {
            toward = normal;
        }
    }
    float facing = dot(toward, normal);
    if (diffuse == 1u) {
        return amount * facing;
    }
    if (diffuse == 2u) {
        return amount * max(0.0, facing);
    }
    return amount;
}

float modulated(float material, float light) {
    int value = clamp(int(light), 0, 255);
    return float((int(material) * (value + (value >> 7))) >> 8) / 255.0;
}

uint light_mask(uint control) {
    return (control & 2u) != 0u ? (((control >> 2) & 15u) | (((control >> 11) & 15u) << 4)) : 0u;
}

void light_colors(int light, int matrix_row, uint word, vec3 eye, vec3 raw, inout vec4 c0, inout vec4 c1) {
    vec4 controls = row(light);
    int first = matrix_row + int(word >> 2);
    vec4 n0 = row(first);
    vec4 n1 = row(first + 1);
    vec4 n2 = row(first + 2);
    float m[12] = float[12](n0.x, n0.y, n0.z, n0.w, n1.x, n1.y, n1.z, n1.w, n2.x, n2.y, n2.z, n2.w);
    uint k = word & 3u;
    vec3 normal = unit(vec3(m[k] * raw.x + m[k + 1u] * raw.y + m[k + 2u] * raw.z,
                            m[k + 3u] * raw.x + m[k + 4u] * raw.y + m[k + 5u] * raw.z,
                            m[k + 6u] * raw.x + m[k + 7u] * raw.y + m[k + 8u] * raw.z));
    vec4 given[2] = vec4[2](c0, c1);
    vec4 result[2];
    for (uint channel = 0u; channel < 2u; channel++) {
        vec4 vertex = floor(given[channel] * 255.0 + 0.5);
        vec4 material = row(light + 1 + int(channel));
        vec4 ambient = row(light + 3 + int(channel));
        uint color = uint(channel == 0u ? controls.x : controls.y);
        uint alpha = uint(channel == 0u ? controls.z : controls.w);
        vec3 color_material = (color & 1u) != 0u ? vertex.rgb : material.rgb;
        float alpha_material = (alpha & 1u) != 0u ? vertex.a : material.a;
        uint color_mask = light_mask(color);
        uint alpha_mask = light_mask(alpha);
        vec3 sum = (color & 64u) != 0u ? vertex.rgb : ambient.rgb;
        float alpha_sum = (alpha & 64u) != 0u ? vertex.a : ambient.a;
        for (uint i = 0u; i < 8u; i++) {
            uint bit = 1u << i;
            int at = light + 8 + 4 * int(i);
            if ((color_mask & bit) != 0u) {
                sum += row(at).rgb * light_factor(at, color, eye, normal);
            }
            if ((alpha_mask & bit) != 0u) {
                alpha_sum += row(at).a * light_factor(at, alpha, eye, normal);
            }
        }
        if ((color & 2u) != 0u) {
            result[channel].rgb = vec3(modulated(color_material.r, sum.r), modulated(color_material.g, sum.g), modulated(color_material.b, sum.b));
        } else {
            result[channel].rgb = color_material / 255.0;
        }
        result[channel].a = (alpha & 2u) != 0u ? modulated(alpha_material, alpha_sum) : alpha_material / 255.0;
    }
    c0 = result[0];
    c1 = result[1];
}

void main() {
    int run = run_row;
    vec4 p0 = row(run);
    vec4 p1 = row(run + 1);
    vec4 p2 = row(run + 2);
    vec4 p3 = row(run + 3);
    int origin = run + int(p3.z);
    precise vec3 eye = transformed(origin + int(p1.w) + int(a_m0.x), a_position);
    precise float x;
    precise float y;
    precise float z;
    precise float w;
    if (p1.z != 0.0) {
        x = p0.x * eye.x + p0.y;
        y = p0.z * eye.y + p0.w;
        z = p1.x * eye.z + p1.y;
        w = 1.0;
    } else {
        x = p0.x * eye.x + p0.y * eye.z;
        y = p0.z * eye.y + p0.w * eye.z;
        z = p1.x * eye.z + p1.y;
        w = -eye.z;
    }
    precise vec4 position;
    position.x = x * p2.x + w * p2.y;
    position.y = y * p2.z + w * p2.w;
    position.z = (z * p3.x + w * (16777215.0 - p3.y)) / 16777215.0;
    position.w = w;
    uint setup = uint(p3.w);
    uint count = min(setup & 15u, 8u);
    bool dual = (setup & 16u) != 0u;
    uint indices[8] = uint[8](a_m0.y, a_m0.z, a_m0.w, a_m1.x, a_m1.y, a_m1.z, a_m1.w, a_m2.x);
    vec2 inputs[8] = vec2[8](a_t0, a_t1, a_t2, a_t3, a_t4, a_t5, a_t6, a_t7);
    vec3 uv[8];
    for (uint k = 0u; k < 8u; k++) {
        uv[k] = vec3(0.0);
    }
    for (uint i = 0u; i < count; i++) {
        int place = run + 7 + 7 * int(i);
        vec4 info = row(place);
        uint source = uint(info.x);
        uint flags = uint(info.y);
        vec3 given = vec3(0.0, 0.0, 1.0);
        if (source == 0u) {
            given = a_position;
        } else if (source == 1u) {
            given = a_normal;
        } else if (source >= 5u && source < 13u) {
            given.xy = inputs[source - 5u];
        }
        if ((flags & 2u) == 0u) {
            given.z = 1.0;
        }
        vec3 coord = transformed(origin + int(info.z) + int(indices[i]), given);
        if ((flags & 1u) == 0u) {
            coord.z = 1.0;
        }
        if (dual) {
            if ((flags & 4u) != 0u) {
                float extent = sqrt(coord.x * coord.x + coord.y * coord.y + coord.z * coord.z);
                if (extent > 0.0) {
                    coord /= extent;
                }
            }
            coord = transformed(place + 4, coord);
        }
        if (coord.z == 0.0) {
            coord.xy = clamp(coord.xy / 2.0, -1.0, 1.0);
            coord.z = 1.0;
        }
        uv[i] = coord;
    }
    vec4 c0 = a_c0;
    vec4 c1 = a_c1;
    if ((setup & 32u) != 0u) {
        int light = run + 7 + 7 * int(count);
        if (p1.w == 0.0) {
            light_colors(light, origin + 66, (a_m0.x & 31u) * 3u, eye, a_normal, c0, c1);
        } else {
            light_colors(light, light + 5, 0u, eye, a_normal, c0, c1);
        }
    }
    v_c0 = c0;
    v_c1 = c1;
    for (int k = 0; k < 8; k++) {
        v_uv[k] = uv[k];
    }
    float depth = depth_zero_to_one != 0 ? position.z : 2.0 * position.z - position.w;
    gl_Position = vec4(position.x, -position.y, depth, position.w);
}
)GLSL";

const char* kTevFragment = R"GLSL(
layout(std140) uniform Constants {
    vec4 initial[4];
    vec4 konst[4];
    uvec4 stage[16];
    uvec4 header;
    uvec4 swaps;
    vec4 texdims[8];
    ivec4 indmtx[6];
    uvec4 indirect;
    uvec4 tevind[16];
    vec4 fog;
    ivec4 fog_integer;
    vec4 fog_range[3];
};

uniform sampler2D t0;
uniform sampler2D t1;
uniform sampler2D t2;
uniform sampler2D t3;
uniform sampler2D t4;
uniform sampler2D t5;
uniform sampler2D t6;
uniform sampler2D t7;

in vec4 v_c0;
in vec4 v_c1;
in vec3 v_uv[8];

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

vec2 projected(vec3 uv) {
    return uv.z == 0.0 ? clamp(uv.xy / 2.0, -1.0, 1.0) : uv.xy / uv.z;
}

vec2 select_uv(uint i) {
    switch (i) {
    case 0u: return projected(v_uv[0]);
    case 1u: return projected(v_uv[1]);
    case 2u: return projected(v_uv[2]);
    case 3u: return projected(v_uv[3]);
    case 4u: return projected(v_uv[4]);
    case 5u: return projected(v_uv[5]);
    case 6u: return projected(v_uv[6]);
    default: return projected(v_uv[7]);
    }
}

vec4 sample_map(uint m, vec2 uv, vec2 dx, vec2 dy) {
    switch (m) {
    case 0u: return textureGrad(t0, uv, dx, dy);
    case 1u: return textureGrad(t1, uv, dx, dy);
    case 2u: return textureGrad(t2, uv, dx, dy);
    case 3u: return textureGrad(t3, uv, dx, dy);
    case 4u: return textureGrad(t4, uv, dx, dy);
    case 5u: return textureGrad(t5, uv, dx, dy);
    case 6u: return textureGrad(t6, uv, dx, dy);
    default: return textureGrad(t7, uv, dx, dy);
    }
}

const int kFractions[8] = int[8](255, 223, 191, 159, 128, 96, 64, 32);

ivec4 quantized(vec4 value) {
    return ivec4(roundEven(value * 255.0));
}

ivec3 konst_color(uint sel, ivec4 k[4]) {
    if (sel < 8u) {
        return ivec3(kFractions[sel]);
    }
    if (sel >= 12u && sel < 16u) {
        return k[sel - 12u].rgb;
    }
    if (sel >= 16u) {
        uint index = (sel - 16u) & 3u;
        uint component = (sel - 16u) >> 2u;
        return ivec3(k[index][component]);
    }
    return ivec3(0);
}

int konst_alpha(uint sel, ivec4 k[4]) {
    if (sel < 8u) {
        return kFractions[sel];
    }
    if (sel >= 16u) {
        uint index = (sel - 16u) & 3u;
        uint component = (sel - 16u) >> 2u;
        return k[index][component];
    }
    return 0;
}

ivec3 color_input(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, ivec3 kc) {
    switch (sel) {
    case 0u: return r[0].rgb;
    case 1u: return r[0].aaa;
    case 2u: return r[1].rgb;
    case 3u: return r[1].aaa;
    case 4u: return r[2].rgb;
    case 5u: return r[2].aaa;
    case 6u: return r[3].rgb;
    case 7u: return r[3].aaa;
    case 8u: return tex.rgb;
    case 9u: return tex.aaa;
    case 10u: return ras.rgb;
    case 11u: return ras.aaa;
    case 12u: return ivec3(255);
    case 13u: return ivec3(128);
    case 14u: return kc;
    default: return ivec3(0);
    }
}

int alpha_input(uint sel, ivec4 r[4], ivec4 tex, ivec4 ras, int ka) {
    switch (sel) {
    case 0u: return r[0].a;
    case 1u: return r[1].a;
    case 2u: return r[2].a;
    case 3u: return r[3].a;
    case 4u: return tex.a;
    case 5u: return ras.a;
    case 6u: return ka;
    default: return 0;
    }
}

bool compare_alpha(uint mode, uint value, uint reference) {
    switch (mode) {
    case 0u: return false;
    case 1u: return value < reference;
    case 2u: return value == reference;
    case 3u: return value <= reference;
    case 4u: return value > reference;
    case 5u: return value != reference;
    case 6u: return value >= reference;
    default: return true;
    }
}

bool compare_inputs(uint mode, bool eq, ivec3 a, ivec3 b) {
    int left = a.r;
    int right = b.r;
    if (mode == 1u) {
        left = a.r + a.g * 256;
        right = b.r + b.g * 256;
    } else if (mode == 2u) {
        left = a.r + a.g * 256 + a.b * 65536;
        right = b.r + b.g * 256 + b.b * 65536;
    }
    return eq ? left == right : left > right;
}

ivec3 compare_color(uint mode, bool eq, ivec3 a, ivec3 b, ivec3 c) {
    if (mode == 3u) {
        bvec3 passed = eq ? equal(a, b) : greaterThan(a, b);
        return ivec3(passed.r ? c.r : 0, passed.g ? c.g : 0, passed.b ? c.b : 0);
    }
    return compare_inputs(mode, eq, a, b) ? c : ivec3(0);
}

ivec4 apply_swap(ivec4 value, uint table) {
    uint order_bits = swaps[table];
    int components[4] = int[4](value.r, value.g, value.b, value.a);
    return ivec4(components[order_bits & 3u], components[(order_bits >> 2u) & 3u], components[(order_bits >> 4u) & 3u], components[(order_bits >> 6u) & 3u]);
}

ivec3 combine(ivec3 a, ivec3 b, ivec3 c, ivec3 d, uint bias_code, bool subtract, uint scale_code) {
    int bias = bias_code == 1u ? 128 : (bias_code == 2u ? -128 : 0);
    int shift = scale_code == 1u ? 1 : (scale_code == 2u ? 2 : 0);
    int rounding = scale_code == 3u ? 0 : (subtract ? 127 : 128);
    ivec3 blended = ((((a << 8) + (b - a) * (c + (c >> 7))) << shift) + rounding) >> 8;
    ivec3 base = (d + bias) << shift;
    ivec3 value = subtract ? base - blended : base + blended;
    return scale_code == 3u ? value >> 1 : value;
}

int idot(ivec3 a, ivec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

const int kWraps[5] = int[5](256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7);
const int kFormatShifts[4] = int[4](0, 3, 4, 5);
const int kBumpShifts[4] = int[4](0, 5, 4, 3);

ivec2 fixpoint(uint coord) {
    return ivec2(select_uv(coord) * texdims[coord].zw * 128.0);
}

ivec4 sample_fixed(uint map, ivec2 coords, vec2 dx, vec2 dy, uint coord) {
    vec2 size = texdims[map].xy;
    vec2 ratio = texdims[coord].zw / size;
    return quantized(sample_map(map, vec2(coords) / (size * 128.0), dx * ratio, dy * ratio));
}

void main() {
    vec2 gradient_x[8];
    vec2 gradient_y[8];
    for (int g = 0; g < 8; g++) {
        vec2 value = projected(v_uv[g]);
        gradient_x[g] = dFdx(value);
        gradient_y[g] = dFdy(value);
    }
    ivec4 k[4] = ivec4[4](quantized(konst[0]), quantized(konst[1]), quantized(konst[2]), quantized(konst[3]));
    ivec4 r[4] = ivec4[4](quantized(initial[0]), quantized(initial[1]), quantized(initial[2]), quantized(initial[3]));
    ivec4 c0 = quantized(v_c0);
    ivec4 c1 = quantized(v_c1);
    uint count = header.x;
    if (count == 0u) {
        r[0] = c0;
    }
    uint indirect_stages = indirect.x & 7u;
    uint texgens = (indirect.x >> 8u) & 15u;
    ivec3 indirect_texel[4] = ivec3[4](ivec3(0), ivec3(0), ivec3(0), ivec3(0));
    for (uint j = 0u; j < indirect_stages && j < 4u; j++) {
        uint reference = indirect.y >> (6u * j);
        uint map = reference & 7u;
        uint coord = (reference >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint scales = j < 2u ? indirect.z : indirect.w;
        uint shift_s = (scales >> ((j & 1u) * 8u)) & 15u;
        uint shift_t = (scales >> ((j & 1u) * 8u + 4u)) & 15u;
        ivec2 base = fixpoint(coord);
        indirect_texel[j] = sample_fixed(map, ivec2(base.x >> shift_s, base.y >> shift_t), gradient_x[coord], gradient_y[coord], coord).abg;
    }
    ivec2 tevcoord = ivec2(0);
    int alphabump = 0;
    for (uint i = 0u; i < count; i++) {
        uint ce = stage[i].x;
        uint ae = stage[i].y;
        uint order = stage[i].z;
        uint ksel = stage[i].w;
        uint coord = (order >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint ind = tevind[i].x;
        uint bt = ind & 3u;
        uint format = (ind >> 2u) & 3u;
        uint bias = (ind >> 4u) & 7u;
        uint bump = (ind >> 7u) & 3u;
        uint matrix_index = (ind >> 9u) & 3u;
        uint matrix_id = (ind >> 11u) & 3u;
        uint wrap_s = (ind >> 13u) & 7u;
        uint wrap_t = (ind >> 16u) & 7u;
        bool has_stage = bt < indirect_stages;
        ivec2 base = fixpoint(coord);
        ivec2 offset = ivec2(0);
        if (has_stage && bump != 0u) {
            alphabump = (indirect_texel[bt][max(bump, 1u) - 1u] << kBumpShifts[format]) & 248;
        }
        if (has_stage && matrix_index != 0u) {
            ivec3 crd = indirect_texel[bt] >> kFormatShifts[format];
            int add = format == 0u ? -128 : 1;
            if ((bias & 1u) != 0u) crd.x += add;
            if ((bias & 2u) != 0u) crd.y += add;
            if ((bias & 4u) != 0u) crd.z += add;
            uint m = 2u * (matrix_index - 1u);
            if (matrix_id == 0u) {
                offset = ivec2(idot(indmtx[m].xyz, crd), idot(indmtx[m + 1u].xyz, crd)) >> 3;
            } else if (matrix_id == 1u) {
                offset = (base * crd.xx) >> 8;
            } else if (matrix_id == 2u) {
                offset = (base * crd.yy) >> 8;
            }
            int shift = indmtx[m].w;
            offset = shift >= 0 ? (offset >> shift) : (offset << (-shift));
        }
        ivec2 wrapped;
        wrapped.x = wrap_s == 0u ? base.x : (wrap_s >= 6u ? 0 : (base.x & (kWraps[max(wrap_s, 1u) - 1u] - 1)));
        wrapped.y = wrap_t == 0u ? base.y : (wrap_t >= 6u ? 0 : (base.y & (kWraps[max(wrap_t, 1u) - 1u] - 1)));
        tevcoord = ((ind >> 20u) & 1u) != 0u ? tevcoord + wrapped + offset : wrapped + offset;
        tevcoord = (tevcoord << 8) >> 8;
        ivec4 tex = ivec4(255);
        if ((order & 0x40u) != 0u) {
            tex = texgens == 0u ? ivec4(0) : apply_swap(sample_fixed(order & 7u, tevcoord, gradient_x[coord], gradient_y[coord], coord), (ae >> 2u) & 3u);
        }
        ivec4 ras = ivec4(0);
        uint chan = (order >> 7u) & 7u;
        if (chan == 0u) {
            ras = c0;
        } else if (chan == 1u) {
            ras = c1;
        } else if (chan == 5u) {
            ras = ivec4(alphabump);
        } else if (chan == 6u) {
            ras = ivec4(alphabump | (alphabump >> 5));
        }
        ras = apply_swap(ras, ae & 3u);
        ivec3 kc = konst_color(ksel & 31u, k);
        int ka = konst_alpha((ksel >> 5u) & 31u, k);
        ivec3 a = color_input((ce >> 12u) & 15u, r, tex, ras, kc) & 255;
        ivec3 b = color_input((ce >> 8u) & 15u, r, tex, ras, kc) & 255;
        ivec3 c = color_input((ce >> 4u) & 15u, r, tex, ras, kc) & 255;
        ivec3 d = color_input(ce & 15u, r, tex, ras, kc);
        uint bias_code = (ce >> 16u) & 3u;
        uint scale_code = (ce >> 20u) & 3u;
        ivec3 color;
        if (bias_code == 3u) {
            color = d + compare_color(scale_code, ((ce >> 18u) & 1u) != 0u, a, b, c);
        } else {
            color = combine(a, b, c, d, bias_code, ((ce >> 18u) & 1u) != 0u, scale_code);
        }
        color = ((ce >> 19u) & 1u) != 0u ? clamp(color, 0, 255) : clamp(color, -1024, 1023);
        int aa = alpha_input((ae >> 13u) & 7u, r, tex, ras, ka) & 255;
        int ab = alpha_input((ae >> 10u) & 7u, r, tex, ras, ka) & 255;
        int ac = alpha_input((ae >> 7u) & 7u, r, tex, ras, ka) & 255;
        int ad = alpha_input((ae >> 4u) & 7u, r, tex, ras, ka);
        uint abias_code = (ae >> 16u) & 3u;
        uint ascale_code = (ae >> 20u) & 3u;
        int alpha;
        if (abias_code == 3u) {
            bool aequal = ((ae >> 18u) & 1u) != 0u;
            bool apass = ascale_code == 3u ? (aequal ? aa == ab : aa > ab) : compare_inputs(ascale_code, aequal, a, b);
            alpha = ad + (apass ? ac : 0);
        } else {
            alpha = combine(ivec3(aa, 0, 0), ivec3(ab, 0, 0), ivec3(ac, 0, 0), ivec3(ad, 0, 0), abias_code, ((ae >> 18u) & 1u) != 0u, ascale_code).x;
        }
        alpha = ((ae >> 19u) & 1u) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);
        uint color_dest = (ce >> 22u) & 3u;
        uint alpha_dest = (ae >> 22u) & 3u;
        r[color_dest].rgb = color;
        r[alpha_dest].a = alpha;
    }
    ivec4 result = r[0];
    if (count > 0u) {
        uint last_color = (stage[max(count, 1u) - 1u].x >> 22u) & 3u;
        uint last_alpha = (stage[max(count, 1u) - 1u].y >> 22u) & 3u;
        result = ivec4(r[last_color].rgb, r[last_alpha].a);
    }
    result &= 255;
    if ((header.w & 2u) != 0u) {
        ivec2 dither = ivec2(gl_FragCoord.xy) & 1;
        result.rgb = (result.rgb - (result.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;
    }
    uint fog_select = uint(fog_integer.x) & 15u;
    uint fog_type = fog_select >> 1u;
    if (fog_type != 0u) {
        int z = clamp(int(gl_FragCoord.z * 16777216.0), 0, 0xFFFFFF);
        float ze = (fog_select & 1u) == 0u ? (fog.x * 16777216.0) / float(fog_integer.y - (z >> fog_integer.w)) : fog.x * float(z) / 16777216.0;
        if ((fog_integer.x & 16) != 0) {
            float offset = (2.0 * (gl_FragCoord.x / fog.w)) - 1.0 - fog.z;
            float index = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
            uint lower = uint(index);
            uint upper = min(lower + 1u, 9u);
            float kr = mix(fog_range[lower >> 2u][lower & 3u], fog_range[upper >> 2u][upper & 3u], fract(index));
            ze *= sqrt(offset * offset + kr * kr) / kr;
        }
        float amount = clamp(ze - fog.y, 0.0, 1.0);
        if (fog_type == 4u) {
            amount = 1.0 - exp2(-8.0 * amount);
        } else if (fog_type == 5u) {
            amount = 1.0 - exp2(-8.0 * amount * amount);
        } else if (fog_type == 6u) {
            amount = exp2(-8.0 * (1.0 - amount));
        } else if (fog_type == 7u) {
            amount = exp2(-8.0 * (1.0 - amount) * (1.0 - amount));
        }
        int weight = int(roundEven(amount * 256.0));
        ivec3 fog_color = ivec3((fog_integer.z >> 16) & 255, (fog_integer.z >> 8) & 255, fog_integer.z & 255);
        result.rgb = (result.rgb * (256 - weight) + fog_color * weight) >> 8;
    }
    uint compare = header.y;
    uint value = uint(result.a);
    bool first = compare_alpha((compare >> 16u) & 7u, value, compare & 255u);
    bool second = compare_alpha((compare >> 19u) & 7u, value, (compare >> 8u) & 255u);
    uint logic = (compare >> 22u) & 3u;
    bool accepted = logic == 0u ? (first && second) : (logic == 1u ? (first || second) : (logic == 2u ? (first != second) : (first == second)));
    if (!accepted) {
        discard;
    }
    out_blend = vec4(result) / 255.0;
    uint source = (header.w >> 2u) & 3u;
    ivec4 written = source == 0u ? result : (source == 1u ? ivec4(0) : (source == 2u ? ivec4(255) : 255 - result));
    out_color = vec4(written) / 255.0;
    if ((header.z & 0x100u) != 0u) {
        out_color.a = float(header.z & 255u) / 255.0;
    }
    if ((header.w & 1u) != 0u) {
        out_color.rgb = vec3(written.rgb >> 2) / 63.0;
        out_color.a = float(((header.z & 0x100u) != 0u ? int(header.z & 255u) : written.a) >> 2) / 63.0;
    }
}
)GLSL";

struct Target {
    GLuint texture = 0;
    GLuint framebuffer = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct CachedTexture {
    GLuint texture = 0;
    bool owned = true;
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t levels = 0;
    uint64_t verified_epoch = ~0ull;
    bool mipmaps = false;
    bool custom = false;
};

struct TexturePack {
    bool dump = false;
    bool load = false;
    std::filesystem::path dump_directory;
    custom_textures::Index index;
    std::unordered_set<std::string> dumped;
    std::unordered_map<std::string, GLuint> loaded;
};

struct CopiedTexture {
    Target converted;
    GLuint view = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes = 0;
    uint64_t guest_hash = 0;
    uint32_t pending_writes = 0;
    uint32_t copied_frame = 0;
    uint32_t copied_format = 0;
};

struct PendingWriteBack {
    GLuint buffer = 0;
    GLsync fence = nullptr;
    uint32_t address = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t row_bytes = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t texture_format = 0;
    uint64_t frame = 0;
};

struct Program {
    GLuint id = 0;
    GLint flip = -1;
    GLint row = -1;
};

struct Device {
    Program tev;
    Program early_tev;
    Program present;
    Program overlay;
    Program copy;
    Program clear;
    GLint clear_color = -1;
    GLint present_adjust = -1;
    GLint clear_depth = -1;
    GLuint tev_layout = 0;
    GLuint empty_layout = 0;
    GLuint vertex_buffer = 0;
    GLuint constants = 0;
    GLuint copy_constants = 0;
    GLuint linear_sampler = 0;
    GLuint point_sampler = 0;
    GLuint efb_color = 0;
    GLuint efb_depth = 0;
    GLuint efb_framebuffer = 0;
    Target frame;
    Target write_back;
    std::vector<GLuint> free_buffers;
    std::vector<PendingWriteBack> write_backs;
    GLuint overlay_texture = 0;
    uint32_t overlay_width = 0;
    uint32_t overlay_height = 0;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    GLuint hud_texture = 0;
    uint32_t hud_width = 0;
    uint32_t hud_height = 0;
    uint64_t hud_version = 0;
    bool hud_visible = false;
    perf::Corner hud_corner = perf::Corner::TopLeft;
    GLuint bar_texture = 0;
    uint32_t bar_width = 0;
    uint32_t bar_height = 0;
    uint64_t bar_version = 0;
    bool clip_control = false;
    Program run;
    Program early_run;
    GLuint run_layout = 0;
    GLuint run_vertices = 0;
    GLuint run_indices = 0;
    GLuint run_rows = 0;
    GLuint run_rows_texture = 0;
    GLuint kept_vertices = 0;
    GLuint kept_indices = 0;
    std::map<uint64_t, GLuint> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    bool failed = false;
    bool ready = false;
};

struct PendingBatch {
    std::vector<ScreenVertex> vertices;
    Constants constants;
    GLuint views[kTextureMaps];
    GLuint samplers[kTextureMaps];
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
    uint32_t scissor_offset = 0;
};

Device g_device;
PendingBatch g_pending;
TexturePack g_pack;
uint32_t g_custom_maps = 0;

void flush_pending();
bool g_log_gx = std::getenv("WP_LOG_GX") != nullptr;
bool g_logged_palette = false;
bool g_logged_copy_format = false;
uint64_t g_frame = 0;
constexpr uint64_t kWriteBackFrames = 2;
constexpr GLuint64 kWaitForever = 0xFFFFFFFFFFFFFFFFull;
uint64_t g_texture_epoch = 0;
int g_scale = 1;
uint32_t g_vertex_cursor = kVertexCapacity;
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;
uint32_t g_write_backs = 0;
double g_write_back_seconds = 0.0;

int scaled(int value) {
    return value * g_scale;
}

GLuint compile_stage(GLenum type, const std::string& source) {
    GLuint shader = gl.CreateShader(type);
    const char* text = source.c_str();
    gl.ShaderSource(shader, 1, &text, nullptr);
    gl.CompileShader(shader);
    GLint ok = 0;
    gl.GetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.GetShaderInfoLog(shader, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        gl.DeleteShader(shader);
        return 0;
    }
    return shader;
}

bool link(Program& program, const std::string& vertex, const std::string& fragment) {
    GLuint vs = compile_stage(GL_VERTEX_SHADER, vertex);
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs) {
        return false;
    }
    GLuint id = gl.CreateProgram();
    gl.AttachShader(id, vs);
    gl.AttachShader(id, fs);
    gl.LinkProgram(id);
    gl.DeleteShader(vs);
    gl.DeleteShader(fs);
    GLint ok = 0;
    gl.GetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        gl.GetProgramInfoLog(id, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader link error: %s\n", log);
        return false;
    }
    program.id = id;
    program.flip = gl.GetUniformLocation(id, "flip");
    return true;
}

void bind_samplers(GLuint program, std::initializer_list<const char*> names) {
    gl.UseProgram(program);
    GLint unit = 0;
    for (const char* name : names) {
        gl.Uniform1i(gl.GetUniformLocation(program, name), unit++);
    }
}

bool has_extension(const char* name) {
    GLint count = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        const char* extension = reinterpret_cast<const char*>(gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (extension && std::strcmp(extension, name) == 0) {
            return true;
        }
    }
    return false;
}

void set_texture_levels(GLuint texture, uint32_t levels) {
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(levels) - 1);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

bool create_render_texture(Target& target, uint32_t width, uint32_t height) {
    gl.GenTextures(1, &target.texture);
    set_texture_levels(target.texture, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenFramebuffers(1, &target.framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    target.width = width;
    target.height = height;
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void release_target(Target& target) {
    if (target.framebuffer) {
        gl.DeleteFramebuffers(1, &target.framebuffer);
    }
    if (target.texture) {
        gl.DeleteTextures(1, &target.texture);
    }
    target = Target{};
}

bool create_efb() {
    gl.GenTextures(1, &g_device.efb_color);
    set_texture_levels(g_device.efb_color, 1);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, scaled(kEfbWidth), scaled(kEfbHeight), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.GenTextures(1, &g_device.efb_depth);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_depth);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, scaled(kEfbWidth), scaled(kEfbHeight), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    gl.GenFramebuffers(1, &g_device.efb_framebuffer);
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_device.efb_framebuffer);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_device.efb_color, 0);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, g_device.efb_depth, 0);
    return gl.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void release_efb() {
    if (g_device.efb_framebuffer) {
        gl.DeleteFramebuffers(1, &g_device.efb_framebuffer);
    }
    GLuint textures[2] = {g_device.efb_color, g_device.efb_depth};
    gl.DeleteTextures(2, textures);
    g_device.efb_framebuffer = 0;
    g_device.efb_color = 0;
    g_device.efb_depth = 0;
}

GLuint make_sampler(GLint min_filter, GLint mag_filter, GLint wrap) {
    GLuint sampler = 0;
    gl.GenSamplers(1, &sampler);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, mag_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, wrap);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, wrap);
    return sampler;
}

bool create_pipeline() {
    GLint major = 0;
    GLint minor = 0;
    gl.GetIntegerv(GL_MAJOR_VERSION, &major);
    gl.GetIntegerv(GL_MINOR_VERSION, &minor);
    int version = major * 10 + minor;
    if (version >= 45 || has_extension("GL_ARB_clip_control")) {
        gl.ClipControl = reinterpret_cast<decltype(gl.ClipControl)>(SDL_GL_GetProcAddress("glClipControl"));
    }
    if (gl.ClipControl) {
        gl.ClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        g_device.clip_control = true;
    }
    std::string fragment = std::string("#version 410 core\n") + kTevFragment;
    if (!link(g_device.tev, kTevVertex, fragment)) {
        return false;
    }
    if (has_extension("GL_ARB_shader_image_load_store")) {
        std::string early = std::string("#version 410 core\n#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n") + kTevFragment;
        if (!link(g_device.early_tev, kTevVertex, early)) {
            return false;
        }
    } else {
        g_device.early_tev = g_device.tev;
    }
    for (Program* program : {&g_device.tev, &g_device.early_tev}) {
        bind_samplers(program->id, {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7"});
        gl.Uniform1i(gl.GetUniformLocation(program->id, "depth_zero_to_one"), g_device.clip_control ? 1 : 0);
        gl.UniformBlockBinding(program->id, gl.GetUniformBlockIndex(program->id, "Constants"), 0);
    }
    if (!link(g_device.present, kFullscreenVertex, kPresentFragment) || !link(g_device.overlay, kFullscreenVertex, kOverlayFragment) ||
        !link(g_device.copy, kFullscreenVertex, kCopyFragment) || !link(g_device.clear, kFullscreenVertex, kClearFragment)) {
        return false;
    }
    bind_samplers(g_device.present.id, {"frame"});
    g_device.present_adjust = gl.GetUniformLocation(g_device.present.id, "adjust");
    bind_samplers(g_device.overlay.id, {"overlay"});
    bind_samplers(g_device.copy.id, {"color_source", "depth_source"});
    gl.UniformBlockBinding(g_device.copy.id, gl.GetUniformBlockIndex(g_device.copy.id, "CopyConstants"), 1);
    g_device.clear_color = gl.GetUniformLocation(g_device.clear.id, "clear_color");
    g_device.clear_depth = gl.GetUniformLocation(g_device.clear.id, "clear_depth");
    gl.GenVertexArrays(1, &g_device.empty_layout);
    gl.GenVertexArrays(1, &g_device.tev_layout);
    gl.GenBuffers(1, &g_device.vertex_buffer);
    gl.BindVertexArray(g_device.tev_layout);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.vertex_buffer);
    gl.BufferData(GL_ARRAY_BUFFER, kVertexCapacity * sizeof(ScreenVertex), nullptr, GL_STREAM_DRAW);
    auto attribute = [](GLuint index, GLint size, size_t offset) {
        gl.EnableVertexAttribArray(index);
        gl.VertexAttribPointer(index, size, GL_FLOAT, GL_FALSE, sizeof(ScreenVertex), reinterpret_cast<const void*>(offset));
    };
    attribute(0, 4, 0);
    attribute(1, 4, 16);
    attribute(2, 4, 32);
    for (GLuint i = 0; i < 8; i++) {
        attribute(3 + i, 3, 48 + 12 * i);
    }
    gl.GenBuffers(1, &g_device.constants);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof(Constants), nullptr, GL_STREAM_DRAW);
    gl.GenBuffers(1, &g_device.copy_constants);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.copy_constants);
    gl.BufferData(GL_UNIFORM_BUFFER, 80, nullptr, GL_STREAM_DRAW);
    g_device.linear_sampler = make_sampler(GL_LINEAR, GL_LINEAR, GL_CLAMP_TO_EDGE);
    g_device.point_sampler = make_sampler(GL_NEAREST, GL_NEAREST, GL_CLAMP_TO_EDGE);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.Enable(GL_DEPTH_CLAMP);
    return true;
}

constexpr uint32_t kRunVertexBytes = 16u << 20;
constexpr uint32_t kRunIndexBytes = 4u << 20;
constexpr uint32_t kRunRowCapacity = 1u << 16;
constexpr uint32_t kKeptVertexBytes = 64u << 20;
constexpr uint32_t kKeptIndexBytes = 16u << 20;
constexpr GLuint kRowsUnit = kTextureMaps;

void clear_errors() {
    for (int i = 0; i < 16 && gl.GetError() != GL_NO_ERROR; i++) {
    }
}

bool link_run(Program& program, const std::string& fragment) {
    if (!link(program, kRunVertex, fragment)) {
        return false;
    }
    bind_samplers(program.id, {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "rows"});
    gl.Uniform1i(gl.GetUniformLocation(program.id, "depth_zero_to_one"), g_device.clip_control ? 1 : 0);
    gl.UniformBlockBinding(program.id, gl.GetUniformBlockIndex(program.id, "Constants"), 0);
    program.row = gl.GetUniformLocation(program.id, "run_row");
    return program.row >= 0;
}

void allocate(GLuint buffer, uint32_t bytes, GLenum usage) {
    gl.BindBuffer(GL_ARRAY_BUFFER, buffer);
    gl.BufferData(GL_ARRAY_BUFFER, bytes, nullptr, usage);
}

bool create_run_pipeline() {
    static bool tried = false;
    if (tried) {
        return g_device.run.id != 0;
    }
    tried = true;
    if (!load_run_functions()) {
        log::write("video", "vertices stay on the processor: this OpenGL lacks a function the vertex shader needs");
        return false;
    }
    GLint texels = 0;
    GLint attributes = 0;
    gl.GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
    gl.GetIntegerv(GL_MAX_VERTEX_ATTRIBS, &attributes);
    if (texels < static_cast<GLint>(kRunRowCapacity) || attributes < 15) {
        log::write("video", "vertices stay on the processor: this OpenGL has %d buffer texels and %d vertex inputs", texels, attributes);
        return false;
    }
    clear_errors();
    Program run;
    Program early;
    bool ok = link_run(run, std::string("#version 410 core\n") + kTevFragment);
    if (ok && g_device.early_tev.id != g_device.tev.id) {
        ok = link_run(early, std::string("#version 410 core\n#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n") + kTevFragment);
    } else {
        early = run;
    }
    if (ok) {
        gl.GenVertexArrays(1, &g_device.run_layout);
        GLuint buffers[3] = {};
        gl.GenBuffers(3, buffers);
        g_device.run_vertices = buffers[0];
        g_device.run_indices = buffers[1];
        g_device.run_rows = buffers[2];
        allocate(g_device.run_vertices, kRunVertexBytes, GL_STREAM_DRAW);
        allocate(g_device.run_indices, kRunIndexBytes, GL_STREAM_DRAW);
        allocate(g_device.run_rows, kRunRowCapacity * sizeof(float[4]), GL_STREAM_DRAW);
        gl.GenTextures(1, &g_device.run_rows_texture);
        gl.ActiveTexture(GL_TEXTURE0 + kRowsUnit);
        gl.BindTexture(GL_TEXTURE_BUFFER, g_device.run_rows_texture);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, g_device.run_rows);
        gl.ActiveTexture(GL_TEXTURE0);
        ok = gl.GetError() == GL_NO_ERROR;
    }
    if (!ok) {
        log::write("video", "vertices stay on the processor: the graphics card refused the vertex shader or its buffers");
        return false;
    }
    g_device.run = run;
    g_device.early_run = early;
    log::write("video", "the graphics card places the vertices");
    return true;
}

constexpr GLenum kCompletionStatus = 0x91B1;
const char* const kEarlyPrefix = "#extension GL_ARB_shader_image_load_store : require\nlayout(early_fragment_tests) in;\n";

struct Variant {
    Program program;
    GLuint linking = 0;
    bool done = false;
};

std::unordered_map<std::string, Variant> g_variants[2];
GLuint g_variant_vertex[2] = {};
uint32_t g_variant_budget = 0;
uint32_t g_variants_linking = 0;
constexpr uint32_t kMaxLinking = 4;
uint32_t g_variants_ready = 0;
uint32_t g_variants_failed = 0;
double g_variant_seconds = 0.0;

bool parallel_compile() {
    static const bool available = [] {
        using MaxThreads = void(APIENTRY*)(GLuint);
        for (const char* name : {"KHR", "ARB"}) {
            std::string extension = std::string("GL_") + name + "_parallel_shader_compile";
            auto function = reinterpret_cast<MaxThreads>(SDL_GL_GetProcAddress((std::string("glMaxShaderCompilerThreads") + name).c_str()));
            if (has_extension(extension.c_str()) && function) {
                function(0xFFFFFFFFu);
                log::write("video", "pixel shader variants compile in the background (%s)", extension.c_str());
                return true;
            }
        }
        log::write("video", "pixel shader variants compile one per image: this OpenGL cannot compile in the background");
        return false;
    }();
    return available;
}

std::string variant_fragment(const std::string& key) {
    std::vector<uint32_t> words(key.size() / sizeof(uint32_t));
    std::memcpy(words.data(), key.data(), key.size());
    uint32_t count = words[0];
    std::string source = kTevFragment;
    auto replace = [&source](const char* from, const std::string& to) {
        size_t at = source.find(from);
        if (at != std::string::npos) {
            source.replace(at, std::strlen(from), to);
        }
    };
    auto vector = [](const uint32_t* v) {
        char text[96];
        std::snprintf(text, sizeof text, "uvec4(%uu, %uu, %uu, %uu)", v[0], v[1], v[2], v[3]);
        return std::string(text);
    };
    std::string stages = "const uvec4 stage[16] = uvec4[16](";
    std::string indirects = "const uvec4 tevind[16] = uvec4[16](";
    const uint32_t zero[4] = {};
    for (uint32_t i = 0; i < kMaxStages; i++) {
        const uint32_t* stage = i < count ? &words[kVariantHeader + i * 5] : zero;
        const uint32_t indirect[4] = {i < count ? words[kVariantHeader + i * 5 + 4] : 0, 0, 0, 0};
        stages += vector(stage) + (i + 1 < kMaxStages ? ", " : ");\n");
        indirects += vector(indirect) + (i + 1 < kMaxStages ? ", " : ");\n");
    }
    replace("    uvec4 stage[16];", "    uvec4 stage_slot[16];");
    replace("    uvec4 swaps;", "    uvec4 swaps_slot;");
    replace("    uvec4 indirect;", "    uvec4 indirect_slot;");
    replace("    uvec4 tevind[16];", "    uvec4 tevind_slot[16];");
    replace("uniform sampler2D t0;", stages + indirects + "const uvec4 swaps = " + vector(&words[2]) + ";\nconst uvec4 indirect = " + vector(&words[6]) +
                                         ";\nuniform sampler2D t0;");
    replace("uint count = header.x;", "const uint count = " + std::to_string(count) + "u;");
    return std::string("#version 410 core\n") + (words[1] ? kEarlyPrefix : "") + source;
}

void start_variant(Variant& variant, int kind, const std::string& key) {
    if (!g_variant_vertex[kind]) {
        g_variant_vertex[kind] = compile_stage(GL_VERTEX_SHADER, kind ? kRunVertex : kTevVertex);
    }
    std::string fragment = variant_fragment(key);
    const char* text = fragment.c_str();
    GLuint fs = gl.CreateShader(GL_FRAGMENT_SHADER);
    gl.ShaderSource(fs, 1, &text, nullptr);
    gl.CompileShader(fs);
    GLuint id = gl.CreateProgram();
    gl.AttachShader(id, g_variant_vertex[kind]);
    gl.AttachShader(id, fs);
    gl.LinkProgram(id);
    gl.DeleteShader(fs);
    variant.linking = id;
    g_variants_linking++;
}

void finish_variant(Variant& variant, int kind) {
    GLuint id = variant.linking;
    variant.linking = 0;
    g_variants_linking--;
    variant.done = true;
    GLint ok = 0;
    gl.GetProgramiv(id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char text[4096];
        gl.GetProgramInfoLog(id, sizeof text, nullptr, text);
        log::write("video", "a pixel shader variant did not build, the generic shader keeps drawing it: %s", text);
        gl.DeleteProgram(id);
        g_variants_failed++;
        return;
    }
    if (kind) {
        bind_samplers(id, {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "rows"});
        variant.program.row = gl.GetUniformLocation(id, "run_row");
    } else {
        bind_samplers(id, {"t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7"});
    }
    gl.Uniform1i(gl.GetUniformLocation(id, "depth_zero_to_one"), g_device.clip_control ? 1 : 0);
    gl.UniformBlockBinding(id, gl.GetUniformBlockIndex(id, "Constants"), 0);
    variant.program.id = id;
    g_variants_ready++;
}

const Program& tev_program(const Constants& constants, bool early_depth, int kind) {
    const Program& generic = kind ? (early_depth ? g_device.early_run : g_device.run) : (early_depth ? g_device.early_tev : g_device.tev);
    static const bool off = std::getenv("WP_GENERIC_SHADER") != nullptr;
    if (off) {
        return generic;
    }
    bool early = early_depth && g_device.early_tev.id != g_device.tev.id;
    std::string key = variant_key(constants, early);
    Variant& variant = g_variants[kind][key];
    if (variant.program.id) {
        return variant.program;
    }
    if (variant.done) {
        return generic;
    }
    bool parallel = parallel_compile();
    auto started = std::chrono::steady_clock::now();
    if (!variant.linking) {
        if (parallel ? g_variants_linking >= kMaxLinking : g_variant_budget == 0) {
            return generic;
        }
        g_variant_budget -= parallel ? 0 : 1;
        start_variant(variant, kind, key);
    }
    GLint complete = 1;
    if (parallel) {
        gl.GetProgramiv(variant.linking, kCompletionStatus, &complete);
    }
    if (complete) {
        finish_variant(variant, kind);
    }
    g_variant_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return variant.program.id ? variant.program : generic;
}

bool create_kept_buffers() {
    static bool tried = false;
    if (tried) {
        return g_device.kept_indices != 0;
    }
    tried = true;
    clear_errors();
    GLuint buffers[2] = {};
    gl.GenBuffers(2, buffers);
    allocate(buffers[0], kKeptVertexBytes, GL_STATIC_DRAW);
    allocate(buffers[1], kKeptIndexBytes, GL_STATIC_DRAW);
    if (gl.GetError() != GL_NO_ERROR) {
        gl.DeleteBuffers(2, buffers);
        log::write("video", "models are decoded on every image: the graphics card refused the buffers that would keep them");
        return false;
    }
    g_device.kept_vertices = buffers[0];
    g_device.kept_indices = buffers[1];
    return true;
}

void prepare_texture_pack() {
    const char* directory = game::description().textures_directory;
    if (!directory || !*directory) {
        return;
    }
    std::filesystem::path base = std::filesystem::path(directory);
    g_pack.dump = settings::flag("video.dump_textures", "WP_DUMP_TEXTURES");
    g_pack.dump_directory = base / "dump";
    if (settings::flag("video.custom_textures", "WP_CUSTOM_TEXTURES")) {
        size_t count = g_pack.index.scan(base / "load");
        g_pack.load = count > 0;
        if (count > 0) {
            std::fprintf(stderr, "custom textures: %zu files in %s/load\n", count, directory);
        }
    }
    if (g_pack.dump) {
        std::fprintf(stderr, "dumping textures to %s/dump\n", directory);
    }
}

int requested_scale() {
    static const settings::LiveNumber scale("video.scale", "WP_SCALE");
    return std::clamp(scale(), 1, 6);
}

void APIENTRY report_gl_message(GLenum, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message, const void*) {
    static std::unordered_set<GLuint> reported;
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || reported.size() >= 200 || !reported.insert(id).second) {
        return;
    }
    log::write("gl", "type 0x%x severity 0x%x id %u: %s", type, severity, id, message);
}

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    g_scale = requested_scale();
    if (!video::make_gl_current() || !load_functions() || !create_pipeline() || !create_efb()) {
        std::fputs("cannot initialise the OpenGL renderer\n", stderr);
        g_device.failed = true;
        return false;
    }
    std::fprintf(stderr, "OpenGL renderer: %s, %s\n", reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)),
                 reinterpret_cast<const char*>(gl.GetString(GL_VERSION)));
    log::write("video", "OpenGL renderer: %s, %s", reinterpret_cast<const char*>(gl.GetString(GL_RENDERER)),
               reinterpret_cast<const char*>(gl.GetString(GL_VERSION)));
    if (std::getenv("WP_GL_DEBUG")) {
        using DebugCallbackSetter = void (*)(GLDEBUGPROC, const void*);
        DebugCallbackSetter set_callback = reinterpret_cast<DebugCallbackSetter>(SDL_GL_GetProcAddress("glDebugMessageCallback"));
        if (!set_callback) {
            set_callback = reinterpret_cast<DebugCallbackSetter>(SDL_GL_GetProcAddress("glDebugMessageCallbackKHR"));
        }
        if (set_callback) {
            gl.Enable(GL_DEBUG_OUTPUT);
            gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            set_callback(report_gl_message, nullptr);
        } else {
            log::write("video", "this OpenGL driver has no debug message callback");
        }
    }
    prepare_texture_pack();
    g_device.ready = true;
    return true;
}

std::string g_copy_dump_prefix;
int g_copy_dump_index = 0;
FILE* g_texture_notes = nullptr;
std::map<uint64_t, bool> g_noted_textures;

void note_texture(uint32_t map, uint32_t address, uint32_t width, uint32_t height, uint32_t format, const char* source) {
    if (!g_texture_notes) {
        return;
    }
    uint64_t key = (static_cast<uint64_t>(address) << 24) ^ (static_cast<uint64_t>(width) << 12) ^ height ^ (static_cast<uint64_t>(format) << 56) ^
                   (static_cast<uint64_t>(map) << 60);
    if (g_noted_textures[key]) {
        return;
    }
    g_noted_textures[key] = true;
    std::fprintf(g_texture_notes, "map %u address %08x %ux%u format %u: %s\n", map, address, width, height, format, source);
}

GLuint upload_levels(const std::vector<std::vector<uint32_t>>& images, uint32_t width, uint32_t height, bool generate) {
    GLuint texture = 0;
    gl.GenTextures(1, &texture);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    for (size_t level = 0; level < images.size(); level++) {
        gl.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8, static_cast<GLsizei>(std::max(width >> level, 1u)),
                      static_cast<GLsizei>(std::max(height >> level, 1u)), 0, GL_RGBA, GL_UNSIGNED_BYTE, images[level].data());
    }
    if (generate) {
        uint32_t levels = 1;
        while ((width >> levels) != 0 || (height >> levels) != 0) {
            levels++;
        }
        set_texture_levels(texture, levels);
        gl.GenerateMipmap(GL_TEXTURE_2D);
    } else {
        set_texture_levels(texture, static_cast<uint32_t>(images.size()));
    }
    return texture;
}

GLuint custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key, 0);
    if (!inserted) {
        return found->second;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return 0;
    }
    if (mipmaps) {
        for (uint32_t level = 1; (width >> level) != 0 || (height >> level) != 0; level++) {
            const std::filesystem::path* level_path = g_pack.index.find(custom_textures::level_name(key, level));
            std::vector<uint32_t> pixels;
            uint32_t level_width = 0;
            uint32_t level_height = 0;
            if (!level_path || !custom_textures::decode_png(*level_path, pixels, level_width, level_height) ||
                level_width != std::max(width >> level, 1u) || level_height != std::max(height >> level, 1u)) {
                break;
            }
            images.push_back(std::move(pixels));
        }
    }
    bool generate = mipmaps && images.size() == 1 && (width > 1 || height > 1);
    found->second = upload_levels(images, width, height, generate);
    return found->second;
}

void dump_texture(const custom_textures::TextureName& name, const std::vector<std::vector<uint32_t>>& pixels, uint32_t width, uint32_t height) {
    std::string full = name.full();
    if (!g_pack.dumped.insert(full).second) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories(g_pack.dump_directory, error);
    for (uint32_t level = 0; level < pixels.size(); level++) {
        std::filesystem::path path = g_pack.dump_directory / (custom_textures::level_name(full, level) + ".png");
        if (!std::filesystem::exists(path, error)) {
            video::save_png_rgba(path.string().c_str(), pixels[level], std::max(width >> level, 1u), std::max(height >> level, 1u));
        }
    }
}

void release_cached(CachedTexture& entry) {
    if (entry.texture && entry.owned) {
        gl.DeleteTextures(1, &entry.texture);
    }
    entry.texture = 0;
}

GLuint texture_for(uint32_t map) {
    g_custom_maps &= ~(1u << map);
    TextureRequest request = describe_texture(map);
    if (!request.supported && !g_logged_palette) {
        g_logged_palette = true;
        std::fprintf(stderr, "unsupported texture format %u\n", request.format);
    }
    if (!request.valid) {
        return 0;
    }
    uint32_t address = request.address;
    uint32_t width = request.width;
    uint32_t height = request.height;
    uint32_t format = request.format;
    uint32_t levels = request.levels;
    const uint8_t* source = request.source;
    auto copied = g_device.copies.find(address);
    if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && copied->second.logical_width == width &&
        copied->second.logical_height == height && format <= 6) {
        note_texture(map, address, width, height, format, "EFB copy");
        return copied->second.view;
    }
    if (!g_device.write_backs.empty() && write_back_overlaps(address, request.size)) {
        finish_write_backs();
    }
    if (copied != g_device.copies.end() && copied->second.logical_width == width && copied->second.logical_height == height && format <= 6 &&
        copied->second.guest_hash == sample_hash(source, copied->second.bytes)) {
        note_texture(map, address, width, height, format, "EFB copy");
        return copied->second.view;
    }
    if (copied != g_device.copies.end()) {
        char reason[160];
        std::snprintf(reason, sizeof reason, "EFB copy rejected (copy %ux%u, hash %s), read from RAM", copied->second.logical_width, copied->second.logical_height,
                      copied->second.guest_hash == sample_hash(source, copied->second.bytes) ? "same" : "changed");
        note_texture(map, address, width, height, format, reason);
    } else {
        note_texture(map, address, width, height, format, "RAM");
    }
    bool mipmaps = request.mipmaps;
    CachedTexture& entry = g_device.textures[request.key];
    auto use = [&](CachedTexture& texture) {
        if (texture.custom) {
            g_custom_maps |= 1u << map;
        }
        return texture.texture;
    };
    auto matches = [&]() {
        return entry.texture && entry.width == width && entry.height == height && entry.format == format && entry.levels == levels && entry.mipmaps == mipmaps;
    };
    if (matches() && entry.verified_epoch == g_texture_epoch) {
        return use(entry);
    }
    uint64_t hash = texture_hash(request);
    if (matches() && entry.hash == hash) {
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    }
    flush_pending();
    release_cached(entry);
    entry.custom = false;
    entry.owned = true;
    auto finish = [&]() {
        entry.hash = hash;
        entry.width = width;
        entry.height = height;
        entry.format = format;
        entry.levels = levels;
        entry.mipmaps = mipmaps;
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    };
    bool named = request.supported && copied == g_device.copies.end() && (g_pack.dump || g_pack.load);
    custom_textures::TextureName name;
    GLuint custom = 0;
    if (named) {
        name = custom_textures::name_texture(source, request.size, width, height, format, mipmaps, request.tlut);
        if (g_pack.load) {
            std::string resolved = g_pack.index.resolve(name);
            if (!resolved.empty()) {
                custom = custom_texture(resolved, mipmaps);
            }
        }
    }
    if (custom && !g_pack.dump) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    std::vector<std::vector<uint32_t>> pixels = decode_levels(request);
    if (named && g_pack.dump) {
        dump_texture(name, pixels, width, height);
    }
    if (custom) {
        entry.texture = custom;
        entry.owned = false;
        entry.custom = true;
        return finish();
    }
    pixels.resize(levels);
    entry.texture = upload_levels(pixels, width, height, false);
    return finish();
}

GLint gl_wrap(uint32_t wrap) {
    return wrap == 1 ? GL_REPEAT : wrap == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE;
}

GLuint sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return found->second;
    }
    GLint min_filter = params.mip_mode == 2 ? (params.min_linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR)
                                            : (params.min_linear ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST);
    GLuint sampler = 0;
    gl.GenSamplers(1, &sampler);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, min_filter);
    gl.SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, params.mag_linear ? GL_LINEAR : GL_NEAREST);
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, gl_wrap(params.wrap_s));
    gl.SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, gl_wrap(params.wrap_t));
    gl.SamplerParameterf(sampler, GL_TEXTURE_MIN_LOD, params.min_lod);
    gl.SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, params.unlimited_lod ? 1000.0f : params.max_lod);
    gl.SamplerParameterf(sampler, GL_TEXTURE_LOD_BIAS, params.lod_bias);
    g_device.samplers[params.key] = sampler;
    return sampler;
}

GLenum gl_factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SourceColor:
        return GL_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return GL_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return GL_DST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return GL_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestinationAlpha:
        return GL_DST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return GL_ONE_MINUS_DST_ALPHA;
    case BlendFactor::TevColor:
        return GL_SRC1_COLOR;
    case BlendFactor::TevAlpha:
        return GL_SRC1_ALPHA;
    case BlendFactor::InverseTevAlpha:
        return GL_ONE_MINUS_SRC1_ALPHA;
    }
    return GL_ONE;
}

GLenum gl_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return GL_FUNC_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return GL_FUNC_REVERSE_SUBTRACT;
    default:
        return GL_FUNC_ADD;
    }
}

void apply_blend(uint32_t word) {
    static std::map<uint32_t, BlendState> cache;
    auto found = cache.find(word);
    if (found == cache.end()) {
        BlendState state = blend_state_for(word);
        if (state.logic_op && g_log_gx) {
            std::fprintf(stderr, "GX logic op %s (%u), %s, blend mode %06x\n", logic_op_name(state.logic_mode), state.logic_mode,
                         logic_op_exact(state.logic_mode) ? "exact" : "approximated with blending", word);
        }
        found = cache.emplace(word, state).first;
    }
    const BlendState& blend = found->second;
    if (blend.enable) {
        gl.Enable(GL_BLEND);
        gl.BlendFuncSeparate(gl_factor(blend.source), gl_factor(blend.destination), gl_factor(blend.source_alpha), gl_factor(blend.destination_alpha));
        gl.BlendEquationSeparate(gl_operation(blend.operation), gl_operation(blend.alpha_operation));
    } else {
        gl.Disable(GL_BLEND);
    }
    gl.ColorMask(blend.color_update, blend.color_update, blend.color_update, blend.alpha_update);
}

void apply_depth(uint32_t word) {
    if (word & 1) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_NEVER + ((word >> 1) & 7));
        gl.DepthMask((word & (1u << 4)) ? GL_TRUE : GL_FALSE);
    } else {
        gl.Disable(GL_DEPTH_TEST);
    }
}

void bind_efb() {
    gl.BindFramebuffer(GL_FRAMEBUFFER, g_device.efb_framebuffer);
    gl.Viewport(0, 0, scaled(kEfbWidth), scaled(kEfbHeight));
}

constexpr uint32_t kNoPalette = ~0u;

struct RunDraw {
    uint32_t layout = 0;
    uint32_t stride = 0;
    uint32_t cull = 0;
    uint32_t vertex_offset = 0;
    uint32_t first_index = 0;
    uint32_t indices = 0;
    uint32_t row = 0;
    bool kept = false;
};

std::vector<RunDraw> g_run_draws;
std::vector<uint8_t> g_run_vertex_bytes;
std::vector<uint16_t> g_run_index_words;
std::vector<std::array<float, 4>> g_run_row_values;
uint32_t g_run_vertex_used = 0;
uint32_t g_run_index_used = 0;
uint32_t g_run_row_used = 0;
uint32_t g_run_palette = kNoPalette;
uint64_t g_run_palette_serial = 0;
uint32_t g_run_vertex_cursor = kRunVertexBytes;
uint32_t g_run_index_cursor = kRunIndexBytes;
uint32_t g_run_row_cursor = kRunRowCapacity * sizeof(float[4]);
uint32_t g_run_kept = 0;
uint32_t g_run_kept_indices = 0;
uint32_t g_kept_vertex_used = 0;
uint32_t g_kept_index_used = 0;
uint32_t g_kept_generation = 1;

bool batch_empty() {
    return g_pending.vertices.empty() && g_run_draws.empty();
}

template <typename T>
void reserve_run(std::vector<T>& values, size_t wanted) {
    if (values.size() < wanted) {
        values.resize(std::max(wanted, values.size() * 2));
    }
}

bool upload_run(GLuint buffer, uint32_t& cursor, uint32_t capacity, const void* data, uint32_t size, uint32_t& start) {
    if (size == 0) {
        return true;
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, buffer);
    bool orphaned = false;
    if (cursor + size > capacity) {
        gl.BufferData(GL_ARRAY_BUFFER, capacity, nullptr, GL_STREAM_DRAW);
        cursor = 0;
        orphaned = true;
    }
    void* mapped = gl.MapBufferRange(GL_ARRAY_BUFFER, cursor, size, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
    if (!mapped) {
        return false;
    }
    std::memcpy(mapped, data, size);
    if (!gl.UnmapBuffer(GL_ARRAY_BUFFER)) {
        return false;
    }
    if (orphaned && buffer == g_device.run_rows) {
        gl.ActiveTexture(GL_TEXTURE0 + kRowsUnit);
        gl.BindTexture(GL_TEXTURE_BUFFER, g_device.run_rows_texture);
        gl.TexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, g_device.run_rows);
    }
    start = cursor;
    cursor += size;
    return true;
}

const void* buffer_offset(size_t bytes) {
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(bytes));
}

void point_run_inputs(uint32_t layout, uint32_t stride, GLuint buffer, uint32_t base) {
    bool normal = (layout & 1) != 0;
    bool indices = (layout & 2) != 0;
    uint32_t inputs = layout >> 2;
    GLsizei step = static_cast<GLsizei>(stride);
    gl.BindBuffer(GL_ARRAY_BUFFER, buffer);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, step, buffer_offset(base));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, step, buffer_offset(base + 12));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, step, buffer_offset(base + 16));
    uint32_t offset = base + 20;
    if (normal) {
        gl.EnableVertexAttribArray(3);
        gl.VertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, step, buffer_offset(offset));
        offset += 12;
    } else {
        gl.DisableVertexAttribArray(3);
        gl.VertexAttrib4f(3, 0.0f, 0.0f, 0.0f, 1.0f);
    }
    for (GLuint i = 0; i < 3; i++) {
        if (indices) {
            gl.EnableVertexAttribArray(4 + i);
            gl.VertexAttribIPointer(4 + i, 4, GL_UNSIGNED_BYTE, step, buffer_offset(offset + 4 * i));
        } else {
            gl.DisableVertexAttribArray(4 + i);
            gl.VertexAttribI4ui(4 + i, 0, 0, 0, 0);
        }
    }
    offset += indices ? 12 : 0;
    for (GLuint i = 0; i < 8; i++) {
        if (i < inputs) {
            gl.EnableVertexAttribArray(7 + i);
            gl.VertexAttribPointer(7 + i, 2, GL_FLOAT, GL_FALSE, step, buffer_offset(offset + 8 * i));
        } else {
            gl.DisableVertexAttribArray(7 + i);
            gl.VertexAttrib4f(7 + i, 0.0f, 0.0f, 0.0f, 1.0f);
        }
    }
}

void issue_runs(bool early_depth, uint32_t vertex_start, uint32_t index_start, uint32_t row_start) {
    const Program& program = tev_program(g_pending.constants, early_depth, 1);
    gl.UseProgram(program.id);
    gl.ActiveTexture(GL_TEXTURE0 + kRowsUnit);
    gl.BindTexture(GL_TEXTURE_BUFFER, g_device.run_rows_texture);
    gl.BindVertexArray(g_device.run_layout);
    const RunDraw* group = nullptr;
    GLuint index_buffer = 0;
    uint32_t cull = 0;
    for (const RunDraw& draw : g_run_draws) {
        if (!group || group->layout != draw.layout || group->stride != draw.stride || group->kept != draw.kept) {
            group = &draw;
            point_run_inputs(draw.layout, draw.stride, draw.kept ? g_device.kept_vertices : g_device.run_vertices, draw.kept ? 0 : vertex_start + draw.vertex_offset);
            GLuint wanted = draw.kept ? g_device.kept_indices : g_device.run_indices;
            if (index_buffer != wanted) {
                index_buffer = wanted;
                gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, wanted);
            }
        }
        if (draw.cull != cull) {
            if (cull == 0) {
                gl.Enable(GL_CULL_FACE);
            }
            cull = draw.cull;
            if (cull == 0) {
                gl.Disable(GL_CULL_FACE);
            } else {
                gl.CullFace(cull == 1 ? GL_BACK : GL_FRONT);
            }
        }
        uint32_t first_index = draw.kept ? draw.first_index : index_start + draw.first_index;
        uint32_t base_vertex = draw.kept ? draw.vertex_offset / draw.stride : (draw.vertex_offset - group->vertex_offset) / draw.stride;
        gl.Uniform1i(program.row, static_cast<GLint>(row_start + draw.row));
        gl.DrawElementsBaseVertex(GL_TRIANGLES, static_cast<GLsizei>(draw.indices), GL_UNSIGNED_SHORT, buffer_offset(static_cast<size_t>(first_index) * sizeof(uint16_t)),
                                  static_cast<GLint>(base_vertex));
    }
    if (cull != 0) {
        gl.Disable(GL_CULL_FACE);
    }
}

void set_batch_state(const PendingBatch& pending) {
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof(Constants), &pending.constants, GL_STREAM_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 0, g_device.constants);
    bind_efb();
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        gl.ActiveTexture(GL_TEXTURE0 + map);
        gl.BindTexture(GL_TEXTURE_2D, pending.views[map]);
        gl.BindSampler(map, pending.samplers[map]);
    }
    int left = static_cast<int>(((pending.top_left >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset));
    int top = static_cast<int>((pending.top_left & 0x7FF) - scissor_offset_y(pending.scissor_offset));
    int right = static_cast<int>(((pending.bottom_right >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset) + 1);
    int bottom = static_cast<int>((pending.bottom_right & 0x7FF) - scissor_offset_y(pending.scissor_offset) + 1);
    left = std::max(left, 0);
    top = std::max(top, 0);
    right = std::min(right, kEfbWidth);
    bottom = std::min(bottom, kEfbHeight);
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(scaled(left), scaled(top), std::max(0, scaled(right - left)), std::max(0, scaled(bottom - top)));
    apply_blend(pending.blend);
    apply_depth(pending.depth);
}

void draw_runs() {
    uint32_t vertex_start = 0;
    uint32_t index_start = 0;
    uint32_t row_start = 0;
    bool ok = upload_run(g_device.run_rows, g_run_row_cursor, kRunRowCapacity * sizeof(float[4]), g_run_row_values.data(), g_run_row_used * sizeof(float[4]), row_start) &&
              upload_run(g_device.run_vertices, g_run_vertex_cursor, kRunVertexBytes, g_run_vertex_bytes.data(), g_run_vertex_used, vertex_start) &&
              upload_run(g_device.run_indices, g_run_index_cursor, kRunIndexBytes, g_run_index_words.data(), g_run_index_used * sizeof(uint16_t), index_start);
    if (ok) {
        issue_runs(g_pending.early_depth, vertex_start, index_start / static_cast<uint32_t>(sizeof(uint16_t)), row_start / static_cast<uint32_t>(sizeof(float[4])));
    }
    g_run_draws.clear();
    g_run_kept = 0;
    g_run_kept_indices = 0;
    g_run_vertex_used = 0;
    g_run_index_used = 0;
    g_run_row_used = 0;
    g_run_palette = kNoPalette;
}

void flush_pending() {
    PendingBatch& pending = g_pending;
    if (!g_run_draws.empty()) {
        auto started = std::chrono::steady_clock::now();
        g_batches++;
        g_batch_vertices += g_run_index_used + g_run_kept_indices;
        set_batch_state(pending);
        draw_runs();
        g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return;
    }
    if (pending.vertices.empty()) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    g_batches++;
    g_batch_vertices += static_cast<uint32_t>(pending.vertices.size());
    set_batch_state(pending);
    gl.UseProgram(tev_program(pending.constants, pending.early_depth, 0).id);
    gl.BindVertexArray(g_device.tev_layout);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.vertex_buffer);
    size_t total = pending.vertices.size();
    size_t offset = 0;
    while (offset < total) {
        uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexCapacity));
        if (g_vertex_cursor + batch > kVertexCapacity) {
            gl.BufferData(GL_ARRAY_BUFFER, kVertexCapacity * sizeof(ScreenVertex), nullptr, GL_STREAM_DRAW);
            g_vertex_cursor = 0;
        }
        gl.BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(g_vertex_cursor) * sizeof(ScreenVertex), batch * sizeof(ScreenVertex),
                         pending.vertices.data() + offset);
        gl.DrawArrays(GL_TRIANGLES, static_cast<GLint>(g_vertex_cursor), static_cast<GLsizei>(batch));
        g_vertex_cursor += batch;
        offset += batch;
    }
    pending.vertices.clear();
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void plain_state() {
    gl.Disable(GL_BLEND);
    gl.Disable(GL_DEPTH_TEST);
    gl.Disable(GL_SCISSOR_TEST);
    gl.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    gl.BindVertexArray(g_device.empty_layout);
}

void use_fullscreen(const Program& program, float flip) {
    gl.UseProgram(program.id);
    gl.Uniform1f(program.flip, flip);
}

}

const char* api_name() {
    return "OpenGL";
}

bool interpolating() {
    return false;
}

namespace {

void select_batch() {
    // Most tiny draws only change a matrix: with no register the batch depends on written, join the pending batch as is.
    static uint64_t batched_serial = ~0ull;
    static uint64_t batched_epoch = 0;
    static int batched_scale = 0;
    PendingBatch& pending = g_pending;
    if (!batch_empty() && batched_serial == register_serial() && batched_epoch == g_texture_epoch && batched_scale == g_scale) {
        return;
    }
    const uint32_t* bp = bp_registers();
    Constants constants;
    fill_constants(constants, g_scale);
    GLuint views[kTextureMaps] = {};
    GLuint samplers[kTextureMaps] = {};
    uint32_t used = used_maps(constants);
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        if (used & (1u << map)) {
            views[map] = texture_for(map);
            samplers[map] = sampler_for(map);
        }
    }
    uint32_t pixel_format = bp[0x43] & 7;
    AlphaTestResult alpha_test = alpha_test_result(bp[0xF3]);
    uint32_t blend = (bp[0x41] & 0xFFFF) | (static_cast<uint32_t>(pixel_format == 1) << 16) | (((bp[0x42] >> 8) & 1) << 17) |
                     (static_cast<uint32_t>(!alpha_test.can_pass) << 18);
    uint32_t depth = bp[0x40] & 0x1F;
    bool early_depth = (depth & 1) != 0 && (bp[0x43] & (1u << 6)) != 0 && alpha_test.can_pass && alpha_test.can_fail;
    bool same = !batch_empty() && pending.blend == blend && pending.depth == depth && pending.early_depth == early_depth && pending.top_left == bp[0x20] &&
                pending.bottom_right == bp[0x21] && pending.scissor_offset == bp[kBpScissorOffset] && std::memcmp(&pending.constants, &constants, sizeof constants) == 0 &&
                std::memcmp(pending.views, views, sizeof views) == 0 && std::memcmp(pending.samplers, samplers, sizeof samplers) == 0;
    if (!same) {
        flush_pending();
        pending.constants = constants;
        std::memcpy(pending.views, views, sizeof views);
        std::memcpy(pending.samplers, samplers, sizeof samplers);
        pending.blend = blend;
        pending.depth = depth;
        pending.early_depth = early_depth;
        pending.top_left = bp[0x20];
        pending.bottom_right = bp[0x21];
        pending.scissor_offset = bp[kBpScissorOffset];
    }
    batched_serial = register_serial();
    batched_epoch = g_texture_epoch;
    batched_scale = g_scale;
}

bool add_run(const VertexRun& run, uint32_t bytes, uint32_t index_count) {
    uint32_t rows = run.row_count + (run.palette ? kPaletteRows + kNormalRows : 0);
    if (!g_pending.vertices.empty() || g_run_vertex_used + bytes > kRunVertexBytes / 2 || (g_run_index_used + index_count) * sizeof(uint16_t) > kRunIndexBytes / 2 ||
        g_run_row_used + rows > kRunRowCapacity / 2) {
        flush_pending();
    }
    select_batch();
    reserve_run(g_run_vertex_bytes, g_run_vertex_used + bytes);
    reserve_run(g_run_index_words, g_run_index_used + index_count);
    reserve_run(g_run_row_values, g_run_row_used + rows);
    if (run.palette && (g_run_palette == kNoPalette || g_run_palette_serial != run.palette_serial)) {
        std::memcpy(g_run_row_values.data() + g_run_row_used, run.palette, kPaletteRows * sizeof(float[4]));
        g_run_palette = g_run_row_used;
        g_run_palette_serial = run.palette_serial;
        g_run_row_used += kPaletteRows;
        std::memcpy(g_run_row_values.data() + g_run_row_used, run.palette + kNormalWords, kNormalRows * sizeof(float[4]));
        g_run_row_used += kNormalRows;
    }
    std::memcpy(g_run_row_values.data() + g_run_row_used, run.rows, run.row_count * sizeof(float[4]));
    if (run.palette) {
        g_run_row_values[g_run_row_used + 3][2] = static_cast<float>(static_cast<int>(g_run_palette) - static_cast<int>(g_run_row_used));
    }
    RunDraw draw;
    draw.layout = (run.normal ? 1u : 0u) | (run.matrix_indices ? 2u : 0u) | (run.texture_inputs << 2);
    draw.stride = run.stride;
    draw.cull = run.cull;
    draw.vertex_offset = g_run_vertex_used;
    draw.first_index = g_run_index_used;
    draw.indices = run.indices;
    draw.row = g_run_row_used;
    g_run_draws.push_back(draw);
    g_run_row_used += run.row_count;
    return true;
}

}

bool transforms_vertices() {
    static const settings::LiveFlag enabled("video.gpu_vertices", "WP_GPU_VERTICES");
    return enabled() && initialize() && create_run_pipeline();
}

bool begin_run(const VertexRun& run, uint8_t** vertices, uint16_t** indices) {
    uint32_t bytes = run.vertices * run.stride;
    if (!add_run(run, bytes, run.indices)) {
        return false;
    }
    *vertices = g_run_vertex_bytes.data() + g_run_vertex_used;
    *indices = g_run_index_words.data() + g_run_index_used;
    g_run_vertex_used += bytes;
    g_run_index_used += run.indices;
    return true;
}

bool draw_kept(const VertexRun& run, const KeptRun& kept) {
    if (kept.generation != g_kept_generation || !add_run(run, 0, 0)) {
        return false;
    }
    RunDraw& draw = g_run_draws.back();
    draw.kept = true;
    draw.vertex_offset = kept.vertex;
    draw.first_index = kept.index;
    g_run_kept++;
    g_run_kept_indices += run.indices;
    return true;
}

bool keep_run(const VertexRun& run, const uint8_t* vertices, const uint16_t* indices, KeptRun& kept, bool placed) {
    if (!create_kept_buffers()) {
        return false;
    }
    uint32_t bytes = run.vertices * run.stride;
    if (placed && kept.generation == g_kept_generation) {
        if (g_run_kept != 0) {
            flush_pending();
        }
    } else {
        uint32_t vertex = (g_kept_vertex_used + run.stride - 1) / run.stride * run.stride;
        if (vertex + bytes > kKeptVertexBytes || (g_kept_index_used + run.indices) * sizeof(uint16_t) > kKeptIndexBytes) {
            return false;
        }
        kept.vertex = vertex;
        kept.index = g_kept_index_used;
        kept.generation = g_kept_generation;
        g_kept_vertex_used = vertex + bytes;
        g_kept_index_used += run.indices;
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.kept_vertices);
    gl.BufferSubData(GL_ARRAY_BUFFER, kept.vertex, bytes, vertices);
    gl.BindBuffer(GL_ARRAY_BUFFER, g_device.kept_indices);
    gl.BufferSubData(GL_ARRAY_BUFFER, static_cast<GLintptr>(kept.index) * sizeof(uint16_t), static_cast<GLsizeiptr>(run.indices) * sizeof(uint16_t), indices);
    return true;
}

void reset_kept() {
    if (g_run_kept != 0) {
        flush_pending();
    }
    g_kept_generation++;
    g_kept_vertex_used = 0;
    g_kept_index_used = 0;
}

void finish_frame() {}

bool service_presents() {
    return false;
}

uint32_t take_interpolated_frames() {
    return 0;
}

uint32_t take_late_frames() {
    return 0;
}

GpuFigures gpu_figures() {
    return {};
}

void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource*) {
    if (count == 0 || !initialize()) {
        return;
    }
    if (!g_run_draws.empty()) {
        flush_pending();
    }
    select_batch();
    g_pending.vertices.insert(g_pending.vertices.end(), vertices, vertices + count);
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    batches = g_batches;
    vertices = g_batch_vertices;
    seconds = g_batch_seconds;
    g_batches = 0;
    g_batch_vertices = 0;
    g_batch_seconds = 0.0;
}

void take_copy_statistics(uint32_t& write_backs, double& wait_seconds) {
    write_backs = g_write_backs;
    wait_seconds = g_write_back_seconds;
    g_write_backs = 0;
    g_write_back_seconds = 0.0;
}

// OS globals and exception vectors: no texture lives below the start of the game code.
constexpr uint32_t kLowMemoryEnd = 0x4000;

bool guest_range_valid(uint32_t address, size_t size) {
    return static_cast<size_t>(address & kAddressMask) + size <= kPhysicalSize;
}

void invalidate_textures() {
    g_texture_epoch++;
}

void release_copy(CopiedTexture& entry) {
    release_target(entry.converted);
    entry.view = 0;
}

void dump_copy(const CopiedTexture& entry);

bool run_copy(const Target& target, uint32_t target_width, uint32_t target_height, int x, int y, int width, int height, uint32_t format, bool intensity,
              bool alpha, bool depth, const CopyFilter& filter) {
    struct CopyConstants {
        uint32_t params[4];
        float region[4];
        uint32_t filter[4];
        float rows[4];
        float extent[4];
    } constants{};
    uint32_t sum = filter.coefficients[0] + filter.coefficients[1] + filter.coefficients[2];
    int efb_height = scaled(kEfbHeight);
    int top = filter.clamp_top ? scaled(y) : 0;
    int bottom = (filter.clamp_bottom ? scaled(y + height) : efb_height) - 1;
    constants.params[0] = format;
    constants.params[1] = intensity ? 1u : 0u;
    constants.params[2] = alpha ? 1u : 0u;
    constants.params[3] = depth ? 1u : 0u;
    constants.region[0] = static_cast<float>(scaled(x));
    constants.region[1] = static_cast<float>(scaled(y));
    constants.region[2] = static_cast<float>(scaled(x + width) - scaled(x));
    constants.region[3] = static_cast<float>(scaled(y + height) - scaled(y));
    constants.filter[0] = filter.coefficients[0];
    constants.filter[1] = filter.coefficients[1];
    constants.filter[2] = filter.coefficients[2];
    constants.filter[3] = sum >= 128 ? 1u : 0u;
    constants.rows[0] = 1.0f / filter.gamma;
    constants.rows[1] = static_cast<float>(top) + 0.5f;
    constants.rows[2] = static_cast<float>(bottom) + 0.5f;
    constants.extent[0] = static_cast<float>(scaled(kEfbWidth));
    constants.extent[1] = static_cast<float>(efb_height);
    gl.BindBuffer(GL_UNIFORM_BUFFER, g_device.copy_constants);
    gl.BufferData(GL_UNIFORM_BUFFER, sizeof constants, &constants, GL_STREAM_DRAW);
    gl.BindBufferBase(GL_UNIFORM_BUFFER, 1, g_device.copy_constants);
    gl.BindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    gl.Viewport(0, 0, static_cast<GLsizei>(target_width), static_cast<GLsizei>(target_height));
    plain_state();
    use_fullscreen(g_device.copy, -1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_color);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, g_device.efb_depth);
    gl.BindSampler(1, g_device.point_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    return true;
}

GLuint take_buffer() {
    if (!g_device.free_buffers.empty()) {
        GLuint buffer = g_device.free_buffers.back();
        g_device.free_buffers.pop_back();
        return buffer;
    }
    GLuint buffer = 0;
    gl.GenBuffers(1, &buffer);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    gl.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(kEfbWidth) * kEfbHeight * 4, nullptr, GL_STREAM_READ);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return buffer;
}

constexpr size_t kMaxPendingWriteBacks = 32;
void complete_write_backs(bool wait);

bool queue_write_back(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                      bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back.framebuffer && !create_render_texture(g_device.write_back, kEfbWidth, kEfbHeight)) {
        return false;
    }
    if (g_device.write_backs.size() >= kMaxPendingWriteBacks) {
        complete_write_backs(true);
    }
    uint32_t texture_format = copy_texture_format(format);
    Layout layout = layout_for(texture_format);
    uint32_t blocks_x = (logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t row_bytes = stride * 32;
    uint32_t bytes = block_bytes(texture_format);
    size_t span = static_cast<size_t>(blocks_y - 1) * row_bytes + blocks_x * bytes;
    // The game copies some render targets to a null buffer. Dolphin keeps EFB copies on the GPU by default, so this never
    // hurt there; writing them to RAM overwrote the OS globals and interrupt table. Such copies stay texture-only.
    if (blocks_x * bytes > row_bytes || !guest_range_valid(address, span) || (address & kAddressMask) < kLowMemoryEnd) {
        return false;
    }
    if (!run_copy(g_device.write_back, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter)) {
        return false;
    }
    PendingWriteBack pending;
    pending.buffer = take_buffer();
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pending.buffer);
    gl.ReadPixels(0, 0, static_cast<GLsizei>(logical_width), static_cast<GLsizei>(logical_height), GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    pending.fence = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    pending.address = address;
    pending.start = address & kAddressMask;
    pending.end = pending.start + static_cast<uint32_t>(span);
    pending.row_bytes = row_bytes;
    pending.logical_width = logical_width;
    pending.logical_height = logical_height;
    pending.texture_format = texture_format;
    pending.frame = g_frame;
    g_device.write_backs.push_back(pending);
    return true;
}

bool write_back(const PendingWriteBack& pending, bool wait) {
    auto wait_start = std::chrono::steady_clock::now();
    GLenum status = gl.ClientWaitSync(pending.fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? kWaitForever : 0);
    if (status == GL_TIMEOUT_EXPIRED) {
        return false;
    }
    gl.DeleteSync(pending.fence);
    Layout layout = layout_for(pending.texture_format);
    uint32_t blocks_x = (pending.logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (pending.logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t bytes = block_bytes(pending.texture_format);
    size_t pitch = static_cast<size_t>(pending.logical_width) * 4;
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, pending.buffer);
    const uint8_t* pixels = static_cast<const uint8_t*>(gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(pitch * pending.logical_height), GL_MAP_READ_BIT));
    g_write_back_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_start).count();
    g_write_backs++;
    if (pixels) {
        uint8_t block[64];
        for (uint32_t by = 0; by < blocks_y; by++) {
            uint8_t* row = host(pending.address + by * pending.row_bytes);
            for (uint32_t bx = 0; bx < blocks_x; bx++) {
                std::memset(block, 0, sizeof block);
                for (uint32_t ty = 0; ty < layout.block_height; ty++) {
                    uint32_t py = std::min(by * layout.block_height + ty, pending.logical_height - 1);
                    for (uint32_t tx = 0; tx < layout.block_width; tx++) {
                        uint32_t px = std::min(bx * layout.block_width + tx, pending.logical_width - 1);
                        encode_texel(block, pending.texture_format, ty * layout.block_width + tx, pixels + py * pitch + px * 4);
                    }
                }
                std::memcpy(row + bx * bytes, block, bytes);
            }
        }
        gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return true;
}

void complete_write_backs(bool wait) {
    size_t done = 0;
    for (const PendingWriteBack& pending : g_device.write_backs) {
        if (!write_back(pending, wait || pending.frame + kWriteBackFrames <= g_frame)) {
            break;
        }
        done++;
        g_device.free_buffers.push_back(pending.buffer);
        auto copied = g_device.copies.find(pending.address);
        if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && --copied->second.pending_writes == 0) {
            CopiedTexture& entry = copied->second;
            entry.guest_hash = guest_range_valid(pending.address, entry.bytes) ? sample_hash(host(pending.address), entry.bytes) : 0;
        }
    }
    g_device.write_backs.erase(g_device.write_backs.begin(), g_device.write_backs.begin() + static_cast<std::ptrdiff_t>(done));
}

void finish_write_backs() {
    complete_write_backs(true);
}

bool write_back_overlaps(uint32_t address, size_t size) {
    uint32_t start = address & kAddressMask;
    uint32_t end = start + static_cast<uint32_t>(size);
    for (const PendingWriteBack& pending : g_device.write_backs) {
        if (start < pending.end && pending.start < end) {
            return true;
        }
    }
    return false;
}

void copy_to_texture(uint32_t address, uint32_t stride, int x, int y, int width, int height, bool half, uint32_t format, bool intensity, bool depth, bool alpha,
                     const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    if (format > 12) {
        if (!g_logged_copy_format) {
            g_logged_copy_format = true;
            std::fprintf(stderr, "unsupported EFB copy format %u\n", format);
        }
        auto stale = g_device.copies.find(address);
        if (stale != g_device.copies.end()) {
            release_copy(stale->second);
            g_device.copies.erase(stale);
        }
        return;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    flush_pending();
    CopiedTexture& entry = g_device.copies[address];
    // Buffers copied again every frame stay on the GPU; see the Direct3D renderer.
    constexpr uint32_t kRecurringFrames = 2;
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    uint32_t logical_width = half ? std::max(1u, w / 2) : w;
    uint32_t logical_height = half ? std::max(1u, h / 2) : h;
    bool recurring = entry.view != 0 && g_frame - entry.copied_frame <= kRecurringFrames && entry.copied_format == format &&
                     entry.logical_width == logical_width && entry.logical_height == logical_height;
    entry.copied_frame = g_frame;
    entry.copied_format = format;
    entry.logical_width = logical_width;
    entry.logical_height = logical_height;
    uint32_t target_width = std::max(1u, entry.logical_width * g_scale);
    uint32_t target_height = std::max(1u, entry.logical_height * g_scale);
    if (!entry.converted.framebuffer || entry.converted.width != target_width || entry.converted.height != target_height) {
        release_copy(entry);
        if (!create_render_texture(entry.converted, target_width, target_height)) {
            release_copy(entry);
            g_device.copies.erase(address);
            return;
        }
    }
    run_copy(entry.converted, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter);
    entry.view = entry.converted.texture;
    entry.bytes = entry.logical_width * entry.logical_height * copy_bits(format) / 8;
    if (!recurring && queue_write_back(address, stride, x, y, width, height, entry.logical_width, entry.logical_height, format, intensity, alpha || depth, depth, filter)) {
        entry.pending_writes++;
    } else if (entry.pending_writes == 0) {
        entry.guest_hash = guest_range_valid(address, entry.bytes) ? sample_hash(host(address), entry.bytes) : 0;
    }
    dump_copy(entry);
}

void finish_copies() {
    if (g_device.ready) {
        complete_write_backs(true);
    }
}

bool read_target(GLuint framebuffer, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels);

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    if (!initialize()) {
        return;
    }
    flush_pending();
    complete_write_backs(false);
    static GLsync frames_in_flight[2] = {};
    GLsync& oldest = frames_in_flight[g_frame % 2];
    if (oldest) {
        gl.ClientWaitSync(oldest, GL_SYNC_FLUSH_COMMANDS_BIT, kWaitForever);
        gl.DeleteSync(oldest);
    }
    oldest = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    g_variant_budget = 1;
    static auto logged = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (g_variant_seconds > 0.0 && now - logged >= std::chrono::seconds(1)) {
        logged = now;
        log::write("video", "%u pixel shader variants ready, %u refused, %.1f ms spent building them", g_variants_ready, g_variants_failed, g_variant_seconds * 1000.0);
        g_variant_seconds = 0.0;
    }
    g_frame++;
    g_texture_epoch++;
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    uint32_t physical_width = static_cast<uint32_t>(scaled(width));
    uint32_t physical_height = static_cast<uint32_t>(scaled(height));
    if (!g_device.frame.framebuffer || g_device.frame.width != physical_width || g_device.frame.height != physical_height) {
        release_target(g_device.frame);
        if (!create_render_texture(g_device.frame, physical_width, physical_height)) {
            release_target(g_device.frame);
            return;
        }
    }
    run_copy(g_device.frame, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
    static const char* const sequence = std::getenv("WP_DUMP_SEQUENCE");
    if (sequence) {
        static const uint64_t first = std::strtoull(sequence, nullptr, 10);
        static const uint64_t frames = std::strchr(sequence, ',') ? std::strtoull(std::strchr(sequence, ',') + 1, nullptr, 10) : 30;
        std::vector<uint32_t> pixels;
        if (g_frame >= first && g_frame < first + frames && read_target(g_device.frame.framebuffer, physical_width, physical_height, pixels)) {
            std::error_code created;
            std::filesystem::create_directories("captures/seq", created);
            char name[64];
            std::snprintf(name, sizeof name, "captures/seq/%06llu_f.png", static_cast<unsigned long long>(g_frame));
            video::save_png(name, pixels, physical_width, physical_height);
        }
    }
}

void apply_scale() {
    int scale = requested_scale();
    if (scale == g_scale) {
        return;
    }
    flush_pending();
    int previous = g_scale;
    release_efb();
    g_scale = scale;
    if (!create_efb()) {
        release_efb();
        g_scale = previous;
        if (!create_efb()) {
            std::fputs("cannot recreate the EFB after a scale change\n", stderr);
            g_device.ready = false;
            g_device.failed = true;
            return;
        }
    }
    bind_efb();
    plain_state();
    gl.DepthMask(GL_TRUE);
    gl.ClearColor(0, 0, 0, 0);
    gl.ClearDepth(1.0);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void upload_image(GLuint& texture, uint32_t& width, uint32_t& height, const options::Image& image) {
    if (image.pixels.size() != static_cast<size_t>(image.width) * image.height || image.width == 0 || image.height == 0) {
        return;
    }
    if (!texture) {
        gl.GenTextures(1, &texture);
        set_texture_levels(texture, 1);
    }
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_BGRA, GL_UNSIGNED_BYTE,
                  image.pixels.data());
    width = image.width;
    height = image.height;
}

void draw_titlebar(int client_width, int client_height, int bar) {
    options::Image image;
    if (titlebar::take(g_device.bar_version, image)) {
        upload_image(g_device.bar_texture, g_device.bar_width, g_device.bar_height, image);
    }
    if (bar <= 0 || !g_device.bar_texture) {
        return;
    }
    gl.Viewport(0, client_height - bar, client_width, bar);
    use_fullscreen(g_device.overlay, 1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.bar_texture);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

void draw_image(GLuint texture, const options::Rect& placed, int client_height) {
    if (!texture || placed.width <= 0 || placed.height <= 0) {
        return;
    }
    gl.Viewport(placed.x, client_height - placed.y - placed.height, placed.width, placed.height);
    gl.Enable(GL_BLEND);
    gl.BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    gl.BlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
    use_fullscreen(g_device.overlay, 1.0f);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, texture);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    gl.Disable(GL_BLEND);
}

void draw_overlay(int client_width, int client_height, int bar) {
    options::Image image;
    if (perf::take(g_device.hud_version, image, g_device.hud_visible, g_device.hud_corner) && !image.pixels.empty()) {
        upload_image(g_device.hud_texture, g_device.hud_width, g_device.hud_height, image);
    }
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty()) {
        upload_image(g_device.overlay_texture, g_device.overlay_width, g_device.overlay_height, image);
    }
    if (g_device.hud_visible) {
        options::Rect placed = perf::corner_rect(client_width, client_height - bar, static_cast<int>(g_device.hud_width), static_cast<int>(g_device.hud_height), g_device.hud_corner);
        placed.y += bar;
        draw_image(g_device.hud_texture, placed, client_height);
    }
    if (g_device.overlay_visible) {
        draw_image(g_device.overlay_texture, options::place(client_width, client_height, static_cast<int>(g_device.overlay_width), static_cast<int>(g_device.overlay_height)),
                   client_height);
    }
}

bool present_frame(void* window_handle, double aspect) {
    if (!g_device.ready || !g_device.frame.texture || !window_handle) {
        return false;
    }
    SDL_Window* window = static_cast<SDL_Window*>(window_handle);
    int client_width = 0;
    int client_height = 0;
    if (!SDL_GetWindowSizeInPixels(window, &client_width, &client_height) || client_width <= 0 || client_height <= 0) {
        return false;
    }
    flush_pending();
    gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    plain_state();
    gl.Viewport(0, 0, client_width, client_height);
    gl.ClearColor(0, 0, 0, 1);
    gl.Clear(GL_COLOR_BUFFER_BIT);
    int bar = titlebar::height(window);
    int area = std::max(1, client_height - bar);
    double width = client_width;
    double height = width / aspect;
    if (height > area) {
        height = area;
        width = height * aspect;
    }
    gl.Viewport(static_cast<GLint>((client_width - width) / 2), static_cast<GLint>((area - height) / 2), static_cast<GLsizei>(width),
                static_cast<GLsizei>(height));
    use_fullscreen(g_device.present, 1.0f);
    color::Adjust colors = color::current();
    gl.Uniform4f(g_device.present_adjust, colors.brightness, colors.contrast, colors.saturation, 1.0f / colors.gamma);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, g_device.frame.texture);
    gl.BindSampler(0, g_device.linear_sampler);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
    draw_overlay(client_width, client_height, bar);
    draw_titlebar(client_width, client_height, bar);
    SDL_GL_SwapWindow(window);
    apply_scale();
    return true;
}

bool read_target(GLuint framebuffer, uint32_t width, uint32_t height, std::vector<uint32_t>& pixels) {
    std::vector<uint8_t> bytes(static_cast<size_t>(width) * height * 4);
    gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    gl.ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE, bytes.data());
    pixels.resize(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < pixels.size(); i++) {
        pixels[i] = (static_cast<uint32_t>(bytes[i * 4]) << 16) | (static_cast<uint32_t>(bytes[i * 4 + 1]) << 8) | bytes[i * 4 + 2];
    }
    return true;
}

void dump_target(GLuint framebuffer, uint32_t width, uint32_t height, const char* suffix) {
    std::vector<uint32_t> pixels;
    if (framebuffer && read_target(framebuffer, width, height, pixels)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_target(g_device.efb_framebuffer, static_cast<uint32_t>(scaled(kEfbWidth)), static_cast<uint32_t>(scaled(kEfbHeight)), "_efb");
    dump_target(entry.converted.framebuffer, entry.converted.width, entry.converted.height, "");
}

void dump_copies(const char* prefix) {
    g_copy_dump_prefix = prefix ? prefix : "";
    g_copy_dump_index = 0;
    if (g_texture_notes) {
        std::fclose(g_texture_notes);
        g_texture_notes = nullptr;
    }
    g_noted_textures.clear();
    if (prefix) {
        g_texture_notes = std::fopen((g_copy_dump_prefix + "_textures.txt").c_str(), "w");
    }
}

bool read_recent(std::vector<RecentImage>&) {
    return false;
}

bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    if (!g_device.ready || !g_device.frame.framebuffer) {
        return false;
    }
    flush_pending();
    width = g_device.frame.width;
    height = g_device.frame.height;
    return read_target(g_device.frame.framebuffer, width, height, pixels);
}

void clear(int x, int y, int width, int height) {
    if (!initialize()) {
        return;
    }
    flush_pending();
    const uint32_t* bp = bp_registers();
    uint32_t pixel_format = bp[0x43] & 7;
    bool color_enable = (bp[0x41] & (1u << 3)) != 0;
    bool has_alpha = pixel_format != 0 && pixel_format != 2 && pixel_format != 3;
    bool alpha_enable = (bp[0x41] & (1u << 4)) != 0 && has_alpha;
    bool depth_enable = (bp[0x40] & (1u << 4)) != 0;
    if (!color_enable && !alpha_enable && !depth_enable) {
        return;
    }
    uint32_t red = bp[0x4F] & 0xFF;
    uint32_t alpha = (bp[0x4F] >> 8) & 0xFF;
    uint32_t green = (bp[0x50] >> 8) & 0xFF;
    uint32_t blue = bp[0x50] & 0xFF;
    uint32_t depth = bp[0x51] & 0xFFFFFF;
    if (pixel_format == 1) {
        red = quantize(red, 6);
        green = quantize(green, 6);
        blue = quantize(blue, 6);
        alpha = quantize(alpha, 6);
    } else if (pixel_format == 2) {
        red = quantize(red, 5);
        green = quantize(green, 6);
        blue = quantize(blue, 5);
        depth = (depth & 0xFFFF00) | (depth >> 16);
    }
    if (!has_alpha) {
        alpha_enable = true;
        alpha = 0;
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    bind_efb();
    plain_state();
    gl.ColorMask(color_enable, color_enable, color_enable, alpha_enable);
    if (depth_enable) {
        gl.Enable(GL_DEPTH_TEST);
        gl.DepthFunc(GL_ALWAYS);
        gl.DepthMask(GL_TRUE);
    }
    gl.Enable(GL_SCISSOR_TEST);
    gl.Scissor(scaled(x), scaled(y), scaled(x + width) - scaled(x), scaled(y + height) - scaled(y));
    use_fullscreen(g_device.clear, -1.0f);
    gl.Uniform4f(g_device.clear_color, red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f);
    gl.Uniform1f(g_device.clear_depth, static_cast<float>(depth) / 16777215.0f);
    gl.DrawArrays(GL_TRIANGLES, 0, 3);
}

}
