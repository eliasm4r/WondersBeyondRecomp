#version 450

#ifdef WP_EARLY
layout(early_fragment_tests) in;
#endif

layout(constant_id = 0) const bool kVariant = false;
layout(constant_id = 1) const uint kCount = 0u;
layout(constant_id = 2) const uint kSwap0 = 0u;
layout(constant_id = 3) const uint kSwap1 = 0u;
layout(constant_id = 4) const uint kSwap2 = 0u;
layout(constant_id = 5) const uint kSwap3 = 0u;
layout(constant_id = 6) const uint kIndirect0 = 0u;
layout(constant_id = 7) const uint kIndirect1 = 0u;
layout(constant_id = 8) const uint kIndirect2 = 0u;
layout(constant_id = 9) const uint kIndirect3 = 0u;
layout(constant_id = 10) const uint kStage0_0 = 0u;
layout(constant_id = 11) const uint kStage0_1 = 0u;
layout(constant_id = 12) const uint kStage0_2 = 0u;
layout(constant_id = 13) const uint kStage0_3 = 0u;
layout(constant_id = 14) const uint kStage0_4 = 0u;
layout(constant_id = 15) const uint kStage1_0 = 0u;
layout(constant_id = 16) const uint kStage1_1 = 0u;
layout(constant_id = 17) const uint kStage1_2 = 0u;
layout(constant_id = 18) const uint kStage1_3 = 0u;
layout(constant_id = 19) const uint kStage1_4 = 0u;
layout(constant_id = 20) const uint kStage2_0 = 0u;
layout(constant_id = 21) const uint kStage2_1 = 0u;
layout(constant_id = 22) const uint kStage2_2 = 0u;
layout(constant_id = 23) const uint kStage2_3 = 0u;
layout(constant_id = 24) const uint kStage2_4 = 0u;
layout(constant_id = 25) const uint kStage3_0 = 0u;
layout(constant_id = 26) const uint kStage3_1 = 0u;
layout(constant_id = 27) const uint kStage3_2 = 0u;
layout(constant_id = 28) const uint kStage3_3 = 0u;
layout(constant_id = 29) const uint kStage3_4 = 0u;
layout(constant_id = 30) const uint kStage4_0 = 0u;
layout(constant_id = 31) const uint kStage4_1 = 0u;
layout(constant_id = 32) const uint kStage4_2 = 0u;
layout(constant_id = 33) const uint kStage4_3 = 0u;
layout(constant_id = 34) const uint kStage4_4 = 0u;
layout(constant_id = 35) const uint kStage5_0 = 0u;
layout(constant_id = 36) const uint kStage5_1 = 0u;
layout(constant_id = 37) const uint kStage5_2 = 0u;
layout(constant_id = 38) const uint kStage5_3 = 0u;
layout(constant_id = 39) const uint kStage5_4 = 0u;
layout(constant_id = 40) const uint kStage6_0 = 0u;
layout(constant_id = 41) const uint kStage6_1 = 0u;
layout(constant_id = 42) const uint kStage6_2 = 0u;
layout(constant_id = 43) const uint kStage6_3 = 0u;
layout(constant_id = 44) const uint kStage6_4 = 0u;
layout(constant_id = 45) const uint kStage7_0 = 0u;
layout(constant_id = 46) const uint kStage7_1 = 0u;
layout(constant_id = 47) const uint kStage7_2 = 0u;
layout(constant_id = 48) const uint kStage7_3 = 0u;
layout(constant_id = 49) const uint kStage7_4 = 0u;
layout(constant_id = 50) const uint kStage8_0 = 0u;
layout(constant_id = 51) const uint kStage8_1 = 0u;
layout(constant_id = 52) const uint kStage8_2 = 0u;
layout(constant_id = 53) const uint kStage8_3 = 0u;
layout(constant_id = 54) const uint kStage8_4 = 0u;
layout(constant_id = 55) const uint kStage9_0 = 0u;
layout(constant_id = 56) const uint kStage9_1 = 0u;
layout(constant_id = 57) const uint kStage9_2 = 0u;
layout(constant_id = 58) const uint kStage9_3 = 0u;
layout(constant_id = 59) const uint kStage9_4 = 0u;
layout(constant_id = 60) const uint kStage10_0 = 0u;
layout(constant_id = 61) const uint kStage10_1 = 0u;
layout(constant_id = 62) const uint kStage10_2 = 0u;
layout(constant_id = 63) const uint kStage10_3 = 0u;
layout(constant_id = 64) const uint kStage10_4 = 0u;
layout(constant_id = 65) const uint kStage11_0 = 0u;
layout(constant_id = 66) const uint kStage11_1 = 0u;
layout(constant_id = 67) const uint kStage11_2 = 0u;
layout(constant_id = 68) const uint kStage11_3 = 0u;
layout(constant_id = 69) const uint kStage11_4 = 0u;
layout(constant_id = 70) const uint kStage12_0 = 0u;
layout(constant_id = 71) const uint kStage12_1 = 0u;
layout(constant_id = 72) const uint kStage12_2 = 0u;
layout(constant_id = 73) const uint kStage12_3 = 0u;
layout(constant_id = 74) const uint kStage12_4 = 0u;
layout(constant_id = 75) const uint kStage13_0 = 0u;
layout(constant_id = 76) const uint kStage13_1 = 0u;
layout(constant_id = 77) const uint kStage13_2 = 0u;
layout(constant_id = 78) const uint kStage13_3 = 0u;
layout(constant_id = 79) const uint kStage13_4 = 0u;
layout(constant_id = 80) const uint kStage14_0 = 0u;
layout(constant_id = 81) const uint kStage14_1 = 0u;
layout(constant_id = 82) const uint kStage14_2 = 0u;
layout(constant_id = 83) const uint kStage14_3 = 0u;
layout(constant_id = 84) const uint kStage14_4 = 0u;
layout(constant_id = 85) const uint kStage15_0 = 0u;
layout(constant_id = 86) const uint kStage15_1 = 0u;
layout(constant_id = 87) const uint kStage15_2 = 0u;
layout(constant_id = 88) const uint kStage15_3 = 0u;
layout(constant_id = 89) const uint kStage15_4 = 0u;
layout(constant_id = 90) const bool kNoDiscard = false;

