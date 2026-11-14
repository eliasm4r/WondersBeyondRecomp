#include "wp/gx_render.h"
#include "wp/gx.h"
#include "wp/log.h"

#include "gx_render_common.h"

#include <d3d11.h>
#include <emmintrin.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wp/color.h"
#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/memory.h"
#include "wp/frame_pacing.h"
#include "wp/options.h"
#include "wp/perf_overlay.h"
#include "wp/screenshot.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx::render {

void finish_write_backs();
bool write_back_overlaps(uint32_t address, size_t size);

namespace {

constexpr uint32_t kVertexCapacity = 1 << 18;
constexpr uint32_t kVertexBytes = kVertexCapacity * sizeof(ScreenVertex);

const char* kPresentShaderSource = R"HLSL(
Texture2D frame : register(t0);
SamplerState frame_sampler : register(s0);

cbuffer PresentConstants : register(b0) {
    float4 adjust;
};

float3 adjusted(float3 c) {
    c = ((c - 0.5) * adjust.y + 0.5) * adjust.x;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = saturate(luma + (c - luma) * adjust.z);
    return pow(c, adjust.w);
}

struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

Output vertex_main(uint id : SV_VertexID) {
    Output output;
    float2 uv = float2((id << 1) & 2, id & 2);
    output.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = uv;
    return output;
}

float4 pixel_main(Output input) : SV_Target {
    return float4(adjusted(frame.Sample(frame_sampler, input.uv).rgb), 1);
}

// Averages the whole footprint of a screen pixel, so an internal resolution above the window's becomes anti-aliasing.
float4 pixel_smooth(Output input) : SV_Target {
    float2 dx = ddx(input.uv);
    float2 dy = ddy(input.uv);
    float3 sum = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            sum += frame.SampleLevel(frame_sampler, input.uv + dx * ((x + 0.5) / 4 - 0.5) + dy * ((y + 0.5) / 4 - 0.5), 0).rgb;
        }
    }
    return float4(adjusted(sum / 16), 1);
}
)HLSL";

const char* kOverlayShaderSource = R"HLSL(
Texture2D overlay : register(t0);
SamplerState overlay_sampler : register(s0);

float4 pixel_main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return overlay.Sample(overlay_sampler, uv);
}
)HLSL";

const char* kClearShaderSource = R"HLSL(
cbuffer ClearConstants : register(b0) {
    float4 color;
    float4 depth;
};

struct Output {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

Output pixel_main(float4 position : SV_Position, float2 uv : TEXCOORD0) {
    Output output;
    output.color = color;
    output.depth = depth.x;
    return output;
}
)HLSL";

const char* kCopyShaderSource = R"HLSL(
Texture2D color_source : register(t0);
Texture2D<float> depth_source : register(t1);
SamplerState source_sampler : register(s0);

cbuffer CopyConstants : register(b0) {
    uint4 params;
    float4 region;
    uint4 filter;
    float4 rows;
    float4 extent;
};

float4 convert(uint4 raw) {
    if (params.z == 0) {
        raw.a = 255;
    }
    if (params.y != 0) {
        const float4 y_const = float4(66, 129, 25, 16);
        const float4 u_const = float4(-38, -74, 112, 128);
        const float4 v_const = float4(112, -94, -18, 128);
        uint3 yuv = uint3(dot(y_const, float4(raw.rgb, 256)), dot(u_const, float4(raw.rgb, 256)), dot(v_const, float4(raw.rgb, 256)));
        raw.rgb = min((yuv >> 8) + ((yuv >> 7) & 1u), uint3(255, 255, 255));
    }
    float4 value = float4(raw) / 255.0;
    switch (params.x) {
    case 0: {
        float red = float(raw.r & 0xF0u) / 240.0;
        return float4(red, red, red, red);
    }
    case 1:
    case 8: return value.rrrr;
    case 2: {
        float2 red_alpha = float2(raw.ra & 0xF0u) / 240.0;
        return red_alpha.rrrg;
    }
    case 3: return value.rrra;
    case 4: {
        float2 red_blue = float2(raw.rb & 0xF8u) / 248.0;
        float green = float(raw.g & 0xFCu) / 252.0;
        return float4(red_blue.r, green, red_blue.g, 1.0);
    }
    case 5: return float4(float3(raw.rgb & 0xF8u) / 248.0, float(raw.a & 0xE0u) / 224.0);
    case 7: return value.aaaa;
    case 9: return value.gggg;
    case 10: return value.bbbb;
    case 11: return value.rrrg;
    case 12: return value.gggb;
    case 13: return float4(value.rgb, 1.0);
    default: return value;
    }
}

uint4 sample_efb(float2 position, float offset) {
    float y = clamp(position.y + offset, rows.y, rows.z);
    if (params.w != 0) {
        uint depth = min(uint(saturate(1.0 - depth_source.Load(int3(int(position.x), int(y), 0))) * 16777216.0), 0xFFFFFFu);
        return uint4((depth >> 16) & 255u, (depth >> 8) & 255u, depth & 255u, 255u);
    }
    return uint4(round(saturate(color_source.SampleLevel(source_sampler, float2(position.x, y) / extent.xy, 0)) * 255.0));
}

float4 pixel_main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 source = region.xy + uv * region.zw;
    uint4 current = sample_efb(source, 0.0);
    uint3 combined = current.rgb * filter.y;
    if (filter.x != 0 || filter.z != 0) {
        combined += sample_efb(source, -1.0).rgb * filter.x + sample_efb(source, 1.0).rgb * filter.z;
    }
    uint4 raw = uint4(combined >> 6, current.a);
    if (filter.w != 0) {
        raw &= 0x1FFu;
    }
    raw = min(raw, uint4(255, 255, 255, 255));
    if (rows.x != 1.0) {
        raw.rgb = uint3(round(pow(float3(raw.rgb) / 255.0, rows.x) * 255.0));
    }
    return convert(raw);
}
)HLSL";

const char* kShaderSource = R"HLSL(
cbuffer Constants : register(b0) {
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

Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
Texture2D t2 : register(t2);
Texture2D t3 : register(t3);
Texture2D t4 : register(t4);
Texture2D t5 : register(t5);
Texture2D t6 : register(t6);
Texture2D t7 : register(t7);
SamplerState s0 : register(s0);
SamplerState s1 : register(s1);
SamplerState s2 : register(s2);
SamplerState s3 : register(s3);
SamplerState s4 : register(s4);
SamplerState s5 : register(s5);
SamplerState s6 : register(s6);
SamplerState s7 : register(s7);

struct VertexInput {
    float4 position : POSITION;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float3 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float3 uv2 : TEXCOORD2;
    float3 uv3 : TEXCOORD3;
    float3 uv4 : TEXCOORD4;
    float3 uv5 : TEXCOORD5;
    float3 uv6 : TEXCOORD6;
    float3 uv7 : TEXCOORD7;
};

struct PixelInput {
    float4 position : SV_Position;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float3 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float3 uv2 : TEXCOORD2;
    float3 uv3 : TEXCOORD3;
    float3 uv4 : TEXCOORD4;
    float3 uv5 : TEXCOORD5;
    float3 uv6 : TEXCOORD6;
    float3 uv7 : TEXCOORD7;
};

PixelInput vertex_main(VertexInput input) {
    PixelInput output;
    output.position = input.position;
    output.c0 = input.c0;
    output.c1 = input.c1;
    output.uv0 = input.uv0;
    output.uv1 = input.uv1;
    output.uv2 = input.uv2;
    output.uv3 = input.uv3;
    output.uv4 = input.uv4;
    output.uv5 = input.uv5;
    output.uv6 = input.uv6;
    output.uv7 = input.uv7;
    return output;
}

cbuffer Motion : register(b1) {
    row_major float4x4 motion[8];
};

PixelInput vertex_moved(VertexInput input, float4 moved : MOVED) {
    if (moved.w < -1.5e30) {
        input.position = mul(motion[uint(moved.x)], input.position);
    } else if (moved.w > -1e29) {
        input.position = moved;
    }
    return vertex_main(input);
}

float2 projected(float3 uv) {
    return uv.z == 0.0 ? clamp(uv.xy / 2.0, -1.0, 1.0) : uv.xy / uv.z;
}

float2 select_uv(PixelInput p, uint i) {
    switch (i) {
    case 0: return projected(p.uv0);
    case 1: return projected(p.uv1);
    case 2: return projected(p.uv2);
    case 3: return projected(p.uv3);
    case 4: return projected(p.uv4);
    case 5: return projected(p.uv5);
    case 6: return projected(p.uv6);
    default: return projected(p.uv7);
    }
}

float4 sample_map(uint m, float2 uv, float2 dx, float2 dy) {
    switch (m) {
    case 0: return t0.SampleGrad(s0, uv, dx, dy);
    case 1: return t1.SampleGrad(s1, uv, dx, dy);
    case 2: return t2.SampleGrad(s2, uv, dx, dy);
    case 3: return t3.SampleGrad(s3, uv, dx, dy);
    case 4: return t4.SampleGrad(s4, uv, dx, dy);
    case 5: return t5.SampleGrad(s5, uv, dx, dy);
    case 6: return t6.SampleGrad(s6, uv, dx, dy);
    default: return t7.SampleGrad(s7, uv, dx, dy);
    }
}

static const int kFractions[8] = {255, 223, 191, 159, 128, 96, 64, 32};

int4 quantized(float4 value) {
    return int4(round(value * 255.0));
}

int3 konst_color(uint sel, int4 k[4]) {
    if (sel < 8) {
        return int3(1, 1, 1) * kFractions[sel];
    }
    if (sel >= 12 && sel < 16) {
        return k[sel - 12].rgb;
    }
    if (sel >= 16) {
        uint index = (sel - 16) & 3;
        uint component = (sel - 16) >> 2;
        return int3(1, 1, 1) * k[index][component];
    }
    return int3(0, 0, 0);
}

int konst_alpha(uint sel, int4 k[4]) {
    if (sel < 8) {
        return kFractions[sel];
    }
    if (sel >= 16) {
        uint index = (sel - 16) & 3;
        uint component = (sel - 16) >> 2;
        return k[index][component];
    }
    return 0;
}

int3 color_input(uint sel, int4 r[4], int4 tex, int4 ras, int3 kc) {
    switch (sel) {
    case 0: return r[0].rgb;
    case 1: return r[0].aaa;
    case 2: return r[1].rgb;
    case 3: return r[1].aaa;
    case 4: return r[2].rgb;
    case 5: return r[2].aaa;
    case 6: return r[3].rgb;
    case 7: return r[3].aaa;
    case 8: return tex.rgb;
    case 9: return tex.aaa;
    case 10: return ras.rgb;
    case 11: return ras.aaa;
    case 12: return int3(255, 255, 255);
    case 13: return int3(128, 128, 128);
    case 14: return kc;
    default: return int3(0, 0, 0);
    }
}

int alpha_input(uint sel, int4 r[4], int4 tex, int4 ras, int ka) {
    switch (sel) {
    case 0: return r[0].a;
    case 1: return r[1].a;
    case 2: return r[2].a;
    case 3: return r[3].a;
    case 4: return tex.a;
    case 5: return ras.a;
    case 6: return ka;
    default: return 0;
    }
}

bool compare_alpha(uint mode, uint value, uint reference) {
    switch (mode) {
    case 0: return false;
    case 1: return value < reference;
    case 2: return value == reference;
    case 3: return value <= reference;
    case 4: return value > reference;
    case 5: return value != reference;
    case 6: return value >= reference;
    default: return true;
    }
}

bool compare_inputs(uint mode, bool equal, int3 a, int3 b) {
    int left = a.r;
    int right = b.r;
    if (mode == 1) {
        left = a.r + a.g * 256;
        right = b.r + b.g * 256;
    } else if (mode == 2) {
        left = a.r + a.g * 256 + a.b * 65536;
        right = b.r + b.g * 256 + b.b * 65536;
    }
    return equal ? left == right : left > right;
}

int3 compare_color(uint mode, bool equal, int3 a, int3 b, int3 c) {
    if (mode == 3) {
        bool3 passed = equal ? (a == b) : (a > b);
        return int3(passed.r ? c.r : 0, passed.g ? c.g : 0, passed.b ? c.b : 0);
    }
    return compare_inputs(mode, equal, a, b) ? c : int3(0, 0, 0);
}

int4 apply_swap(int4 value, uint table) {
    uint packed = swaps[table];
    int components[4] = {value.r, value.g, value.b, value.a};
    return int4(components[packed & 3], components[(packed >> 2) & 3], components[(packed >> 4) & 3], components[(packed >> 6) & 3]);
}

int3 combine(int3 a, int3 b, int3 c, int3 d, uint bias_code, bool subtract, uint scale_code) {
    int bias = bias_code == 1 ? 128 : (bias_code == 2 ? -128 : 0);
    int shift = scale_code == 1 ? 1 : (scale_code == 2 ? 2 : 0);
    int rounding = scale_code == 3 ? 0 : (subtract ? 127 : 128);
    int3 lerp = ((((a << 8) + (b - a) * (c + (c >> 7))) << shift) + rounding) >> 8;
    int3 base = (d + bias) << shift;
    int3 value = subtract ? base - lerp : base + lerp;
    return scale_code == 3 ? value >> 1 : value;
}

static const int kWraps[5] = {256 << 7, 128 << 7, 64 << 7, 32 << 7, 16 << 7};
static const int kFormatShifts[4] = {0, 3, 4, 5};
static const int kBumpShifts[4] = {0, 5, 4, 3};

int2 fixpoint(PixelInput p, uint coord) {
    return int2(select_uv(p, coord) * texdims[coord].zw * 128.0);
}

int4 sample_fixed(uint map, int2 coords, float2 dx, float2 dy, uint coord) {
    float2 size = texdims[map].xy;
    float2 ratio = texdims[coord].zw / size;
    return quantized(sample_map(map, float2(coords) / (size * 128.0), dx * ratio, dy * ratio));
}

struct PixelOutput {
    float4 color : SV_Target0;
    float4 blend : SV_Target1;
};

PixelOutput pixel_main(PixelInput p) {
    float2 gradient_x[8] = {ddx(projected(p.uv0)), ddx(projected(p.uv1)), ddx(projected(p.uv2)), ddx(projected(p.uv3)), ddx(projected(p.uv4)), ddx(projected(p.uv5)), ddx(projected(p.uv6)), ddx(projected(p.uv7))};
    float2 gradient_y[8] = {ddy(projected(p.uv0)), ddy(projected(p.uv1)), ddy(projected(p.uv2)), ddy(projected(p.uv3)), ddy(projected(p.uv4)), ddy(projected(p.uv5)), ddy(projected(p.uv6)), ddy(projected(p.uv7))};
    int4 k[4] = {quantized(konst[0]), quantized(konst[1]), quantized(konst[2]), quantized(konst[3])};
    int4 r[4] = {quantized(initial[0]), quantized(initial[1]), quantized(initial[2]), quantized(initial[3])};
    int4 c0 = quantized(p.c0);
    int4 c1 = quantized(p.c1);
    uint count = header.x;
    if (count == 0) {
        r[0] = c0;
    }
    uint indirect_stages = indirect.x & 7;
    uint texgens = (indirect.x >> 8) & 15;
    int3 indirect_texel[4] = {int3(0, 0, 0), int3(0, 0, 0), int3(0, 0, 0), int3(0, 0, 0)};
    for (uint j = 0; j < indirect_stages && j < 4; j++) {
        uint reference = indirect.y >> (6 * j);
        uint map = reference & 7;
        uint coord = (reference >> 3) & 7;
        if (coord >= texgens) {
            coord = 0;
        }
        uint scales = j < 2 ? indirect.z : indirect.w;
        uint shift_s = (scales >> ((j & 1) * 8)) & 15;
        uint shift_t = (scales >> ((j & 1) * 8 + 4)) & 15;
        int2 base = fixpoint(p, coord);
        indirect_texel[j] = sample_fixed(map, int2(base.x >> shift_s, base.y >> shift_t), gradient_x[coord], gradient_y[coord], coord).abg;
    }
    int2 tevcoord = int2(0, 0);
    int alphabump = 0;
    for (uint i = 0; i < count; i++) {
        uint ce = stage[i].x;
        uint ae = stage[i].y;
        uint order = stage[i].z;
        uint ksel = stage[i].w;
        uint coord = (order >> 3) & 7;
        if (coord >= texgens) {
            coord = 0;
        }
        uint ind = tevind[i].x;
        uint bt = ind & 3;
        uint format = (ind >> 2) & 3;
        uint bias = (ind >> 4) & 7;
        uint bump = (ind >> 7) & 3;
        uint matrix_index = (ind >> 9) & 3;
        uint matrix_id = (ind >> 11) & 3;
        uint wrap_s = (ind >> 13) & 7;
        uint wrap_t = (ind >> 16) & 7;
        bool has_stage = bt < indirect_stages;
        int2 base = fixpoint(p, coord);
        int2 offset = int2(0, 0);
        if (has_stage && bump != 0) {
            alphabump = (indirect_texel[bt][bump - 1] << kBumpShifts[format]) & 248;
        }
        if (has_stage && matrix_index != 0) {
            int3 crd = indirect_texel[bt] >> kFormatShifts[format];
            int add = format == 0 ? -128 : 1;
            if ((bias & 1) != 0) crd.x += add;
            if ((bias & 2) != 0) crd.y += add;
            if ((bias & 4) != 0) crd.z += add;
            uint m = 2 * (matrix_index - 1);
            if (matrix_id == 0) {
                offset = int2(dot(indmtx[m].xyz, crd), dot(indmtx[m + 1].xyz, crd)) >> 3;
            } else if (matrix_id == 1) {
                offset = (base * crd.xx) >> 8;
            } else if (matrix_id == 2) {
                offset = (base * crd.yy) >> 8;
            }
            int shift = indmtx[m].w;
            offset = shift >= 0 ? (offset >> shift) : (offset << (-shift));
        }
        int2 wrapped;
        wrapped.x = wrap_s == 0 ? base.x : (wrap_s >= 6 ? 0 : (base.x & (kWraps[clamp(wrap_s, 1u, 5u) - 1] - 1)));
        wrapped.y = wrap_t == 0 ? base.y : (wrap_t >= 6 ? 0 : (base.y & (kWraps[clamp(wrap_t, 1u, 5u) - 1] - 1)));
        tevcoord = ((ind >> 20) & 1) != 0 ? tevcoord + wrapped + offset : wrapped + offset;
        tevcoord = (tevcoord << 8) >> 8;
        int4 tex = int4(255, 255, 255, 255);
        if ((order & 0x40) != 0) {
            tex = texgens == 0 ? int4(0, 0, 0, 0) : apply_swap(sample_fixed(order & 7, tevcoord, gradient_x[coord], gradient_y[coord], coord), (ae >> 2) & 3);
        }
        int4 ras = int4(0, 0, 0, 0);
        uint chan = (order >> 7) & 7;
        if (chan == 0) {
            ras = c0;
        } else if (chan == 1) {
            ras = c1;
        } else if (chan == 5) {
            ras = int4(1, 1, 1, 1) * alphabump;
        } else if (chan == 6) {
            ras = int4(1, 1, 1, 1) * (alphabump | (alphabump >> 5));
        }
        ras = apply_swap(ras, ae & 3);
        int3 kc = konst_color(ksel & 31, k);
        int ka = konst_alpha((ksel >> 5) & 31, k);
        int3 a = color_input((ce >> 12) & 15, r, tex, ras, kc) & 255;
        int3 b = color_input((ce >> 8) & 15, r, tex, ras, kc) & 255;
        int3 c = color_input((ce >> 4) & 15, r, tex, ras, kc) & 255;
        int3 d = color_input(ce & 15, r, tex, ras, kc);
        uint bias_code = (ce >> 16) & 3;
        uint scale_code = (ce >> 20) & 3;
        int3 color;
        if (bias_code == 3) {
            color = d + compare_color(scale_code, ((ce >> 18) & 1) != 0, a, b, c);
        } else {
            color = combine(a, b, c, d, bias_code, ((ce >> 18) & 1) != 0, scale_code);
        }
        color = ((ce >> 19) & 1) != 0 ? clamp(color, 0, 255) : clamp(color, -1024, 1023);
        int aa = alpha_input((ae >> 13) & 7, r, tex, ras, ka) & 255;
        int ab = alpha_input((ae >> 10) & 7, r, tex, ras, ka) & 255;
        int ac = alpha_input((ae >> 7) & 7, r, tex, ras, ka) & 255;
        int ad = alpha_input((ae >> 4) & 7, r, tex, ras, ka);
        uint abias_code = (ae >> 16) & 3;
        uint ascale_code = (ae >> 20) & 3;
        int alpha;
        if (abias_code == 3) {
            bool aequal = ((ae >> 18) & 1) != 0;
            bool apass = ascale_code == 3 ? (aequal ? aa == ab : aa > ab) : compare_inputs(ascale_code, aequal, a, b);
            alpha = ad + (apass ? ac : 0);
        } else {
            alpha = combine(int3(aa, 0, 0), int3(ab, 0, 0), int3(ac, 0, 0), int3(ad, 0, 0), abias_code, ((ae >> 18) & 1) != 0, ascale_code).x;
        }
        alpha = ((ae >> 19) & 1) != 0 ? clamp(alpha, 0, 255) : clamp(alpha, -1024, 1023);
        uint color_dest = (ce >> 22) & 3;
        uint alpha_dest = (ae >> 22) & 3;
        r[color_dest].rgb = color;
        r[alpha_dest].a = alpha;
    }
    int4 result = r[0];
    if (count > 0) {
        uint last_color = (stage[max(count, 1u) - 1].x >> 22) & 3;
        uint last_alpha = (stage[max(count, 1u) - 1].y >> 22) & 3;
        result = int4(r[last_color].rgb, r[last_alpha].a);
    }
    result &= 255;
    if ((header.w & 2) != 0) {
        int2 dither = int2(p.position.xy) & 1;
        result.rgb = (result.rgb - (result.rgb >> 6)) + (dither.x ^ dither.y) * 2 + dither.y;
    }
    uint fog_select = uint(fog_integer.x) & 15;
    uint fog_type = fog_select >> 1;
    if (fog_type != 0) {
        int z = clamp(int((1.0 - p.position.z) * 16777216.0), 0, 0xFFFFFF);
        float ze = (fog_select & 1) == 0 ? (fog.x * 16777216.0) / float(fog_integer.y - (z >> fog_integer.w)) : fog.x * float(z) / 16777216.0;
        if ((fog_integer.x & 16) != 0) {
            float offset = (2.0 * (p.position.x / fog.w)) - 1.0 - fog.z;
            float index = clamp(9.0 - abs(offset) * 9.0, 0.0, 9.0);
            uint lower = uint(index);
            uint upper = min(lower + 1, 9u);
            float k = lerp(fog_range[lower >> 2][lower & 3], fog_range[upper >> 2][upper & 3], frac(index));
            ze *= sqrt(offset * offset + k * k) / k;
        }
        float amount = clamp(ze - fog.y, 0.0, 1.0);
        if (fog_type == 4) {
            amount = 1.0 - exp2(-8.0 * amount);
        } else if (fog_type == 5) {
            amount = 1.0 - exp2(-8.0 * amount * amount);
        } else if (fog_type == 6) {
            amount = exp2(-8.0 * (1.0 - amount));
        } else if (fog_type == 7) {
            amount = exp2(-8.0 * (1.0 - amount) * (1.0 - amount));
        }
        int weight = int(round(amount * 256.0));
        int3 fog_color = int3((fog_integer.z >> 16) & 255, (fog_integer.z >> 8) & 255, fog_integer.z & 255);
        result.rgb = (result.rgb * (256 - weight) + fog_color * weight) >> 8;
    }
    uint compare = header.y;
    uint value = (uint)result.a;
    bool first = compare_alpha((compare >> 16) & 7, value, compare & 255);
    bool second = compare_alpha((compare >> 19) & 7, value, (compare >> 8) & 255);
    uint logic = (compare >> 22) & 3;
    bool accepted = logic == 0 ? (first && second) : (logic == 1 ? (first || second) : (logic == 2 ? (first != second) : (first == second)));
    if (!accepted) {
        discard;
    }
    PixelOutput output;
    output.blend = float4(result) / 255.0;
    uint source = (header.w >> 2) & 3;
    int4 written = source == 0 ? result : (source == 1 ? int4(0, 0, 0, 0) : (source == 2 ? int4(255, 255, 255, 255) : 255 - result));
    output.color = float4(written) / 255.0;
    if ((header.z & 0x100) != 0) {
        output.color.a = float(header.z & 255) / 255.0;
    }
    if ((header.w & 1) != 0) {
        output.color.rgb = float3(written.rgb >> 2) / 63.0;
        output.color.a = float(((header.z & 0x100) != 0 ? int(header.z & 255) : written.a) >> 2) / 63.0;
    }
    return output;
}
)HLSL";

