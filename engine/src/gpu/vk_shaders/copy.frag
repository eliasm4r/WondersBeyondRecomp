#version 450

layout(set = 0, binding = 0) uniform sampler2D color_source;
layout(set = 0, binding = 1) uniform sampler2D depth_source;

layout(push_constant) uniform CopyConstants {
    uvec4 params;
    vec4 region;
    uvec4 filter_taps;
    vec4 rows;
    vec4 extent;
};

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

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
