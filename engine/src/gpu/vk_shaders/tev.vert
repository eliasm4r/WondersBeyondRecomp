#version 450

layout(set = 0, binding = 1, std430) readonly buffer Words {
    float words[];
};

layout(set = 0, binding = 2, std430) readonly buffer Moved {
    vec4 moved[];
};

layout(push_constant) uniform Draw {
    uint base;
    uint stride;
    uint layout_bits;
    uint row;
};

layout(location = 0) out vec4 v_c0;
layout(location = 1) out vec4 v_c1;
layout(location = 2) out vec3 v_uv[8];

invariant gl_Position;

void main() {
    uint at = base + uint(gl_VertexIndex) * 36u;
    vec4 position = vec4(words[at], words[at + 1u], words[at + 2u], words[at + 3u]);
    vec4 c0 = vec4(words[at + 4u], words[at + 5u], words[at + 6u], words[at + 7u]);
    vec4 c1 = vec4(words[at + 8u], words[at + 9u], words[at + 10u], words[at + 11u]);
    if ((layout_bits & 1u) != 0u) {
        uint slot = row + 32u + (stride + uint(gl_VertexIndex)) * 3u;
        vec4 shift = moved[slot];
        vec4 moved_c0 = moved[slot + 1u];
        if (moved_c0.w > -0.5) {
            c0 = moved_c0;
            c1 = moved[slot + 2u];
        }
        if (shift.w < -1.5e30) {
            uint m = row + uint(shift.x) * 4u;
            position = vec4(dot(moved[m], position), dot(moved[m + 1u], position), dot(moved[m + 2u], position), dot(moved[m + 3u], position));
        } else if (shift.w > -1e29) {
            position = shift;
        }
    }
    gl_Position = vec4(position.x, -position.y, position.z, position.w);
    v_c0 = c0;
    v_c1 = c1;
    for (uint i = 0u; i < 8u; i++) {
        uint uv = at + 12u + 3u * i;
        v_uv[i] = vec3(words[uv], words[uv + 1u], words[uv + 2u]);
    }
}