template <typename T>
void release(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

struct CachedTexture {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    uint64_t hash = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t levels = 0;
    uint64_t verified_epoch = ~0ull;
    bool mipmaps = false;
    bool custom = false;
};

struct CustomTexture {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
};

struct TexturePack {
    bool dump = false;
    bool load = false;
    std::filesystem::path dump_directory;
    custom_textures::Index index;
    std::unordered_set<std::string> dumped;
    std::unordered_map<std::string, CustomTexture> loaded;
};

struct CopiedTexture {
    ID3D11Texture2D* converted = nullptr;
    ID3D11ShaderResourceView* converted_view = nullptr;
    ID3D11RenderTargetView* converted_target = nullptr;
    uint32_t converted_width = 0;
    uint32_t converted_height = 0;
    ID3D11ShaderResourceView* view = nullptr;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t bytes = 0;
    uint64_t guest_hash = 0;
    uint32_t pending_writes = 0;
    uint32_t copied_frame = 0;
    uint32_t copied_format = 0;
};

struct PendingWriteBack {
    uint32_t atlas_x = 0;
    uint32_t atlas_y = 0;
    uint32_t address = 0;
    uint32_t start = 0;
    uint32_t end = 0;
    uint32_t row_bytes = 0;
    uint32_t logical_width = 0;
    uint32_t logical_height = 0;
    uint32_t texture_format = 0;
    uint64_t frame = 0;
};

constexpr size_t kMaxBetween = 3;
constexpr size_t kMaxCameras = 8;

struct Device {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11VertexShader* vertex_shader = nullptr;
    ID3D11PixelShader* pixel_shader = nullptr;
    ID3D11PixelShader* early_pixel_shader = nullptr;
    ID3D11InputLayout* layout = nullptr;
    ID3D11InputLayout* trimmed_layouts[8] = {};
    ID3D11Buffer* zero_buffer = nullptr;
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11VertexShader* moved_shader = nullptr;
    ID3D11InputLayout* moved_layout = nullptr;
    ID3D11Buffer* moved_buffer = nullptr;
    ID3D11Buffer* motion = nullptr;
    ID3DBlob* run_code = nullptr;
    ID3D11VertexShader* run_shader = nullptr;
    ID3D11InputLayout* run_layouts[2][2][9] = {};
    ID3D11Buffer* run_vertices = nullptr;
    ID3D11Buffer* run_indices = nullptr;
    ID3D11Buffer* run_rows = nullptr;
    ID3D11ShaderResourceView* run_rows_view = nullptr;
    ID3D11Buffer* run_numbers = nullptr;
    ID3D11RasterizerState* run_culling[2] = {};
    ID3D11Buffer* kept_vertices = nullptr;
    ID3D11Buffer* kept_indices = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11RenderTargetView* target_view = nullptr;
    ID3D11Texture2D* depth = nullptr;
    ID3D11DepthStencilView* depth_view = nullptr;
    ID3D11ShaderResourceView* depth_resource = nullptr;
    ID3D11ShaderResourceView* target_resource = nullptr;
    ID3D11RenderTargetView* frame_target = nullptr;
    ID3D11Texture2D* frame = nullptr;
    ID3D11ShaderResourceView* frame_view = nullptr;
    ID3D11Texture2D* interpolated[kMaxBetween] = {};
    ID3D11ShaderResourceView* interpolated_view[kMaxBetween] = {};
    ID3D11RenderTargetView* interpolated_target[kMaxBetween] = {};
    uint32_t interpolated_width = 0;
    uint32_t interpolated_height = 0;
    ID3D11Texture2D* write_back = nullptr;
    ID3D11ShaderResourceView* write_back_view = nullptr;
    ID3D11RenderTargetView* write_back_target = nullptr;
    ID3D11Texture2D* staging_atlas = nullptr;
    uint32_t atlas_x = 0;
    uint32_t atlas_y = 0;
    uint32_t atlas_row = 0;
    std::vector<PendingWriteBack> write_backs;
    uint32_t frame_width = 0;
    uint32_t frame_height = 0;
    IDXGISwapChain1* swapchain = nullptr;
    ID3D11RenderTargetView* backbuffer_view = nullptr;
    UINT swapchain_width = 0;
    UINT swapchain_height = 0;
    ID3D11VertexShader* present_vertex = nullptr;
    ID3D11PixelShader* present_pixel = nullptr;
    ID3D11PixelShader* present_smooth = nullptr;
    int anisotropy = 1;
    ID3D11SamplerState* present_sampler = nullptr;
    ID3D11PixelShader* copy_pixel = nullptr;
    ID3D11Buffer* present_constants = nullptr;
    ID3D11PixelShader* clear_pixel = nullptr;
    ID3D11Buffer* clear_constants = nullptr;
    ID3D11BlendState* clear_blend[16] = {};
    ID3D11DepthStencilState* clear_depth[2] = {};
    ID3D11RasterizerState* present_rasterizer = nullptr;
    ID3D11PixelShader* overlay_pixel = nullptr;
    ID3D11BlendState* overlay_blend = nullptr;
    ID3D11Texture2D* overlay = nullptr;
    ID3D11ShaderResourceView* overlay_view = nullptr;
    uint32_t overlay_width = 0;
    uint32_t overlay_height = 0;
    uint64_t overlay_version = 0;
    bool overlay_visible = false;
    bool overlay_failed = false;
    ID3D11Texture2D* hud = nullptr;
    ID3D11ShaderResourceView* hud_view = nullptr;
    uint32_t hud_width = 0;
    uint32_t hud_height = 0;
    uint64_t hud_version = 0;
    bool hud_visible = false;
    perf::Corner hud_corner = perf::Corner::TopLeft;
    ID3D11RasterizerState* rasterizer = nullptr;
    std::map<uint32_t, ID3D11BlendState*> blend_states;
    std::map<uint32_t, ID3D11DepthStencilState*> depth_states;
    std::map<uint64_t, ID3D11SamplerState*> samplers;
    std::map<uint64_t, CachedTexture> textures;
    std::map<uint32_t, CopiedTexture> copies;
    bool failed = false;
    bool ready = false;
};

struct BatchState {
    Constants constants;
    // Owned references: a later texture lookup may replace the cache entry a view came from.
    ID3D11ShaderResourceView* views[kTextureMaps] = {};
    ID3D11SamplerState* samplers[kTextureMaps] = {};
    uint32_t blend = 0;
    uint32_t depth = 0;
    bool early_depth = false;
    uint32_t top_left = 0;
    uint32_t bottom_right = 0;
    uint32_t scissor_offset = 0;
};

struct PendingBatch : BatchState {
    size_t begin = 0;
};

struct RecordedDraw {
    uint64_t key = 0;
    uint64_t exact = 0;
    uint32_t camera = 0;
    uint32_t first_position = 0;
    uint32_t positions = 0;
    size_t first_vertex = 0;
    uint32_t vertices = 0;
};

struct CopyCommand {
    ID3D11RenderTargetView* target = nullptr;
    uint32_t target_width = 0;
    uint32_t target_height = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    uint32_t format = 0;
    bool intensity = false;
    bool alpha = false;
    bool depth = false;
    CopyFilter filter;
};

struct ClearCommand {
    UINT mask = 0;
    bool depth_enable = false;
    float constants[8] = {};
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

constexpr uint32_t kNoPalette = ~0u;

struct RunDraw {
    ID3D11InputLayout* layout = nullptr;
    uint32_t stride = 0;
    uint32_t cull = 0;
    uint32_t vertex_offset = 0;
    uint32_t first_index = 0;
    uint32_t indices = 0;
    uint32_t row = 0;
    uint32_t rows = 0;
    uint32_t palette = kNoPalette;
    uint32_t matrix = 0;
    uint32_t camera = 0;
    uint64_t key = 0;
    bool kept = false;
};

struct RecordedRun {
    RunDraw draw;
    uint32_t match = ~0u;
};

struct RecordedCommand {
    enum Kind { kBatch, kCopy, kClear, kRuns } kind = kBatch;
    BatchState batch;
    size_t begin = 0;
    size_t end = 0;
    CopyCommand copy;
    ClearCommand clear;
};

struct ReplayTarget {
    ID3D11Texture2D* before = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frame = ~0ull;
};

struct RecordedTarget {
    ID3D11RenderTargetView* target = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    bool twin = false;
};

struct FrameRecord {
    std::vector<uint16_t> source_of;
    std::vector<std::array<float, 4>> positions;
    std::vector<RecordedDraw> draws;
    std::vector<RecordedCommand> commands;
    std::vector<RecordedTarget> targets;
    std::vector<RecordedRun> runs;
    std::vector<std::array<float, 4>> rows;
};

[[gnu::used]] const char kBuildSalt[] = "SALT23076";

Device g_device;
PendingBatch g_pending;
std::vector<ScreenVertex> g_triangles;
size_t g_frame_vertices = 0;
bool g_recording = false;
bool g_record_broken = false;
FrameRecord g_record;
FrameRecord g_previous;
uint32_t g_interpolated_ready = 0;
std::atomic<uint32_t> g_interpolated_frames{0};
TexturePack g_pack;
uint32_t g_custom_maps = 0;

void flush_pending();
bool g_log_gx = std::getenv("WP_LOG_GX") != nullptr;
bool g_logged_palette = false;
bool g_logged_copy_format = false;
uint64_t g_frame = 0;
constexpr uint64_t kWriteBackFrames = 2;
uint64_t g_texture_epoch = 0;
int g_scale = 1;
uint32_t g_vertex_cursor = kVertexCapacity;
uint32_t g_trimmed_cursor = kVertexBytes;
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;
uint32_t g_write_backs = 0;
double g_write_back_seconds = 0.0;

int scaled(int value) {
    return value * g_scale;
}

struct ShaderCache {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::string> code;
    std::vector<std::string> variants;
    FILE* file = nullptr;
    uint32_t compiled = 0;
    uint32_t reused = 0;
    double compile_seconds = 0.0;
};
ShaderCache g_shader_cache;

uint64_t shader_hash(const char* source, const char* entry, const char* profile) {
    uint64_t hash = hash_bytes(reinterpret_cast<const uint8_t*>(source), std::strlen(source));
    hash = (hash ^ hash_bytes(reinterpret_cast<const uint8_t*>(entry), std::strlen(entry))) * 1099511628211ull;
    return (hash ^ hash_bytes(reinterpret_cast<const uint8_t*>(profile), std::strlen(profile))) * 1099511628211ull;
}

void store_shader(uint64_t hash, const std::string& variant, const void* code, size_t size) {
    std::lock_guard<std::mutex> lock(g_shader_cache.mutex);
    if (!g_shader_cache.code.try_emplace(hash, static_cast<const char*>(code), size).second || !g_shader_cache.file) {
        return;
    }
    const uint32_t sizes[2] = {static_cast<uint32_t>(variant.size()), static_cast<uint32_t>(size)};
    std::fwrite(&hash, sizeof hash, 1, g_shader_cache.file);
    std::fwrite(sizes, sizeof sizes, 1, g_shader_cache.file);
    std::fwrite(variant.data(), 1, variant.size(), g_shader_cache.file);
    std::fwrite(code, 1, size, g_shader_cache.file);
    std::fflush(g_shader_cache.file);
}

bool compile(const char* source, const char* entry, const char* profile, ID3DBlob** blob, const std::string& variant = {}) {
    uint64_t hash = shader_hash(source, entry, profile);
    {
        std::lock_guard<std::mutex> lock(g_shader_cache.mutex);
        auto found = g_shader_cache.code.find(hash);
        if (found != g_shader_cache.code.end() && SUCCEEDED(D3DCreateBlob(found->second.size(), blob))) {
            std::memcpy((*blob)->GetBufferPointer(), found->second.data(), found->second.size());
            g_shader_cache.reused++;
            return true;
        }
    }
    auto start = std::chrono::steady_clock::now();
    ID3DBlob* errors = nullptr;
    HRESULT result = D3DCompile(source, std::strlen(source), "gx", nullptr, nullptr, entry, profile, 0, 0, blob, &errors);
    if (FAILED(result)) {
        if (errors) {
            std::fprintf(stderr, "shader compile error: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        return false;
    }
    release(errors);
    store_shader(hash, variant, (*blob)->GetBufferPointer(), (*blob)->GetBufferSize());
    std::lock_guard<std::mutex> lock(g_shader_cache.mutex);
    g_shader_cache.compiled++;
    g_shader_cache.compile_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return true;
}

bool create_target() {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = scaled(kEfbWidth);
    description.Height = scaled(kEfbHeight);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device.device->CreateTexture2D(&description, nullptr, &g_device.target)) ||
        FAILED(g_device.device->CreateRenderTargetView(g_device.target, nullptr, &g_device.target_view)) ||
        FAILED(g_device.device->CreateShaderResourceView(g_device.target, nullptr, &g_device.target_resource))) {
        return false;
    }
    description.Format = DXGI_FORMAT_R32_TYPELESS;
    description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};
    depth_view.Format = DXGI_FORMAT_D32_FLOAT;
    depth_view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC depth_resource{};
    depth_resource.Format = DXGI_FORMAT_R32_FLOAT;
    depth_resource.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    depth_resource.Texture2D.MipLevels = 1;
    if (FAILED(g_device.device->CreateTexture2D(&description, nullptr, &g_device.depth)) ||
        FAILED(g_device.device->CreateDepthStencilView(g_device.depth, &depth_view, &g_device.depth_view)) ||
        FAILED(g_device.device->CreateShaderResourceView(g_device.depth, &depth_resource, &g_device.depth_resource))) {
        return false;
    }
    return true;
}

double lap(std::chrono::steady_clock::time_point& from) {
    auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - from).count();
    from = now;
    return seconds;
}

bool g_nvidia = false;

const char* kRunSource = R"(
Buffer<float4> rows : register(t0);

struct RunInput {
    float4 position : POSITION;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float4 normal : NORMAL;
    uint4 m0 : MATRIX0;
    uint4 m1 : MATRIX1;
    uint4 m2 : MATRIX2;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
    float4 t4 : TEXCOORD4;
    float4 t5 : TEXCOORD5;
    float4 t6 : TEXCOORD6;
    float4 t7 : TEXCOORD7;
    uint4 run : RUN;
};

struct PixelInput {
    float4 position : SV_Position;
    float4 c0 : COLOR0;
    float4 c1 : COLOR1;
    float3 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float3 uv2 : TEXCOORD2;
    float3 uv3 : TEXCOORD3;
    float3 uv4 : TEXCOORD4;
    float3 uv5 : TEXCOORD5;
    float3 uv6 : TEXCOORD6;
    float3 uv7 : TEXCOORD7;
};

float3 transformed(int at, float3 v) {
    float4 r0 = rows.Load(at);
    float4 r1 = rows.Load(at + 1);
    float4 r2 = rows.Load(at + 2);
    precise float3 result;
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

float light_factor(int at, uint control, float3 eye, float3 normal) {
    float4 a = rows.Load(at + 1);
    float4 b = rows.Load(at + 2);
    float4 c = rows.Load(at + 3);
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
        float distance = sqrt(square);
        toward = toward / distance;
        amount = max(0.0, dot(toward, direction));
        float cone = angle.x + angle.y * amount + angle.z * amount * amount;
        float fade = range.x + range.y * distance + range.z * square;
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

void light_colors(int light, int matrix_row, uint word, float3 eye, float3 raw, inout float4 c0, inout float4 c1) {
    float4 controls = rows.Load(light);
    int first = matrix_row + int(word >> 2);
    float4 n0 = rows.Load(first);
    float4 n1 = rows.Load(first + 1);
    float4 n2 = rows.Load(first + 2);
    float m[12] = {n0.x, n0.y, n0.z, n0.w, n1.x, n1.y, n1.z, n1.w, n2.x, n2.y, n2.z, n2.w};
    uint k = word & 3u;
    float3 normal = unit(float3(m[k] * raw.x + m[k + 1u] * raw.y + m[k + 2u] * raw.z,
                                m[k + 3u] * raw.x + m[k + 4u] * raw.y + m[k + 5u] * raw.z,
                                m[k + 6u] * raw.x + m[k + 7u] * raw.y + m[k + 8u] * raw.z));
    float4 given[2] = {c0, c1};
    float4 result[2];
    for (uint channel = 0u; channel < 2u; channel++) {
        float4 vertex = floor(given[channel] * 255.0 + 0.5);
        float4 material = rows.Load(light + 1 + int(channel));
        float4 ambient = rows.Load(light + 3 + int(channel));
        uint color = uint(channel == 0u ? controls.x : controls.y);
        uint alpha = uint(channel == 0u ? controls.z : controls.w);
        float3 color_material = (color & 1u) != 0u ? vertex.rgb : material.rgb;
        float alpha_material = (alpha & 1u) != 0u ? vertex.a : material.a;
        uint color_mask = light_mask(color);
        uint alpha_mask = light_mask(alpha);
        float3 sum = (color & 64u) != 0u ? vertex.rgb : ambient.rgb;
        float alpha_sum = (alpha & 64u) != 0u ? vertex.a : ambient.a;
        for (uint i = 0u; i < 8u; i++) {
            uint bit = 1u << i;
            int at = light + 8 + 4 * int(i);
            if ((color_mask & bit) != 0u) {
                sum += rows.Load(at).rgb * light_factor(at, color, eye, normal);
            }
            if ((alpha_mask & bit) != 0u) {
                alpha_sum += rows.Load(at).a * light_factor(at, alpha, eye, normal);
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

PixelInput vertex_run(RunInput input) {
    int run = int(input.run.x);
    float4 p0 = rows.Load(run);
    float4 p1 = rows.Load(run + 1);
    float4 p2 = rows.Load(run + 2);
    float4 p3 = rows.Load(run + 3);
    int origin = run + int(p3.z);
    precise float3 eye = transformed(origin + int(p1.w) + int(input.m0.x), input.position.xyz);
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
    precise float4 position;
    position.x = x * p2.x + w * p2.y;
    position.y = y * p2.z + w * p2.w;
    position.z = (w * p3.y - z * p3.x) / 16777215.0;
    position.w = w;
    uint setup = uint(p3.w);
    uint count = min(setup & 15u, 8u);
    bool dual = (setup & 16u) != 0u;
    uint indices[8] = {input.m0.y, input.m0.z, input.m0.w, input.m1.x, input.m1.y, input.m1.z, input.m1.w, input.m2.x};
    float4 inputs[8] = {input.t0, input.t1, input.t2, input.t3, input.t4, input.t5, input.t6, input.t7};
    float3 uv[8];
    for (uint k = 0; k < 8; k++) {
        uv[k] = float3(0.0, 0.0, 0.0);
    }
    for (uint i = 0; i < count; i++) {
        int place = run + 7 + 7 * int(i);
        float4 info = rows.Load(place);
        uint source = uint(info.x);
        uint flags = uint(info.y);
        float3 given = float3(0.0, 0.0, 1.0);
        if (source == 0u) {
            given = input.position.xyz;
        } else if (source == 1u) {
            given = input.normal.xyz;
        } else if (source >= 5u && source < 13u) {
            given.xy = inputs[source - 5u].xy;
        }
        if ((flags & 2u) == 0u) {
            given.z = 1.0;
        }
        float3 coord = transformed(origin + int(info.z) + int(indices[i]), given);
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
    PixelInput output;
    output.position = position;
    output.c0 = input.c0;
    output.c1 = input.c1;
    if ((setup & 32u) != 0u) {
        int light = run + 7 + 7 * int(count);
        if (p1.w == 0.0) {
            light_colors(light, origin + 66, (input.m0.x & 31u) * 3u, eye, input.normal.xyz, output.c0, output.c1);
        } else {
            light_colors(light, light + 5, 0u, eye, input.normal.xyz, output.c0, output.c1);
        }
    }
    output.uv0 = uv[0];
    output.uv1 = uv[1];
    output.uv2 = uv[2];
    output.uv3 = uv[3];
    output.uv4 = uv[4];
    output.uv5 = uv[5];
    output.uv6 = uv[6];
    output.uv7 = uv[7];
    return output;
}
)";

constexpr uint32_t kShaderCacheMagic = 0x43535057;
constexpr uint32_t kShaderCacheFormat = 1;

void open_shader_cache() {
    namespace fs = std::filesystem;
    const char* setting = std::getenv("WP_SHADER_CACHE");
    if (setting && std::strcmp(setting, "0") == 0) {
        return;
    }
    fs::path path = fs::path(game::description().settings_file).parent_path() / "shaders.bin";
    uint64_t sources = 0;
    for (const char* source : {kShaderSource, kRunSource, kPresentShaderSource, kOverlayShaderSource, kClearShaderSource, kCopyShaderSource}) {
        sources = (sources ^ hash_bytes(reinterpret_cast<const uint8_t*>(source), std::strlen(source))) * 1099511628211ull;
    }
    const uint32_t header[4] = {kShaderCacheMagic, kShaderCacheFormat, static_cast<uint32_t>(sources), static_cast<uint32_t>(sources >> 32)};
    std::string bytes;
    if (FILE* file = fs::exists(path) ? std::fopen(path.string().c_str(), "rb") : nullptr) {
        char buffer[1 << 16];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof buffer, file)) > 0) {
            bytes.append(buffer, read);
        }
        std::fclose(file);
    }
    size_t good = 0;
    if (bytes.size() >= sizeof header && std::memcmp(bytes.data(), header, sizeof header) == 0) {
        good = sizeof header;
        std::lock_guard<std::mutex> lock(g_shader_cache.mutex);
        while (bytes.size() - good >= 16) {
            uint64_t hash = 0;
            uint32_t sizes[2] = {};
            std::memcpy(&hash, bytes.data() + good, 8);
            std::memcpy(sizes, bytes.data() + good + 8, 8);
            size_t end = good + 16 + static_cast<size_t>(sizes[0]) + sizes[1];
            if (sizes[1] == 0 || end > bytes.size()) {
                break;
            }
            const char* variant = bytes.data() + good + 16;
            if (g_shader_cache.code.try_emplace(hash, variant + sizes[0], sizes[1]).second && sizes[0] > 0) {
                g_shader_cache.variants.emplace_back(variant, sizes[0]);
            }
            good = end;
        }
    }
    std::error_code error;
    if (good == 0) {
        fs::create_directories(path.parent_path(), error);
        g_shader_cache.file = std::fopen(path.string().c_str(), "wb");
        if (g_shader_cache.file) {
            std::fwrite(header, sizeof header, 1, g_shader_cache.file);
            std::fflush(g_shader_cache.file);
        }
    } else {
        if (good < bytes.size()) {
            fs::resize_file(path, good, error);
        }
        g_shader_cache.file = error ? nullptr : std::fopen(path.string().c_str(), "ab");
    }
    log::write("video", "shader cache %s: %u shaders, %u of them specialised pixel shaders", g_shader_cache.file ? path.string().c_str() : "not writable",
               static_cast<unsigned>(g_shader_cache.code.size()), static_cast<unsigned>(g_shader_cache.variants.size()));
}

constexpr uint32_t kRunVertexBytes = 16u << 20;
constexpr uint32_t kRunIndexBytes = 4u << 20;
constexpr uint32_t kRunRowCapacity = 1u << 16;

bool create_run_pipeline() {
    static bool tried = false;
    if (tried) {
        return g_device.run_shader != nullptr;
    }
    tried = true;
    ID3D11Device* device = g_device.device;
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    if (!g_device.zero_buffer || FAILED(device->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof options)) || !options.MapNoOverwriteOnDynamicBufferSRV) {
        log::write("video", "vertices stay on the processor: this Direct3D cannot append to a buffer that shaders read");
        return false;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    buffer.ByteWidth = kRunVertexBytes;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bool ok = SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &g_device.run_vertices));
    buffer.ByteWidth = kRunIndexBytes;
    buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ok = ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &g_device.run_indices));
    buffer.ByteWidth = kRunRowCapacity * sizeof(float[4]);
    buffer.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(device->CreateBuffer(&buffer, nullptr, &g_device.run_rows));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    view.Buffer.NumElements = kRunRowCapacity;
    ok = ok && SUCCEEDED(device->CreateShaderResourceView(g_device.run_rows, &view, &g_device.run_rows_view));
    std::vector<uint32_t> numbers(kRunRowCapacity);
    for (uint32_t i = 0; i < kRunRowCapacity; i++) {
        numbers[i] = i;
    }
    D3D11_BUFFER_DESC fixed{};
    fixed.ByteWidth = kRunRowCapacity * sizeof(uint32_t);
    fixed.Usage = D3D11_USAGE_IMMUTABLE;
    fixed.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA initial{numbers.data(), 0, 0};
    ok = ok && SUCCEEDED(device->CreateBuffer(&fixed, &initial, &g_device.run_numbers));
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = FALSE;
    raster.CullMode = D3D11_CULL_BACK;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &g_device.run_culling[0]));
    raster.CullMode = D3D11_CULL_FRONT;
    ok = ok && SUCCEEDED(device->CreateRasterizerState(&raster, &g_device.run_culling[1]));
    ok = ok && compile(kRunSource, "vertex_run", "vs_5_0", &g_device.run_code);
    if (ok && FAILED(device->CreateVertexShader(g_device.run_code->GetBufferPointer(), g_device.run_code->GetBufferSize(), nullptr, &g_device.run_shader))) {
        g_device.run_shader = nullptr;
    }
    if (!g_device.run_shader) {
        log::write("video", "vertices stay on the processor: the graphics card refused the vertex shader or its buffers");
    }
    return g_device.run_shader != nullptr;
}

