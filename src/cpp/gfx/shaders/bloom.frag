#version 460

layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(set = 0, binding = 1) uniform sampler2D u_glow;
layout(push_constant) uniform Settings {
    vec2 texel;
    int pass;
    float strength;
    int linear_input;
} settings;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

vec3 Bright(vec3 color) {
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    // Smooth threshold keeps isolated highlights, such as lanterns, without lifting dark fog.
    return color * smoothstep(0.35, 0.8, luma) * 1.8;
}

vec3 ToLinear(vec3 color) {
    return mix(color / 12.92, pow((color + 0.055) / 1.055, vec3(2.4)),
               vec3(greaterThan(color, vec3(0.04045))));
}

vec3 ToSrgb(vec3 color) {
    return mix(color * 12.92, 1.055 * pow(max(color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
               vec3(greaterThan(color, vec3(0.0031308))));
}

void main() {
    if (settings.pass == 0) {
        vec3 color = vec3(0.0);
        for (int y = -1; y <= 1; ++y) {
            for (int x = -1; x <= 1; ++x) {
                vec3 sample_color = texture(u_scene, v_uv + vec2(x, y) * settings.texel).rgb;
                color += Bright(settings.linear_input != 0 ? sample_color : ToLinear(sample_color));
            }
        }
        o_color = vec4(color / 9.0, 1.0);
    } else if (settings.pass == 1 || settings.pass == 2) {
        vec2 direction = settings.pass == 1 ? vec2(settings.texel.x, 0.0) : vec2(0.0, settings.texel.y);
        vec3 color = texture(u_scene, v_uv).rgb * 0.227027;
        color += texture(u_scene, v_uv + direction * 1.384615).rgb * 0.316216;
        color += texture(u_scene, v_uv - direction * 1.384615).rgb * 0.316216;
        color += texture(u_scene, v_uv + direction * 3.230769).rgb * 0.070270;
        color += texture(u_scene, v_uv - direction * 3.230769).rgb * 0.070270;
        o_color = vec4(color, 1.0);
    } else {
        vec3 scene_sample = texture(u_scene, v_uv).rgb;
        vec3 scene = settings.linear_input != 0 ? scene_sample : ToLinear(scene_sample);
        vec3 glow = texture(u_glow, v_uv).rgb;
        vec3 radiance = scene + glow * settings.strength;
        // Compress the generated highlight energy in linear light before SDR output.
        vec3 mapped = radiance / (1.0 + 0.15 * radiance) * 1.15;
        o_color = vec4(ToSrgb(clamp(mapped, 0.0, 1.0)), 1.0);
    }
}
