#version 450

layout(set = 0, binding = 1, std430) readonly buffer Words {
    float words[];
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
    gl_Position = vec4(position.x, -position.y, position.z, position.w);
    v_c0 = vec4(words[at + 4u], words[at + 5u], words[at + 6u], words[at + 7u]);
    v_c1 = vec4(words[at + 8u], words[at + 9u], words[at + 10u], words[at + 11u]);
    for (uint i = 0u; i < 8u; i++) {
        uint uv = at + 12u + 3u * i;
        v_uv[i] = vec3(words[uv], words[uv + 1u], words[uv + 2u]);
    }
}
