// Original procedural profile normal maps for enhanced 3D meshes. Authored game UVs are
// retained; genuinely untextured meshes use world-position projection. If the optional
// map pack is missing, textured meshes fall back to subdued albedo-derived detail.
// GS colour, alpha, and all 2D/shadow draws remain untouched.

vec3 DetailAlbedo(vec2 uv) {
    if (kTextureMode == 1) return texture(TEXTURE, uv).rgb;
    if (kTextureMode == 2) return PaletteEntry(texture(TEXTURE, uv).r).rgb;
    return vec3(0.0);
}

vec3 MaterialDetailNormal(vec3 world_normal, vec3 world_position, vec2 uv,
                          vec3 albedo, uint mesh_flags) {
    vec3 n = normalize(world_normal);
    if ((kTextureMode == 0 && mc.material_maps.x == 0u) ||
        (mesh_flags & kMeshShadow) != 0u ||
        mc.normal_detail_strength <= 0.0) return n;

    vec2 detail_uv = MaterialMapUv(n, world_position, uv, kTextureMode == 0);

    vec2 texel = 1.0 / vec2(textureSize(TEXTURE, 0));
    // The signed UV Jacobian retains the correct tangent direction on mirrored strips.
    vec2 uv_x = dFdx(detail_uv), uv_y = dFdy(detail_uv);
    float determinant = uv_x.x * uv_y.y - uv_x.y * uv_y.x;
    if (abs(determinant) < 0.0000001) return n;
    vec3 pos_x = dFdx(world_position), pos_y = dFdy(world_position);
    vec3 tangent = (pos_x * uv_y.y - pos_y * uv_x.y) / determinant;
    vec3 bitangent = (pos_y * uv_x.x - pos_x * uv_y.x) / determinant;
    tangent -= n * dot(n, tangent);
    bitangent -= n * dot(n, bitangent);
    if (dot(tangent, tangent) < 0.000001 || dot(bitangent, bitangent) < 0.000001)
        return n;
    tangent = normalize(tangent);
    bitangent = normalize(bitangent);

    if (mc.material_maps.x != 0u) {
        vec3 mapped = SampleMaterialMap(mc.material_maps.x, detail_uv).xyz * 2.0 - 1.0;
        float strength = clamp(mc.normal_detail_strength, 0.0, 1.0);
        if (mc.material_maps.z == kProfileCloth) {
            // The c01d atlas contains both woven poncho and smoother skin.
            float pale_skin = smoothstep(0.20, 0.42, albedo.b) *
                              (1.0 - smoothstep(1.35, 1.75,
                                  albedo.r / max(albedo.g, 0.01)));
            strength *= mix(0.75, 0.18, pale_skin);
        }
        mapped.xy *= strength;
        return normalize(tangent * mapped.x + bitangent * mapped.y + n * mapped.z);
    }

    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    float left_height = dot(DetailAlbedo(uv - vec2(texel.x, 0.0)), luma);
    float right_height = dot(DetailAlbedo(uv + vec2(texel.x, 0.0)), luma);
    float down_height = dot(DetailAlbedo(uv - vec2(0.0, texel.y)), luma);
    float up_height = dot(DetailAlbedo(uv + vec2(0.0, texel.y)), luma);
    vec2 slope = vec2(right_height - left_height, up_height - down_height) * 0.5;

    float strength = 0.035; // neutral props retain slight authored surface detail
    if ((mesh_flags & kMeshWetTimber) != 0u) {
        // Damp planks retain shallow grain. The visible wet lobe comes from roughness/F0,
        // not from exaggerated normal noise or a new colour texture.
        strength = 0.22;
        slope *= vec2(1.0, 0.65);
    } else if ((mesh_flags & kMeshCharacterMatte) != 0u) {
        // c01d atlases combine Toan's cloth and skin. Skin is pale and comparatively blue;
        // leave it nearly smooth while orange/green fabric takes restrained woven relief.
        float pale_skin = smoothstep(0.20, 0.42, albedo.b) *
                          (1.0 - smoothstep(1.35, 1.75, albedo.r / max(albedo.g, 0.01)));
        strength = mix(0.07, 0.012, pale_skin);
    } else if (max(max(mc.specular.r, mc.specular.g), mc.specular.b) > 0.12) {
        strength = 0.055; // shallow worn-metal variation; preserve its sharp silhouette
    }

    strength *= clamp(mc.normal_detail_strength, 0.0, 1.0);
    vec3 detailed = normalize(n - strength * (slope.x * tangent + slope.y * bitangent));
    // Even a palette discontinuity must not fold the shading normal under the surface.
    return normalize(mix(n, detailed, smoothstep(0.55, 0.8, dot(n, detailed))));
}
