#version 460

layout(location = 0) in vec4 v_current_clip;
layout(location = 1) in vec4 v_previous_clip;
layout(location = 2) flat in uint v_motion_valid;

// Current pixel to its previous displayed location, in signed NDC units. Streamline should
// receive mvecScale = {1, 1} and motionVectorsJittered = false for this unjittered field.
layout(location = 0) out vec2 o_motion;

void main() {
    if (v_motion_valid == 0u || v_previous_clip.w <= 0.0) {
        o_motion = vec2(0.0);
        return;
    }
    vec2 current_ndc = v_current_clip.xy / v_current_clip.w;
    vec2 previous_ndc = v_previous_clip.xy / v_previous_clip.w;
    o_motion = previous_ndc - current_ndc;
}
