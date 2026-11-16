#pragma once

namespace wp::gx::render::metal_shaders {

constexpr const char* kCommon = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct Constants {
    float4 initial[4];
    float4 konst[4];
    uint4 stage[16];
    uint4 header;
    uint4 swaps;
    float4 texdims[8];
    int4 indmtx[6];
    uint4 indirect;
    uint4 tevind[16];
    float4 fog;
    int4 fog_integer;
    float4 fog_range[3];
};

struct Varyings {
    float4 position [[position, invariant]];
    float4 c0;
    float4 c1;
    float3 uv0;
    float3 uv1;
    float3 uv2;
    float3 uv3;
    float3 uv4;
    float3 uv5;
    float3 uv6;
    float3 uv7;
};

struct Fullscreen {
    float4 position [[position]];
    float2 uv;
};
)MSL";

constexpr const char* kVertices = R"MSL(
struct ScreenVertex {
    float4 position;
    float4 c0;
    float4 c1;
    packed_float3 uv[8];
};

vertex Varyings tev_vertex(const device ScreenVertex* vertices [[buffer(0)]], uint id [[vertex_id]]) {
    ScreenVertex v = vertices[id];
    Varyings out;
    out.position = v.position;
    out.c0 = v.c0;
    out.c1 = v.c1;
    out.uv0 = float3(v.uv[0]);
    out.uv1 = float3(v.uv[1]);
    out.uv2 = float3(v.uv[2]);
    out.uv3 = float3(v.uv[3]);
    out.uv4 = float3(v.uv[4]);
    out.uv5 = float3(v.uv[5]);
    out.uv6 = float3(v.uv[6]);
    out.uv7 = float3(v.uv[7]);
    return out;
}

struct RunDraw {
    uint base;
    uint stride;
    uint layout;
    uint row;
};

float word_float(const device uint* words, uint at) {
    return as_type<float>(words[at]);
}

uint4 word_bytes(uint word) {
    return uint4(word & 255u, (word >> 8) & 255u, (word >> 16) & 255u, word >> 24);
}

float3 transformed(const device float4* rows, int at, float3 v) {
    float4 r0 = rows[at];
    float4 r1 = rows[at + 1];
    float4 r2 = rows[at + 2];
    float3 result;
    result.x = r0.x * v.x + r0.y * v.y + r0.z * v.z + r0.w;
    result.y = r1.x * v.x + r1.y * v.y + r1.z * v.z + r1.w;
    result.z = r2.x * v.x + r2.y * v.y + r2.z * v.z + r2.w;
    return result;
}

float3 unit(float3 v) {
    float extent = sqrt(dot(v, v));
    return extent == 0.0 ? v : v / extent;
}

float ratio(float n, float d) {
    return d == 0.0 ? (n > 0.0 ? 1.0 : 0.0) : n / d;
}