const uvec4 kSwaps = uvec4(kSwap0, kSwap1, kSwap2, kSwap3);
const uvec4 kIndirect = uvec4(kIndirect0, kIndirect1, kIndirect2, kIndirect3);
uvec4 spec_stage(uint i) {
    switch (i) {
    case 0u: return uvec4(kStage0_0, kStage0_1, kStage0_2, kStage0_3);
    case 1u: return uvec4(kStage1_0, kStage1_1, kStage1_2, kStage1_3);
    case 2u: return uvec4(kStage2_0, kStage2_1, kStage2_2, kStage2_3);
    case 3u: return uvec4(kStage3_0, kStage3_1, kStage3_2, kStage3_3);
    case 4u: return uvec4(kStage4_0, kStage4_1, kStage4_2, kStage4_3);
    case 5u: return uvec4(kStage5_0, kStage5_1, kStage5_2, kStage5_3);
    case 6u: return uvec4(kStage6_0, kStage6_1, kStage6_2, kStage6_3);
    case 7u: return uvec4(kStage7_0, kStage7_1, kStage7_2, kStage7_3);
    case 8u: return uvec4(kStage8_0, kStage8_1, kStage8_2, kStage8_3);
    case 9u: return uvec4(kStage9_0, kStage9_1, kStage9_2, kStage9_3);
    case 10u: return uvec4(kStage10_0, kStage10_1, kStage10_2, kStage10_3);
    case 11u: return uvec4(kStage11_0, kStage11_1, kStage11_2, kStage11_3);
    case 12u: return uvec4(kStage12_0, kStage12_1, kStage12_2, kStage12_3);
    case 13u: return uvec4(kStage13_0, kStage13_1, kStage13_2, kStage13_3);
    case 14u: return uvec4(kStage14_0, kStage14_1, kStage14_2, kStage14_3);
    case 15u: return uvec4(kStage15_0, kStage15_1, kStage15_2, kStage15_3);
    default: return uvec4(0u);
    }
}

