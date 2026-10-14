#version 460

layout(set = 0, binding = 0) uniform sampler2D u_depth;
layout(push_constant) uniform CameraMotion {
    mat4 clip_to_previous;
    uint reset;
    uint pad;
    vec2 jitter_ndc;
} pc;

layout(location = 0) out vec2 o_motion;

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(u_depth, pixel, 0).r;
    if (pc.reset != 0u || depth <= 0.000001) {
        o_motion = vec2(0.0);
        return;
    }
    vec2 uv = (vec2(pixel) + 0.5) / vec2(textureSize(u_depth, 0));
    vec4 current = vec4(uv * 2.0 - 1.0, depth, 1.0);
    current.xy -= pc.jitter_ndc;
    vec4 previous = pc.clip_to_previous * current;
    o_motion = previous.w > 0.000001 ? previous.xy / previous.w - current.xy : vec2(0.0);
}