constexpr uint32_t kKeptVertexBytes = 64u << 20;
constexpr uint32_t kKeptIndexBytes = 16u << 20;

bool create_kept_buffers() {
    static bool tried = false;
    if (tried) {
        return g_device.kept_indices != nullptr;
    }
    tried = true;
    D3D11_BUFFER_DESC buffer{};
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.ByteWidth = kKeptVertexBytes;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(g_device.device->CreateBuffer(&buffer, nullptr, &g_device.kept_vertices))) {
        g_device.kept_vertices = nullptr;
    }
    buffer.ByteWidth = kKeptIndexBytes;
    buffer.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (!g_device.kept_vertices || FAILED(g_device.device->CreateBuffer(&buffer, nullptr, &g_device.kept_indices))) {
        g_device.kept_indices = nullptr;
        log::write("video", "models are decoded on every image: the graphics card refused the buffers that would keep them");
    }
    return g_device.kept_indices != nullptr;
}

ID3D11InputLayout* run_layout(bool normal, bool indices, uint32_t inputs) {
    ID3D11InputLayout*& layout = g_device.run_layouts[normal][indices][inputs];
    if (layout) {
        return layout;
    }
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    UINT offset = 20;
    elements.push_back({"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, normal ? 0u : 1u, normal ? offset : 0u, D3D11_INPUT_PER_VERTEX_DATA, 0});
    offset += normal ? 12 : 0;
    for (UINT i = 0; i < 3; i++) {
        elements.push_back({"MATRIX", i, DXGI_FORMAT_R8G8B8A8_UINT, indices ? 0u : 1u, indices ? offset + 4 * i : 0u, D3D11_INPUT_PER_VERTEX_DATA, 0});
    }
    offset += indices ? 12 : 0;
    for (UINT i = 0; i < 8; i++) {
        bool present = i < inputs;
        elements.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32_FLOAT, present ? 0u : 1u, present ? offset + 8 * i : 0u, D3D11_INPUT_PER_VERTEX_DATA, 0});
    }
    elements.push_back({"RUN", 0, DXGI_FORMAT_R32_UINT, 2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1});
    g_device.device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), g_device.run_code->GetBufferPointer(), g_device.run_code->GetBufferSize(), &layout);
    return layout;
}

bool create_pipeline() {
    ID3DBlob* vertex_code = nullptr;
    ID3DBlob* pixel_code = nullptr;
    if (!compile(kShaderSource, "vertex_main", "vs_5_0", &vertex_code) || !compile(kShaderSource, "pixel_main", "ps_5_0", &pixel_code)) {
        return false;
    }
    std::string early_source = kShaderSource;
    early_source.replace(early_source.find("PixelOutput pixel_main("), 0, "[earlydepthstencil] ");
    ID3DBlob* early_code = nullptr;
    if (!compile(early_source.c_str(), "pixel_main", "ps_5_0", &early_code)) {
        release(vertex_code);
        release(pixel_code);
        return false;
    }
    bool ok = SUCCEEDED(g_device.device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &g_device.vertex_shader)) &&
              SUCCEEDED(g_device.device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &g_device.pixel_shader)) &&
              SUCCEEDED(g_device.device->CreatePixelShader(early_code->GetBufferPointer(), early_code->GetBufferSize(), nullptr, &g_device.early_pixel_shader));
    release(early_code);
    if (ok) {
        std::vector<D3D11_INPUT_ELEMENT_DESC> elements = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        for (UINT i = 0; i < 8; i++) {
            elements.push_back({"TEXCOORD", i, DXGI_FORMAT_R32G32B32_FLOAT, 0, 48 + 12 * i, D3D11_INPUT_PER_VERTEX_DATA, 0});
        }
        ok = SUCCEEDED(g_device.device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), vertex_code->GetBufferPointer(),
                                                         vertex_code->GetBufferSize(), &g_device.layout));
        bool trimmed = ok;
        for (UINT used = 0; trimmed && used < 8; used++) {
            std::vector<D3D11_INPUT_ELEMENT_DESC> kept = elements;
            for (UINT i = used; i < 8; i++) {
                kept[3 + i].InputSlot = 1;
                kept[3 + i].AlignedByteOffset = 0;
            }
            trimmed = SUCCEEDED(g_device.device->CreateInputLayout(kept.data(), static_cast<UINT>(kept.size()), vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(),
                                                                   &g_device.trimmed_layouts[used]));
        }
        static const float zeros[4] = {};
        D3D11_BUFFER_DESC blank{};
        blank.ByteWidth = sizeof zeros;
        blank.Usage = D3D11_USAGE_IMMUTABLE;
        blank.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA nothing{zeros, 0, 0};
        if (trimmed) {
            g_device.device->CreateBuffer(&blank, &nothing, &g_device.zero_buffer);
        }
        ID3DBlob* moved_code = nullptr;
        if (ok && compile(kShaderSource, "vertex_moved", "vs_5_0", &moved_code)) {
            elements.push_back({"MOVED", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0});
            if (FAILED(g_device.device->CreateVertexShader(moved_code->GetBufferPointer(), moved_code->GetBufferSize(), nullptr, &g_device.moved_shader)) ||
                FAILED(g_device.device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), moved_code->GetBufferPointer(),
                                                          moved_code->GetBufferSize(), &g_device.moved_layout))) {
                release(g_device.moved_shader);
            }
            release(moved_code);
        }
    }
    release(vertex_code);
    release(pixel_code);
    if (!ok) {
        return false;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = kVertexCapacity * sizeof(ScreenVertex);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_device.device->CreateBuffer(&buffer, nullptr, &g_device.vertex_buffer))) {
        return false;
    }
    buffer.ByteWidth = kVertexCapacity * sizeof(float[4]);
    if (g_device.moved_shader && FAILED(g_device.device->CreateBuffer(&buffer, nullptr, &g_device.moved_buffer))) {
        release(g_device.moved_shader);
    }
    D3D11_BUFFER_DESC motion{};
    motion.ByteWidth = sizeof(float[kMaxCameras][4][4]);
    motion.Usage = D3D11_USAGE_DEFAULT;
    motion.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (g_device.moved_shader && FAILED(g_device.device->CreateBuffer(&motion, nullptr, &g_device.motion))) {
        release(g_device.moved_shader);
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;
    raster.DepthClipEnable = FALSE;
    return SUCCEEDED(g_device.device->CreateRasterizerState(&raster, &g_device.rasterizer));
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

IDXGIAdapter* fastest_adapter() {
    IDXGIFactory6* factory = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    if (FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, __uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter)))) {
        adapter = nullptr;
    }
    factory->Release();
    return adapter;
}

void log_adapter() {
    IDXGIDevice* dxgi_device = nullptr;
    IDXGIAdapter* adapter = nullptr;
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(g_device.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&description))) {
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof name, nullptr, nullptr);
        LARGE_INTEGER version{};
        adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
        g_nvidia = description.VendorId == 0x10DE && !std::getenv("WP_MAP_CONSTANTS");
        log::write("video", "Direct3D 11 on %s, %llu MB, driver %u.%u.%u.%u", name, static_cast<unsigned long long>(description.DedicatedVideoMemory >> 20),
                   static_cast<unsigned>(HIWORD(version.HighPart)), static_cast<unsigned>(LOWORD(version.HighPart)), static_cast<unsigned>(HIWORD(version.LowPart)),
                   static_cast<unsigned>(LOWORD(version.LowPart)));
    }
    release(adapter);
    release(dxgi_device);
}

void preload_variants();

bool initialize() {
    if (g_device.ready) {
        return true;
    }
    if (g_device.failed) {
        return false;
    }
    g_scale = requested_scale();
    D3D_FEATURE_LEVEL level;
    // WP_D3D_WARP=1 renders with the Windows software rasterizer, to tell driver faults from renderer ones.
    D3D_DRIVER_TYPE driver = std::getenv("WP_D3D_WARP") ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    // WP_D3D_DEBUG=1 turns on the Direct3D debug layer (Windows optional feature "Graphics Tools").
    UINT device_flags = std::getenv("WP_D3D_DEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0;
    IDXGIAdapter* fastest = driver == D3D_DRIVER_TYPE_HARDWARE ? fastest_adapter() : nullptr;
    HRESULT result = E_FAIL;
    if (fastest) {
        result = D3D11CreateDevice(fastest, D3D_DRIVER_TYPE_UNKNOWN, nullptr, device_flags, nullptr, 0, D3D11_SDK_VERSION, &g_device.device, &level, &g_device.context);
        fastest->Release();
    }
    if (FAILED(result)) {
        result = D3D11CreateDevice(nullptr, driver, nullptr, device_flags, nullptr, 0, D3D11_SDK_VERSION, &g_device.device, &level, &g_device.context);
    }
    if (FAILED(result)) {
        result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &g_device.device, &level,
                                   &g_device.context);
    }
    ID3D11InfoQueue* info_queue = nullptr;
    if (SUCCEEDED(result) && device_flags && SUCCEEDED(g_device.device->QueryInterface(__uuidof(ID3D11InfoQueue), reinterpret_cast<void**>(&info_queue)))) {
        // An invalid call then stops right where it is made, so the crash report shows the caller.
        info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        info_queue->SetBreakOnSeverity(D3D11_MESSAGE_SEVERITY_ERROR, TRUE);
        info_queue->Release();
        log::write("video", "Direct3D debug layer on");
    }
    open_shader_cache();
    if (FAILED(result) || !create_target() || !create_pipeline()) {
        std::fputs("cannot initialise the D3D11 renderer\n", stderr);
        g_device.failed = true;
        return false;
    }
    log_adapter();
    prepare_texture_pack();
    preload_variants();
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

ID3D11ShaderResourceView* custom_texture(const std::string& key, bool mipmaps) {
    auto [found, inserted] = g_pack.loaded.try_emplace(key);
    CustomTexture& custom = found->second;
    if (!inserted) {
        return custom.view;
    }
    const std::filesystem::path* path = g_pack.index.find(key);
    std::vector<std::vector<uint32_t>> images(1);
    uint32_t width = 0;
    uint32_t height = 0;
    if (!path || !custom_textures::decode_png(*path, images[0], width, height)) {
        std::fprintf(stderr, "custom texture %s cannot be read, using the original\n", key.c_str());
        return nullptr;
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
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = generate ? 0 : static_cast<UINT>(images.size());
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = generate ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | (generate ? D3D11_BIND_RENDER_TARGET : 0);
    description.MiscFlags = generate ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    std::vector<D3D11_SUBRESOURCE_DATA> initial(images.size());
    for (size_t level = 0; level < images.size(); level++) {
        initial[level] = {images[level].data(), std::max(width >> level, 1u) * 4, 0};
    }
    if (FAILED(g_device.device->CreateTexture2D(&description, generate ? nullptr : initial.data(), &custom.texture)) ||
        FAILED(g_device.device->CreateShaderResourceView(custom.texture, nullptr, &custom.view))) {
        release(custom.texture);
        std::fprintf(stderr, "custom texture %s cannot be created (%ux%u), using the original\n", key.c_str(), width, height);
        return nullptr;
    }
    if (generate) {
        g_device.context->UpdateSubresource(custom.texture, 0, nullptr, images[0].data(), width * 4, 0);
        g_device.context->GenerateMips(custom.view);
    }
    return custom.view;
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

ID3D11ShaderResourceView* texture_for(uint32_t map) {
    g_custom_maps &= ~(1u << map);
    TextureRequest request = describe_texture(map);
    if (!request.supported && !g_logged_palette) {
        g_logged_palette = true;
        std::fprintf(stderr, "unsupported texture format %u\n", request.format);
    }
    if (!request.valid) {
        return nullptr;
    }
    uint32_t address = request.address;
    uint32_t width = request.width;
    uint32_t height = request.height;
    uint32_t format = request.format;
    uint32_t levels = request.levels;
    size_t size = request.size;
    const uint8_t* source = request.source;
    const uint8_t* tlut = request.tlut;
    auto copied = g_device.copies.find(address);
    if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && copied->second.logical_width == width &&
        copied->second.logical_height == height && format <= 6) {
        note_texture(map, address, width, height, format, "EFB copy");
        return copied->second.view;
    }
    if (!g_device.write_backs.empty() && write_back_overlaps(address, size)) {
        finish_write_backs();
    }
    if (copied != g_device.copies.end() && copied->second.logical_width == width && copied->second.logical_height == height &&
        format <= 6 && copied->second.guest_hash == sample_hash(source, copied->second.bytes)) {
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
    uint64_t key = request.key;
    bool mipmaps = request.mipmaps;
    CachedTexture& entry = g_device.textures[key];
    auto use = [&](CachedTexture& texture) {
        if (texture.custom) {
            g_custom_maps |= 1u << map;
        }
        return texture.view;
    };
    if (entry.view && entry.verified_epoch == g_texture_epoch && entry.width == width && entry.height == height && entry.format == format &&
        entry.levels == levels && entry.mipmaps == mipmaps) {
        return use(entry);
    }
    uint64_t hash = texture_hash(request);
    if (entry.view && entry.hash == hash && entry.width == width && entry.height == height && entry.format == format && entry.levels == levels &&
        entry.mipmaps == mipmaps) {
        entry.verified_epoch = g_texture_epoch;
        return use(entry);
    }
    flush_pending();
    release(entry.view);
    release(entry.texture);
    entry.custom = false;
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
    ID3D11ShaderResourceView* custom = nullptr;
    if (named) {
        name = custom_textures::name_texture(source, size, width, height, format, mipmaps, tlut);
        if (g_pack.load) {
            std::string resolved = g_pack.index.resolve(name);
            if (!resolved.empty()) {
                custom = custom_texture(resolved, mipmaps);
            }
        }
    }
    if (custom && !g_pack.dump) {
        custom->AddRef();
        entry.view = custom;
        entry.custom = true;
        return finish();
    }
    static const bool block_compression = [] {
        const char* setting = std::getenv("WP_BC1");
        return !setting || std::atoi(setting) != 0;
    }();
    bool compressed = block_compression && format == 14 && !(named && g_pack.dump) && width % 4 == 0 && height % 4 == 0;
    std::vector<std::vector<uint32_t>> pixels;
    std::vector<std::vector<uint8_t>> blocks;
    std::vector<D3D11_SUBRESOURCE_DATA> initial(levels);
    if (compressed) {
        blocks.resize(levels);
        for (uint32_t level = 0; level < levels; level++) {
            uint32_t level_width = std::max(width >> level, 1u);
            blocks[level] = cmpr_to_bc1(source + request.offsets[level], level_width, std::max(height >> level, 1u));
            initial[level] = {blocks[level].data(), (level_width + 3) / 4 * 8, 0};
        }
    } else {
        pixels = decode_levels(request);
        for (uint32_t level = 0; level < levels; level++) {
            initial[level] = {pixels[level].data(), std::max(width >> level, 1u) * 4, 0};
        }
    }
    if (named && g_pack.dump) {
        dump_texture(name, pixels, width, height);
    }
    if (custom) {
        custom->AddRef();
        entry.view = custom;
        entry.custom = true;
        return finish();
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = levels;
    description.ArraySize = 1;
    description.Format = compressed ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device.device->CreateTexture2D(&description, initial.data(), &entry.texture)) ||
        FAILED(g_device.device->CreateShaderResourceView(entry.texture, nullptr, &entry.view))) {
        return nullptr;
    }
    return finish();
}

ID3D11SamplerState* sampler_for(uint32_t map) {
    SamplerParams params = sampler_params(map, (g_custom_maps >> map) & 1);
    auto found = g_device.samplers.find(params.key);
    if (found != g_device.samplers.end()) {
        return found->second;
    }
    auto address = [](uint32_t wrap) {
        return wrap == 1 ? D3D11_TEXTURE_ADDRESS_WRAP : wrap == 2 ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_CLAMP;
    };
    D3D11_SAMPLER_DESC description{};
    D3D11_FILTER_TYPE min_type = params.min_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE mag_type = params.mag_linear ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    D3D11_FILTER_TYPE mip_type = params.mip_mode == 2 ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT;
    description.Filter = D3D11_ENCODE_BASIC_FILTER(min_type, mag_type, mip_type, false);
    if (g_device.anisotropy > 1 && params.min_linear && params.mag_linear) {
        description.Filter = D3D11_FILTER_ANISOTROPIC;
        description.MaxAnisotropy = static_cast<UINT>(g_device.anisotropy);
    }
    description.AddressU = address(params.wrap_s);
    description.AddressV = address(params.wrap_t);
    description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    description.MinLOD = params.min_lod;
    description.MaxLOD = params.unlimited_lod ? D3D11_FLOAT32_MAX : params.max_lod;
    description.MipLODBias = params.lod_bias;
    ID3D11SamplerState* state = nullptr;
    g_device.device->CreateSamplerState(&description, &state);
    g_device.samplers[params.key] = state;
    return state;
}

D3D11_BLEND d3d_factor(BlendFactor factor) {
    switch (factor) {
    case BlendFactor::Zero:
        return D3D11_BLEND_ZERO;
    case BlendFactor::One:
        return D3D11_BLEND_ONE;
    case BlendFactor::SourceColor:
        return D3D11_BLEND_SRC_COLOR;
    case BlendFactor::InverseSourceColor:
        return D3D11_BLEND_INV_SRC_COLOR;
    case BlendFactor::SourceAlpha:
        return D3D11_BLEND_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return D3D11_BLEND_INV_SRC_ALPHA;
    case BlendFactor::DestinationColor:
        return D3D11_BLEND_DEST_COLOR;
    case BlendFactor::InverseDestinationColor:
        return D3D11_BLEND_INV_DEST_COLOR;
    case BlendFactor::DestinationAlpha:
        return D3D11_BLEND_DEST_ALPHA;
    case BlendFactor::InverseDestinationAlpha:
        return D3D11_BLEND_INV_DEST_ALPHA;
    case BlendFactor::TevColor:
        return D3D11_BLEND_SRC1_COLOR;
    case BlendFactor::TevAlpha:
        return D3D11_BLEND_SRC1_ALPHA;
    case BlendFactor::InverseTevAlpha:
        return D3D11_BLEND_INV_SRC1_ALPHA;
    }
    return D3D11_BLEND_ONE;
}

D3D11_BLEND_OP d3d_operation(BlendOperation operation) {
    switch (operation) {
    case BlendOperation::Subtract:
        return D3D11_BLEND_OP_SUBTRACT;
    case BlendOperation::ReverseSubtract:
        return D3D11_BLEND_OP_REV_SUBTRACT;
    default:
        return D3D11_BLEND_OP_ADD;
    }
}

ID3D11BlendState* blend_for(uint32_t word) {
    uint32_t key = word;
    auto found = g_device.blend_states.find(key);
    if (found != g_device.blend_states.end()) {
        return found->second;
    }
    BlendState blend = blend_state_for(word);
    if (blend.logic_op && g_log_gx) {
        std::fprintf(stderr, "GX logic op %s (%u), %s, blend mode %06x\n", logic_op_name(blend.logic_mode), blend.logic_mode,
                     logic_op_exact(blend.logic_mode) ? "exact" : "approximated with blending", word);
    }
    D3D11_BLEND_DESC description{};
    D3D11_RENDER_TARGET_BLEND_DESC& target = description.RenderTarget[0];
    target.BlendEnable = blend.enable;
    target.SrcBlend = d3d_factor(blend.source);
    target.DestBlend = d3d_factor(blend.destination);
    target.BlendOp = d3d_operation(blend.operation);
    target.SrcBlendAlpha = d3d_factor(blend.source_alpha);
    target.DestBlendAlpha = d3d_factor(blend.destination_alpha);
    target.BlendOpAlpha = d3d_operation(blend.alpha_operation);
    UINT mask = 0;
    if (blend.color_update) {
        mask |= D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    }
    if (blend.alpha_update) {
        mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    }
    target.RenderTargetWriteMask = static_cast<UINT8>(mask);
    ID3D11BlendState* state = nullptr;
    g_device.device->CreateBlendState(&description, &state);
    g_device.blend_states[key] = state;
    return state;
}

ID3D11DepthStencilState* depth_for(uint32_t word) {
    uint32_t key = word & 0x1F;
    auto found = g_device.depth_states.find(key);
    if (found != g_device.depth_states.end()) {
        return found->second;
    }
    D3D11_DEPTH_STENCIL_DESC description{};
    description.DepthEnable = (word & 1) != 0;
    uint32_t function = (word >> 1) & 7;
    description.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>((((function & 1) << 2) | (function & 2) | (function >> 2)) + 1);
    description.DepthWriteMask = (word & (1u << 4)) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    ID3D11DepthStencilState* state = nullptr;
    g_device.device->CreateDepthStencilState(&description, &state);
    g_device.depth_states[key] = state;
    return state;
}

// Specialised pixel shaders. kShaderSource interprets the whole TEV configuration for every pixel, which takes most
// of the GPU time at high internal resolutions. A variant has that configuration compiled in as constants, so the
// compiler folds the interpreter away. Variants compile on a worker thread; the generic shader draws until then.
struct ShaderVariant {
    std::atomic<ID3D11PixelShader*> shader{nullptr};
};

constexpr size_t kVariantHeader = 10;
std::unordered_map<std::string, ShaderVariant> g_variants;
std::mutex g_variant_mutex;
std::condition_variable g_variant_wake;
std::vector<std::pair<std::string, ShaderVariant*>> g_variant_jobs;

// Key words: stage count, early depth, swaps, indirect, then five words per stage (the stage and its indirect setup).
std::string variant_key(const Constants& constants, bool early_depth) {
    uint32_t words[kVariantHeader + kMaxStages * 5] = {};
    uint32_t count = std::min(constants.header[0], kMaxStages);
    words[0] = count;
    words[1] = early_depth ? 1 : 0;
    std::memcpy(words + 2, constants.swaps, sizeof constants.swaps);
    std::memcpy(words + 6, constants.indirect, sizeof constants.indirect);
    for (uint32_t i = 0; i < count; i++) {
        std::memcpy(words + kVariantHeader + i * 5, constants.stage[i], sizeof constants.stage[i]);
        words[kVariantHeader + i * 5 + 4] = constants.tevind[i][0];
    }
    return std::string(reinterpret_cast<const char*>(words), (kVariantHeader + count * 5) * sizeof(uint32_t));
}

std::string variant_source(const std::string& key) {
    std::vector<uint32_t> words(key.size() / sizeof(uint32_t));
    std::memcpy(words.data(), key.data(), key.size());
    uint32_t count = words[0];
    std::string source = kShaderSource;
    auto replace = [&source](const char* from, const std::string& to) {
        size_t at = source.find(from);
        if (at != std::string::npos) {
            source.replace(at, std::strlen(from), to);
        }
    };
    auto vector = [](const uint32_t* v) {
        char text[96];
        std::snprintf(text, sizeof text, "uint4(%uu, %uu, %uu, %uu)", v[0], v[1], v[2], v[3]);
        return std::string(text);
    };
    std::string stages = "static const uint4 stage[16] = {";
    std::string indirects = "static const uint4 tevind[16] = {";
    const uint32_t zero[4] = {};
    for (uint32_t i = 0; i < kMaxStages; i++) {
        const uint32_t* stage = i < count ? &words[kVariantHeader + i * 5] : zero;
        const uint32_t indirect[4] = {i < count ? words[kVariantHeader + i * 5 + 4] : 0, 0, 0, 0};
        stages += vector(stage) + (i + 1 < kMaxStages ? ", " : "};\n");
        indirects += vector(indirect) + (i + 1 < kMaxStages ? ", " : "};\n");
    }
    // The constant buffer keeps its layout; the members that became constants only change name.
    replace("    uint4 stage[16];", "    uint4 stage_slot[16];");
    replace("    uint4 swaps;", "    uint4 swaps_slot;");
    replace("    uint4 indirect;", "    uint4 indirect_slot;");
    replace("    uint4 tevind[16];", "    uint4 tevind_slot[16];");
    replace("Texture2D t0 : register(t0);", stages + indirects + "static const uint4 swaps = " + vector(&words[2]) + ";\nstatic const uint4 indirect = " +
                                                vector(&words[6]) + ";\nTexture2D t0 : register(t0);");
    replace("uint count = header.x;", "const uint count = " + std::to_string(count) + ";");
    uint32_t indirect_stages = std::min(words[6] & 7, 4u);
    if (indirect_stages > 0) {
        replace("for (uint j = 0; j < indirect_stages && j < 4; j++)", "[unroll] for (uint j = 0; j < " + std::to_string(indirect_stages) + "; j++)");
    }
    if (count > 0) {
        replace("for (uint i = 0; i < count; i++)", "[unroll] for (uint i = 0; i < " + std::to_string(count) + "; i++)");
    }
    if (words[1] != 0) {
        replace("PixelOutput pixel_main(", "[earlydepthstencil] PixelOutput pixel_main(");
    }
    return source;
}

void variant_worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        std::pair<std::string, ShaderVariant*> job;
        {
            std::unique_lock<std::mutex> lock(g_variant_mutex);
            g_variant_wake.wait(lock, [] { return !g_variant_jobs.empty(); });
            job = std::move(g_variant_jobs.back());
            g_variant_jobs.pop_back();
        }
        ID3DBlob* code = nullptr;
        ID3D11PixelShader* shader = nullptr;
        // A variant that does not compile stays null: the generic shader keeps drawing it.
        if (compile(variant_source(job.first).c_str(), "pixel_main", "ps_5_0", &code, job.first)) {
            g_device.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
            release(code);
        }
        job.second->shader = shader;
    }
}