uvec4 spec_tevind(uint i) {
    switch (i) {
    case 0u: return uvec4(kStage0_4, 0u, 0u, 0u);
    case 1u: return uvec4(kStage1_4, 0u, 0u, 0u);
    case 2u: return uvec4(kStage2_4, 0u, 0u, 0u);
    case 3u: return uvec4(kStage3_4, 0u, 0u, 0u);
    case 4u: return uvec4(kStage4_4, 0u, 0u, 0u);
    case 5u: return uvec4(kStage5_4, 0u, 0u, 0u);
    case 6u: return uvec4(kStage6_4, 0u, 0u, 0u);
    case 7u: return uvec4(kStage7_4, 0u, 0u, 0u);
    case 8u: return uvec4(kStage8_4, 0u, 0u, 0u);
    case 9u: return uvec4(kStage9_4, 0u, 0u, 0u);
    case 10u: return uvec4(kStage10_4, 0u, 0u, 0u);
    case 11u: return uvec4(kStage11_4, 0u, 0u, 0u);
    case 12u: return uvec4(kStage12_4, 0u, 0u, 0u);
    case 13u: return uvec4(kStage13_4, 0u, 0u, 0u);
    case 14u: return uvec4(kStage14_4, 0u, 0u, 0u);
    case 15u: return uvec4(kStage15_4, 0u, 0u, 0u);
    default: return uvec4(0u);
    }
}

#define STAGE(i) (kVariant ? spec_stage(i) : stage[i])
#define TEVIND(i) (kVariant ? spec_tevind(i) : tevind[i])
#define SWAPS (kVariant ? kSwaps : swaps)
#define INDIRECT (kVariant ? kIndirect : indirect)
#define COUNT (kVariant ? kCount : header.x)

layout(set = 0, binding = 0, std140) uniform Constants {
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

layout(set = 1, binding = 0) uniform sampler2D t0;
layout(set = 1, binding = 1) uniform sampler2D t1;
layout(set = 1, binding = 2) uniform sampler2D t2;
layout(set = 1, binding = 3) uniform sampler2D t3;
layout(set = 1, binding = 4) uniform sampler2D t4;
layout(set = 1, binding = 5) uniform sampler2D t5;
layout(set = 1, binding = 6) uniform sampler2D t6;
layout(set = 1, binding = 7) uniform sampler2D t7;

layout(location = 0) in vec4 v_c0;
layout(location = 1) in vec4 v_c1;
layout(location = 2) in vec3 v_uv[8];

#ifdef WP_SINGLE_OUTPUT
layout(location = 0) out vec4 out_color;
vec4 out_blend;
#else
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;
#endif

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
    uint order_bits = SWAPS[table];
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
    uint count = COUNT;
    if (count == 0u) {
        r[0] = c0;
    }
    uint indirect_stages = INDIRECT.x & 7u;
    uint texgens = (INDIRECT.x >> 8u) & 15u;
    ivec3 indirect_texel[4] = ivec3[4](ivec3(0), ivec3(0), ivec3(0), ivec3(0));
    for (uint j = 0u; j < indirect_stages && j < 4u; j++) {
        uint reference = INDIRECT.y >> (6u * j);
        uint map = reference & 7u;
        uint coord = (reference >> 3u) & 7u;
        if (coord >= texgens) {
            coord = 0u;
        }
        uint scales = j < 2u ? INDIRECT.z : INDIRECT.w;
        uint shift_s = (scales >> ((j & 1u) * 8u)) & 15u;
        uint shift_t = (scales >> ((j & 1u) * 8u + 4u)) & 15u;
        ivec2 base = fixpoint(coord);
        indirect_texel[j] = sample_fixed(map, ivec2(base.x >> shift_s, base.y >> shift_t), gradient_x[coord], gradient_y[coord], coord).abg;
    }
    ivec2 tevcoord = ivec2(0);
    int alphabump = 0;
    for (uint i = 0u; i < count; i++) {
        uint ce = STAGE(i).x;
        uint ae = STAGE(i).y;
        uint order = STAGE(i).z;
        uint ksel = STAGE(i).w;
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
        uint last_color = (STAGE(max(count, 1u) - 1u).x >> 22u) & 3u;
        uint last_alpha = (STAGE(max(count, 1u) - 1u).y >> 22u) & 3u;
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
    if (!accepted && !kNoDiscard) {
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
