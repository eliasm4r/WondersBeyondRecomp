#include "wp/gx_render.h"
#include "wp/gx.h"
#include "wp/log.h"

#include "gx_render_common.h"

#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>

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

#include "wp/custom_textures.h"
#include "wp/game.h"
#include "wp/gx_state.h"
#include "wp/memory.h"
#include "wp/frame_pacing.h"
#include "wp/options.h"
#include "wp/perf_overlay.h"
#include "wp/settings.h"
#include "wp/video.h"

namespace wp::gx::render {

void finish_write_backs();
bool write_back_overlaps(uint32_t address, size_t size);

namespace {

constexpr uint32_t kVertexCapacity = 1 << 18;

const char* kPresentShaderSource = R"HLSL(
Texture2D frame : register(t0);
SamplerState frame_sampler : register(s0);

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
    return float4(frame.Sample(frame_sampler, input.uv).rgb, 1);
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
    return float4(sum / 16, 1);
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
        uint depth = min(uint(saturate(depth_source.Load(int3(int(position.x), int(y), 0))) * 16777216.0), 0xFFFFFFu);
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
        int z = clamp(int(p.position.z * 16777216.0), 0, 0xFFFFFF);
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
    ID3D11Buffer* vertex_buffer = nullptr;
    ID3D11VertexShader* moved_shader = nullptr;
    ID3D11InputLayout* moved_layout = nullptr;
    ID3D11Buffer* moved_buffer = nullptr;
    ID3D11Buffer* motion = nullptr;
    ID3D11Buffer* constants = nullptr;
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
    ID3D11Buffer* copy_constants = nullptr;
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

struct RecordedCommand {
    enum Kind { kBatch, kCopy, kClear } kind = kBatch;
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
};

[[gnu::used]] const char kBuildSalt[] = "SALT3333";

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
uint32_t g_batches = 0;
uint32_t g_batch_vertices = 0;
double g_batch_seconds = 0.0;
uint32_t g_write_backs = 0;
double g_write_back_seconds = 0.0;

int scaled(int value) {
    return value * g_scale;
}

bool compile(const char* source, const char* entry, const char* profile, ID3DBlob** blob) {
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
    buffer.ByteWidth = sizeof(Constants);
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_device.device->CreateBuffer(&buffer, nullptr, &g_device.constants))) {
        return false;
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
    HRESULT result = D3D11CreateDevice(nullptr, driver, nullptr, device_flags, nullptr, 0, D3D11_SDK_VERSION, &g_device.device, &level,
                                       &g_device.context);
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
    if (FAILED(result) || !create_target() || !create_pipeline()) {
        std::fputs("cannot initialise the D3D11 renderer\n", stderr);
        g_device.failed = true;
        return false;
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
    std::vector<std::vector<uint32_t>> pixels = decode_levels(request);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(levels);
    for (uint32_t level = 0; level < levels; level++) {
        initial[level] = {pixels[level].data(), std::max(width >> level, 1u) * 4, 0};
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
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    description.DepthFunc = static_cast<D3D11_COMPARISON_FUNC>(((word >> 1) & 7) + 1);
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
        if (compile(variant_source(job.first).c_str(), "pixel_main", "ps_5_0", &code)) {
            g_device.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
            release(code);
        }
        job.second->shader = shader;
    }
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
        [[maybe_unused]] static const bool started = [] {
            unsigned workers = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 6u);
            for (unsigned i = 0; i < workers; i++) {
                std::thread(variant_worker).detach();
            }
            return true;
        }();
        found = g_variants.try_emplace(key).first;
        {
            std::lock_guard<std::mutex> lock(g_variant_mutex);
            g_variant_jobs.emplace_back(key, &found->second);
        }
        g_variant_wake.notify_one();
    }
    return found->second.shader.load();
}