void start_variant_workers() {
    [[maybe_unused]] static const bool started = [] {
        unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
        for (unsigned i = 0; i < workers; i++) {
            std::thread(variant_worker).detach();
        }
        return true;
    }();
}

void preload_variants() {
    if (std::getenv("WP_GENERIC_SHADER") || g_shader_cache.variants.empty()) {
        return;
    }
    start_variant_workers();
    {
        std::lock_guard<std::mutex> lock(g_variant_mutex);
        for (auto it = g_shader_cache.variants.rbegin(); it != g_shader_cache.variants.rend(); ++it) {
            auto [found, inserted] = g_variants.try_emplace(*it);
            if (inserted) {
                g_variant_jobs.emplace_back(*it, &found->second);
            }
        }
    }
    g_variant_wake.notify_all();
    g_shader_cache.variants.clear();
    g_shader_cache.variants.shrink_to_fit();
}

// WP_GENERIC_SHADER=1 draws everything with the generic shader, to tell a specialisation fault from a renderer one.
ID3D11PixelShader* variant_for(const Constants& constants, bool early_depth) {
    static const bool generic = std::getenv("WP_GENERIC_SHADER") != nullptr;
    if (generic) {
        return nullptr;
    }
    std::string key = variant_key(constants, early_depth);
    auto found = g_variants.find(key);
    if (found == g_variants.end()) {
        start_variant_workers();
        found = g_variants.try_emplace(key).first;
        {
            std::lock_guard<std::mutex> lock(g_variant_mutex);
            g_variant_jobs.emplace_back(key, &found->second);
        }
        g_variant_wake.notify_one();
    }
    return found->second.shader.load();
}

double g_state_seconds = 0.0;
double g_constant_seconds = 0.0;
double g_copy_seconds = 0.0;
uint64_t g_copy_bytes = 0;
bool g_trimming = false;
double g_draw_seconds = 0.0;
uint32_t g_constants_created = 0;

ID3D11Buffer* constant_buffer(const void* data, size_t size) {
    if (!g_nvidia) {
        static std::map<size_t, ID3D11Buffer*> rewritten;
        ID3D11Buffer*& buffer = rewritten[size];
        if (!buffer) {
            D3D11_BUFFER_DESC description{};
            description.ByteWidth = static_cast<UINT>(size);
            description.Usage = D3D11_USAGE_DYNAMIC;
            description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            g_device.device->CreateBuffer(&description, nullptr, &buffer);
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (buffer && SUCCEEDED(g_device.context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, data, size);
            g_device.context->Unmap(buffer, 0);
        }
        return buffer;
    }
    struct CachedConstants {
        ID3D11Buffer* buffer = nullptr;
        bool used = false;
    };
    static std::map<std::string, CachedConstants> cache;
    constexpr size_t kCachedConstants = 4096;
    if (cache.size() >= kCachedConstants) {
        for (auto entry = cache.begin(); entry != cache.end();) {
            if (entry->second.used) {
                entry->second.used = false;
                ++entry;
            } else {
                release(entry->second.buffer);
                entry = cache.erase(entry);
            }
        }
        if (cache.size() >= kCachedConstants * 3 / 4) {
            for (auto& [bytes, cached] : cache) {
                release(cached.buffer);
            }
            cache.clear();
        }
    }
    auto [found, added] = cache.try_emplace(std::string(static_cast<const char*>(data), size));
    if (added) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(size);
        description.Usage = D3D11_USAGE_IMMUTABLE;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{data, 0, 0};
        g_device.device->CreateBuffer(&description, &initial, &found->second.buffer);
        g_constants_created++;
    }
    found->second.used = true;
    return found->second.buffer;
}

void set_batch_state(const BatchState& pending, bool moved) {
    auto clock = std::chrono::steady_clock::now();
    ID3D11DeviceContext* context = g_device.context;
    context->OMSetRenderTargets(1, &g_device.target_view, g_device.depth_view);
    context->PSSetShaderResources(0, kTextureMaps, pending.views);
    context->PSSetSamplers(0, kTextureMaps, pending.samplers);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(scaled(kEfbWidth)), static_cast<float>(scaled(kEfbHeight)), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    D3D11_RECT scissor;
    scissor.left = static_cast<LONG>(((pending.top_left >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset));
    scissor.top = static_cast<LONG>((pending.top_left & 0x7FF) - scissor_offset_y(pending.scissor_offset));
    scissor.right = static_cast<LONG>(((pending.bottom_right >> 12) & 0x7FF) - scissor_offset_x(pending.scissor_offset) + 1);
    scissor.bottom = static_cast<LONG>((pending.bottom_right & 0x7FF) - scissor_offset_y(pending.scissor_offset) + 1);
    scissor.left = std::max<LONG>(scissor.left, 0);
    scissor.top = std::max<LONG>(scissor.top, 0);
    scissor.right = std::min<LONG>(scissor.right, kEfbWidth);
    scissor.bottom = std::min<LONG>(scissor.bottom, kEfbHeight);
    scissor.left *= g_scale;
    scissor.top *= g_scale;
    scissor.right *= g_scale;
    scissor.bottom *= g_scale;
    context->RSSetScissorRects(1, &scissor);
    context->RSSetState(g_device.rasterizer);
    float blend_factor[4] = {1, 1, 1, 1};
    context->OMSetBlendState(blend_for(pending.blend), blend_factor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(depth_for(pending.depth), 0);
    ID3D11Buffer* buffers[2] = {g_device.vertex_buffer, g_device.moved_buffer};
    UINT strides[2] = {sizeof(ScreenVertex), sizeof(float[4])};
    UINT offsets[2] = {0, 0};
    context->IASetVertexBuffers(0, moved ? 2 : 1, buffers, strides, offsets);
    context->IASetInputLayout(moved ? g_device.moved_layout : g_device.layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(moved ? g_device.moved_shader : g_device.vertex_shader, nullptr, 0);
    ID3D11PixelShader* variant = variant_for(pending.constants, pending.early_depth);
    context->PSSetShader(variant ? variant : (pending.early_depth ? g_device.early_pixel_shader : g_device.pixel_shader), nullptr, 0);
    g_state_seconds += lap(clock);
    ID3D11Buffer* constants = constant_buffer(&pending.constants, sizeof pending.constants);
    context->PSSetConstantBuffers(0, 1, &constants);
    g_constant_seconds += lap(clock);
}

uint32_t coordinates_used(const ScreenVertex* vertices, size_t count) {
    __m128i seen[6] = {};
    for (size_t i = 0; i < count; i++) {
        const auto* uv = reinterpret_cast<const __m128i*>(vertices[i].uv);
        for (int part = 0; part < 6; part++) {
            seen[part] = _mm_or_si128(seen[part], _mm_loadu_si128(uv + part));
        }
    }
    uint32_t words[24];
    std::memcpy(words, seen, sizeof words);
    uint32_t used = 0;
    for (uint32_t slot = 0; slot < 8; slot++) {
        if (words[slot * 3] | words[slot * 3 + 1] | words[slot * 3 + 2]) {
            used = slot + 1;
        }
    }
    return used;
}

void draw_trimmed(const ScreenVertex* vertices, size_t total) {
    ID3D11DeviceContext* context = g_device.context;
    auto clock = std::chrono::steady_clock::now();
    uint32_t used = coordinates_used(vertices, total);
    UINT stride = offsetof(ScreenVertex, uv) + used * sizeof(float[3]);
    ID3D11Buffer* buffers[2] = {g_device.vertex_buffer, g_device.zero_buffer};
    UINT strides[2] = {stride, 0};
    context->IASetInputLayout(used == 8 ? g_device.layout : g_device.trimmed_layouts[used]);
    static std::vector<uint8_t> packed;
    const uint8_t* source = reinterpret_cast<const uint8_t*>(vertices);
    if (used != 8) {
        packed.resize(total * stride + 16);
        uint8_t* to = packed.data();
        for (size_t i = 0; i < total; i++, to += stride) {
            const uint8_t* from = reinterpret_cast<const uint8_t*>(vertices + i);
            for (UINT part = 0; part < stride; part += 16) {
                _mm_storeu_si128(reinterpret_cast<__m128i*>(to + part), _mm_loadu_si128(reinterpret_cast<const __m128i*>(from + part)));
            }
        }
        source = packed.data();
    }
    size_t offset = 0;
    while (offset < total) {
        uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexBytes / stride));
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_trimmed_cursor + batch * stride > kVertexBytes) {
            mode = D3D11_MAP_WRITE_DISCARD;
            g_trimmed_cursor = 0;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(g_device.vertex_buffer, 0, mode, 0, &mapped))) {
            break;
        }
        std::memcpy(static_cast<uint8_t*>(mapped.pData) + g_trimmed_cursor, source + offset * stride, static_cast<size_t>(batch) * stride);
        context->Unmap(g_device.vertex_buffer, 0);
        g_copy_bytes += static_cast<uint64_t>(batch) * stride;
        g_copy_seconds += lap(clock);
        UINT offsets[2] = {g_trimmed_cursor, 0};
        context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
        context->Draw(batch, 0);
        g_draw_seconds += lap(clock);
        g_trimmed_cursor += batch * stride;
        offset += batch;
    }
}

uint32_t draw_batch(const BatchState& pending, const ScreenVertex* vertices, size_t total) {
    set_batch_state(pending, false);
    if (!g_recording && g_trimming) {
        draw_trimmed(vertices, total);
        return ~0u;
    }
    ID3D11DeviceContext* context = g_device.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t start = total > kVertexCapacity ? ~0u : (g_vertex_cursor + total > kVertexCapacity ? 0u : g_vertex_cursor);
    size_t offset = 0;
    while (offset < total) {
        uint32_t batch = static_cast<uint32_t>(std::min<size_t>(total - offset, kVertexCapacity));
        D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_vertex_cursor + batch > kVertexCapacity) {
            mode = D3D11_MAP_WRITE_DISCARD;
            g_vertex_cursor = 0;
        }
        auto clock = std::chrono::steady_clock::now();
        if (FAILED(context->Map(g_device.vertex_buffer, 0, mode, 0, &mapped))) {
            break;
        }
        std::memcpy(static_cast<ScreenVertex*>(mapped.pData) + g_vertex_cursor, vertices + offset, batch * sizeof(ScreenVertex));
        context->Unmap(g_device.vertex_buffer, 0);
        g_copy_bytes += static_cast<uint64_t>(batch) * sizeof(ScreenVertex);
        g_copy_seconds += lap(clock);
        context->Draw(batch, g_vertex_cursor);
        g_draw_seconds += lap(clock);
        g_vertex_cursor += batch;
        offset += batch;
    }
    return start;
}

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
uint32_t g_run_row_cursor = kRunRowCapacity;
bool g_run_ring_recorded = false;
uint32_t g_run_kept = 0;
uint32_t g_run_kept_indices = 0;
uint32_t g_kept_vertex_used = 0;
uint32_t g_kept_index_used = 0;
uint32_t g_kept_generation = 1;

bool batch_empty() {
    return g_pending.begin == g_triangles.size() && g_run_draws.empty();
}

template <typename T>
void reserve_run(std::vector<T>& values, size_t wanted) {
    if (values.size() < wanted) {
        values.resize(std::max(wanted, values.size() * 2));
    }
}

bool upload_run(ID3D11Buffer* buffer, uint32_t& cursor, uint32_t capacity, const void* data, uint32_t size, uint32_t& start) {
    if (size == 0) {
        return true;
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (cursor + size > capacity) {
        mode = D3D11_MAP_WRITE_DISCARD;
        cursor = 0;
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(g_device.context->Map(buffer, 0, mode, 0, &mapped))) {
        return false;
    }
    std::memcpy(static_cast<uint8_t*>(mapped.pData) + cursor, data, size);
    g_device.context->Unmap(buffer, 0);
    start = cursor;
    cursor += size;
    g_copy_bytes += size;
    return true;
}

void issue_runs(const RunDraw* draws, size_t count, uint32_t vertex_start, uint32_t index_start, uint32_t row_start) {
    ID3D11DeviceContext* context = g_device.context;
    context->VSSetShader(g_device.run_shader, nullptr, 0);
    context->VSSetShaderResources(0, 1, &g_device.run_rows_view);
    const RunDraw* group = nullptr;
    ID3D11Buffer* index_buffer = nullptr;
    uint32_t cull = 0;
    for (size_t i = 0; i < count; i++) {
        const RunDraw& draw = draws[i];
        if (!group || group->layout != draw.layout || group->stride != draw.stride || group->kept != draw.kept) {
            group = &draw;
            ID3D11Buffer* buffers[3] = {draw.kept ? g_device.kept_vertices : g_device.run_vertices, g_device.zero_buffer, g_device.run_numbers};
            UINT strides[3] = {draw.stride, 0, sizeof(uint32_t)};
            UINT offsets[3] = {draw.kept ? 0 : vertex_start + draw.vertex_offset, 0, 0};
            context->IASetInputLayout(draw.layout);
            context->IASetVertexBuffers(0, 3, buffers, strides, offsets);
            ID3D11Buffer* wanted = draw.kept ? g_device.kept_indices : g_device.run_indices;
            if (index_buffer != wanted) {
                index_buffer = wanted;
                context->IASetIndexBuffer(wanted, DXGI_FORMAT_R16_UINT, 0);
            }
        }
        if (draw.cull != cull) {
            cull = draw.cull;
            context->RSSetState(cull == 0 ? g_device.rasterizer : g_device.run_culling[cull - 1]);
        }
        UINT first_index = draw.kept ? draw.first_index : index_start + draw.first_index;
        UINT base_vertex = draw.kept ? draw.vertex_offset / draw.stride : (draw.vertex_offset - group->vertex_offset) / draw.stride;
        context->DrawIndexedInstanced(draw.indices, 1, first_index, static_cast<INT>(base_vertex), row_start + draw.row);
    }
}

void record_runs(const BatchState& pending, uint32_t vertex_start, uint32_t index_start) {
    RecordedCommand command;
    command.kind = RecordedCommand::kRuns;
    command.batch = pending;
    for (ID3D11ShaderResourceView* view : command.batch.views) {
        if (view) {
            view->AddRef();
        }
    }
    command.begin = g_record.runs.size();
    uint32_t base = static_cast<uint32_t>(g_record.rows.size());
    g_record.rows.insert(g_record.rows.end(), g_run_row_values.begin(), g_run_row_values.begin() + g_run_row_used);
    for (const RunDraw& draw : g_run_draws) {
        RecordedRun run;
        run.draw = draw;
        if (!draw.kept) {
            run.draw.vertex_offset += vertex_start;
            run.draw.first_index += index_start;
        }
        run.draw.row += base;
        run.draw.matrix += base;
        if (draw.palette != kNoPalette) {
            run.draw.palette += base;
        }
        g_record.runs.push_back(run);
    }
    command.end = g_record.runs.size();
    g_record.commands.push_back(command);
}

void draw_runs(const BatchState& pending) {
    set_batch_state(pending, false);
    auto clock = std::chrono::steady_clock::now();
    uint32_t vertex_start = 0;
    uint32_t index_start = 0;
    uint32_t row_start = 0;
    uint32_t vertex_before = g_run_vertex_cursor;
    uint32_t index_before = g_run_index_cursor;
    bool ok = upload_run(g_device.run_rows, g_run_row_cursor, kRunRowCapacity * sizeof(float[4]), g_run_row_values.data(), g_run_row_used * sizeof(float[4]), row_start) &&
              upload_run(g_device.run_vertices, g_run_vertex_cursor, kRunVertexBytes, g_run_vertex_bytes.data(), g_run_vertex_used, vertex_start) &&
              upload_run(g_device.run_indices, g_run_index_cursor, kRunIndexBytes, g_run_index_words.data(), g_run_index_used * sizeof(uint16_t), index_start);
    g_copy_seconds += lap(clock);
    index_start /= static_cast<uint32_t>(sizeof(uint16_t));
    if (ok) {
        issue_runs(g_run_draws.data(), g_run_draws.size(), vertex_start, index_start, row_start / static_cast<uint32_t>(sizeof(float[4])));
    }
    if (g_recording) {
        bool wrapped = (g_run_vertex_used != 0 && g_run_vertex_cursor - g_run_vertex_used < vertex_before) ||
                       (g_run_index_used != 0 && g_run_index_cursor - g_run_index_used * sizeof(uint16_t) < index_before);
        if (!ok || (wrapped && g_run_ring_recorded)) {
            g_record_broken = true;
        }
        g_run_ring_recorded = g_run_ring_recorded || g_run_vertex_used != 0;
        record_runs(pending, vertex_start, index_start);
    }
    g_draw_seconds += lap(clock);
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
        draw_runs(pending);
        g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return;
    }
    size_t end = g_triangles.size();
    if (pending.begin == end) {
        return;
    }
    auto started = std::chrono::steady_clock::now();
    g_batches++;
    g_batch_vertices += static_cast<uint32_t>(end - pending.begin);
    uint32_t start = draw_batch(pending, g_triangles.data() + pending.begin, end - pending.begin);
    if (g_recording) {
        if (start != g_frame_vertices) {
            g_record_broken = true;
        }
        RecordedCommand command;
        command.kind = RecordedCommand::kBatch;
        command.batch = pending;
        for (ID3D11ShaderResourceView* view : command.batch.views) {
            if (view) {
                view->AddRef();
            }
        }
        command.begin = g_frame_vertices;
        command.end = g_frame_vertices + (end - pending.begin);
        g_record.commands.push_back(command);
        g_frame_vertices += end - pending.begin;
    }
    g_triangles.clear();
    pending.begin = 0;
    g_batch_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void append(const ScreenVertex* vertices, uint32_t count, const DrawSource* source) {
    size_t first = g_frame_vertices + g_triangles.size();
    g_triangles.insert(g_triangles.end(), vertices, vertices + count);
    if (!g_recording) {
        return;
    }
    if (source && source->key != 0) {
        RecordedDraw draw;
        draw.key = source->key;
        draw.exact = source->exact;
        draw.camera = source->camera;
        draw.first_position = static_cast<uint32_t>(g_record.positions.size());
        draw.positions = source->count;
        draw.first_vertex = first;
        draw.vertices = count;
        g_record.draws.push_back(draw);
        const auto* positions = reinterpret_cast<const std::array<float, 4>*>(source->positions);
        g_record.positions.insert(g_record.positions.end(), positions, positions + source->count);
        g_record.source_of.insert(g_record.source_of.end(), source->source_of, source->source_of + count);
    } else {
        g_record.source_of.insert(g_record.source_of.end(), count, kNoSource);
    }
}

void service_deferred();

}

const char* api_name() {
    return "Direct3D 11";
}

bool interpolating() {
    return g_recording;
}

