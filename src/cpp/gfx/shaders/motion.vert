#version 460

layout(location = 0) in vec3 a_position;

layout(push_constant, std430) uniform Push {
    vec4 xform;
} pc;

// gfx::MeshConstants; only transforms, clip planes and motion validity are read here.
layout(set = 0, binding = 0, std140) uniform MeshConstants {
    mat4 mvp;
    mat3 normal_matrix;
    vec4 light_direction[4];
    vec4 light_color[4];
    vec4 ambient;
    vec4 diffuse;
    vec4 ambient_material;
    vec4 specular;
    vec4 fog;
    vec4 clip_plane[2];
    vec4 world_uv[2];
    uint flags;
    uint light_count;
    float material_roughness;
    float normal_detail_strength;
    mat4 model_to_world;
    vec4 camera_position;
    vec4 camera_forward;
    mat4 previous_mvp;
    uint motion_valid;
    vec2 jitter_ndc;
    uint motion_pad;
    uvec4 material_maps;
} mc;

layout(location = 0) out vec4 v_current_clip;
layout(location = 1) out vec4 v_previous_clip;
layout(location = 2) flat out uint v_motion_valid;

out float gl_ClipDistance[2];

void main() {
    vec4 position = vec4(a_position, 1.0);
    vec4 current_clip = mc.mvp * position;
    vec4 previous_clip = mc.previous_mvp * position;
    vec2 pixel_ndc = pc.xform.xy / 16.0;
    current_clip.xy = round((current_clip.xy / current_clip.w) / pixel_ndc) * pixel_ndc * current_clip.w;
    previous_clip.xy = round((previous_clip.xy / previous_clip.w) / pixel_ndc) * pixel_ndc * previous_clip.w;
    gl_Position = current_clip;
    gl_Position.xy += mc.jitter_ndc * gl_Position.w;
    gl_ClipDistance[0] = (mc.flags & 16u) != 0u ? dot(mc.clip_plane[0], position) : 1.0;
    gl_ClipDistance[1] = (mc.flags & 32u) != 0u ? dot(mc.clip_plane[1], position) : 1.0;
    v_current_clip = current_clip;
    v_previous_clip = previous_clip;
    v_motion_valid = mc.motion_valid;
}
