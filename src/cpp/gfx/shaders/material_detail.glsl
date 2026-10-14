vec3 MaterialDetailNormal(vec3 world_normal, vec3 world_position, vec2 uv,
                          uint mesh_flags, float floor_film) {
    vec3 n = normalize(world_normal);
    if (mc.material_maps.x == 0u ||
        mc.material_maps.z == kProfileFlat || mc.material_maps.z == kProfileEffect ||
        (mesh_flags & kMeshShadow) != 0u ||
        mc.normal_detail_strength <= 0.0) return n;

    vec2 detail_uv = MaterialMapUv(n, world_position, uv, kTextureMode == 0);

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

    return ApplyMaterialNormal(n, tangent, bitangent,
        SampleMaterialMap(mc.material_maps.x, detail_uv), mc.normal_detail_strength,
        mesh_flags, mc.material_maps.z, mc.material_maps.w, floor_film);
}