namespace {

void select_batch() {
    // Scenes such as the open zones send hundreds of thousands of tiny draws a second, mostly with only a new matrix in
    // between: when no register the batch depends on changed, this draw joins the pending batch without recomputing it.
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
    ID3D11ShaderResourceView* views[kTextureMaps] = {};
    ID3D11SamplerState* samplers[kTextureMaps] = {};
    uint32_t used = used_maps(constants);
    for (uint32_t map = 0; map < kTextureMaps; map++) {
        if (used & (1u << map)) {
            views[map] = texture_for(map);
            if (views[map]) {
                views[map]->AddRef();
            }
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
    if (same) {
        for (ID3D11ShaderResourceView*& view : views) {
            release(view);
        }
    } else {
        flush_pending();
        for (ID3D11ShaderResourceView*& view : pending.views) {
            release(view);
        }
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

}

void draw(const ScreenVertex* vertices, uint32_t count, const DrawSource* source) {
    finish_frame();
    if (count == 0) {
        if (g_recording && source && source->key != 0) {
            append(vertices, 0, source);
        }
        return;
    }
    if (!initialize()) {
        return;
    }
    static uint32_t draws = 0;
    if ((++draws & 31) == 0) {
        service_deferred();
    }
    if (!g_run_draws.empty()) {
        flush_pending();
    }
    select_batch();
    append(vertices, count, source);
}

bool transforms_vertices() {
    static const settings::LiveFlag enabled("video.gpu_vertices", "WP_GPU_VERTICES");
    static const settings::LiveFlag between("video.gpu_between", "WP_GPU_BETWEEN");
    return enabled() && (!g_recording || between()) && initialize() && create_run_pipeline();
}

namespace {

bool add_run(const VertexRun& run, uint32_t bytes, uint32_t index_count) {
    finish_frame();
    ID3D11InputLayout* layout = run_layout(run.normal, run.matrix_indices, run.texture_inputs);
    if (!layout) {
        return false;
    }
    service_deferred();
    uint32_t rows = run.row_count + (run.palette ? kPaletteRows + kNormalRows : 0);
    if (g_pending.begin != g_triangles.size() || g_run_vertex_used + bytes > kRunVertexBytes / 2 || (g_run_index_used + index_count) * sizeof(uint16_t) > kRunIndexBytes / 2 ||
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
    draw.layout = layout;
    draw.stride = run.stride;
    draw.cull = run.cull;
    draw.vertex_offset = g_run_vertex_used;
    draw.first_index = g_run_index_used;
    draw.indices = run.indices;
    draw.row = g_run_row_used;
    draw.rows = run.row_count;
    draw.palette = run.palette ? g_run_palette : kNoPalette;
    draw.matrix = run.palette ? g_run_palette + run.matrix : g_run_row_used + 4;
    draw.camera = run.camera;
    draw.key = run.key;
    g_run_draws.push_back(draw);
    g_run_row_used += run.row_count;
    return true;
}

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
    auto clock = std::chrono::steady_clock::now();
    D3D11_BOX box{kept.vertex, 0, 0, kept.vertex + bytes, 1, 1};
    g_device.context->UpdateSubresource(g_device.kept_vertices, 0, &box, vertices, 0, 0);
    box.left = kept.index * static_cast<UINT>(sizeof(uint16_t));
    box.right = box.left + run.indices * static_cast<UINT>(sizeof(uint16_t));
    g_device.context->UpdateSubresource(g_device.kept_indices, 0, &box, indices, 0, 0);
    g_copy_bytes += bytes + run.indices * sizeof(uint16_t);
    g_copy_seconds += lap(clock);
    return true;
}

void reset_kept() {
    if (g_run_kept != 0) {
        flush_pending();
    }
    if (g_recording && !g_record.runs.empty()) {
        g_record_broken = true;
    }
    g_kept_generation++;
    g_kept_vertex_used = 0;
    g_kept_index_used = 0;
}

void take_statistics(uint32_t& batches, uint32_t& vertices, double& seconds) {
    batches = g_batches;
    vertices = g_batch_vertices;
    seconds = g_batch_seconds;
    log::write("batches", "%.0f ms setting state, %.0f ms on constants (%u new buffers), %.0f ms copying %u MB of vertices, %.0f ms in draw calls", g_state_seconds * 1000.0,
               g_constant_seconds * 1000.0, g_constants_created, g_copy_seconds * 1000.0, static_cast<unsigned>(g_copy_bytes >> 20), g_draw_seconds * 1000.0);
    static const double slow = std::getenv("WP_TRIM_BELOW") ? std::atof(std::getenv("WP_TRIM_BELOW")) : 6.0;
    if (!g_trimming && g_device.zero_buffer && g_copy_bytes >= (128u << 20) && static_cast<double>(g_copy_bytes) < g_copy_seconds * slow * 1e9) {
        g_trimming = true;
        log::write("video", "vertices reach the graphics card at %.1f GB/s: only the texture coordinates in use are sent from now on", static_cast<double>(g_copy_bytes) / g_copy_seconds / 1e9);
    }
    g_batches = 0;
    g_batch_vertices = 0;
    g_batch_seconds = 0.0;
    g_state_seconds = 0.0;
    g_constant_seconds = 0.0;
    g_copy_seconds = 0.0;
    g_copy_bytes = 0;
    g_draw_seconds = 0.0;
    g_constants_created = 0;
    std::lock_guard<std::mutex> lock(g_shader_cache.mutex);
    if (g_shader_cache.compiled + g_shader_cache.reused > 0) {
        log::write("shaders", "%u compiled in %.0f ms of processor time, %u taken from the disk cache", g_shader_cache.compiled, g_shader_cache.compile_seconds * 1000.0,
                   g_shader_cache.reused);
    }
    g_shader_cache.compiled = 0;
    g_shader_cache.reused = 0;
    g_shader_cache.compile_seconds = 0.0;
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

bool create_present_pipeline();

void release_copy(CopiedTexture& entry) {
    release(entry.converted_target);
    release(entry.converted_view);
    release(entry.converted);
    entry.view = nullptr;
}

void dump_copy(const CopiedTexture& entry);

bool run_copy(ID3D11RenderTargetView* target, uint32_t target_width, uint32_t target_height, int x, int y, int width, int height, uint32_t format,
              bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!create_present_pipeline()) {
        return false;
    }
    struct Constants {
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
    ID3D11DeviceContext* context = g_device.context;
    ID3D11Buffer* buffer = constant_buffer(&constants, sizeof constants);
    if (!buffer) {
        return false;
    }
    context->OMSetRenderTargets(1, &target, nullptr);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(target_width), static_cast<float>(target_height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_device.present_rasterizer);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_device.present_vertex, nullptr, 0);
    context->PSSetShader(g_device.copy_pixel, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &buffer);
    ID3D11ShaderResourceView* sources[2] = {g_device.target_resource, g_device.depth_resource};
    context->PSSetShaderResources(0, 2, sources);
    context->PSSetSamplers(0, 1, &g_device.present_sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none[2] = {};
    context->PSSetShaderResources(0, 2, none);
    return true;
}

bool create_copy_texture(ID3D11Texture2D*& texture, ID3D11ShaderResourceView*& view, ID3D11RenderTargetView*& target, uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    return SUCCEEDED(g_device.device->CreateTexture2D(&description, nullptr, &texture)) &&
           SUCCEEDED(g_device.device->CreateShaderResourceView(texture, nullptr, &view)) &&
           SUCCEEDED(g_device.device->CreateRenderTargetView(texture, nullptr, &target));
}

constexpr uint32_t kAtlasSize = 2048;
void complete_write_backs(bool wait);

// Every pending copy shares one staging texture: reading them back costs one GPU synchronisation per frame instead of
// one per copy, which took most of a frame in scenes with dozens of copies.
bool reserve_staging(uint32_t width, uint32_t height, uint32_t& x, uint32_t& y) {
    if (!g_device.staging_atlas) {
        D3D11_TEXTURE2D_DESC description{};
        g_device.write_back->GetDesc(&description);
        description.Width = kAtlasSize;
        description.Height = kAtlasSize;
        description.BindFlags = 0;
        description.Usage = D3D11_USAGE_STAGING;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device.device->CreateTexture2D(&description, nullptr, &g_device.staging_atlas))) {
            return false;
        }
    }
    if (g_device.atlas_x + width > kAtlasSize) {
        g_device.atlas_x = 0;
        g_device.atlas_y += g_device.atlas_row;
        g_device.atlas_row = 0;
    }
    if (g_device.atlas_y + height > kAtlasSize) {
        complete_write_backs(true);
    }
    x = g_device.atlas_x;
    y = g_device.atlas_y;
    g_device.atlas_x += width;
    g_device.atlas_row = std::max(g_device.atlas_row, height);
    return true;
}

bool queue_write_back(uint32_t address, uint32_t stride, int x, int y, int width, int height, uint32_t logical_width, uint32_t logical_height, uint32_t format,
                      bool intensity, bool alpha, bool depth, const CopyFilter& filter) {
    if (!g_device.write_back && !create_copy_texture(g_device.write_back, g_device.write_back_view, g_device.write_back_target, kEfbWidth, kEfbHeight)) {
        return false;
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
    PendingWriteBack pending;
    if (!reserve_staging(logical_width, logical_height, pending.atlas_x, pending.atlas_y) ||
        !run_copy(g_device.write_back_target, logical_width, logical_height, x, y, width, height, format, intensity, alpha, depth, filter)) {
        return false;
    }
    D3D11_BOX box{0, 0, 0, logical_width, logical_height, 1};
    g_device.context->CopySubresourceRegion(g_device.staging_atlas, 0, pending.atlas_x, pending.atlas_y, 0, g_device.write_back, 0, &box);
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

void write_back(const PendingWriteBack& pending, const D3D11_MAPPED_SUBRESOURCE& mapped) {
    Layout layout = layout_for(pending.texture_format);
    uint32_t blocks_x = (pending.logical_width + layout.block_width - 1) / layout.block_width;
    uint32_t blocks_y = (pending.logical_height + layout.block_height - 1) / layout.block_height;
    uint32_t bytes = block_bytes(pending.texture_format);
    const uint8_t* pixels = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(pending.atlas_y) * mapped.RowPitch + pending.atlas_x * 4;
    uint8_t block[64];
    for (uint32_t by = 0; by < blocks_y; by++) {
        uint8_t* row = host(pending.address + by * pending.row_bytes);
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            std::memset(block, 0, sizeof block);
            for (uint32_t ty = 0; ty < layout.block_height; ty++) {
                uint32_t py = std::min(by * layout.block_height + ty, pending.logical_height - 1);
                for (uint32_t tx = 0; tx < layout.block_width; tx++) {
                    uint32_t px = std::min(bx * layout.block_width + tx, pending.logical_width - 1);
                    encode_texel(block, pending.texture_format, ty * layout.block_width + tx, pixels + static_cast<size_t>(py) * mapped.RowPitch + px * 4);
                }
            }
            std::memcpy(row + bx * bytes, block, bytes);
        }
    }
}

void complete_write_backs(bool wait) {
    if (g_device.write_backs.empty()) {
        return;
    }
    wait = wait || g_device.write_backs.front().frame + kWriteBackFrames <= g_frame;
    D3D11_MAPPED_SUBRESOURCE mapped;
    auto wait_start = std::chrono::steady_clock::now();
    HRESULT mapped_result = g_device.context->Map(g_device.staging_atlas, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (mapped_result == DXGI_ERROR_WAS_STILL_DRAWING) {
        return;
    }
    g_write_back_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - wait_start).count();
    g_write_backs += static_cast<uint32_t>(g_device.write_backs.size());
    for (const PendingWriteBack& pending : g_device.write_backs) {
        if (SUCCEEDED(mapped_result)) {
            write_back(pending, mapped);
        }
        auto copied = g_device.copies.find(pending.address);
        if (copied != g_device.copies.end() && copied->second.pending_writes > 0 && --copied->second.pending_writes == 0) {
            CopiedTexture& entry = copied->second;
            entry.guest_hash = guest_range_valid(pending.address, entry.bytes) ? sample_hash(host(pending.address), entry.bytes) : 0;
        }
    }
    if (SUCCEEDED(mapped_result)) {
        g_device.context->Unmap(g_device.staging_atlas, 0);
    }
    g_device.write_backs.clear();
    g_device.atlas_x = 0;
    g_device.atlas_y = 0;
    g_device.atlas_row = 0;
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

std::unordered_map<ID3D11RenderTargetView*, ReplayTarget> g_replay_targets;

void release_twin(ReplayTarget& twin) {
    release(twin.target);
    release(twin.view);
    release(twin.texture);
    release(twin.before);
}

void note_copy_target(ID3D11Texture2D* texture, ID3D11ShaderResourceView* view, ID3D11RenderTargetView* target, uint32_t width, uint32_t height) {
    for (const RecordedTarget& seen : g_record.targets) {
        if (seen.target == target) {
            return;
        }
    }
    bool sampled = false;
    for (const RecordedCommand& command : g_record.commands) {
        if (command.kind == RecordedCommand::kBatch && std::find(std::begin(command.batch.views), std::end(command.batch.views), view) != std::end(command.batch.views)) {
            sampled = true;
            break;
        }
    }
    if (!sampled) {
        texture->AddRef();
        view->AddRef();
        target->AddRef();
        g_record.targets.push_back({target, texture, view, false});
        return;
    }
    ReplayTarget& twin = g_replay_targets[target];
    if (!twin.texture || twin.width != width || twin.height != height) {
        release_twin(twin);
        ID3D11ShaderResourceView* unused_view = nullptr;
        ID3D11RenderTargetView* unused_target = nullptr;
        bool made = create_copy_texture(twin.before, unused_view, unused_target, width, height) &&
                    create_copy_texture(twin.texture, twin.view, twin.target, width, height);
        release(unused_view);
        release(unused_target);
        if (!made) {
            release_twin(twin);
            g_replay_targets.erase(target);
            g_record_broken = true;
            return;
        }
        twin.width = width;
        twin.height = height;
    }
    twin.frame = g_frame;
    g_device.context->CopyResource(twin.before, texture);
    texture->AddRef();
    view->AddRef();
    target->AddRef();
    g_record.targets.push_back({target, texture, view, true});
}

void prune_twins() {
    for (auto it = g_replay_targets.begin(); it != g_replay_targets.end();) {
        if (it->second.frame + 600 < g_frame) {
            release_twin(it->second);
            it = g_replay_targets.erase(it);
        } else {
            ++it;
        }
    }
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
    finish_frame();
    flush_pending();
    CopiedTexture& entry = g_device.copies[address];
    // A buffer copied again every frame is a render target of an effect, and the game only samples it: it stays on the
    // GPU, as in Dolphin's default. Reading dozens of them back each frame held the cave at 42 fps instead of 60.
    // A one-off copy, like a snapshot, is still written to RAM. The photo is taken into a buffer an effect fills every
    // frame, in another size and format: a copy only counts as recurring when it has the shape of the one before.
    constexpr uint32_t kRecurringFrames = 2;
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    uint32_t logical_width = half ? std::max(1u, w / 2) : w;
    uint32_t logical_height = half ? std::max(1u, h / 2) : h;
    bool recurring = entry.converted && g_frame - entry.copied_frame <= kRecurringFrames && entry.copied_format == format &&
                     entry.logical_width == logical_width && entry.logical_height == logical_height;
    entry.copied_frame = g_frame;
    entry.copied_format = format;
    entry.logical_width = logical_width;
    entry.logical_height = logical_height;
    uint32_t target_width = std::max(1u, entry.logical_width * g_scale);
    uint32_t target_height = std::max(1u, entry.logical_height * g_scale);
    if (!entry.converted || entry.converted_width != target_width || entry.converted_height != target_height) {
        release_copy(entry);
        if (!create_copy_texture(entry.converted, entry.converted_view, entry.converted_target, target_width, target_height)) {
            release_copy(entry);
            g_device.copies.erase(address);
            return;
        }
        entry.converted_width = target_width;
        entry.converted_height = target_height;
    }
    if (g_recording) {
        note_copy_target(entry.converted, entry.converted_view, entry.converted_target, target_width, target_height);
    }
    if (!run_copy(entry.converted_target, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter)) {
        release_copy(entry);
        g_device.copies.erase(address);
        return;
    }
    if (g_recording) {
        RecordedCommand command;
        command.kind = RecordedCommand::kCopy;
        command.copy = {entry.converted_target, target_width, target_height, x, y, width, height, format, intensity, alpha || depth, depth, filter};
        entry.converted_target->AddRef();
        g_record.commands.push_back(command);
    }
    entry.view = entry.converted_view;
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

void run_clear(const ClearCommand& command);


bool copy_frame(int& x, int& y, int& width, int& height, bool depth, const CopyFilter& filter) {
    if (!initialize()) {
        return false;
    }
    flush_pending();
    complete_write_backs(false);
    g_frame++;
    g_texture_epoch++;
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return false;
    }
    uint32_t physical_width = static_cast<uint32_t>(scaled(width));
    uint32_t physical_height = static_cast<uint32_t>(scaled(height));
    if (!g_device.frame || g_device.frame_width != physical_width || g_device.frame_height != physical_height) {
        release(g_device.frame_target);
        release(g_device.frame_view);
        release(g_device.frame);
        if (!create_copy_texture(g_device.frame, g_device.frame_view, g_device.frame_target, physical_width, physical_height)) {
            release(g_device.frame_target);
            release(g_device.frame_view);
            release(g_device.frame);
            return false;
        }
        g_device.frame_width = physical_width;
        g_device.frame_height = physical_height;
    }
    return run_copy(g_device.frame_target, physical_width, physical_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

void release_record(FrameRecord& record) {
    for (RecordedCommand& command : record.commands) {
        for (ID3D11ShaderResourceView*& view : command.batch.views) {
            release(view);
        }
        release(command.copy.target);
    }
    for (RecordedTarget& target : record.targets) {
        release(target.target);
        release(target.texture);
        release(target.view);
    }
    record.targets.clear();
    record.commands.clear();
    record.source_of.clear();
    record.positions.clear();
    record.draws.clear();
    record.runs.clear();
    record.rows.clear();
}

constexpr float kMaxStep = 0.5f;

bool close_enough(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f) {
        return true;
    }
    return std::fabs(b[0] / b[3] - a[0] / a[3]) <= kMaxStep && std::fabs(b[1] / b[3] - a[1] / a[3]) <= kMaxStep;
}

bool close_draw(const RecordedDraw& before, const RecordedDraw& draw) {
    uint32_t step = std::max(1u, draw.positions / 8);
    for (uint32_t i = 0; i < draw.positions; i += step) {
        if (!close_enough(g_previous.positions[before.first_position + i], g_record.positions[draw.first_position + i])) {
            return false;
        }
    }
    return true;
}

struct EfbSnapshot {
    ID3D11Texture2D* color = nullptr;
    ID3D11Texture2D* depth = nullptr;
};

EfbSnapshot g_efb_before;
EfbSnapshot g_efb_after;
bool g_efb_before_valid = false;

bool same_size(ID3D11Texture2D* source, ID3D11Texture2D*& copy) {
    D3D11_TEXTURE2D_DESC wanted{};
    source->GetDesc(&wanted);
    if (copy) {
        D3D11_TEXTURE2D_DESC held{};
        copy->GetDesc(&held);
        if (held.Width == wanted.Width && held.Height == wanted.Height && held.Format == wanted.Format) {
            return true;
        }
        release(copy);
    }
    return SUCCEEDED(g_device.device->CreateTexture2D(&wanted, nullptr, &copy));
}

bool save_efb(EfbSnapshot& snapshot) {
    if (!same_size(g_device.target, snapshot.color) || !same_size(g_device.depth, snapshot.depth)) {
        return false;
    }
    g_device.context->CopyResource(snapshot.color, g_device.target);
    g_device.context->CopyResource(snapshot.depth, g_device.depth);
    return true;
}

void load_efb(const EfbSnapshot& snapshot) {
    g_device.context->CopyResource(g_device.target, snapshot.color);
    g_device.context->CopyResource(g_device.depth, snapshot.depth);
}

constexpr uint32_t kUnmatched = ~0u;
constexpr uint32_t kRejected = ~0u - 1;
constexpr uint32_t kByCamera = 0xFFFFFF00u;

constexpr double kStillScreen = 2e-3;

struct CameraMotion {
    uint32_t id = 0;
    bool claimed = false;
    bool known = false;
    float back[4][4] = {};
    size_t second = kMaxCameras;
    bool is_second = false;
    std::vector<std::array<float, 4>> from;
    std::vector<std::array<float, 4>> to;
};

CameraMotion g_cameras[kMaxCameras];

double g_time_pairing = 0.0;
double g_time_filling = 0.0;
double g_time_sending = 0.0;

std::vector<uint8_t> g_world_camera;
std::vector<uint8_t> g_world_moving;
std::vector<std::array<float, 2>> g_decal_shift;
double g_forward[kMaxCameras][4][4];

bool invert(const float (&in)[4][4], double (&out)[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            m[r][c] = in[r][c];
            m[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 4; column++) {
        int pivot = column;
        for (int r = column + 1; r < 4; r++) {
            if (std::fabs(m[r][column]) > std::fabs(m[pivot][column])) {
                pivot = r;
            }
        }
        if (std::fabs(m[pivot][column]) < 1e-300) {
            return false;
        }
        std::swap(m[column], m[pivot]);
        double scale = 1.0 / m[column][column];
        for (double& value : m[column]) {
            value *= scale;
        }
        for (int r = 0; r < 4; r++) {
            if (r != column && m[r][column] != 0.0) {
                double factor = m[r][column];
                for (int c = 0; c < 8; c++) {
                    m[r][c] -= factor * m[column][c];
                }
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r][c] = m[r][4 + c];
        }
    }
    return true;
}

enum Stillness : uint8_t { kUntested, kStill, kMoved };

Stillness stillness(const std::array<float, 4>& a, const std::array<float, 4>& b, const CameraMotion& camera) {
    constexpr double kScreen = kStillScreen;
    constexpr double kDepth = 2e-5;
    if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f || std::fabs(b[0]) > 1.5f * b[3] || std::fabs(b[1]) > 1.5f * b[3]) {
        return kUntested;
    }
    double p[4];
    for (int r = 0; r < 4; r++) {
        p[r] = static_cast<double>(camera.back[r][0]) * b[0] + static_cast<double>(camera.back[r][1]) * b[1] + static_cast<double>(camera.back[r][2]) * b[2] +
               static_cast<double>(camera.back[r][3]) * b[3];
    }
    bool moved = p[3] <= 0.0 || std::fabs(p[0] / p[3] - a[0] / a[3]) > kScreen || std::fabs(p[1] / p[3] - a[1] / a[3]) > kScreen ||
                 std::fabs(p[2] / p[3] - a[2] / a[3]) > kDepth;
    return moved ? kMoved : kStill;
}

bool moves_in_world(const RecordedDraw& draw, const RecordedDraw& before, const CameraMotion& camera) {
    for (uint32_t i = 0; i < draw.positions; i++) {
        if (stillness(g_previous.positions[before.first_position + i], g_record.positions[draw.first_position + i], camera) == kMoved) {
            return true;
        }
    }
    return false;
}

using Row = std::array<float, 4>;

struct RunCamera {
    bool valid = false;
    float move[3][4] = {};
    bool known = false;
    uint32_t id = 0;
    float back[4][4] = {};
};

RunCamera g_run_camera;
bool g_run_cut = false;
std::array<std::string, 256> g_pair_notes;
bool g_pair_notes_wanted = false;
uint32_t g_run_counts[4] = {};
std::vector<Row> g_replay_rows;
std::vector<RunDraw> g_replay_draws;

enum MatrixChange : uint8_t { kSameMatrix, kNearMatrix, kOtherMatrix };

MatrixChange matrix_change(const Row* a, const Row* b) {
    float scale = 0.0f;
    float turn = 0.0f;
    float step = 0.0f;
    float length = 0.0f;
    bool same = true;
    bool turned = false;
    for (int i = 0; i < 3; i++) {
        float row_scale = 0.0f;
        float row_turn = 0.0f;
        for (int j = 0; j < 4; j++) {
            float x = a[i][j];
            float y = b[i][j];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                return kOtherMatrix;
            }
            same = same && x == y;
            if (j == 3) {
                step += (y - x) * (y - x);
                length += y * y;
            } else {
                scale = std::max(scale, std::fabs(y));
                turn = std::max(turn, std::fabs(y - x));
                row_scale = std::max({row_scale, std::fabs(x), std::fabs(y)});
                row_turn = std::max(row_turn, std::fabs(y - x));
            }
        }
        turned = turned || row_turn > 0.5f * row_scale;
    }
    if (same) {
        return kSameMatrix;
    }
    return turned || turn > 0.3f * scale || std::sqrt(step) > 0.5f * std::sqrt(length) + 200.0f ? kOtherMatrix : kNearMatrix;
}

void lerp_rows(Row* out, const Row* a, const Row* b, uint32_t count, float t) {
    for (uint32_t i = 0; i < count; i++) {
        for (int j = 0; j < 4; j++) {
            float x = a[i][j];
            float y = b[i][j];
            if (x != y && std::isfinite(x) && std::isfinite(y)) {
                out[i][j] = x + (y - x) * t;
            }
        }
    }
}

void blend_matrix(Row* out, const Row* a, const Row* b, float t) {
    if (matrix_change(a, b) == kNearMatrix) {
        lerp_rows(out, a, b, 3, t);
    }
}

void blend_texture_matrix(Row* out, const Row* a, const Row* b, float t) {
    MatrixChange change = matrix_change(a, b);
    if (change == kNearMatrix) {
        lerp_rows(out, a, b, 3, t);
    }
    g_run_counts[3] += change == kOtherMatrix;
}

bool same_shape(const RunDraw& before, const RunDraw& now, const Row* a, const Row* b) {
    if (before.rows != now.rows || a[1][2] != b[1][2] || a[1][3] != b[1][3] || a[3][3] != b[3][3] || (before.palette == kNoPalette) != (now.palette == kNoPalette)) {
        return false;
    }
    uint32_t setup = static_cast<uint32_t>(b[3][3]);
    uint32_t units = std::min(setup & 15u, 8u);
    bool lit = (setup & 32u) != 0;
    uint32_t light = kRunHeaderRows + kRunUnitRows * units;
    if (light + (lit ? kRunLightRows : 0) > now.rows) {
        return false;
    }
    for (uint32_t i = 0; i < units; i++) {
        if (a[kRunHeaderRows + kRunUnitRows * i] != b[kRunHeaderRows + kRunUnitRows * i]) {
            return false;
        }
    }
    return !lit || a[light] == b[light];
}

void blend_run(Row* out, const Row* a, const Row* b, float t) {
    lerp_rows(out, a, b, 2, t);
    bool own = b[1][3] != 0.0f;
    if (own) {
        blend_matrix(out + 4, a + 4, b + 4, t);
    }
    uint32_t setup = static_cast<uint32_t>(b[3][3]);
    uint32_t units = std::min(setup & 15u, 8u);
    for (uint32_t i = 0; i < units; i++) {
        uint32_t at = kRunHeaderRows + kRunUnitRows * i;
        if (b[at][2] != 0.0f) {
            blend_texture_matrix(out + at + 1, a + at + 1, b + at + 1, t);
        }
        blend_texture_matrix(out + at + 4, a + at + 4, b + at + 4, t);
    }
    if (setup & 32u) {
        uint32_t light = kRunHeaderRows + kRunUnitRows * units;
        if (own) {
            lerp_rows(out + light + 5, a + light + 5, b + light + 5, 3, t);
        }
        for (uint32_t n = 0; n < 8; n++) {
            lerp_rows(out + light + 9 + n * 4, a + light + 9 + n * 4, b + light + 9 + n * 4, 3, t);
        }
    }
}

void blend_palette(Row* out, const Row* a, const Row* b, float t) {
    for (uint32_t m = 0; m + 3 <= 64; m += 3) {
        blend_matrix(out + m, a + m, b + m, t);
    }
    lerp_rows(out + kPaletteRows, a + kPaletteRows, b + kPaletteRows, kNormalRows, t);
}

bool run_perspective(const FrameRecord& record, const RecordedRun& run) {
    return record.rows[run.draw.row + 1][2] == 0.0f;
}

bool load_matrix(const Row* rows, float (&out)[3][4]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float value = rows[i][j];
            if (!std::isfinite(value) || std::fabs(value) > 1e7f) {
                return false;
            }
            out[i][j] = value;
        }
    }
    return true;
}

bool invert_affine(const float (&m)[3][4], float (&out)[3][4]) {
    float c[3][3];
    c[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    c[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    c[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    c[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    c[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    c[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    c[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    c[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    c[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    float det = m[0][0] * c[0][0] + m[0][1] * c[1][0] + m[0][2] * c[2][0];
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = c[i][j] / det;
        }
        out[i][3] = -(out[i][0] * m[0][3] + out[i][1] * m[1][3] + out[i][2] * m[2][3]);
    }
    return true;
}

bool close_motion(const float (&a)[3][4], const float (&b)[3][4]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (std::fabs(a[i][j] - b[i][j]) > 0.02f) {
                return false;
            }
        }
        if (std::fabs(a[i][3] - b[i][3]) > 2.0f + 0.02f * std::fabs(b[i][3])) {
            return false;
        }
    }
    return true;
}

bool clip_matrix(const FrameRecord& record, const RecordedRun& run, float (&out)[4][4]) {
    const Row* h = &record.rows[run.draw.row];
    const Row* m = &record.rows[run.draw.matrix];
    double x[4] = {h[0][0], 0.0, 0.0, 0.0};
    double y[4] = {0.0, h[0][2], 0.0, 0.0};
    double z[4] = {0.0, 0.0, h[1][0], h[1][1]};
    double w[4] = {0.0, 0.0, 0.0, 0.0};
    if (h[1][2] != 0.0f) {
        x[3] = h[0][1];
        y[3] = h[0][3];
        w[3] = 1.0;
    } else {
        x[2] = h[0][1];
        y[2] = h[0][3];
        w[2] = -1.0;
    }
    double clip[4][4];
    for (int j = 0; j < 4; j++) {
        clip[0][j] = x[j] * h[2][0] + w[j] * h[2][1];
        clip[1][j] = y[j] * h[2][2] + w[j] * h[2][3];
        clip[2][j] = (w[j] * h[3][1] - z[j] * h[3][0]) / 16777215.0;
        clip[3][j] = w[j];
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double sum = c == 3 ? clip[r][3] : 0.0;
            for (int k = 0; k < 3; k++) {
                sum += clip[r][k] * m[k][c];
            }
            if (!std::isfinite(sum)) {
                return false;
            }
            out[r][c] = static_cast<float>(sum);
        }
    }
    return true;
}

void find_run_camera(const std::vector<uint32_t>& unique) {
    constexpr size_t kSamples = 48;
    static float moves[kSamples][3][4];
    static uint32_t sampled[kSamples];
    size_t total = 0;
    for (uint32_t before : unique) {
        total += before != kUnmatched;
    }
    size_t step = total / kSamples + 1;
    size_t count = 0;
    size_t seen = 0;
    for (size_t i = 0; i < unique.size() && count < kSamples; i++) {
        if (unique[i] == kUnmatched || seen++ % step != 0) {
            continue;
        }
        const RecordedRun& run = g_record.runs[i];
        const RecordedRun& before = g_previous.runs[unique[i]];
        float now[3][4];
        float then[3][4];
        float back[3][4];
        if (!run_perspective(g_record, run) || !run_perspective(g_previous, before) || !load_matrix(&g_record.rows[run.draw.matrix], now) ||
            !load_matrix(&g_previous.rows[before.draw.matrix], then) || !invert_affine(then, back)) {
            continue;
        }
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                moves[count][r][c] = now[r][0] * back[0][c] + now[r][1] * back[1][c] + now[r][2] * back[2][c] + (c == 3 ? now[r][3] : 0.0f);
            }
        }
        sampled[count++] = static_cast<uint32_t>(i);
    }
    size_t best = 0;
    size_t best_votes = 0;
    for (size_t i = 0; i < count; i++) {
        size_t votes = 0;
        for (size_t k = 0; k < count; k++) {
            votes += close_motion(moves[i], moves[k]);
        }
        if (votes > best_votes) {
            best_votes = votes;
            best = i;
        }
    }
    g_run_camera.valid = best_votes >= 3 && best_votes * 3 >= count;
    g_run_camera.known = false;
    if (!g_run_camera.valid) {
        return;
    }
    std::memcpy(g_run_camera.move, moves[best], sizeof g_run_camera.move);
    const RecordedRun& run = g_record.runs[sampled[best]];
    const RecordedRun& before = g_previous.runs[unique[sampled[best]]];
    float now[4][4];
    float then[4][4];
    double inverse[4][4];
    if (run.draw.camera == before.draw.camera && clip_matrix(g_record, run, now) && clip_matrix(g_previous, before, then) && invert(now, inverse)) {
        g_run_camera.known = true;
        g_run_camera.id = run.draw.camera;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                double sum = 0.0;
                for (int k = 0; k < 4; k++) {
                    sum += then[r][k] * inverse[k][c];
                }
                g_run_camera.back[r][c] = static_cast<float>(sum);
                g_run_camera.known = g_run_camera.known && std::isfinite(sum);
            }
        }
    }
}

float run_distance(const RecordedRun& before, const RecordedRun& run) {
    const Row* a = &g_previous.rows[before.draw.matrix];
    const Row* b = &g_record.rows[run.draw.matrix];
    float distance = 0.0f;
    if (g_run_camera.valid && run_perspective(g_previous, before) && run_perspective(g_record, run)) {
        for (int i = 0; i < 3; i++) {
            const float* r = g_run_camera.move[i];
            distance += std::fabs(r[0] * a[0][3] + r[1] * a[1][3] + r[2] * a[2][3] + r[3] - b[i][3]);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            distance += std::fabs(b[i][3] - a[i][3]);
        }
    }
    return std::isfinite(distance) ? distance : 1e30f;
}

void pair_runs() {
    auto started = std::chrono::steady_clock::now();
    std::vector<RecordedRun>& runs = g_record.runs;
    const std::vector<RecordedRun>& before = g_previous.runs;
    g_run_cut = false;
    g_run_camera.valid = false;
    g_run_camera.known = false;
    for (RecordedRun& run : runs) {
        run.match = kUnmatched;
    }
    if (runs.empty() || before.empty()) {
        return;
    }
    constexpr uint32_t kCandidates = 48;
    static std::vector<uint64_t> keys;
    static std::vector<uint32_t> head;
    static std::vector<uint32_t> next;
    static std::vector<uint32_t> unique;
    size_t size = 64;
    while (size < before.size() * 2) {
        size *= 2;
    }
    keys.assign(size, 0);
    head.assign(size, kUnmatched);
    next.assign(before.size(), kUnmatched);
    auto slot_of = [size](uint64_t key) {
        size_t slot = static_cast<size_t>(key ^ (key >> 29)) & (size - 1);
        while (keys[slot] != 0 && keys[slot] != key) {
            slot = (slot + 1) & (size - 1);
        }
        return slot;
    };
    for (size_t i = before.size(); i-- > 0;) {
        uint64_t key = before[i].draw.key;
        if (key == 0) {
            continue;
        }
        size_t slot = slot_of(key);
        keys[slot] = key;
        next[i] = head[slot];
        head[slot] = static_cast<uint32_t>(i);
    }
    unique.assign(runs.size(), kUnmatched);
    static std::vector<uint32_t> last;
    static std::vector<uint32_t> later;
    last.assign(size, kUnmatched);
    later.assign(runs.size(), kUnmatched);
    for (size_t i = runs.size(); i-- > 0;) {
        uint64_t key = runs[i].draw.key;
        size_t slot = key != 0 ? slot_of(key) : 0;
        if (key != 0 && keys[slot] == key) {
            later[i] = last[slot];
            last[slot] = static_cast<uint32_t>(i);
        }
    }
    for (size_t i = 0; i < runs.size(); i++) {
        uint64_t key = runs[i].draw.key;
        size_t slot = key != 0 ? slot_of(key) : 0;
        if (key != 0 && keys[slot] == key && head[slot] != kUnmatched && next[head[slot]] == kUnmatched) {
            unique[i] = head[slot];
        }
    }
    find_run_camera(unique);
    uint32_t paired = 0;
    uint32_t rejected = 0;
    for (size_t i = 0; i < runs.size(); i++) {
        RecordedRun& run = runs[i];
        uint64_t key = run.draw.key;
        if (key == 0) {
            continue;
        }
        size_t slot = slot_of(key);
        if (keys[slot] != key || head[slot] == kUnmatched) {
            continue;
        }
        uint32_t* link = &head[slot];
        uint32_t* best = link;
        float best_distance = next[*link] == kUnmatched && later[i] == kUnmatched ? 0.0f : run_distance(before[*link], run);
        link = &next[*link];
        for (uint32_t n = 1; n < kCandidates && *link != kUnmatched && best_distance > 0.0f; n++, link = &next[*link]) {
            float distance = run_distance(before[*link], run);
            if (distance < best_distance) {
                best_distance = distance;
                best = link;
            }
        }
        bool taken = false;
        for (uint32_t n = 0, other = later[i]; n < kCandidates && other != kUnmatched && !taken && best_distance > 0.0f; n++, other = later[other]) {
            taken = run_distance(before[*best], runs[other]) < best_distance;
        }
        if (taken) {
            continue;
        }
        run.match = *best;
        *best = next[*best];
        paired++;
        rejected += matrix_change(&g_previous.rows[before[run.match].draw.matrix], &g_record.rows[run.draw.matrix]) == kOtherMatrix;
    }
    g_run_cut = rejected > 8 && rejected * 3 > paired;
    if (g_pair_notes_wanted) {
        std::string& note = g_pair_notes[g_frame % g_pair_notes.size()];
        char part[160];
        std::snprintf(part, sizeof part, "frame %llu runs %zu paired %u far %u cut %d:", static_cast<unsigned long long>(g_frame), runs.size(), paired, rejected, g_run_cut);
        note = part;
        for (size_t i = 0; i < runs.size() && note.size() < 6000; i++) {
            uint64_t key = runs[i].draw.key;
            size_t slot = key != 0 ? slot_of(key) : 0;
            uint32_t copies = 0;
            for (uint32_t other = key != 0 && keys[slot] == key ? last[slot] : kUnmatched; other != kUnmatched && copies < 4; other = later[other]) {
                copies++;
            }
            if (copies < 4) {
                continue;
            }
            const Row* b = &g_record.rows[runs[i].draw.matrix];
            float size = std::max({std::fabs(b[0][0]), std::fabs(b[0][1]), std::fabs(b[0][2])});
            if (runs[i].match == kUnmatched) {
                std::snprintf(part, sizeof part, " | %zu new %04x at %.1f %.1f %.1f size %.2f", i, static_cast<unsigned>(key & 0xFFFF), b[0][3], b[1][3], b[2][3], size);
            } else {
                const Row* a = &g_previous.rows[before[runs[i].match].draw.matrix];
                std::snprintf(part, sizeof part, " | %zu<-%u %04x kind %d from %.1f %.1f %.1f to %.1f %.1f %.1f size %.2f<-%.2f", i, runs[i].match, static_cast<unsigned>(key & 0xFFFF),
                              static_cast<int>(matrix_change(a, b)), a[0][3], a[1][3], a[2][3], b[0][3], b[1][3], b[2][3], size,
                              std::max({std::fabs(a[0][0]), std::fabs(a[0][1]), std::fabs(a[0][2])}));
            }
            note += part;
        }
    }
    g_run_counts[0] += static_cast<uint32_t>(runs.size());
    g_run_counts[1] += paired;
    g_run_counts[2] += rejected;
    g_time_pairing += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void adopt_run_camera() {
    if (!g_run_camera.known) {
        return;
    }
    CameraMotion* chosen = nullptr;
    for (CameraMotion& camera : g_cameras) {
        if (camera.claimed && camera.id == g_run_camera.id && !camera.is_second) {
            chosen = camera.known ? nullptr : &camera;
            break;
        }
        if (!camera.claimed && !chosen) {
            chosen = &camera;
        }
    }
    if (chosen) {
        chosen->claimed = true;
        chosen->known = true;
        chosen->id = g_run_camera.id;
        std::memcpy(chosen->back, g_run_camera.back, sizeof chosen->back);
    }
}

bool send_replayed() {
    if (g_replay_draws.empty()) {
        return true;
    }
    uint32_t start = 0;
    bool ok = upload_run(g_device.run_rows, g_run_row_cursor, kRunRowCapacity * sizeof(float[4]), g_replay_rows.data(),
                         static_cast<uint32_t>(g_replay_rows.size() * sizeof(float[4])), start);
    if (ok) {
        g_device.context->RSSetState(g_device.rasterizer);
        issue_runs(g_replay_draws.data(), g_replay_draws.size(), 0, 0, start / static_cast<uint32_t>(sizeof(float[4])));
    }
    g_replay_draws.clear();
    g_replay_rows.clear();
    return ok;
}

bool replay_runs(const RecordedCommand& command, float t) {
    uint32_t palette_at = kNoPalette;
    uint32_t palette_now = kNoPalette;
    uint32_t palette_before = kNoPalette;
    for (size_t i = command.begin; i < command.end; i++) {
        const RecordedRun& run = g_record.runs[i];
        const Row* now = &g_record.rows[run.draw.row];
        const RecordedRun* before = run.match != kUnmatched ? &g_previous.runs[run.match] : nullptr;
        const Row* then = before ? &g_previous.rows[before->draw.row] : nullptr;
        if (before && !same_shape(before->draw, run.draw, then, now)) {
            before = nullptr;
        }
        if (g_replay_rows.size() + run.draw.rows + kPaletteRows + kNormalRows > kRunRowCapacity / 2) {
            if (!send_replayed()) {
                return false;
            }
            palette_at = kNoPalette;
        }
        if (run.draw.palette != kNoPalette) {
            uint32_t other = before ? before->draw.palette : kNoPalette;
            if (palette_at == kNoPalette || palette_now != run.draw.palette || palette_before != other) {
                palette_at = static_cast<uint32_t>(g_replay_rows.size());
                palette_now = run.draw.palette;
                palette_before = other;
                const Row* palette = &g_record.rows[run.draw.palette];
                g_replay_rows.insert(g_replay_rows.end(), palette, palette + kPaletteRows + kNormalRows);
                if (other != kNoPalette) {
                    blend_palette(&g_replay_rows[palette_at], &g_previous.rows[other], palette, t);
                }
            }
        }
        uint32_t at = static_cast<uint32_t>(g_replay_rows.size());
        g_replay_rows.insert(g_replay_rows.end(), now, now + run.draw.rows);
        Row* out = &g_replay_rows[at];
        if (before) {
            blend_run(out, then, now, t);
        }
        if (run.draw.palette != kNoPalette) {
            out[3][2] = static_cast<float>(static_cast<int>(palette_at) - static_cast<int>(at));
        }
        RunDraw draw = run.draw;
        draw.row = at;
        g_replay_draws.push_back(draw);
    }
    return send_replayed();
}

bool replay_between(const std::vector<std::pair<uint32_t, uint32_t>>& matches, float t, ID3D11RenderTargetView* target, int x, int y, int width,
                    int height, bool depth, const CopyFilter& filter) {
    auto started = std::chrono::steady_clock::now();
    ID3D11DeviceContext* context = g_device.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(context->Map(g_device.moved_buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    auto* moved = static_cast<std::array<float, 4>*>(mapped.pData);
    constexpr std::array<float, 4> kKeep = {0.0f, 0.0f, 0.0f, -1e30f};
    float motion[kMaxCameras][4][4];
    for (size_t k = 0; k < kMaxCameras; k++) {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                motion[k][r][c] = g_cameras[k].back[r][c] * (1.0f - t) + (r == c ? t : 0.0f);
            }
        }
    }
    size_t written = 0;
    for (const auto& [d, p] : matches) {
        const RecordedDraw& draw = g_record.draws[d];
        std::fill(moved + written, moved + draw.first_vertex, kKeep);
        size_t camera = p >= kByCamera ? p - kByCamera : g_world_camera[d];
        if (p >= kByCamera || (camera < kMaxCameras && !g_world_moving[d])) {
            written = draw.first_vertex + draw.vertices;
            std::fill(moved + draw.first_vertex, moved + written, std::array<float, 4>{static_cast<float>(camera), 0.0f, 0.0f, -2e30f});
            continue;
        }
        const RecordedDraw& before = g_previous.draws[p];
        if (camera < kMaxCameras) {
            const double (*forward)[4] = g_forward[camera];
            const float (*now)[4] = motion[camera];
            size_t second = g_cameras[camera].second;
            for (uint32_t v = 0; v < draw.vertices; v++) {
                size_t index = draw.first_vertex + v;
                uint16_t source = g_record.source_of[index];
                const std::array<float, 4>* a = source == kNoSource ? nullptr : &g_previous.positions[before.first_position + source];
                const std::array<float, 4>* b = source == kNoSource ? nullptr : &g_record.positions[draw.first_position + source];
                if (!a || std::isnan((*a)[0]) || std::isnan((*b)[0])) {
                    moved[index] = kKeep;
                    continue;
                }
                size_t follow = stillness(*a, *b, g_cameras[camera]) != kMoved ? camera
                                : second < kMaxCameras && stillness(*a, *b, g_cameras[second]) == kStill ? second : kMaxCameras;
                if (follow < kMaxCameras) {
                    moved[index] = {static_cast<float>(follow), 0.0f, 0.0f, -2e30f};
                    continue;
                }
                double world[4];
                for (int r = 0; r < 4; r++) {
                    double seen = forward[r][0] * (*a)[0] + forward[r][1] * (*a)[1] + forward[r][2] * (*a)[2] + forward[r][3] * (*a)[3];
                    world[r] = seen + ((*b)[r] - seen) * t;
                }
                for (int r = 0; r < 4; r++) {
                    moved[index][r] = static_cast<float>(now[r][0] * world[0] + now[r][1] * world[1] + now[r][2] * world[2] + now[r][3] * world[3]);
                }
            }
            written = draw.first_vertex + draw.vertices;
            continue;
        }
        for (uint32_t v = 0; v < draw.vertices; v++) {
            size_t index = draw.first_vertex + v;
            uint16_t source = g_record.source_of[index];
            const std::array<float, 4>* a = source == kNoSource ? nullptr : &g_previous.positions[before.first_position + source];
            const std::array<float, 4>* b = source == kNoSource ? nullptr : &g_record.positions[draw.first_position + source];
            if (!a || std::isnan((*a)[0]) || std::isnan((*b)[0])) {
                moved[index] = kKeep;
                continue;
            }
            float ax = (*a)[0] + g_decal_shift[d][0];
            float ay = (*a)[1] + g_decal_shift[d][1];
            moved[index] = {ax + ((*b)[0] - ax) * t, ay + ((*b)[1] - ay) * t, (*a)[2] + ((*b)[2] - (*a)[2]) * t, (*a)[3] + ((*b)[3] - (*a)[3]) * t};
        }
        written = draw.first_vertex + draw.vertices;
    }
    std::fill(moved + written, moved + g_frame_vertices, kKeep);
    context->Unmap(g_device.moved_buffer, 0);
    auto filled = std::chrono::steady_clock::now();
    g_time_filling += std::chrono::duration<double>(filled - started).count();
    struct Sending {
        std::chrono::steady_clock::time_point from;
        ~Sending() { g_time_sending += std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(); }
    } sending{filled};
    context->UpdateSubresource(g_device.motion, 0, nullptr, motion, 0, 0);
    context->VSSetConstantBuffers(1, 1, &g_device.motion);
    load_efb(g_efb_before);
    static std::vector<std::pair<const RecordedTarget*, ReplayTarget*>> twins;
    twins.clear();
    for (const RecordedTarget& target : g_record.targets) {
        if (!target.twin) {
            continue;
        }
        auto found = g_replay_targets.find(target.target);
        if (found == g_replay_targets.end()) {
            return false;
        }
        context->CopyResource(found->second.texture, found->second.before);
        twins.emplace_back(&target, &found->second);
    }
    auto twin_of = [](ID3D11RenderTargetView* target) {
        for (const auto& [recorded, twin] : twins) {
            if (recorded->target == target) {
                return twin->target;
            }
        }
        return target;
    };
    BatchState swapped;
    for (const RecordedCommand& command : g_record.commands) {
        switch (command.kind) {
        case RecordedCommand::kBatch:
        case RecordedCommand::kRuns: {
            const BatchState* state = &command.batch;
            for (size_t map = 0; map < kTextureMaps; map++) {
                for (const auto& [recorded, twin] : twins) {
                    if (command.batch.views[map] == recorded->view) {
                        if (state != &swapped) {
                            swapped = command.batch;
                            state = &swapped;
                        }
                        swapped.views[map] = twin->view;
                    }
                }
            }
            if (command.kind == RecordedCommand::kRuns) {
                set_batch_state(*state, false);
                if (!replay_runs(command, t)) {
                    return false;
                }
                break;
            }
            set_batch_state(*state, true);
            context->Draw(static_cast<UINT>(command.end - command.begin), static_cast<UINT>(command.begin));
            break;
        }
        case RecordedCommand::kCopy: {
            const CopyCommand& copy = command.copy;
            run_copy(twin_of(copy.target), copy.target_width, copy.target_height, copy.x, copy.y, copy.width, copy.height, copy.format, copy.intensity, copy.alpha, copy.depth,
                     copy.filter);
            break;
        }
        case RecordedCommand::kClear:
            run_clear(command.clear);
            break;
        }
    }
    return run_copy(target, g_device.frame_width, g_device.frame_height, x, y, width, height, kXfbFormat, false, false, depth, filter);
}

enum PairKind : uint8_t { kNoPair, kCountChanged, kSameCount, kOnly };

template <typename Body>
void parallel_for(size_t count, const Body& body) {
    struct Pool {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable done;
        const std::function<void(size_t)>* job = nullptr;
        size_t count = 0;
        std::atomic<size_t> next{0};
        uint64_t generation = 0;
        size_t working = 0;
        size_t helpers = 0;
    };
    constexpr size_t kChunk = 64;
    static Pool* pool = [] {
        Pool* made = new Pool;
        size_t cores = std::thread::hardware_concurrency();
        made->helpers = cores > 4 ? std::min<size_t>(3, cores - 3) : 0;
        for (size_t h = 0; h < made->helpers; h++) {
            std::thread([made] {
                uint64_t seen = 0;
                std::unique_lock<std::mutex> lock(made->mutex);
                while (true) {
                    made->wake.wait(lock, [&] { return made->generation != seen; });
                    seen = made->generation;
                    const std::function<void(size_t)>* job = made->job;
                    size_t count = made->count;
                    lock.unlock();
                    for (size_t i = made->next.fetch_add(kChunk); i < count; i = made->next.fetch_add(kChunk)) {
                        for (size_t j = i; j < std::min(count, i + kChunk); j++) {
                            (*job)(j);
                        }
                    }
                    lock.lock();
                    if (--made->working == 0) {
                        made->done.notify_all();
                    }
                }
            }).detach();
        }
        return made;
    }();
    std::function<void(size_t)> job = body;
    if (pool->helpers == 0 || count < 256) {
        for (size_t i = 0; i < count; i++) {
            job(i);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->job = &job;
        pool->count = count;
        pool->next = 0;
        pool->working = pool->helpers;
        pool->generation++;
    }
    pool->wake.notify_all();
    for (size_t i = pool->next.fetch_add(kChunk); i < count; i = pool->next.fetch_add(kChunk)) {
        for (size_t j = i; j < std::min(count, i + kChunk); j++) {
            job(j);
        }
    }
    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->done.wait(lock, [&] { return pool->working == 0; });
}

void match_by(uint64_t RecordedDraw::*key, bool trusted, std::vector<uint32_t>& partner, std::vector<uint8_t>& taken, std::vector<uint8_t>& kind,
              uint32_t& moved_too_far) {
    static std::vector<std::pair<uint32_t, uint32_t>> candidates;
    static std::vector<uint8_t> close;
    candidates.clear();
    struct Slot {
        uint64_t key = 0;
        uint32_t next = ~0u;
        uint32_t before = 0;
        uint32_t now = 0;
    };
    static std::vector<Slot> table;
    static std::vector<uint32_t> next_same;
    size_t size = 1;
    while (size < (g_previous.draws.size() + g_record.draws.size()) * 2) {
        size <<= 1;
    }
    table.assign(size, Slot{});
    auto slot = [&](uint64_t value) -> Slot& {
        size_t i = (value * 0x9E3779B97F4A7C15ull) >> 20 & (size - 1);
        while (table[i].key != 0 && table[i].key != value) {
            i = (i + 1) & (size - 1);
        }
        table[i].key = value;
        return table[i];
    };
    next_same.assign(g_previous.draws.size(), ~0u);
    for (size_t i = g_previous.draws.size(); i-- > 0;) {
        uint64_t value = g_previous.draws[i].*key;
        if (value == 0 || taken[i]) {
            continue;
        }
        Slot& entry = slot(value);
        next_same[i] = entry.next;
        entry.next = static_cast<uint32_t>(i);
        entry.before++;
    }
    for (size_t d = 0; d < g_record.draws.size(); d++) {
        if (partner[d] == kUnmatched && g_record.draws[d].*key != 0) {
            slot(g_record.draws[d].*key).now++;
        }
    }
    for (size_t d = 0; d < g_record.draws.size(); d++) {
        const RecordedDraw& draw = g_record.draws[d];
        if (partner[d] != kUnmatched || draw.*key == 0) {
            continue;
        }
        Slot& entry = slot(draw.*key);
        uint32_t p = entry.next;
        if (p == ~0u) {
            continue;
        }
        entry.next = next_same[p];
        kind[d] = entry.before != entry.now ? kCountChanged : (entry.now == 1 ? kOnly : kSameCount);
        const RecordedDraw& before = g_previous.draws[p];
        if (before.positions != draw.positions || draw.vertices == 0) {
            continue;
        }
        candidates.emplace_back(static_cast<uint32_t>(d), p);
    }
    close.assign(candidates.size(), 0);
    parallel_for(candidates.size(), [](size_t i) {
        close[i] = close_draw(g_previous.draws[candidates[i].second], g_record.draws[candidates[i].first]);
    });
    for (size_t i = 0; i < candidates.size(); i++) {
        auto [d, p] = candidates[i];
        if (!close[i]) {
            moved_too_far++;
            if (!trusted) {
                partner[d] = kRejected;
                continue;
            }
        }
        partner[d] = p;
        taken[p] = 1;
    }
}

bool interpolated_targets() {
    if (g_device.interpolated[0] && g_device.interpolated_width == g_device.frame_width && g_device.interpolated_height == g_device.frame_height) {
        return true;
    }
    for (size_t i = 0; i < kMaxBetween; i++) {
        release(g_device.interpolated_target[i]);
        release(g_device.interpolated_view[i]);
        release(g_device.interpolated[i]);
    }
    for (size_t i = 0; i < kMaxBetween; i++) {
        if (!create_copy_texture(g_device.interpolated[i], g_device.interpolated_view[i], g_device.interpolated_target[i], g_device.frame_width,
                                 g_device.frame_height)) {
            for (size_t j = 0; j <= i; j++) {
                release(g_device.interpolated_target[j]);
                release(g_device.interpolated_view[j]);
                release(g_device.interpolated[j]);
            }
            return false;
        }
    }
    g_device.interpolated_width = g_device.frame_width;
    g_device.interpolated_height = g_device.frame_height;
    return true;
}

uint32_t g_match[6] = {};

void find_cameras(const std::vector<uint32_t>& partner, const std::vector<uint8_t>& kind) {
    for (CameraMotion& camera : g_cameras) {
        camera.claimed = false;
        camera.known = false;
        camera.second = kMaxCameras;
        camera.is_second = false;
        camera.from.clear();
        camera.to.clear();
    }
    constexpr size_t kMaxSamples = 8192;
    constexpr size_t kThreePoints = 2048;
    for (size_t d = 0; d < partner.size(); d++) {
        if (partner[d] >= kRejected || kind[d] != kOnly) {
            continue;
        }
        const RecordedDraw& draw = g_record.draws[d];
        const RecordedDraw& before = g_previous.draws[partner[d]];
        CameraMotion* camera = nullptr;
        for (CameraMotion& candidate : g_cameras) {
            if (!candidate.claimed) {
                candidate.claimed = true;
                candidate.id = draw.camera;
            }
            if (candidate.id == draw.camera) {
                camera = &candidate;
                break;
            }
        }
        if (!camera || camera->from.size() >= kMaxSamples || draw.positions == 0) {
            continue;
        }
        bool three = camera->from.size() < kThreePoints;
        for (uint32_t i : {draw.positions / 2, 0u, draw.positions - 1}) {
            const std::array<float, 4>& a = g_previous.positions[before.first_position + i];
            const std::array<float, 4>& b = g_record.positions[draw.first_position + i];
            if (!std::isnan(a[0]) && !std::isnan(b[0])) {
                camera->from.push_back(b);
                camera->to.push_back(a);
            }
            if (!three) {
                break;
            }
        }
    }
    for (CameraMotion& camera : g_cameras) {
        camera.known = camera.claimed && gx::fit_motion(reinterpret_cast<const float(*)[4]>(camera.from.data()), reinterpret_cast<const float(*)[4]>(camera.to.data()),
                                                        camera.from.size(), camera.back);
    }
    static std::vector<std::array<float, 4>> rest_from;
    static std::vector<std::array<float, 4>> rest_to;
    for (size_t k = 0; k < kMaxCameras; k++) {
        CameraMotion& camera = g_cameras[k];
        if (!camera.known || camera.is_second) {
            continue;
        }
        size_t free = kMaxCameras;
        for (size_t j = 0; j < kMaxCameras; j++) {
            if (!g_cameras[j].claimed) {
                free = j;
                break;
            }
        }
        if (free == kMaxCameras) {
            break;
        }
        rest_from.clear();
        rest_to.clear();
        for (size_t i = 0; i < camera.from.size(); i++) {
            const std::array<float, 4>& b = camera.from[i];
            const std::array<float, 4>& a = camera.to[i];
            if (a[3] <= 0.0f || b[3] <= 0.0f) {
                continue;
            }
            double p[4];
            for (int r = 0; r < 4; r++) {
                p[r] = static_cast<double>(camera.back[r][0]) * b[0] + static_cast<double>(camera.back[r][1]) * b[1] + static_cast<double>(camera.back[r][2]) * b[2] +
                       static_cast<double>(camera.back[r][3]) * b[3];
            }
            if (p[3] <= 0.0 || std::max(std::fabs(p[0] / p[3] - a[0] / a[3]), std::fabs(p[1] / p[3] - a[1] / a[3])) > kStillScreen) {
                rest_from.push_back(b);
                rest_to.push_back(a);
            }
        }
        constexpr size_t kMinimumRest = 96;
        CameraMotion& other = g_cameras[free];
        if (rest_from.size() < kMinimumRest ||
            !gx::fit_motion(reinterpret_cast<const float(*)[4]>(rest_from.data()), reinterpret_cast<const float(*)[4]>(rest_to.data()), rest_from.size(), other.back)) {
            continue;
        }
        other.claimed = true;
        other.known = true;
        other.is_second = true;
        other.id = camera.id;
        camera.second = free;
    }
}

size_t camera_of(uint32_t id) {
    for (size_t k = 0; k < kMaxCameras; k++) {
        if (g_cameras[k].known && g_cameras[k].id == id) {
            return k;
        }
    }
    return kMaxCameras;
}

bool near_camera_path(const RecordedDraw& draw, const RecordedDraw& before, const CameraMotion& camera) {
    const std::array<float, 4>& a = g_previous.positions[before.first_position];
    const std::array<float, 4>& b = g_record.positions[draw.first_position];
    if (std::isnan(a[0]) || std::isnan(b[0])) {
        return false;
    }
    float p[4];
    for (int r = 0; r < 4; r++) {
        p[r] = camera.back[r][0] * b[0] + camera.back[r][1] * b[1] + camera.back[r][2] * b[2] + camera.back[r][3] * b[3];
    }
    constexpr float kNear = 0.05f;
    return p[3] > 0.0f && a[3] > 0.0f && std::fabs(p[0] / p[3] - a[0] / a[3]) <= kNear && std::fabs(p[1] / p[3] - a[1] / a[3]) <= kNear;
}

bool fixed_on_screen(const RecordedDraw& draw, const RecordedDraw& before) {
    return draw.positions != 0 && draw.positions == before.positions &&
           std::memcmp(&g_record.positions[draw.first_position], &g_previous.positions[before.first_position], draw.positions * sizeof(std::array<float, 4>)) == 0;
}

std::vector<std::pair<uint32_t, uint32_t>> g_matches;
bool g_camera_cut = false;

void follow_decals(const std::vector<uint32_t>& partner, const std::vector<uint8_t>& kind, const std::vector<uint8_t>& column, std::vector<uint32_t>& target) {
    constexpr size_t kMaxGroup = 24;
    constexpr float kSame = 1e-3f;
    constexpr float kMaxMove = 0.1f;
    g_decal_shift.assign(partner.size(), {0.0f, 0.0f});
    static std::vector<uint8_t> grouped;
    static std::vector<uint32_t> group;
    static std::vector<std::array<float, 2>> before;
    static std::vector<std::array<float, 2>> now;
    grouped.assign(partner.size(), 0);
    auto candidate = [&](size_t d) { return !grouped[d] && column[d] == 2 && kind[d] == kSameCount && partner[d] < kRejected; };
    for (size_t first = 0; first < partner.size(); first++) {
        if (!candidate(first)) {
            continue;
        }
        uint32_t id = g_record.draws[first].camera;
        group.clear();
        before.clear();
        now.clear();
        bool flat = true;
        for (size_t d = first; d < partner.size(); d++) {
            if (!candidate(d) || g_record.draws[d].camera != id) {
                continue;
            }
            grouped[d] = 1;
            group.push_back(static_cast<uint32_t>(d));
            const std::array<float, 4>& a = g_previous.positions[g_previous.draws[partner[d]].first_position];
            const std::array<float, 4>& b = g_record.positions[g_record.draws[d].first_position];
            flat = flat && a[3] == 1.0f && b[3] == 1.0f && !std::isnan(a[0]) && !std::isnan(b[0]);
            before.push_back({a[0], a[1]});
            now.push_back({b[0], b[1]});
        }
        size_t count = group.size();
        if (!flat || count < 2 || count > kMaxGroup) {
            continue;
        }
        auto rests = [&](size_t k, float x, float y) {
            for (size_t l = 0; l < count; l++) {
                if (std::fabs(now[k][0] - before[l][0] - x) < kSame && std::fabs(now[k][1] - before[l][1] - y) < kSame) {
                    return true;
                }
            }
            return false;
        };
        size_t best = 0;
        float shift[2] = {};
        for (size_t i = 0; i < count; i++) {
            for (size_t j = 0; j < count; j++) {
                float x = now[i][0] - before[j][0];
                float y = now[i][1] - before[j][1];
                size_t support = 0;
                for (size_t k = 0; k < count; k++) {
                    support += rests(k, x, y);
                }
                if (support > best) {
                    best = support;
                    shift[0] = x;
                    shift[1] = y;
                }
            }
        }
        if (best * 2 < count) {
            continue;
        }
        for (size_t k = 0; k < count; k++) {
            if (rests(k, shift[0], shift[1]) || std::fabs(now[k][0] - before[k][0] - shift[0]) > kMaxMove || std::fabs(now[k][1] - before[k][1] - shift[1]) > kMaxMove) {
                continue;
            }
            target[group[k]] = partner[group[k]];
            g_decal_shift[group[k]] = {shift[0], shift[1]};
        }
    }
}

void pair_frame() {
    auto started = std::chrono::steady_clock::now();
    static std::vector<uint32_t> partner;
    static std::vector<uint8_t> taken;
    partner.assign(g_record.draws.size(), kUnmatched);
    taken.assign(g_previous.draws.size(), 0);
    uint32_t moved_too_far = 0;
    static std::vector<uint8_t> kind;
    kind.assign(g_record.draws.size(), kNoPair);
    match_by(&RecordedDraw::exact, true, partner, taken, kind, moved_too_far);
    match_by(&RecordedDraw::key, false, partner, taken, kind, moved_too_far);
    find_cameras(partner, kind);
    adopt_run_camera();
    static bool forward[kMaxCameras];
    for (size_t k = 0; k < kMaxCameras; k++) {
        forward[k] = g_cameras[k].known && invert(g_cameras[k].back, g_forward[k]);
    }
    g_world_camera.assign(g_record.draws.size(), static_cast<uint8_t>(kMaxCameras));
    g_world_moving.assign(g_record.draws.size(), 0);
    constexpr uint8_t kNoDraw = 255;
    static std::vector<uint32_t> target;
    static std::vector<uint8_t> column;
    target.assign(partner.size(), kUnmatched);
    column.assign(partner.size(), 0);
    parallel_for(partner.size(), [](size_t d) {
        const RecordedDraw& draw = g_record.draws[d];
        if (draw.vertices == 0) {
            column[d] = kNoDraw;
            return;
        }
        bool pair = partner[d] < kRejected;
        size_t camera = camera_of(draw.camera);
        if (pair && camera < kMaxCameras && forward[camera]) {
            g_world_camera[d] = static_cast<uint8_t>(camera);
            g_world_moving[d] = moves_in_world(draw, g_previous.draws[partner[d]], g_cameras[camera]);
        }
        if (pair && camera < kMaxCameras && kind[d] != kOnly && fixed_on_screen(draw, g_previous.draws[partner[d]])) {
            target[d] = partner[d];
            column[d] = 1;
        } else if (camera < kMaxCameras && !(pair && (kind[d] == kOnly || (kind[d] == kSameCount && near_camera_path(draw, g_previous.draws[partner[d]], g_cameras[camera]))))) {
            target[d] = kByCamera + static_cast<uint32_t>(camera);
            column[d] = 5;
        } else if (pair && camera == kMaxCameras && kind[d] != kOnly) {
            column[d] = 2;
        } else if (pair) {
            target[d] = partner[d];
            column[d] = kind[d] == kOnly ? 0 : 1;
        } else {
            column[d] = partner[d] == kRejected ? 2 : (draw.exact != 0 ? 3 : 4);
        }
    });
    follow_decals(partner, kind, column, target);
    g_matches.clear();
    for (size_t d = 0; d < partner.size(); d++) {
        if (column[d] == kNoDraw) {
            continue;
        }
        g_match[column[d]]++;
        if (target[d] != kUnmatched) {
            g_matches.emplace_back(static_cast<uint32_t>(d), target[d]);
        }
    }
    g_camera_cut = moved_too_far > 8 && moved_too_far * 3 > g_matches.size();
    g_time_pairing += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

bool g_paired = false;

bool can_pair() {
    return g_recording && !g_record_broken && ((!g_previous.draws.empty() && !g_record.draws.empty()) || (!g_previous.runs.empty() && !g_record.runs.empty()));
}

uint32_t run_interpolated(int x, int y, int width, int height, bool depth, const CopyFilter& filter, const pacing::FramePlan& plan) {
    if (!can_pair() || !g_efb_before_valid || !g_device.moved_shader || g_frame_vertices > kVertexCapacity || !interpolated_targets()) {
        return 0;
    }
    if (!g_paired) {
        pair_runs();
        if (!g_previous.draws.empty() && !g_record.draws.empty()) {
            pair_frame();
        } else {
            g_matches.clear();
            g_camera_cut = false;
        }
        g_paired = true;
    }
    if (g_camera_cut || g_run_cut) {
        return 0;
    }
    uint32_t count = std::min<uint32_t>(plan.images, kMaxBetween);
    for (uint32_t step = 0; step < count; step++) {
        if (!replay_between(g_matches, plan.fractions[step], g_device.interpolated_target[step], x, y, width, height, depth, filter)) {
            return step;
        }
    }
    return count;
}

int g_display_hz = 0;
std::atomic<uint32_t> g_late_frames{0};
std::atomic<uint32_t> g_hidden_frames{0};
double g_phase = 0.0;
bool g_show_real = true;
double g_planned_slot = 1.0 / 120.0;

void end_record() {
    if (g_recording && !g_record_broken) {
        g_previous.positions.swap(g_record.positions);
        g_previous.draws.swap(g_record.draws);
        g_previous.runs.swap(g_record.runs);
        g_previous.rows.swap(g_record.rows);
    } else {
        g_previous.positions.clear();
        g_previous.draws.clear();
        g_previous.runs.clear();
        g_previous.rows.clear();
    }
    release_record(g_record);
    g_paired = false;
    if (g_frame % 600 == 0) {
        prune_twins();
    }
    g_triangles.clear();
    g_pending.begin = 0;
    g_frame_vertices = 0;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    g_recording = pacing::interpolation_wanted(static_cast<int>(fps()), g_display_hz);
    g_record_broken = false;
    if (g_recording) {
        g_vertex_cursor = kVertexCapacity;
        g_trimmed_cursor = kVertexBytes;
        g_run_vertex_cursor = kRunVertexBytes;
        g_run_index_cursor = kRunIndexBytes;
    }
    g_run_ring_recorded = false;
}

double g_period = 1.0 / 60.0;
double g_slot = 1.0 / 120.0;

struct FrameEnd {
    bool due = false;
    bool copied = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool depth = false;
    CopyFilter filter;
    bool has_clear = false;
    ClearCommand clear;
};

FrameEnd g_frame_end;

void apply_clear(const ClearCommand& command) {
    run_clear(command);
    if (g_recording) {
        RecordedCommand recorded;
        recorded.kind = RecordedCommand::kClear;
        recorded.clear = command;
        g_record.commands.push_back(recorded);
    }
}

void copy_to_framebuffer(int x, int y, int width, int height, bool depth, const CopyFilter& filter) {
    auto now = std::chrono::steady_clock::now();
    static auto previous = now;
    double period = std::chrono::duration<double>(now - previous).count();
    previous = now;
    if (period > 0.004 && period < 0.1) {
        g_period = g_period * 0.9 + std::min(period, 0.05) * 0.1;
    }
    finish_frame();
    bool copied = copy_frame(x, y, width, height, depth, filter);
    g_interpolated_ready = 0;
    g_frame_end = {true, copied, x, y, width, height, depth, filter, false, {}};
}

bool read_texture(ID3D11Texture2D* texture, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height);

const char* const g_dump_sequence = std::getenv("WP_DUMP_SEQUENCE");

void dump_sequence(uint32_t images) {
    static const uint64_t first = std::strtoull(g_dump_sequence, nullptr, 10);
    static const uint64_t frames = std::strchr(g_dump_sequence, ',') ? std::strtoull(std::strchr(g_dump_sequence, ',') + 1, nullptr, 10) : 30;
    if (g_frame < first || g_frame >= first + frames) {
        return;
    }
    std::error_code created;
    std::filesystem::create_directories("captures/seq", created);
    auto save = [](ID3D11Texture2D* texture, const char* name) {
        std::vector<uint32_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (texture && read_texture(texture, pixels, width, height)) {
            video::save_png(name, pixels, width, height);
        }
    };
    char name[96];
    for (uint32_t i = 0; i < images; i++) {
        std::snprintf(name, sizeof name, "captures/seq/%06llu_%u.png", static_cast<unsigned long long>(g_frame), i);
        save(g_device.interpolated[i], name);
    }
    std::snprintf(name, sizeof name, "captures/seq/%06llu_f.png", static_cast<unsigned long long>(g_frame));
    save(g_device.frame, name);
}

void finish_frame() {
    if (!g_frame_end.due) {
        return;
    }
    FrameEnd end = g_frame_end;
    g_frame_end.due = false;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    constexpr size_t kBehindBytes = 256 << 10;
    bool late = gx::take_frame_held();
    bool saved = end.copied && g_recording && save_efb(g_efb_after);
    auto started = std::chrono::steady_clock::now();
    pacing::FramePlan plan;
    if (saved) {
        plan = pacing::plan_frame(g_phase, g_period, pacing::output_rate(static_cast<int>(fps())), kMaxBetween);
        if (g_dump_sequence || (!late && gx::gpu_backlog() < kBehindBytes)) {
            g_interpolated_ready = run_interpolated(end.x, end.y, end.width, end.height, end.depth, end.filter, plan);
            load_efb(g_efb_after);
        }
        std::swap(g_efb_before, g_efb_after);
    }
    bool complete = plan.images > 0 && g_interpolated_ready == plan.images;
    g_show_real = plan.show_real || !complete;
    g_planned_slot = complete ? plan.slot : g_period / (g_interpolated_ready + 1);
    if (!complete) {
        g_phase = 0.0;
    }
    if (g_dump_sequence && end.copied) {
        dump_sequence(g_interpolated_ready);
    }
    if (end.copied && g_recording && g_interpolated_ready == 0) {
        g_late_frames++;
    }
    if (g_recording) {
        static auto report = started;
        static uint32_t frames = 0, with_images = 0, late_frames = 0, draws = 0;
        static double seconds = 0.0;
        auto now = std::chrono::steady_clock::now();
        frames++;
        with_images += g_interpolated_ready != 0;
        late_frames += late;
        draws += static_cast<uint32_t>(g_record.draws.size());
        seconds += std::chrono::duration<double>(now - started).count();
        if (now - report >= std::chrono::seconds(1)) {
            uint32_t n = std::max(1u, with_images);
            log::write("replay", "%u frames, %u with images, %u late, %.2f ms per frame for the images (pairing %.2f, positions %.2f, commands %.2f), %u draws per frame: %u sure pairs, %u by order, %u rejected, %u unpaired, %u unpaired inline, %u with the camera (%u cameras)",
                       frames, with_images, late_frames, seconds * 1000.0 / frames, g_time_pairing * 1000.0 / frames, g_time_filling * 1000.0 / frames, g_time_sending * 1000.0 / frames, draws / frames, g_match[0] / n, g_match[1] / n, g_match[2] / n, g_match[3] / n, g_match[4] / n,
                       g_match[5] / n, static_cast<uint32_t>(std::count_if(std::begin(g_cameras), std::end(g_cameras), [](const CameraMotion& c) { return c.known; })));
            if (g_run_counts[0] != 0) {
                log::write("runs", "%u models per frame redrawn for the images: %u paired, %u moved too far, %u texture matrices turned in all", g_run_counts[0] / n, g_run_counts[1] / n, g_run_counts[2] / n, g_run_counts[3]);
            }
            std::fill(std::begin(g_match), std::end(g_match), 0u);
            std::fill(std::begin(g_run_counts), std::end(g_run_counts), 0u);
            report = now;
            frames = with_images = late_frames = draws = 0;
            seconds = 0.0;
            g_time_pairing = g_time_filling = g_time_sending = 0.0;
        }
    }
    g_efb_before_valid = saved;
    end_record();
    if (end.has_clear) {
        apply_clear(end.clear);
    }
}

bool create_present_pipeline() {
    if (g_device.present_vertex && g_device.copy_pixel && g_device.present_sampler) {
        return true;
    }
    ID3DBlob* vertex_code = nullptr;
    ID3DBlob* pixel_code = nullptr;
    if (!compile(kPresentShaderSource, "vertex_main", "vs_5_0", &vertex_code) || !compile(kPresentShaderSource, "pixel_main", "ps_5_0", &pixel_code)) {
        return false;
    }
    bool ok = SUCCEEDED(g_device.device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr, &g_device.present_vertex)) &&
              SUCCEEDED(g_device.device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &g_device.present_pixel));
    release(vertex_code);
    release(pixel_code);
    D3D11_BUFFER_DESC adjust{};
    adjust.ByteWidth = 16;
    adjust.Usage = D3D11_USAGE_DEFAULT;
    adjust.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ok = ok && SUCCEEDED(g_device.device->CreateBuffer(&adjust, nullptr, &g_device.present_constants));
    ID3DBlob* smooth_code = nullptr;
    if (ok && compile(kPresentShaderSource, "pixel_smooth", "ps_5_0", &smooth_code)) {
        g_device.device->CreatePixelShader(smooth_code->GetBufferPointer(), smooth_code->GetBufferSize(), nullptr, &g_device.present_smooth);
        release(smooth_code);
    }
    ID3DBlob* copy_code = nullptr;
    if (ok && compile(kCopyShaderSource, "pixel_main", "ps_5_0", &copy_code)) {
        ok = SUCCEEDED(g_device.device->CreatePixelShader(copy_code->GetBufferPointer(), copy_code->GetBufferSize(), nullptr, &g_device.copy_pixel));
        release(copy_code);
    } else {
        ok = false;
    }
    ID3DBlob* clear_code = nullptr;
    if (ok && compile(kClearShaderSource, "pixel_main", "ps_5_0", &clear_code)) {
        ok = SUCCEEDED(g_device.device->CreatePixelShader(clear_code->GetBufferPointer(), clear_code->GetBufferSize(), nullptr, &g_device.clear_pixel));
        release(clear_code);
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 32;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(g_device.device->CreateBuffer(&constants, nullptr, &g_device.clear_constants));
    } else {
        ok = false;
    }
    if (!ok) {
        return false;
    }
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = FALSE;
    return SUCCEEDED(g_device.device->CreateSamplerState(&sampler, &g_device.present_sampler)) &&
           SUCCEEDED(g_device.device->CreateRasterizerState(&raster, &g_device.present_rasterizer));
}

bool ensure_swapchain(HWND window, UINT width, UINT height) {
    if (!g_device.swapchain) {
        IDXGIDevice* dxgi_device = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory = nullptr;
        bool ok = SUCCEEDED(g_device.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) &&
                  SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
                  SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)));
        if (ok) {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = width;
            description.Height = height;
            description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ok = SUCCEEDED(factory->CreateSwapChainForHwnd(g_device.device, window, &description, nullptr, nullptr, &g_device.swapchain));
            if (ok) {
                factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            }
        }
        release(factory);
        release(adapter);
        release(dxgi_device);
        if (!ok || !create_present_pipeline()) {
            release(g_device.swapchain);
            g_device.failed = true;
            return false;
        }
        g_device.swapchain_width = width;
        g_device.swapchain_height = height;
    } else if (width != g_device.swapchain_width || height != g_device.swapchain_height) {
        release(g_device.backbuffer_view);
        if (FAILED(g_device.swapchain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
            return false;
        }
        g_device.swapchain_width = width;
        g_device.swapchain_height = height;
    }
    if (!g_device.backbuffer_view) {
        ID3D11Texture2D* buffer = nullptr;
        if (FAILED(g_device.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&buffer)))) {
            return false;
        }
        HRESULT result = g_device.device->CreateRenderTargetView(buffer, nullptr, &g_device.backbuffer_view);
        release(buffer);
        return SUCCEEDED(result);
    }
    return true;
}

void release_target() {
    release(g_device.target_view);
    release(g_device.target_resource);
    release(g_device.target);
    release(g_device.depth_view);
    release(g_device.depth_resource);
    release(g_device.depth);
}

// The samplers are cached by their GX parameters only, so a new anisotropy level drops them all between two frames.
void apply_anisotropy() {
    static const settings::LiveNumber setting("video.anisotropy", "WP_ANISOTROPY");
    int level = std::clamp(setting(), 1, 16);
    if (level == g_device.anisotropy) {
        return;
    }
    flush_pending();
    for (auto& entry : g_device.samplers) {
        release(entry.second);
    }
    g_device.samplers.clear();
    g_device.anisotropy = level;
}

void apply_scale() {
    int scale = requested_scale();
    if (scale == g_scale) {
        return;
    }
    flush_pending();
    g_record_broken = true;
    ID3D11DeviceContext* context = g_device.context;
    context->OMSetRenderTargets(0, nullptr, nullptr);
    ID3D11ShaderResourceView* none[2] = {};
    context->PSSetShaderResources(0, 2, none);
    int previous = g_scale;
    release_target();
    g_scale = scale;
    if (!create_target()) {
        release_target();
        g_scale = previous;
        if (!create_target()) {
            std::fputs("cannot recreate the EFB after a scale change\n", stderr);
            release_target();
            g_device.ready = false;
            g_device.failed = true;
            return;
        }
    }
    float black[4] = {0, 0, 0, 0};
    context->ClearRenderTargetView(g_device.target_view, black);
    context->ClearDepthStencilView(g_device.depth_view, D3D11_CLEAR_DEPTH, 0.0f, 0);
}

bool create_overlay_pipeline() {
    if (g_device.overlay_pixel && g_device.overlay_blend) {
        return true;
    }
    if (g_device.overlay_failed) {
        return false;
    }
    ID3DBlob* code = nullptr;
    bool ok = compile(kOverlayShaderSource, "pixel_main", "ps_5_0", &code) &&
              SUCCEEDED(g_device.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_device.overlay_pixel));
    release(code);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(g_device.device->CreateBlendState(&blend, &g_device.overlay_blend));
    if (!ok) {
        std::fputs("cannot create the options menu overlay pipeline\n", stderr);
        release(g_device.overlay_pixel);
        release(g_device.overlay_blend);
        g_device.overlay_failed = true;
    }
    return ok;
}

