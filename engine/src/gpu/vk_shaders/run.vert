#version 450

layout(set = 0, binding = 1, std430) readonly buffer Words {
    uint words[];
};

layout(set = 0, binding = 2, std430) readonly buffer Rows {
    vec4 rows[];
};

layout(push_constant) uniform Draw {
    uint base;
    uint stride;
    uint layout_bits;
    uint run_row;
};

layout(location = 0) out vec4 v_c0;
layout(location = 1) out vec4 v_c1;
layout(location = 2) out vec3 v_uv[8];

invariant gl_Position;

vec4 row(int at) {
    return rows[at];
}

float word_float(uint at) {
    return uintBitsToFloat(words[at]);
}

uvec4 word_bytes(uint word) {
    return uvec4(word & 255u, (word >> 8) & 255u, (word >> 16) & 255u, word >> 24);
}

vec3 transformed(int at, vec3 v) {
    vec4 r0 = row(at);
    vec4 r1 = row(at + 1);
    vec4 r2 = row(at + 2);
    precise vec3 result;
    result.x = r0.x * v.x + r0.y * v.y + r0.z * v.z + r0.w;
    result.y = r1.x * v.x + r1.y * v.y + r1.z * v.z + r1.w;
    result.z = r2.x * v.x + r2.y * v.y + r2.z * v.z + r2.w;
    return result;
}

vec3 unit(vec3 v) {
    float extent = sqrt(dot(v, v));
    return extent == 0.0 ? v : v / extent;
}

float ratio(float n, float d) {
    return d == 0.0 ? (n > 0.0 ? 1.0 : 0.0) : n / d;
}