float light_factor(const device float4* rows, int at, uint control, float3 eye, float3 normal) {
    float4 a = rows[at + 1];
    float4 b = rows[at + 2];
    float4 c = rows[at + 3];
    float3 angle = a.xyz;
    float3 range = float3(a.w, b.x, b.y);
    float3 direction = c.yzw;
    uint diffuse = (control >> 7) & 3u;
    uint attenuation = (control >> 9) & 3u;
    float3 toward = float3(b.z, b.w, c.x) - eye;
    float amount = 1.0;
    if (attenuation == 1u) {
        toward = unit(toward);
        amount = dot(toward, normal) >= 0.0 ? max(0.0, dot(direction, normal)) : 0.0;
        float3 powers = float3(1.0, amount, amount * amount);
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

void light_colors(const device float4* rows, int light, int matrix_row, uint word, float3 eye, float3 raw, thread float4& c0, thread float4& c1) {
    float4 controls = rows[light];
    int first = matrix_row + int(word >> 2);
    float4 n0 = rows[first];
    float4 n1 = rows[first + 1];
    float4 n2 = rows[first + 2];
    float m[12] = {n0.x, n0.y, n0.z, n0.w, n1.x, n1.y, n1.z, n1.w, n2.x, n2.y, n2.z, n2.w};
    uint k = word & 3u;
    float3 normal = unit(float3(m[k] * raw.x + m[k + 1u] * raw.y + m[k + 2u] * raw.z,
                                m[k + 3u] * raw.x + m[k + 4u] * raw.y + m[k + 5u] * raw.z,
                                m[k + 6u] * raw.x + m[k + 7u] * raw.y + m[k + 8u] * raw.z));
    float4 given[2] = {c0, c1};
    float4 result[2];
    for (uint channel = 0u; channel < 2u; channel++) {
        float4 vertex_color = floor(given[channel] * 255.0 + 0.5);
        float4 material = rows[light + 1 + int(channel)];
        float4 ambient = rows[light + 3 + int(channel)];
        uint color = uint(channel == 0u ? controls.x : controls.y);
        uint alpha = uint(channel == 0u ? controls.z : controls.w);
        float3 color_material = (color & 1u) != 0u ? vertex_color.rgb : material.rgb;
        float alpha_material = (alpha & 1u) != 0u ? vertex_color.a : material.a;
        uint color_mask = light_mask(color);
        uint alpha_mask = light_mask(alpha);
        float3 sum = (color & 64u) != 0u ? vertex_color.rgb : ambient.rgb;
        float alpha_sum = (alpha & 64u) != 0u ? vertex_color.a : ambient.a;
        for (uint i = 0u; i < 8u; i++) {
            uint bit = 1u << i;
            int at = light + 8 + 4 * int(i);
            if ((color_mask & bit) != 0u) {
                sum += rows[at].rgb * light_factor(rows, at, color, eye, normal);
            }
            if ((alpha_mask & bit) != 0u) {
                alpha_sum += rows[at].a * light_factor(rows, at, alpha, eye, normal);
            }
        }
        if ((color & 2u) != 0u) {
            result[channel].rgb = float3(modulated(color_material.r, sum.r), modulated(color_material.g, sum.g), modulated(color_material.b, sum.b));
        } else {
            result[channel].rgb = color_material / 255.0;
        }
        result[channel].a = (alpha & 2u) != 0u ? modulated(alpha_material, alpha_sum) : alpha_material / 255.0;
    }
    c0 = result[0];
    c1 = result[1];
}

vertex Varyings run_vertex(const device uint* words [[buffer(0)]], const device float4* rows [[buffer(1)]], constant RunDraw& draw [[buffer(2)]],
                           uint id [[vertex_id]]) {
    uint at = draw.base + id * draw.stride;
    float3 a_position = float3(word_float(words, at), word_float(words, at + 1), word_float(words, at + 2));
    float4 a_c0 = unpack_unorm4x8_to_float(words[at + 3]);
    float4 a_c1 = unpack_unorm4x8_to_float(words[at + 4]);
    uint offset = at + 5;
    float3 a_normal = float3(0.0);
    if ((draw.layout & 1u) != 0u) {
        a_normal = float3(word_float(words, offset), word_float(words, offset + 1), word_float(words, offset + 2));
        offset += 3;
    }
    uint4 a_m0 = uint4(0);
    uint4 a_m1 = uint4(0);
    uint4 a_m2 = uint4(0);
    if ((draw.layout & 2u) != 0u) {
        a_m0 = word_bytes(words[offset]);
        a_m1 = word_bytes(words[offset + 1]);
        a_m2 = word_bytes(words[offset + 2]);
        offset += 3;
    }
    uint input_count = draw.layout >> 2;
    float2 inputs[8];
    for (uint k = 0u; k < 8u; k++) {
        inputs[k] = float2(0.0);
        if (k < input_count) {
            inputs[k] = float2(word_float(words, offset + 2u * k), word_float(words, offset + 2u * k + 1u));
        }
    }
    int run = int(draw.row);
    float4 p0 = rows[run];
    float4 p1 = rows[run + 1];
    float4 p2 = rows[run + 2];
    float4 p3 = rows[run + 3];
    int origin = run + int(p3.z);
    float3 eye = transformed(rows, origin + int(p1.w) + int(a_m0.x), a_position);
    float x;
    float y;
    float z;
    float w;
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
    float4 position;
    position.x = x * p2.x + w * p2.y;
    position.y = y * p2.z + w * p2.w;
    position.z = (z * p3.x + w * (16777215.0 - p3.y)) / 16777215.0;
    position.w = w;
    uint setup = uint(p3.w);
    uint count = min(setup & 15u, 8u);
    bool dual = (setup & 16u) != 0u;
    uint indices[8] = {a_m0.y, a_m0.z, a_m0.w, a_m1.x, a_m1.y, a_m1.z, a_m1.w, a_m2.x};
    float3 uv[8];
    for (uint k = 0u; k < 8u; k++) {
        uv[k] = float3(0.0);
    }
    for (uint i = 0u; i < count; i++) {
        int place = run + 7 + 7 * int(i);
        float4 info = rows[place];
        uint source = uint(info.x);
        uint flags = uint(info.y);
        float3 given = float3(0.0, 0.0, 1.0);
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
        float3 coord = transformed(rows, origin + int(info.z) + int(indices[i]), given);
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
            coord = transformed(rows, place + 4, coord);
        }
        if (coord.z == 0.0) {
            coord.xy = clamp(coord.xy / 2.0, -1.0, 1.0);
            coord.z = 1.0;
        }
        uv[i] = coord;
    }
    float4 c0 = a_c0;
    float4 c1 = a_c1;
    if ((setup & 32u) != 0u) {
        int light = run + 7 + 7 * int(count);
        if (p1.w == 0.0) {
            light_colors(rows, light, origin + 66, (a_m0.x & 31u) * 3u, eye, a_normal, c0, c1);
        } else {
            light_colors(rows, light, light + 5, 0u, eye, a_normal, c0, c1);
        }
    }
    Varyings out;
    out.position = position;
    out.c0 = c0;
    out.c1 = c1;
    out.uv0 = uv[0];
    out.uv1 = uv[1];
    out.uv2 = uv[2];
    out.uv3 = uv[3];
    out.uv4 = uv[4];
    out.uv5 = uv[5];
    out.uv6 = uv[6];
    out.uv7 = uv[7];
    return out;
}
)MSL";

