#version 450

layout(constant_id = 0) const int TURNS = 0;

layout(location = 0) out vec2 uv;

void main() {
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = corner;
    vec2 place = corner * 2.0 - 1.0;
    if (TURNS == 1) {
        place = vec2(-place.y, place.x);
    } else if (TURNS == 2) {
        place = -place;
    } else if (TURNS == 3) {
        place = vec2(place.y, -place.x);
    }
    gl_Position = vec4(place, 0.0, 1.0);
}