void upload_overlay(const options::Image& image, ID3D11Texture2D*& texture, ID3D11ShaderResourceView*& view, uint32_t& width, uint32_t& height) {
    if (image.pixels.size() != static_cast<size_t>(image.width) * image.height || image.width == 0 || image.height == 0) {
        return;
    }
    if (texture && width == image.width && height == image.height) {
        g_device.context->UpdateSubresource(texture, 0, nullptr, image.pixels.data(), image.width * 4, 0);
        return;
    }
    release(view);
    release(texture);
    D3D11_TEXTURE2D_DESC description{};
    description.Width = image.width;
    description.Height = image.height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{image.pixels.data(), image.width * 4, 0};
    if (FAILED(g_device.device->CreateTexture2D(&description, &initial, &texture)) || FAILED(g_device.device->CreateShaderResourceView(texture, nullptr, &view))) {
        release(view);
        release(texture);
        return;
    }
    width = image.width;
    height = image.height;
}

void draw_image(ID3D11ShaderResourceView* view, const options::Rect& placed) {
    if (placed.width <= 0 || placed.height <= 0) {
        return;
    }
    ID3D11DeviceContext* context = g_device.context;
    D3D11_VIEWPORT viewport{static_cast<float>(placed.x), static_cast<float>(placed.y), static_cast<float>(placed.width), static_cast<float>(placed.height),
                            0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->OMSetBlendState(g_device.overlay_blend, nullptr, 0xFFFFFFFF);
    context->PSSetShader(g_device.overlay_pixel, nullptr, 0);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &g_device.present_sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void draw_overlay(const RECT& client) {
    options::Image image;
    if (perf::take(g_device.hud_version, image, g_device.hud_visible, g_device.hud_corner) && !image.pixels.empty()) {
        upload_overlay(image, g_device.hud, g_device.hud_view, g_device.hud_width, g_device.hud_height);
    }
    if (options::take_overlay(g_device.overlay_version, image, g_device.overlay_visible) && !image.pixels.empty()) {
        upload_overlay(image, g_device.overlay, g_device.overlay_view, g_device.overlay_width, g_device.overlay_height);
    }
    bool hud = g_device.hud_visible && g_device.hud_view;
    bool menu = g_device.overlay_visible && g_device.overlay_view;
    if ((!hud && !menu) || !create_overlay_pipeline()) {
        return;
    }
    if (hud) {
        draw_image(g_device.hud_view, perf::corner_rect(client.right, client.bottom, static_cast<int>(g_device.hud_width), static_cast<int>(g_device.hud_height), g_device.hud_corner));
    }
    if (menu) {
        draw_image(g_device.overlay_view, options::place(client.right, client.bottom, static_cast<int>(g_device.overlay_width), static_cast<int>(g_device.overlay_height)));
    }
}

bool present_view(HWND window, double aspect, ID3D11ShaderResourceView* view) {
    RECT client;
    GetClientRect(window, &client);
    if (client.right <= 0 || client.bottom <= 0 || !ensure_swapchain(window, static_cast<UINT>(client.right), static_cast<UINT>(client.bottom))) {
        return false;
    }
    flush_pending();
    ID3D11DeviceContext* context = g_device.context;
    float black[4] = {0, 0, 0, 1};
    context->OMSetRenderTargets(1, &g_device.backbuffer_view, nullptr);
    context->ClearRenderTargetView(g_device.backbuffer_view, black);
    double width = client.right;
    double height = width / aspect;
    if (height > client.bottom) {
        height = client.bottom;
        width = height * aspect;
    }
    D3D11_VIEWPORT viewport{static_cast<float>((client.right - width) / 2), static_cast<float>((client.bottom - height) / 2),
                            static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    context->RSSetState(g_device.present_rasterizer);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(nullptr, 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_device.present_vertex, nullptr, 0);
    static const settings::LiveFlag smooth("video.smooth", "WP_SMOOTH");
    context->PSSetShader(smooth() && g_device.present_smooth ? g_device.present_smooth : g_device.present_pixel, nullptr, 0);
    color::Adjust colors = color::current();
    float adjust[4] = {colors.brightness, colors.contrast, colors.saturation, 1.0f / colors.gamma};
    context->UpdateSubresource(g_device.present_constants, 0, nullptr, adjust, 0, 0);
    context->PSSetConstantBuffers(0, 1, &g_device.present_constants);
    context->PSSetShaderResources(0, 1, &view);
    context->PSSetSamplers(0, 1, &g_device.present_sampler);
    context->Draw(3, 0);
    ID3D11ShaderResourceView* none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    draw_overlay(client);
    static const settings::LiveFlag vsync("video.vsync", "WP_VSYNC");
    HRESULT presented = g_device.swapchain->Present(vsync() ? 1 : 0, 0);
    static auto last = std::chrono::steady_clock::now();
    static auto report = last;
    static double longest = 0.0;
    static double shortest = 1e9;
    static double total = 0.0;
    static uint32_t shown = 0;
    auto now = std::chrono::steady_clock::now();
    double interval = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    longest = std::max(longest, interval);
    shortest = std::min(shortest, interval);
    total += interval;
    shown++;
    if (now - report >= std::chrono::seconds(1)) {
        log::write("present", "%u frames shown, %.1f ms apart on average, from %.1f to %.1f ms", shown, total / shown, shortest, longest);
        report = now;
        longest = 0.0;
        shortest = 1e9;
        total = 0.0;
        shown = 0;
    }
    static bool logged_present_failure = false;
    if (FAILED(presented) && !logged_present_failure) {
        logged_present_failure = true;
        HRESULT removed = g_device.device->GetDeviceRemovedReason();
        log::write("video", "Present failed with %08lx, device removed reason %08lx", static_cast<unsigned long>(presented), static_cast<unsigned long>(removed));
    }
    return true;
}

namespace {

HWND g_present_window = nullptr;
double g_present_aspect = 4.0 / 3.0;

struct ShownImage {
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::chrono::steady_clock::time_point due;
    bool interpolated = false;
};

constexpr size_t kShownImages = kMaxBetween + 3;
ShownImage g_shown[kShownImages];
std::deque<size_t> g_show_queue;
std::chrono::steady_clock::time_point g_last_shown;

bool queue_image(ID3D11Texture2D* source, std::chrono::steady_clock::time_point due, bool interpolated) {
    size_t free = kShownImages;
    for (size_t i = 0; i < kShownImages && free == kShownImages; i++) {
        if (std::find(g_show_queue.begin(), g_show_queue.end(), i) == g_show_queue.end()) {
            free = i;
        }
    }
    if (free == kShownImages || !source) {
        return false;
    }
    ShownImage& image = g_shown[free];
    if (!image.texture || image.width != g_device.frame_width || image.height != g_device.frame_height) {
        release(image.view);
        release(image.texture);
        ID3D11RenderTargetView* unused = nullptr;
        if (!create_copy_texture(image.texture, image.view, unused, g_device.frame_width, g_device.frame_height)) {
            release(unused);
            release(image.view);
            release(image.texture);
            return false;
        }
        release(unused);
        image.width = g_device.frame_width;
        image.height = g_device.frame_height;
    }
    g_device.context->CopyResource(image.texture, source);
    image.due = due;
    image.interpolated = interpolated;
    g_show_queue.push_back(free);
    return true;
}

struct Recent {
    std::vector<ID3D11Texture2D*> images;
    std::vector<bool> between;
    size_t next = 0;
    size_t count = 0;
    ID3D11Texture2D* mips = nullptr;
    ID3D11ShaderResourceView* mips_view = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool failed = false;
};

Recent g_recent;

void release_recent() {
    for (ID3D11Texture2D*& image : g_recent.images) {
        release(image);
    }
    release(g_recent.mips_view);
    release(g_recent.mips);
    g_recent = Recent{};
}

bool create_recent(uint32_t width, uint32_t height, uint32_t level) {
    D3D11_TEXTURE2D_DESC description{};
    description.Width = std::max(1u, width >> level);
    description.Height = std::max(1u, height >> level);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    g_recent.images.assign(screenshot::recent_count(description.Width, description.Height), nullptr);
    g_recent.between.assign(g_recent.images.size(), false);
    bool ok = !g_recent.images.empty();
    for (ID3D11Texture2D*& image : g_recent.images) {
        ok = ok && SUCCEEDED(g_device.device->CreateTexture2D(&description, nullptr, &image));
    }
    if (ok && level > 0) {
        description.Width = width;
        description.Height = height;
        description.MipLevels = level + 1;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        description.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ok = SUCCEEDED(g_device.device->CreateTexture2D(&description, nullptr, &g_recent.mips)) &&
             SUCCEEDED(g_device.device->CreateShaderResourceView(g_recent.mips, nullptr, &g_recent.mips_view));
    }
    g_recent.width = width;
    g_recent.height = height;
    return ok;
}

void record_shown(ID3D11Texture2D* source, bool between) {
    static const settings::LiveFlag enabled("video.bug_recorder", "WP_BUG_RECORDER");
    if (!enabled()) {
        if (g_recent.width != 0) {
            release_recent();
        }
        return;
    }
    uint32_t width = g_device.frame_width;
    uint32_t height = g_device.frame_height;
    uint32_t level = screenshot::recent_level(width);
    if (g_recent.width != width || g_recent.height != height) {
        release_recent();
        if (!create_recent(width, height, level)) {
            log::write("video", "the bug recorder could not get its %u images", static_cast<unsigned>(g_recent.images.size()));
            for (ID3D11Texture2D*& image : g_recent.images) {
                release(image);
            }
            g_recent.images.clear();
            g_recent.failed = true;
        }
    }
    g_pair_notes_wanted = !g_recent.failed && !g_recent.images.empty();
    if (g_recent.failed || g_recent.images.empty()) {
        return;
    }
    ID3D11Texture2D* target = g_recent.images[g_recent.next];
    if (level == 0) {
        g_device.context->CopyResource(target, source);
    } else {
        g_device.context->CopySubresourceRegion(g_recent.mips, 0, 0, 0, 0, source, 0, nullptr);
        g_device.context->GenerateMips(g_recent.mips_view);
        g_device.context->CopySubresourceRegion(target, 0, 0, 0, 0, g_recent.mips, level, nullptr);
    }
    g_recent.between[g_recent.next] = between;
    g_recent.next = (g_recent.next + 1) % g_recent.images.size();
    g_recent.count = std::min(g_recent.count + 1, g_recent.images.size());
}

void service_deferred() {
    auto now = std::chrono::steady_clock::now();
    size_t ready = 0;
    while (ready < g_show_queue.size() && g_shown[g_show_queue[ready]].due <= now) {
        ready++;
    }
    if (ready == 0) {
        return;
    }
    size_t skipped = ready > 2 ? ready - 2 : 0;
    ready = skipped + 1;
    const ShownImage& image = g_shown[g_show_queue[ready - 1]];
    if (present_view(g_present_window, g_present_aspect, image.view)) {
        record_shown(image.texture, image.interpolated);
        if (image.interpolated) {
            g_interpolated_frames++;
        }
    }
    g_last_shown = now - image.due < std::chrono::duration<double>(g_slot) ? image.due : now;
    g_show_queue.erase(g_show_queue.begin(), g_show_queue.begin() + static_cast<std::ptrdiff_t>(ready));
}

}

bool present_frame(void* window_handle, double aspect) {
    if (!g_device.ready || !g_device.frame_view || !window_handle) {
        return false;
    }
    finish_frame();
    HWND window = static_cast<HWND>(window_handle);
    g_present_window = window;
    g_present_aspect = aspect;
    static auto checked = std::chrono::steady_clock::time_point{};
    if (std::chrono::steady_clock::now() - checked > std::chrono::seconds(2)) {
        checked = std::chrono::steady_clock::now();
        MONITORINFOEXA monitor{};
        monitor.cbSize = sizeof monitor;
        DEVMODEA mode{};
        mode.dmSize = sizeof mode;
        int hz = 0;
        if (GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) && EnumDisplaySettingsA(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) {
            hz = static_cast<int>(mode.dmDisplayFrequency);
        }
        if (hz != g_display_hz) {
            log::write("video", "display refresh rate %d Hz%s", hz, hz > 0 && hz < 100 ? ", too low for interpolated frames" : "");
            g_display_hz = hz;
            pacing::set_display_hz(hz);
        }
    }
    static uint64_t queued_frame = 0;
    bool fresh = queued_frame != g_frame;
    queued_frame = g_frame;
    bool presented = true;
    if (g_interpolated_ready == 0 && g_show_queue.empty()) {
        if (fresh || !g_recording) {
            presented = present_view(window, aspect, g_device.frame_view);
            if (presented) {
                record_shown(g_device.frame, false);
            }
            g_last_shown = std::chrono::steady_clock::now();
        }
    } else if (!fresh) {
        service_deferred();
    } else {
        auto now = std::chrono::steady_clock::now();
        auto last = g_show_queue.empty() ? g_last_shown : g_shown[g_show_queue.back()].due;
        if (last - now > std::chrono::duration<double>(g_period / 2)) {
            g_show_queue.clear();
            last = g_last_shown;
        }
        g_slot = std::clamp(g_planned_slot, 0.002, 0.025);
        auto step = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(g_slot));
        auto slot = std::max(now, last + step);
        for (uint32_t i = 0; i < g_interpolated_ready; i++) {
            if (queue_image(g_device.interpolated[i], slot, true)) {
                slot += step;
            }
        }
        g_interpolated_ready = 0;
        if (g_show_real) {
            queue_image(g_device.frame, slot, false);
        } else {
            g_hidden_frames++;
        }
        service_deferred();
    }
    apply_scale();
    apply_anisotropy();
    return presented;
}

bool service_presents() {
    service_deferred();
    return !g_show_queue.empty();
}

bool read_recent(std::vector<RecentImage>& images) {
    images.clear();
    for (size_t i = 1; i <= g_pair_notes.size(); i++) {
        const std::string& note = g_pair_notes[(g_frame + i) % g_pair_notes.size()];
        for (size_t at = 0; at < note.size(); at += 400) {
            log::write("runs", "%s", note.substr(at, 400).c_str());
        }
    }
    size_t size = g_recent.images.size();
    for (size_t i = 0; i < g_recent.count; i++) {
        size_t slot = (g_recent.next + size - g_recent.count + i) % size;
        RecentImage image;
        image.between = g_recent.between[slot];
        if (read_texture(g_recent.images[slot], image.pixels, image.width, image.height)) {
            images.push_back(std::move(image));
        }
    }
    return !images.empty();
}

uint32_t take_interpolated_frames() {
    uint32_t shown = g_interpolated_frames.exchange(0);
    uint32_t hidden = g_hidden_frames.exchange(0);
    return shown > hidden ? shown - hidden : 0;
}

uint32_t take_late_frames() {
    return g_late_frames.exchange(0);
}

GpuFigures gpu_figures() {
    constexpr double kGigabyte = 1024.0 * 1024.0 * 1024.0;
    static bool looked = false;
    static IDXGIAdapter3* adapter3 = nullptr;
    static std::string name;
    static double total = -1.0;
    GpuFigures figures;
    if (!g_device.ready) {
        return figures;
    }
    if (!looked) {
        looked = true;
        IDXGIDevice* dxgi_device = nullptr;
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(g_device.device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi_device))) && SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
            DXGI_ADAPTER_DESC description{};
            if (SUCCEEDED(adapter->GetDesc(&description))) {
                char text[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, text, sizeof text, nullptr, nullptr);
                name = text;
                total = description.DedicatedVideoMemory > 0 ? static_cast<double>(description.DedicatedVideoMemory) / kGigabyte : -1.0;
            }
            if (FAILED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3)))) {
                adapter3 = nullptr;
            }
            adapter->Release();
        }
        if (dxgi_device) {
            dxgi_device->Release();
        }
    }
    figures.adapter = name;
    figures.vram_total_gb = total;
    DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
    if (adapter3 && SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory))) {
        figures.vram_used_gb = static_cast<double>(memory.CurrentUsage) / kGigabyte;
    }
    figures.width = scaled(kEfbWidth);
    figures.height = scaled(kEfbHeight);
    figures.refresh_hz = g_display_hz;
    return figures;
}