constexpr const char* kTev = R"MSL(
#ifndef WP_VARIANT
#define STAGE(i) u.stage[i]
#define TEVIND(i) u.tevind[i]
#define SWAPS u.swaps
#define INDIRECT u.indirect
#define COUNT u.header.x
#endif

struct TevOutput {
    float4 color [[color(0), index(0)]];
    float4 blend [[color(0), index(1)]];
};

struct Maps {
    texture2d<float> t0;
    texture2d<float> t1;
    texture2d<float> t2;
    texture2d<float> t3;
    texture2d<float> t4;
    texture2d<float> t5;
    texture2d<float> t6;
    texture2d<float> t7;
    sampler s0;
    sampler s1;
    sampler s2;
    sampler s3;
    sampler s4;
    sampler s5;
    sampler s6;
    sampler s7;
    float4 bias0;
    float4 bias1;
};

float2 projected(float3 uv) {
    return uv.z == 0.0 ? clamp(uv.xy / 2.0, -1.0, 1.0) : uv.xy / uv.z;
}

float2 select_uv(thread const Varyings& in, uint i) {
    switch (i) {
    case 0u: return projected(in.uv0);
    case 1u: return projected(in.uv1);
    case 2u: return projected(in.uv2);
    case 3u: return projected(in.uv3);
    case 4u: return projected(in.uv4);
    case 5u: return projected(in.uv5);
    case 6u: return projected(in.uv6);
    default: return projected(in.uv7);
    }
}

float4 sample_map(thread const Maps& maps, uint m, float2 uv, float2 dx, float2 dy) {
    float bias = m < 4u ? maps.bias0[m] : maps.bias1[m - 4u];
    dx *= bias;
    dy *= bias;
    switch (m) {
    case 0u: return maps.t0.sample(maps.s0, uv, gradient2d(dx, dy));
    case 1u: return maps.t1.sample(maps.s1, uv, gradient2d(dx, dy));
    case 2u: return maps.t2.sample(maps.s2, uv, gradient2d(dx, dy));
    case 3u: return maps.t3.sample(maps.s3, uv, gradient2d(dx, dy));
    case 4u: return maps.t4.sample(maps.s4, uv, gradient2d(dx, dy));
    case 5u: return maps.t5.sample(maps.s5, uv, gradient2d(dx, dy));
    case 6u: return maps.t6.sample(maps.s6, uv, gradient2d(dx, dy));
    default: return maps.t7.sample(maps.s7, uv, gradient2d(dx, dy));
    }
}