float light_factor(int at, uint control, vec3 eye, vec3 normal) {
    vec4 a = row(at + 1);
    vec4 b = row(at + 2);
    vec4 c = row(at + 3);
    vec3 angle = a.xyz;
    vec3 range = vec3(a.w, b.x, b.y);
    vec3 direction = c.yzw;
    uint diffuse = (control >> 7) & 3u;
    uint attenuation = (control >> 9) & 3u;
    vec3 toward = vec3(b.z, b.w, c.x) - eye;
    float amount = 1.0;
    if (attenuation == 1u) {
        toward = unit(toward);
        amount = dot(toward, normal) >= 0.0 ? max(0.0, dot(direction, normal)) : 0.0;
        vec3 powers = vec3(1.0, amount, amount * amount);
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

void light_colors(int light, int matrix_row, uint word, vec3 eye, vec3 raw, inout vec4 c0, inout vec4 c1) {
    vec4 controls = row(light);
    int first = matrix_row + int(word >> 2);
    vec4 n0 = row(first);
    vec4 n1 = row(first + 1);
    vec4 n2 = row(first + 2);
    float m[12] = float[12](n0.x, n0.y, n0.z, n0.w, n1.x, n1.y, n1.z, n1.w, n2.x, n2.y, n2.z, n2.w);
    uint k = word & 3u;
    vec3 normal = unit(vec3(m[k] * raw.x + m[k + 1u] * raw.y + m[k + 2u] * raw.z,
                            m[k + 3u] * raw.x + m[k + 4u] * raw.y + m[k + 5u] * raw.z,
                            m[k + 6u] * raw.x + m[k + 7u] * raw.y + m[k + 8u] * raw.z));
    vec4 given[2] = vec4[2](c0, c1);
    vec4 result[2];
    for (uint channel = 0u; channel < 2u; channel++) {
        vec4 vertex = floor(given[channel] * 255.0 + 0.5);
        vec4 material = row(light + 1 + int(channel));
        vec4 ambient = row(light + 3 + int(channel));
        uint color = uint(channel == 0u ? controls.x : controls.y);
        uint alpha = uint(channel == 0u ? controls.z : controls.w);
        vec3 color_material = (color & 1u) != 0u ? vertex.rgb : material.rgb;
        float alpha_material = (alpha & 1u) != 0u ? vertex.a : material.a;
        uint color_mask = light_mask(color);
        uint alpha_mask = light_mask(alpha);
        vec3 sum = (color & 64u) != 0u ? vertex.rgb : ambient.rgb;
        float alpha_sum = (alpha & 64u) != 0u ? vertex.a : ambient.a;
        for (uint i = 0u; i < 8u; i++) {
            uint bit = 1u << i;
            int at = light + 8 + 4 * int(i);
            if ((color_mask & bit) != 0u) {
                sum += row(at).rgb * light_factor(at, color, eye, normal);
            }
            if ((alpha_mask & bit) != 0u) {
                alpha_sum += row(at).a * light_factor(at, alpha, eye, normal);
            }
        }
        if ((color & 2u) != 0u) {
            result[channel].rgb = vec3(modulated(color_material.r, sum.r), modulated(color_material.g, sum.g), modulated(color_material.b, sum.b));
        } else {
            result[channel].rgb = color_material / 255.0;
        }
        result[channel].a = (alpha & 2u) != 0u ? modulated(alpha_material, alpha_sum) : alpha_material / 255.0;
    }
    c0 = result[0];
    c1 = result[1];
}

void main() {
    uint at = base + uint(gl_VertexIndex) * stride;
    vec3 a_position = vec3(word_float(at), word_float(at + 1u), word_float(at + 2u));
    vec4 a_c0 = unpackUnorm4x8(words[at + 3u]);
    vec4 a_c1 = unpackUnorm4x8(words[at + 4u]);
    uint offset = at + 5u;
    vec3 a_normal = vec3(0.0);
    if ((layout_bits & 1u) != 0u) {
        a_normal = vec3(word_float(offset), word_float(offset + 1u), word_float(offset + 2u));
        offset += 3u;
    }
    uvec4 a_m0 = uvec4(0u);
    uvec4 a_m1 = uvec4(0u);
    uvec4 a_m2 = uvec4(0u);
    if ((layout_bits & 2u) != 0u) {
        a_m0 = word_bytes(words[offset]);
        a_m1 = word_bytes(words[offset + 1u]);
        a_m2 = word_bytes(words[offset + 2u]);
        offset += 3u;
    }
    uint input_count = layout_bits >> 2;
    vec2 inputs[8];
    for (uint k = 0u; k < 8u; k++) {
        inputs[k] = vec2(0.0);
        if (k < input_count) {
            inputs[k] = vec2(word_float(offset + 2u * k), word_float(offset + 2u * k + 1u));
        }
    }
    int run = int(run_row);
    vec4 p0 = row(run);
    vec4 p1 = row(run + 1);
    vec4 p2 = row(run + 2);
    vec4 p3 = row(run + 3);
    int origin = run + int(p3.z);
    precise vec3 eye = transformed(origin + int(p1.w) + int(a_m0.x), a_position);
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
    precise vec4 position;
    position.x = x * p2.x + w * p2.y;
    position.y = y * p2.z + w * p2.w;
    position.z = (z * p3.x + w * (16777215.0 - p3.y)) / 16777215.0;
    position.w = w;
    uint setup = uint(p3.w);
    uint count = min(setup & 15u, 8u);
    bool dual = (setup & 16u) != 0u;
    uint indices[8] = uint[8](a_m0.y, a_m0.z, a_m0.w, a_m1.x, a_m1.y, a_m1.z, a_m1.w, a_m2.x);
    vec3 uv[8];
    for (uint k = 0u; k < 8u; k++) {
        uv[k] = vec3(0.0);
    }
    for (uint i = 0u; i < count; i++) {
        int place = run + 7 + 7 * int(i);
        vec4 info = row(place);
        uint source = uint(info.x);
        uint flags = uint(info.y);
        vec3 given = vec3(0.0, 0.0, 1.0);
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
        vec3 coord = transformed(origin + int(info.z) + int(indices[i]), given);
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
    vec4 c0 = a_c0;
    vec4 c1 = a_c1;
    if ((setup & 32u) != 0u) {
        int light = run + 7 + 7 * int(count);
        if (p1.w == 0.0) {
            light_colors(light, origin + 66, (a_m0.x & 31u) * 3u, eye, a_normal, c0, c1);
        } else {
            light_colors(light, light + 5, 0u, eye, a_normal, c0, c1);
        }
    }
    v_c0 = c0;
    v_c1 = c1;
    for (int k = 0; k < 8; k++) {
        v_uv[k] = uv[k];
    }
    gl_Position = vec4(position.x, -position.y, position.z, position.w);
}
