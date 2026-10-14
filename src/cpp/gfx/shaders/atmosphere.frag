#version 460

layout(set = 0, binding = 0) uniform sampler2D u_scene;
layout(set = 0, binding = 1) uniform sampler2D u_depth;
layout(set = 0, binding = 2, std140) uniform LocalLights {
    uvec4 count;
    vec4 position_radius[8];
    vec4 color_intensity[8];
} lamps;
layout(set = 0, binding = 3, std140) uniform DeckElevations {
    vec4 heights[100];
    vec4 background_color;
    vec4 lower_color;
    vec4 clear_color;
    vec4 density; // background density, clear distance, lower density, lower falloff
    vec4 distance; // far density, far start, far end, uncovered distance
    vec4 effects; // deck cell size, local scattering strength, reserved
    vec4 local_limit;
} deck;

layout(push_constant) uniform FogSettings {
    mat4 inverse_view_projection;
    vec4 camera_position;
    vec4 light_direction;
    vec4 light_color;
    vec4 parameters; // background density, clear distance, stable fallback Y, directional strength
} fog;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float DeckCell(int x, int z) {
    int cell_index = clamp(x, 0, 19) + 20 * clamp(z, 0, 19);
    return deck.heights[cell_index / 4][cell_index % 4];
}

float DeckHeight(vec2 world_xz) {
    vec2 cell = clamp(world_xz / max(deck.effects.x, 0.001), vec2(0.0), vec2(19.0));
    ivec2 low = ivec2(floor(cell));
    // Keep each room's floor level through its cell interior; blend only in a
    // 32-unit band around a cell edge so a doorway does not create a hard seam.
    vec2 fraction = smoothstep(vec2(0.40), vec2(0.60), fract(cell));
    float south = mix(DeckCell(low.x, low.y), DeckCell(low.x + 1, low.y), fraction.x);
    float north = mix(DeckCell(low.x, low.y + 1), DeckCell(low.x + 1, low.y + 1), fraction.x);
    return mix(south, north, fraction.y);
}

void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec4 surface = texelFetch(u_scene, pixel, 0);
    float depth = texelFetch(u_depth, pixel, 0).r;
    // Reverse-Z clear is zero. Uncovered rays extend to the configured cull horizon
    // and start from the scene medium so geometry can emerge continuously through haze.
    bool uncovered = depth <= 0.000001;
    vec2 uv = (vec2(pixel) + 0.5) / vec2(textureSize(u_depth, 0));
    vec4 clip = vec4(uv * 2.0 - 1.0, uncovered ? 1.0 : depth, 1.0);
    vec4 world_h = fog.inverse_view_projection * clip;
    if (abs(world_h.w) < 0.000001) {
        o_color = surface;
        return;
    }
    vec3 world = world_h.xyz / world_h.w;
    vec3 path = world - fog.camera_position.xyz;
    float distance = uncovered ? deck.distance.w : length(path);
    vec3 ray = path / max(length(path), 0.001);

    // Integrate extinction and in-scatter along the actual camera-to-surface ray. The
    // thin lower medium follows static collision-derived decks along this ray.
    vec3 scattering = deck.background_color.rgb;
    vec3 deck_scattering = deck.lower_color.rgb;
    float warmth = max(fog.light_color.r - fog.light_color.b, 0.0);
    float cosine = clamp(dot(ray, normalize(fog.light_direction.xyz)), -1.0, 1.0);
    const float anisotropy = 0.35;
    float phase = (1.0 - anisotropy * anisotropy) /
                  pow(1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosine, 1.5);
    scattering += fog.light_color.rgb * warmth * min(phase, 3.0) * fog.parameters.w;
    float travel = distance;
    float step_length = travel / 12.0;
    float transmittance = 1.0;
    vec3 integrated_light = vec3(0.0);
    for (int sample_index = 0; sample_index < 12; ++sample_index) {
        float along = (float(sample_index) + 0.5) * step_length;
        vec3 sample_world = fog.camera_position.xyz + ray * along;
        float height = sample_world.y;
        float background_density = deck.density.x *
            clamp(exp(-(height - fog.camera_position.y) / 120.0), 0.75, 1.8) *
            smoothstep(deck.density.y - 20.0, deck.density.y + 20.0, along);
        // Extinguish distant room geometry before its cell can leave the
        // conservative cull bounds. The approach is gradual, so the final
        // visible surfaces and the uncovered ray share the same scene medium.
        background_density += deck.distance.x *
            smoothstep(deck.distance.y, max(deck.distance.z, deck.distance.y + 0.001), along);
        float deck_density = deck.density.z *
            exp(-max(height - DeckHeight(sample_world.xz), 0.0) / max(deck.density.w, 0.001)) *
                             smoothstep(10.0, 30.0, along);
        float density = background_density + deck_density;
        float step_transmittance = exp(-density * step_length);
        vec3 source = (background_density * scattering + deck_density * deck_scattering) /
                      max(density, 0.000001);
        integrated_light += transmittance * (1.0 - step_transmittance) * source;
        transmittance *= step_transmittance;
    }
    vec3 local_scattering = vec3(0.0);
    for (uint i = 0u; i < min(lamps.count.x, 8u); ++i) {
        vec3 to_light = lamps.position_radius[i].xyz - fog.camera_position.xyz;
        float radius = lamps.position_radius[i].w * 0.55;
        float along = dot(to_light, ray);
        float radial_squared = max(dot(to_light, to_light) - along * along, 0.0);
        float half_length = sqrt(max(radius * radius - radial_squared, 0.0));
        // The clear-distance delay applies to background haze; nearby lamp auras start locally.
        float entry = max(along - half_length, 0.0);
        float exit = min(along + half_length, distance);
        float lit_length = max(exit - entry, 0.0);
        float radial_falloff = pow(max(1.0 - sqrt(radial_squared) / radius, 0.0), 2.0);
        float light_transmittance = exp(-deck.density.x * (entry + exit) * 0.5);
        local_scattering += lamps.color_intensity[i].rgb * lamps.color_intensity[i].w *
                            lit_length * radial_falloff * light_transmittance * deck.effects.y;
    }
    vec3 clear_medium = deck.clear_color.rgb;
    vec3 color = transmittance * (uncovered ? clear_medium : surface.rgb) + integrated_light +
                 min(local_scattering, deck.local_limit.rgb);
    o_color = vec4(color, surface.a);
}