constant int kFractions[8] = {255, 223, 191, 159, 128, 96, 64, 32};
constant int kWraps[5] = {256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7};
constant int kFormatShifts[4] = {0, 3, 4, 5};
constant int kBumpShifts[4] = {0, 5, 4, 3};

int4 quantized(float4 value) {
    return int4(rint(value * 255.0));
}

int3 konst_color(uint sel, thread const int4* k) {
    if (sel < 8u) {
        return int3(kFractions[sel]);
    }
    if (sel >= 12u && sel < 16u) {
        return k[sel - 12u].rgb;
    }
    if (sel >= 16u) {
        uint index = (sel - 16u) & 3u;
        uint component = (sel - 16u) >> 2u;
        return int3(k[index][component]);
    }
    return int3(0);
}

int konst_alpha(uint sel, thread const int4* k) {
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

int3 color_input(uint sel, thread const int4* r, int4 tex, int4 ras, int3 kc) {
    switch (sel) {
    case 0u: return r[0].rgb;
    case 1u: return int3(r[0].a);
    case 2u: return r[1].rgb;
    case 3u: return int3(r[1].a);
    case 4u: return r[2].rgb;
    case 5u: return int3(r[2].a);
    case 6u: return r[3].rgb;
    case 7u: return int3(r[3].a);
    case 8u: return tex.rgb;
    case 9u: return int3(tex.a);
    case 10u: return ras.rgb;
    case 11u: return int3(ras.a);
    case 12u: return int3(255);
    case 13u: return int3(128);
    case 14u: return kc;
    default: return int3(0);
    }
}

int alpha_input(uint sel, thread const int4* r, int4 tex, int4 ras, int ka) {
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

bool compare_inputs(uint mode, bool eq, int3 a, int3 b) {
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

int3 compare_color(uint mode, bool eq, int3 a, int3 b, int3 c) {
    if (mode == 3u) {
        bool3 passed = eq ? (a == b) : (a > b);
        return int3(passed.r ? c.r : 0, passed.g ? c.g : 0, passed.b ? c.b : 0);
    }
    return compare_inputs(mode, eq, a, b) ? c : int3(0);
}

int4 apply_swap(uint4 swaps, int4 value, uint table) {
    uint order_bits = swaps[table];
    int components[4] = {value.r, value.g, value.b, value.a};
    return int4(components[order_bits & 3u], components[(order_bits >> 2u) & 3u], components[(order_bits >> 4u) & 3u], components[(order_bits >> 6u) & 3u]);
}

int3 combine(int3 a, int3 b, int3 c, int3 d, uint bias_code, bool subtract, uint scale_code) {
    int bias = bias_code == 1u ? 128 : (bias_code == 2u ? -128 : 0);
    int shift = scale_code == 1u ? 1 : (scale_code == 2u ? 2 : 0);
    int rounding = scale_code == 3u ? 0 : (subtract ? 127 : 128);
    int3 blended = ((((a << 8) + (b - a) * (c + (c >> 7))) << shift) + rounding) >> 8;
    int3 base = (d + bias) << shift;
    int3 value = subtract ? base - blended : base + blended;
    return scale_code == 3u ? value >> 1 : value;
}

int idot(int3 a, int3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

int2 fixpoint(constant Constants& u, thread const Varyings& in, uint coord) {
    return int2(select_uv(in, coord) * u.texdims[coord].zw * 128.0);
}

int4 sample_fixed(constant Constants& u, thread const Maps& maps, uint map, int2 coords, float2 dx, float2 dy, uint coord) {
    float2 size = u.texdims[map].xy;
    float2 scale = u.texdims[coord].zw / size;
    return quantized(sample_map(maps, map, float2(coords) / (size * 128.0), dx * scale, dy * scale));
}

TevOutput tev_body(Varyings in, constant Constants& u, thread const Maps& maps) {
    float2 gradient_x[8];
    float2 gradient_y[8];
    for (uint g = 0u; g < 8u; g++) {
        float2 value = select_uv(in, g);
        gradient_x[g] = dfdx(value);
        gradient_y[g] = dfdy(value);
    }
    int4 k[4] = {quantized(u.konst[0]), quantized(u.konst[1]), quantized(u.konst[2]), quantized(u.konst[3])};
    int4 r[4] = {quantized(u.initial[0]), quantized(u.initial[1]), quantized(u.initial[2]), quantized(u.initial[3])};
    int4 c0 = quantized(in.c0);
    int4 c1 = quantized(in.c1);
    uint count = COUNT;
    if (count == 0u) {
        r[0] = c0;
    }
    uint4 indirect = INDIRECT;
    uint4 swaps = SWAPS;
    uint indirect_stages = indirect.x & 7u;
    uint texgens = (indirect.x >> 8u) & 15u;
    int3 indirect_texel[4] = {int3(0), int3(0), int3(0), int3(0)};
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
        int2 base = fixpoint(u, in, coord);
        indirect_texel[j] = sample_fixed(u, maps, map, int2(base.x >> shift_s, base.y >> shift_t), gradient_x[coord], gradient_y[coord], coord).abg;
    }
    int2 tevcoord = int2(0);
    int alphabump = 0;
    for (uint i = 0u; i < count; i++) {
        uint4 stage = STAGE(i);
        uint ce = stage.x;
        uint ae = stage.y;
        uint order = stage.z;
        uint ksel = stage.w;
        uint coord = (order >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint ind = TEVIND(i).x;
        uint bt = ind & 3u;
        uint format = (ind >> 2u) & 3u;
        uint bias = (ind >> 4u) & 7u;
        uint bump = (ind >> 7u) & 3u;
        uint matrix_index = (ind >> 9u) & 3u;
        uint matrix_id = (ind >> 11u) & 3u;
        uint wrap_s = (ind >> 13u) & 7u;
        uint wrap_t = (ind >> 16u) & 7u;
        bool has_stage = bt < indirect_stages;
        int2 base = fixpoint(u, in, coord);
        int2 offset = int2(0);
        if (has_stage && bump != 0u) {
            alphabump = (indirect_texel[bt][max(bump, 1u) - 1u] << kBumpShifts[format]) & 248;
        }
        if (has_stage && matrix_index != 0u) {
            int3 crd = indirect_texel[bt] >> kFormatShifts[format];
            int add = format == 0u ? -128 : 1;
            if ((bias & 1u) != 0u) crd.x += add;
            if ((bias & 2u) != 0u) crd.y += add;
            if ((bias & 4u) != 0u) crd.z += add;
            uint m = 2u * (matrix_index - 1u);
            if (matrix_id == 0u) {
                offset = int2(idot(u.indmtx[m].xyz, crd), idot(u.indmtx[m + 1u].xyz, crd)) >> 3;
            } else if (matrix_id == 1u) {
                offset = (base * crd.xx) >> 8;
            } else if (matrix_id == 2u) {
                offset = (base * crd.yy) >> 8;
            }
            int shift = u.indmtx[m].w;
            offset = shift >= 0 ? (offset >> shift) : (offset << (-shift));
        }
        int2 wrapped;
        wrapped.x = wrap_s == 0u ? base.x : (wrap_s >= 6u ? 0 : (base.x & (kWraps[max(wrap_s, 1u) - 1u] - 1)));
        wrapped.y = wrap_t == 0u ? base.y : (wrap_t >= 6u ? 0 : (base.y & (kWraps[max(wrap_t, 1u) - 1u] - 1)));
        tevcoord = ((ind >> 20u) & 1u) != 0u ? tevcoord + wrapped + offset : wrapped + offset;
        tevcoord = (tevcoord << 8) >> 8;
        int4 tex = int4(255);
        if ((order & 0x40u) != 0u) {
            tex = texgens == 0u ? int4(0) : apply_swap(swaps, sample_fixed(u, maps, order & 7u, tevcoord, gradient_x[coord], gradient_y[coord], coord), (ae >> 2u) & 3u);
        }
        int4 ras = int4(0);
        uint chan = (order >> 7u) & 7u;
        if (chan == 0u) {
            ras = c0;
        } else if (chan == 1u) {
            ras = c1;
        } else if (chan == 5u) {
            ras = int4(alphabump);
        } else if (chan == 6u) {
            ras = int4(alphabump | (alphabump >> 5));
        }
        ras = apply_swap(swaps, ras, ae & 3u);
        int3 kc = konst_color(ksel & 31u, k);
        int ka = konst_alpha((ksel >> 5u) & 31u, k);
        int3 a = color_input((ce >> 12u) & 15u, r, tex, ras, kc) & 255;
        int3 b = color_input((ce >> 8u) & 15u, r, tex, ras, kc) & 255;
        int3 c = color_input((ce >> 4u) & 15u, r, tex, ras, kc) & 255;
        int3 d = color_input(ce & 15u, r, tex, ras, kc);
        uint bias_code = (ce >> 16u) & 3u;
        uint scale_code = (ce >> 20u) & 3u;
        int3 color;
        if (bias_code == 3u) {
            color = d + compare_color(scale_code, ((ce >> 18u) & 1u) != 0u, a, b, c);
        } else {
            color = combine(a, b, c, d, bias_code, ((ce >> 18u) & 1u) != 0u, scale_code);
        }
        color = ((ce >> 19u) & 1u) != 0u ? clamp(color, int3(0), int3(255)) : clamp(color, int3(-1024), int3(1023));
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
            alpha = combine(int3(aa, 0, 0), int3(ab, 0, 0), int3(ac, 0, 0), int3(ad, 0, 0), abias_code, ((ae >> 18u) & 1u) != 0u, ascale_code).x;
        }
        alpha = ((ae >> 19u) & 1u) != 0u ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);
        uint color_dest = (ce >> 22u) & 3u;
        uint alpha_dest = (ae >> 22u) & 3u;
        r[color_dest].rgb = color;
        r[alpha_dest].a = alpha;
    }
    int4 result = r[0];
    if (count > 0u) {
        uint last_color = (STAGE(max(count, 1u) - 1u).x >> 22u) & 3u;
        uint last_alpha = (STAGE(max(count, 1u) - 1u).y >> 22u) & 3u;
        result = int4(r[last_color].rgb, r[last_alpha].a);
    }
    result &= 255;
    if ((u.header.w & 2u) != 0u) {
        int2 dither = int2(in.position.xy) & 1;
        result.rgb = (result.rgb - (result.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;
    }
    uint fog_select = uint(u.fog_integer.x) & 15u;
    uint fog_type = fog_select >> 1u;
    if (fog_type != 0u) {
        int z = clamp(int(in.position.z * 16777216.0), 0, 0xFFFFFF);
        float ze = (fog_select & 1u) == 0u ? (u.fog.x * 16777216.0) / float(u.fog_integer.y - (z >> u.fog_integer.w)) : u.fog.x * float(z) / 16777216.0;
        if ((u.fog_integer.x & 16) != 0) {
            float offset = (2.0 * (in.position.x / u.fog.w)) - 1.0 - u.fog.z;
            float index = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
            uint lower = uint(index);
            uint upper = min(lower + 1u, 9u);
            float kr = mix(u.fog_range[lower >> 2u][lower & 3u], u.fog_range[upper >> 2u][upper & 3u], fract(index));
            ze *= sqrt(offset * offset + kr * kr) / kr;
        }
        float amount = clamp(ze - u.fog.y, 0.0, 1.0);
        if (fog_type == 4u) {
            amount = 1.0 - exp2(-8.0 * amount);
        } else if (fog_type == 5u) {
            amount = 1.0 - exp2(-8.0 * amount * amount);
        } else if (fog_type == 6u) {
            amount = exp2(-8.0 * (1.0 - amount));
        } else if (fog_type == 7u) {
            amount = exp2(-8.0 * (1.0 - amount) * (1.0 - amount));
        }
        int weight = int(rint(amount * 256.0));
        int3 fog_color = int3((u.fog_integer.z >> 16) & 255, (u.fog_integer.z >> 8) & 255, u.fog_integer.z & 255);
        result.rgb = (result.rgb * (256 - weight) + fog_color * weight) >> 8;
    }
    uint compare = u.header.y;
    uint value = uint(result.a);
    bool first = compare_alpha((compare >> 16u) & 7u, value, compare & 255u);
    bool second = compare_alpha((compare >> 19u) & 7u, value, (compare >> 8u) & 255u);
    uint logic = (compare >> 22u) & 3u;
    bool accepted = logic == 0u ? (first && second) : (logic == 1u ? (first || second) : (logic == 2u ? (first != second) : (first == second)));
#ifndef WP_NO_DISCARD
    if (!accepted) {
        discard_fragment();
    }
#endif
    TevOutput out;
    out.blend = float4(result) / 255.0;
    uint source = (u.header.w >> 2u) & 3u;
    int4 written = source == 0u ? result : (source == 1u ? int4(0) : (source == 2u ? int4(255) : 255 - result));
    out.color = float4(written) / 255.0;
    if ((u.header.z & 0x100u) != 0u) {
        out.color.a = float(u.header.z & 255u) / 255.0;
    }
    if ((u.header.w & 1u) != 0u) {
        out.color.rgb = float3(written.rgb >> 2) / 63.0;
        out.color.a = float(((u.header.z & 0x100u) != 0u ? int(u.header.z & 255u) : written.a) >> 2) / 63.0;
    }
    return out;
}

#define WP_TEV_ARGUMENTS                                                                                                                  \
    Varyings in [[stage_in]], constant Constants& u [[buffer(0)]], constant float4* biases [[buffer(1)]],                                  \
        texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]], texture2d<float> t2 [[texture(2)]],                        \
        texture2d<float> t3 [[texture(3)]], texture2d<float> t4 [[texture(4)]], texture2d<float> t5 [[texture(5)]],                        \
        texture2d<float> t6 [[texture(6)]], texture2d<float> t7 [[texture(7)]], sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],      \
        sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]], sampler s4 [[sampler(4)]], sampler s5 [[sampler(5)]],                        \
        sampler s6 [[sampler(6)]], sampler s7 [[sampler(7)]]

