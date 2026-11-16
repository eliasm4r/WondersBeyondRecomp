#version 450

layout(push_constant) uniform Values {
    vec4 value;
    vec4 depth;
};

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    color = value;
    gl_FragDepth = depth.x;
}
