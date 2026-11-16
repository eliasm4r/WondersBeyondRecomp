#version 450

layout(set = 0, binding = 0) uniform sampler2D frame;

layout(push_constant) uniform Adjust {
    vec4 adjust;
};

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 adjusted(vec3 c) {
    c = ((c - 0.5) * adjust.y + 0.5) * adjust.x;
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = clamp(luma + (c - luma) * adjust.z, 0.0, 1.0);
    return pow(c, vec3(adjust.w));
}

void main() {
#ifdef WP_SMOOTH
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    vec3 sum = vec3(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            sum += textureLod(frame, uv + dx * ((float(x) + 0.5) / 4.0 - 0.5) + dy * ((float(y) + 0.5) / 4.0 - 0.5), 0.0).rgb;
        }
    }
    color = vec4(adjusted(sum / 16.0), 1.0);
#else
    color = vec4(adjusted(texture(frame, uv).rgb), 1.0);
#endif
}