#define WP_TEV_MAPS Maps maps = {t0, t1, t2, t3, t4, t5, t6, t7, s0, s1, s2, s3, s4, s5, s6, s7, biases[0], biases[1]};

#ifndef WP_SKIP_LATE
fragment TevOutput tev_fragment(WP_TEV_ARGUMENTS) {
    WP_TEV_MAPS
    return tev_body(in, u, maps);
}
#endif

#ifndef WP_SKIP_EARLY
[[early_fragment_tests]] fragment TevOutput tev_fragment_early(WP_TEV_ARGUMENTS) {
    WP_TEV_MAPS
    return tev_body(in, u, maps);
}
#endif
)MSL";

constexpr const char* kUtility = R"MSL(
vertex Fullscreen fullscreen_vertex(uint id [[vertex_id]]) {
    float2 corner = float2(float((id << 1) & 2u), float(id & 2u));
    Fullscreen out;
    out.uv = corner;
    out.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    return out;
}

float3 adjusted(float3 c, float4 adjust) {
    c = ((c - 0.5) * adjust.y + 0.5) * adjust.x;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = clamp(luma + (c - luma) * adjust.z, 0.0, 1.0);
    return pow(c, float3(adjust.w));
}

fragment float4 present_fragment(Fullscreen in [[stage_in]], texture2d<float> frame [[texture(0)]], sampler linear [[sampler(0)]],
                                 constant float4& adjust [[buffer(0)]]) {
    return float4(adjusted(frame.sample(linear, in.uv).rgb, adjust), 1.0);
}