bool read_texture(ID3D11Texture2D* texture, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    D3D11_TEXTURE2D_DESC description{};
    texture->GetDesc(&description);
    description.BindFlags = 0;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_device.device->CreateTexture2D(&description, nullptr, &staging))) {
        return false;
    }
    g_device.context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped;
    bool ok = SUCCEEDED(g_device.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
    if (ok) {
        width = description.Width;
        height = description.Height;
        pixels.resize(static_cast<size_t>(width) * height);
        for (uint32_t row = 0; row < height; row++) {
            const uint8_t* source = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch;
            for (uint32_t column = 0; column < width; column++) {
                pixels[static_cast<size_t>(row) * width + column] = (static_cast<uint32_t>(source[column * 4]) << 16) |
                                                                    (static_cast<uint32_t>(source[column * 4 + 1]) << 8) | source[column * 4 + 2];
            }
        }
        g_device.context->Unmap(staging, 0);
    }
    release(staging);
    return ok;
}

void dump_texture(ID3D11Texture2D* texture, const char* suffix) {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    if (texture && read_texture(texture, pixels, width, height)) {
        std::string path = g_copy_dump_prefix + "_copy" + std::to_string(g_copy_dump_index) + suffix + ".png";
        video::save_png(path.c_str(), pixels, width, height);
    }
}