void set_batch_state(const BatchState& pending, bool moved) {
    ID3D11DeviceContext* context = g_device.context;
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(g_device.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &pending.constants, sizeof pending.constants);
        context->Unmap(g_device.constants, 0);
    }
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
    context->PSSetConstantBuffers(0, 1, &g_device.constants);
}

uint32_t draw_batch(const BatchState& pending, const ScreenVertex* vertices, size_t total) {
    set_batch_state(pending, false);
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
        if (FAILED(context->Map(g_device.vertex_buffer, 0, mode, 0, &mapped))) {
            break;
        }
        std::memcpy(static_cast<ScreenVertex*>(mapped.pData) + g_vertex_cursor, vertices + offset, batch * sizeof(ScreenVertex));
        context->Unmap(g_device.vertex_buffer, 0);
        context->Draw(batch, g_vertex_cursor);
        g_vertex_cursor += batch;
        offset += batch;
    }
    return start;
}

void flush_pending() {
    PendingBatch& pending = g_pending;
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
    // Scenes such as the open zones send hundreds of thousands of tiny draws a second, mostly with only a new matrix in
    // between: when no register the batch depends on changed, this draw joins the pending batch without recomputing it.
    static uint64_t batched_serial = ~0ull;
    static uint64_t batched_epoch = 0;
    static int batched_scale = 0;
    PendingBatch& pending = g_pending;
    if (pending.begin != g_triangles.size() && batched_serial == register_serial() && batched_epoch == g_texture_epoch && batched_scale == g_scale) {
        append(vertices, count, source);
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
    bool same = pending.begin != g_triangles.size() && pending.blend == blend && pending.depth == depth && pending.early_depth == early_depth && pending.top_left == bp[0x20] &&
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
    append(vertices, count, source);
    batched_serial = register_serial();
    batched_epoch = g_texture_epoch;
    batched_scale = g_scale;
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
    context->UpdateSubresource(g_device.copy_constants, 0, nullptr, &constants, 0, 0);
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
    context->PSSetConstantBuffers(0, 1, &g_device.copy_constants);
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
    // A one-off copy, like a snapshot, is still written to RAM.
    constexpr uint32_t kRecurringFrames = 2;
    bool recurring = entry.converted && g_frame - entry.copied_frame <= kRecurringFrames;
    entry.copied_frame = g_frame;
    uint32_t w = static_cast<uint32_t>(width);
    uint32_t h = static_cast<uint32_t>(height);
    entry.logical_width = half ? std::max(1u, w / 2) : w;
    entry.logical_height = half ? std::max(1u, h / 2) : h;
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

bool moves_in_world(const RecordedDraw& draw, const RecordedDraw& before, const CameraMotion& camera) {
    constexpr double kScreen = kStillScreen;
    constexpr double kDepth = 2e-5;
    uint32_t step = std::max(1u, draw.positions / 8);
    for (uint32_t i = 0; i < draw.positions; i += step) {
        const std::array<float, 4>& a = g_previous.positions[before.first_position + i];
        const std::array<float, 4>& b = g_record.positions[draw.first_position + i];
        if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f || std::fabs(b[0]) > 1.5f * b[3] || std::fabs(b[1]) > 1.5f * b[3]) {
            continue;
        }
        double p[4];
        for (int r = 0; r < 4; r++) {
            p[r] = static_cast<double>(camera.back[r][0]) * b[0] + static_cast<double>(camera.back[r][1]) * b[1] + static_cast<double>(camera.back[r][2]) * b[2] +
                   static_cast<double>(camera.back[r][3]) * b[3];
        }
        if (p[3] <= 0.0 || std::fabs(p[0] / p[3] - a[0] / a[3]) > kScreen || std::fabs(p[1] / p[3] - a[1] / a[3]) > kScreen ||
            std::fabs(p[2] / p[3] - a[2] / a[3]) > kDepth) {
            return true;
        }
    }
    return false;
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
            for (uint32_t v = 0; v < draw.vertices; v++) {
                size_t index = draw.first_vertex + v;
                uint16_t source = g_record.source_of[index];
                const std::array<float, 4>* a = source == kNoSource ? nullptr : &g_previous.positions[before.first_position + source];
                const std::array<float, 4>* b = source == kNoSource ? nullptr : &g_record.positions[draw.first_position + source];
                if (!a || std::isnan((*a)[0]) || std::isnan((*b)[0])) {
                    moved[index] = kKeep;
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
            moved[index] = {(*a)[0] + ((*b)[0] - (*a)[0]) * t, (*a)[1] + ((*b)[1] - (*a)[1]) * t, (*a)[2] + ((*b)[2] - (*a)[2]) * t,
                            (*a)[3] + ((*b)[3] - (*a)[3]) * t};
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
        case RecordedCommand::kBatch: {
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

std::vector<std::pair<uint32_t, uint32_t>> g_matches;
bool g_camera_cut = false;

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
            bool moving = moves_in_world(draw, g_previous.draws[partner[d]], g_cameras[camera]);
            size_t second = g_cameras[camera].second;
            if (moving && second < kMaxCameras && forward[second] && !moves_in_world(draw, g_previous.draws[partner[d]], g_cameras[second])) {
                camera = second;
                moving = false;
            }
            g_world_camera[d] = static_cast<uint8_t>(camera);
            g_world_moving[d] = moving;
        }
        if (camera < kMaxCameras && !(pair && (kind[d] == kOnly || (kind[d] == kSameCount && near_camera_path(draw, g_previous.draws[partner[d]], g_cameras[camera]))))) {
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
    return g_recording && !g_record_broken && !g_previous.draws.empty() && !g_record.draws.empty();
}

uint32_t run_interpolated(int x, int y, int width, int height, bool depth, const CopyFilter& filter, const pacing::FramePlan& plan) {
    if (!can_pair() || !g_efb_before_valid || !g_device.moved_shader || g_frame_vertices > kVertexCapacity || !interpolated_targets()) {
        return 0;
    }
    if (!g_paired) {
        pair_frame();
        g_paired = true;
    }
    if (g_camera_cut) {
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
    } else {
        g_previous.positions.clear();
        g_previous.draws.clear();
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
    }
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
            std::fill(std::begin(g_match), std::end(g_match), 0u);
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
    ID3DBlob* smooth_code = nullptr;
    if (ok && compile(kPresentShaderSource, "pixel_smooth", "ps_5_0", &smooth_code)) {
        g_device.device->CreatePixelShader(smooth_code->GetBufferPointer(), smooth_code->GetBufferSize(), nullptr, &g_device.present_smooth);
        release(smooth_code);
    }
    ID3DBlob* copy_code = nullptr;
    if (ok && compile(kCopyShaderSource, "pixel_main", "ps_5_0", &copy_code)) {
        ok = SUCCEEDED(g_device.device->CreatePixelShader(copy_code->GetBufferPointer(), copy_code->GetBufferSize(), nullptr, &g_device.copy_pixel));
        release(copy_code);
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 80;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        ok = ok && SUCCEEDED(g_device.device->CreateBuffer(&constants, nullptr, &g_device.copy_constants));
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
    context->ClearDepthStencilView(g_device.depth_view, D3D11_CLEAR_DEPTH, 1.0f, 0);
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
    if (present_view(g_present_window, g_present_aspect, image.view) && image.interpolated) {
        g_interpolated_frames++;
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
        }
    }
    static uint64_t queued_frame = 0;
    bool fresh = queued_frame != g_frame;
    queued_frame = g_frame;
    bool presented = true;
    if (g_interpolated_ready == 0 && g_show_queue.empty()) {
        if (fresh || !g_recording) {
            presented = present_view(window, aspect, g_device.frame_view);
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
    ClearCommand command{mask, depth_enable, {red / 255.0f, green / 255.0f, blue / 255.0f, alpha / 255.0f, static_cast<float>(depth) / 16777215.0f, 0, 0, 0}, x, y, width, height};
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