fragment float4 present_smooth_fragment(Fullscreen in [[stage_in]], texture2d<float> frame [[texture(0)]], sampler linear [[sampler(0)]],
                                        constant float4& adjust [[buffer(0)]]) {
    float2 dx = dfdx(in.uv);
    float2 dy = dfdy(in.uv);
    float3 sum = float3(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            sum += frame.sample(linear, in.uv + dx * ((float(x) + 0.5) / 4.0 - 0.5) + dy * ((float(y) + 0.5) / 4.0 - 0.5), level(0.0)).rgb;
        }
    }
    return float4(adjusted(sum / 16.0, adjust), 1.0);
}

fragment float4 overlay_fragment(Fullscreen in [[stage_in]], texture2d<float> overlay [[texture(0)]], sampler linear [[sampler(0)]]) {
    return overlay.sample(linear, in.uv);
}

struct ClearValues {
    float4 color;
    float depth;
};

struct ClearOutput {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment ClearOutput clear_fragment(Fullscreen in [[stage_in]], constant ClearValues& values [[buffer(0)]]) {
    ClearOutput out;
    out.color = values.color;
    out.depth = values.depth;
    return out;
}

struct CopyConstants {
    uint4 params;
    float4 region;
    uint4 filter_taps;
    float4 rows;
    float4 extent;
};

float4 convert(constant CopyConstants& c, uint4 raw) {
    if (c.params.z == 0u) {
        raw.a = 255u;
    }
    if (c.params.y != 0u) {
        const float4 y_const = float4(66.0, 129.0, 25.0, 16.0);
        const float4 u_const = float4(-38.0, -74.0, 112.0, 128.0);
        const float4 v_const = float4(112.0, -94.0, -18.0, 128.0);
        float4 source = float4(float3(raw.rgb), 256.0);
        uint3 yuv = uint3(uint(dot(y_const, source)), uint(dot(u_const, source)), uint(dot(v_const, source)));
        raw.rgb = min((yuv >> 8u) + ((yuv >> 7u) & 1u), uint3(255u));
    }
    float4 value = float4(raw) / 255.0;
    switch (c.params.x) {
    case 0u: {
        float red = float(raw.r & 0xF0u) / 240.0;
        return float4(red);
    }
    case 1u:
    case 8u: return value.rrrr;
    case 2u: {
        float2 red_alpha = float2(uint2(raw.r, raw.a) & 0xF0u) / 240.0;
        return red_alpha.rrrg;
    }
    case 3u: return value.rrra;
    case 4u: {
        float2 red_blue = float2(uint2(raw.r, raw.b) & 0xF8u) / 248.0;
        float green = float(raw.g & 0xFCu) / 252.0;
        return float4(red_blue.r, green, red_blue.g, 1.0);
    }
    case 5u: return float4(float3(raw.rgb & 0xF8u) / 248.0, float(raw.a & 0xE0u) / 224.0);
    case 7u: return value.aaaa;
    case 9u: return value.gggg;
    case 10u: return value.bbbb;
    case 11u: return value.rrrg;
    case 12u: return value.gggb;
    case 13u: return float4(value.rgb, 1.0);
    default: return value;
    }
}

uint4 sample_efb(constant CopyConstants& c, texture2d<float> color_source, depth2d<float> depth_source, sampler linear, float2 position, float offset) {
    float y = clamp(position.y + offset, c.rows.y, c.rows.z);
    if (c.params.w != 0u) {
        uint depth = min(uint(clamp(depth_source.read(uint2(uint(position.x), uint(y))), 0.0, 1.0) * 16777216.0), 0xFFFFFFu);
        return uint4((depth >> 16u) & 255u, (depth >> 8u) & 255u, depth & 255u, 255u);
    }
    return uint4(rint(clamp(color_source.sample(linear, float2(position.x, y) / c.extent.xy, level(0.0)), 0.0, 1.0) * 255.0));
}

fragment float4 copy_fragment(Fullscreen in [[stage_in]], constant CopyConstants& c [[buffer(0)]], texture2d<float> color_source [[texture(0)]],
                              depth2d<float> depth_source [[texture(1)]], sampler linear [[sampler(0)]]) {
    float2 source = c.region.xy + in.uv * c.region.zw;
    uint4 current = sample_efb(c, color_source, depth_source, linear, source, 0.0);
    uint3 combined = current.rgb * c.filter_taps.y;
    if (c.filter_taps.x != 0u || c.filter_taps.z != 0u) {
        combined += sample_efb(c, color_source, depth_source, linear, source, -1.0).rgb * c.filter_taps.x +
                    sample_efb(c, color_source, depth_source, linear, source, 1.0).rgb * c.filter_taps.z;
    }
    uint4 raw = uint4(combined >> 6u, current.a);
    if (c.filter_taps.w != 0u) {
        raw &= 0x1FFu;
    }
    raw = min(raw, uint4(255u));
    if (c.rows.x != 1.0) {
        raw.rgb = uint3(rint(pow(float3(raw.rgb) / 255.0, float3(c.rows.x)) * 255.0));
    }
    return convert(c, raw);
}
)MSL";

}