void dump_copy(const CopiedTexture& entry) {
    if (g_copy_dump_prefix.empty()) {
        return;
    }
    g_copy_dump_index++;
    dump_texture(g_device.target, "_efb");
    dump_texture(entry.converted, "");
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

bool read_frame(std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height) {
    if (!g_device.ready || !g_device.frame) {
        return false;
    }
    flush_pending();
    return read_texture(g_device.frame, pixels, width, height);
}

void run_clear(const ClearCommand& command);

void clear(int x, int y, int width, int height) {
    if (!initialize() || !create_present_pipeline()) {
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
    UINT mask = (color_enable ? (D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE) : 0) |
                (alpha_enable ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
    if (!g_device.clear_blend[mask]) {
        D3D11_BLEND_DESC description{};
        description.RenderTarget[0].BlendEnable = FALSE;
        description.RenderTarget[0].RenderTargetWriteMask = static_cast<UINT8>(mask);
        if (FAILED(g_device.device->CreateBlendState(&description, &g_device.clear_blend[mask]))) {
            return;
        }
    }
    if (!g_device.clear_depth[depth_enable]) {
        D3D11_DEPTH_STENCIL_DESC description{};
        description.DepthEnable = depth_enable;
        description.DepthWriteMask = depth_enable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
        description.DepthFunc = D3D11_COMPARISON_ALWAYS;
        if (FAILED(g_device.device->CreateDepthStencilState(&description, &g_device.clear_depth[depth_enable]))) {
            return;
        }
    }
    x = std::max(0, x);
    y = std::max(0, y);
    width = std::min(width, kEfbWidth - x);
    height = std::min(height, kEfbHeight - y);
    if (width <= 0 || height <= 0) {
        return;
    }
    ClearCommand command{mask, depth_enable, {red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f, static_cast<float>(16777215u - depth) / 16777215.0f, 0, 0, 0}, x, y, width, height};
    if (g_frame_end.due && !g_frame_end.has_clear) {
        g_frame_end.has_clear = true;
        g_frame_end.clear = command;
        return;
    }
    finish_frame();
    apply_clear(command);
}

void run_clear(const ClearCommand& command) {
    UINT mask = command.mask;
    bool depth_enable = command.depth_enable;
    int x = command.x;
    int y = command.y;
    int width = command.width;
    int height = command.height;
    ID3D11DeviceContext* context = g_device.context;
    context->UpdateSubresource(g_device.clear_constants, 0, nullptr, command.constants, 0, 0);
    context->OMSetRenderTargets(1, &g_device.target_view, g_device.depth_view);
    D3D11_VIEWPORT viewport{0, 0, static_cast<float>(scaled(kEfbWidth)), static_cast<float>(scaled(kEfbHeight)), 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);
    D3D11_RECT rectangle{scaled(x), scaled(y), scaled(x + width), scaled(y + height)};
    context->RSSetScissorRects(1, &rectangle);
    context->RSSetState(g_device.rasterizer);
    context->OMSetBlendState(g_device.clear_blend[mask], nullptr, 0xFFFFFFFF);
    context->OMSetDepthStencilState(g_device.clear_depth[depth_enable], 0);
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(g_device.present_vertex, nullptr, 0);
    context->PSSetShader(g_device.clear_pixel, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &g_device.clear_constants);
    context->Draw(3, 0);
}

}
