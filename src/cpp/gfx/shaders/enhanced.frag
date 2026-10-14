#version 460
#extension GL_GOOGLE_include_directive : require

#ifdef RAY_QUERY_AO
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#ifdef EARLY_RAY_OPAQUE
// Only the no-alpha-test mesh pipeline uses this module. It can reject hidden fragments
// before invoking expensive ray queries; alpha-tested meshes keep late depth/discard semantics.
layout(early_fragment_tests) in;
#endif
layout(set = 3, binding = 0) uniform accelerationStructureEXT u_ray_scene;
struct RayVertex {
    vec4 position;
    vec4 normal;
    vec4 color;
    vec4 uv;
};
struct RayHitMaterial {
    uint vertex_base;
    uint index_base;
    uint flags;
    uint light_count;
    uint primitive_base;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint texture_mode;
    uint pad0;
    uint pad1;
    uint pad2;
    vec4 world_uv[2];
    vec4 diffuse;
    vec4 ambient;
    vec4 ambient_material;
    vec4 light_direction[4];
    vec4 light_color[4];
    uvec4 material_maps; // normal slot, ORM slot, profile, reserved
};
layout(set = 3, binding = 1, std430) readonly buffer RayVertices {
    RayVertex values[];
} u_ray_vertices;
layout(set = 3, binding = 2, std430) readonly buffer RayIndices {
    uint values[];
} u_ray_indices;
layout(set = 3, binding = 3, std430) readonly buffer RayMaterials {
    RayHitMaterial values[];
} u_ray_materials;
layout(set = 3, binding = 4, std140) uniform LocalLights {
    uvec4 count;
    vec4 position_radius[8];
    vec4 color_intensity[8];
} u_shipwreck_lights;
layout(set = 3, binding = 5, std430) buffer RayCounters {
    uint ao;
    uint contact;
    uint reflection;
    uint diffuse;
    uint local_shadow;
    uint cloth_hit;
} ray_counts;
layout(set = 3, binding = 6, std430) readonly buffer RayPrimitiveMaterials {
    uint values[];
} u_ray_primitive_materials;
#endif

// The GS pixel pipeline for both 2D and mesh draws: texture function (MODULATE), TEXA, CLUT,
// fog, alpha test, and the source side of the alpha blend. The blend itself is fixed-function;
// see README.md for how ((A - B) * C >> 7) + D is split between this shader and the pipeline.

layout(constant_id = 0) const int kTextureMode = 0; // 0 none, 1 RGBA, 2 index + palette
layout(constant_id = 1) const bool kAlphaTest  = false;
layout(constant_id = 2) const bool kHdrOutput = false;
layout(constant_id = 3) const bool kProfileRays = false;

layout(set = 0, binding = 0) uniform texture2D u_textures[8192];
layout(set = 0, binding = 1) uniform sampler u_samplers[24];

layout(push_constant, std430) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha; // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    uint fog_color;
}
pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_fog;
layout(location = 3) in vec3 v_world_position;
layout(location = 4) in vec3 v_world_normal;
layout(location = 5) in vec3 v_vertex_color;
layout(location = 6) flat in uint v_mesh_flags;

// Same std140 block as mesh.vert. The 2D vertex shader sets v_mesh_flags to zero and never reads it.
layout(set = 1, binding = 0, std140) uniform MeshConstants {
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

layout(location = 0, index = 0) out vec4 o_color;
layout(location = 0, index = 1) out vec4 o_factor;

const uint kFog            = 1u;
const uint kTexa           = 2u;
const uint kAem            = 4u;
const uint kOpaqueTarget   = 8u;
const uint kTransformShift = 4u;
const uint kFactorFix      = 64u;
const uint kLinear         = 128u;

const float kModulate = 255.0 / 128.0;
const uint kMeshLit = 1u;
const uint kMeshVertexColor = 2u;
const uint kMeshFog = 4u;
const uint kMeshShadow = 8u;
const uint kMeshWorldUv = 128u;
const uint kMeshMaterialLighting = 512u;
const uint kMeshShipwreckFog = 1024u;
const uint kMeshWetTimber = 2048u;
const uint kMeshShipwreckWorld = 4096u;
const uint kMeshCharacterMatte = 8192u;
const uint kProfileWoodFloor = 0u;
const uint kProfileWoodWall = 1u;
const uint kProfileCloth = 2u;
const uint kProfileLeather = 3u;
const uint kProfileSkin = 4u;
const uint kProfileMetal = 5u;
const uint kProfileStone = 6u;
const uint kProfileProp = 7u;
const uint kProfileFlat = 8u;
const float kPi = 3.141592653589793;

vec3 SrgbToLinear(vec3 color) {
    return mix(color / 12.92, pow(max((color + 0.055) / 1.055, vec3(0.0)), vec3(2.4)),
               vec3(greaterThan(color, vec3(0.04045))));
}

// The brighter fibres in Shipwreck's painted deck look like a white specular streak even
// with every specular and ray term disabled. Damp wood absorbs that diffuse return most
// strongly where the dry albedo is pale; dark grain and the authored plank pattern remain.
vec3 DampWoodAlbedo(vec3 linear_albedo, float upward) {
    float luminance = dot(linear_albedo, vec3(0.2126, 0.7152, 0.0722));
    float absorption = 1.0 / (1.0 + 4.0 * max(luminance - 0.08, 0.0));
    // The authored floor atlas has peach-coloured pale fibres. Under cool damp illumination
    // they should lose red preferentially, without desaturating dark brown plank pigment.
    float pale_warmth = smoothstep(0.16, 0.48, luminance) *
                        clamp((linear_albedo.r - linear_albedo.b) * 2.0, 0.0, 1.0) * upward;
    vec3 colour_absorption = vec3(1.0 - 0.50 * pale_warmth,
                                 1.0 - 0.15 * pale_warmth, 1.0);
    return linear_albedo * colour_absorption * mix(1.0, absorption, upward);
}

// Slot 7 is linear, trilinear and repeating; the material atlas retains authored UVs.
vec4 SampleMaterialMap(uint slot, vec2 uv) {
#ifdef RAY_QUERY_AO
    return texture(sampler2D(u_textures[nonuniformEXT(slot)], u_samplers[7]), uv);
#else
    return texture(sampler2D(u_textures[slot], u_samplers[7]), uv);
#endif
}

vec4 SampleMaterialMapHit(uint slot, vec2 uv, float lod) {
#ifdef RAY_QUERY_AO
    return textureLod(sampler2D(u_textures[nonuniformEXT(slot)], u_samplers[7]), uv, lod);
#else
    return vec4(0.0);
#endif
}

float MaterialMetalness(float mapped_metalness, uint profile, vec3 albedo) {
    float metalness = mapped_metalness;
    if (profile == kProfileMetal) {
        // c01w01 mixes the blue/gray blade with a brown handle in one atlas.
        // Warm red-over-blue AND red-over-green identifies leather, not metal.
        float warm_handle = smoothstep(0.07, 0.20, albedo.r - albedo.b) *
                            smoothstep(0.025, 0.11, albedo.r - albedo.g);
        metalness *= 1.0 - warm_handle;
    }
    return clamp(metalness, 0.0, 1.0);
}

vec2 MaterialMapUv(vec3 normal, vec3 world_position, vec2 authored_uv, bool untextured) {
    if (!untextured) return authored_uv;
    // Untextured props retain a stable world-anchored detail field rather than
    // inheriting undefined mesh UVs. Dominant-axis projection limits stretching.
    vec3 axis = abs(normal);
    if (axis.y >= axis.x && axis.y >= axis.z) return world_position.xz * 0.08;
    if (axis.x >= axis.z) return world_position.zy * 0.08;
    return world_position.xy * 0.08;
}

#ifdef RAY_QUERY_AO
bool RayEffect(uint bit) { return (u_shipwreck_lights.count.y & bit) != 0u; }

void ProfileRay(uint kind) {
    if (!kProfileRays || u_shipwreck_lights.count.w == 0u) return;
    if (kind == 0u) atomicAdd(ray_counts.ao, 1u);
    else if (kind == 1u) atomicAdd(ray_counts.contact, 1u);
    else if (kind == 2u) atomicAdd(ray_counts.reflection, 1u);
    else if (kind == 3u) atomicAdd(ray_counts.diffuse, 1u);
    else atomicAdd(ray_counts.local_shadow, 1u);
}

bool CandidatePassesAlpha(uint instance_index, uint primitive, vec2 bary);

float TraceSceneOcclusion(vec3 origin, vec3 direction, float distance_limit, uint kind) {
    ProfileRay(kind);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, u_ray_scene, 0u,
                          0xff, origin, 0.5, direction, distance_limit);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT &&
            CandidatePassesAlpha(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                                 rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                                 rayQueryGetIntersectionBarycentricsEXT(query, false))) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) return 0.0;
    // These are local contact effects. Distant cavern geometry must not extinguish
    // the game's authored ambient and directional lighting.
    float distance = rayQueryGetIntersectionTEXT(query, true);
    return 1.0 - smoothstep(1.0, distance_limit, distance);
}

float RayAmbientVisibility(vec3 position, vec3 normal) {
    vec3 n = normalize(normal);
    vec3 tangent = normalize(cross(abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) :
                                      vec3(0.0, 1.0, 0.0), n));
    vec3 bitangent = cross(n, tangent);
    // Slowly rotating stratified directions are spatially coherent; a per-pixel hash made
    // the earlier one-frame ray estimate visibly sparkle on dark timber.
    float phase = fract(dot(position.xz, vec2(0.0011, 0.0017)));
    float visible = 0.0;
    int sample_count = u_shipwreck_lights.count.z != 0u ? 1 : 2;
    for (int sample_index = 0; sample_index < sample_count; sample_index++) {
        float angle = 2.0 * kPi * fract(phase + float(sample_index) * 0.61803398875);
        float radius = sqrt((float(sample_index) + 0.5) / float(sample_count)) * 0.8;
        vec3 direction = normalize(tangent * (radius * cos(angle)) +
                                   bitangent * (radius * sin(angle)) +
                                   n * sqrt(1.0 - radius * radius));
        visible += 1.0 - TraceSceneOcclusion(position + n * 0.5, direction, 12.0, 0u);
    }
    return visible / float(sample_count);
}

// Shade actual ray hit geometry, including vertices that were outside the previous image.
// The hit's flat material and vertex colour are retained with the TLAS; no screen history is read.
vec4 SampleHitTexture(RayHitMaterial material, vec2 uv) {
    vec4 sample_color = textureLod(sampler2D(
        u_textures[nonuniformEXT(material.texture_slot)],
        u_samplers[nonuniformEXT(material.sampler_slot)]), uv, 0.0);
    if (material.texture_mode == 2u) {
        if (material.pad0 == 0u) {
            return texelFetch(sampler2D(
                u_textures[nonuniformEXT(material.palette_slot)], u_samplers[0]),
                ivec2(int(sample_color.r * 255.0 + 0.5), 0), 0);
        }
        // GS CLUT filtering resolves each index to an RGBA colour before blending.
        // Interpolating raw indices selects unrelated palette entries.
        vec2 size = vec2(textureSize(sampler2D(
            u_textures[nonuniformEXT(material.texture_slot)], u_samplers[0]), 0));
        vec2 footprint = uv * size - 0.5;
        vec2 base = floor(footprint);
        vec2 fraction = fract(footprint);
        vec4 colors[4];
        for (int corner = 0; corner < 4; ++corner) {
            vec2 offset = vec2(float(corner & 1), float(corner >> 1));
            float index = textureLod(sampler2D(
                u_textures[nonuniformEXT(material.texture_slot)],
                u_samplers[nonuniformEXT(material.sampler_slot)]),
                (base + offset + 0.5) / size, 0.0).r;
            colors[corner] = texelFetch(sampler2D(
                u_textures[nonuniformEXT(material.palette_slot)], u_samplers[0]),
                ivec2(int(index * 255.0 + 0.5), 0), 0);
        }
        return mix(mix(colors[0], colors[1], fraction.x),
                   mix(colors[2], colors[3], fraction.x), fraction.y);
    }
    return sample_color;
}

bool CandidatePassesAlpha(uint instance_index, uint primitive, vec2 bary) {
    RayHitMaterial instance = u_ray_materials.values[instance_index];
    uint material_index = u_ray_primitive_materials.values[instance.primitive_base + primitive];
    if (material_index == 0xffffffffu) return false;
    RayHitMaterial material = u_ray_materials.values[material_index];
    if ((material.pad1 & 1u) == 0u) return true;
    uint base = material.index_base + primitive * 3u;
    RayVertex a = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base]];
    RayVertex b = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base + 1u]];
    RayVertex c = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base + 2u]];
    vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec2 uv = a.uv.xy * weights.x + b.uv.xy * weights.y + c.uv.xy * weights.z;
    if ((material.flags & kMeshWorldUv) != 0u) {
        vec4 object_position = vec4(a.position.xyz * weights.x + b.position.xyz * weights.y +
                                    c.position.xyz * weights.z, 1.0);
        uv = vec2(dot(material.world_uv[0], object_position),
                  dot(material.world_uv[1], object_position));
    }
    vec4 texel = material.texture_mode == 0u ? vec4(1.0) : SampleHitTexture(material, uv);
    if ((material.pad2 & 1u) != 0u) {
        float ta0 = float((material.pad2 >> 8u) & 0xffu) / 128.0;
        texel.a = (material.pad2 & 2u) != 0u &&
                  texel.r + texel.g + texel.b == 0.0 ? 0.0 : ta0;
    }
    float alpha = material.diffuse.a * texel.a;
    if ((material.flags & kMeshVertexColor) != 0u)
        alpha *= a.color.a * weights.x + b.color.a * weights.y + c.color.a * weights.z;
    int value = int(alpha * 128.0 + 0.5);
    int reference = int((material.pad1 >> 8u) & 0xffu);
    switch ((material.pad1 >> 1u) & 7u) {
        case 0u: return false;
        case 1u: return true;
        case 2u: return value < reference;
        case 3u: return value <= reference;
        case 4u: return value == reference;
        case 5u: return value >= reference;
        case 6u: return value > reference;
        default: return value != reference;
    }
}

vec3 TraceHitRadiance(vec3 origin, vec3 direction, float distance_limit, uint kind,
                      float ray_roughness) {
    ProfileRay(kind);
    rayQueryEXT query;
    rayQueryInitializeEXT(query, u_ray_scene, 0u,
                          0xff, origin, 0.5, direction, distance_limit);
    while (rayQueryProceedEXT(query)) {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT &&
            CandidatePassesAlpha(rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                                 rayQueryGetIntersectionPrimitiveIndexEXT(query, false),
                                 rayQueryGetIntersectionBarycentricsEXT(query, false))) {
            rayQueryConfirmIntersectionEXT(query);
        }
    }
    if (rayQueryGetIntersectionTypeEXT(query, true) !=
        gl_RayQueryCommittedIntersectionTriangleEXT) return vec3(0.0);

    uint instance_index = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
    RayHitMaterial instance = u_ray_materials.values[instance_index];
    RayHitMaterial material = u_ray_materials.values[
        u_ray_primitive_materials.values[instance.primitive_base + primitive]];
    if (kind == 2u && kProfileRays && (material.flags & kMeshCharacterMatte) != 0u)
        atomicAdd(ray_counts.cloth_hit, 1u);
    uint base = material.index_base + primitive * 3u;
    RayVertex a = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base]];
    RayVertex b = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base + 1u]];
    RayVertex c = u_ray_vertices.values[material.vertex_base + u_ray_indices.values[base + 2u]];
    vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
    vec3 weights = vec3(1.0 - bary.x - bary.y, bary.x, bary.y);
    vec3 object_position = a.position.xyz * weights.x + b.position.xyz * weights.y +
                           c.position.xyz * weights.z;
    vec3 object_normal = a.normal.xyz * weights.x + b.normal.xyz * weights.y +
                         c.normal.xyz * weights.z;
    if (dot(object_normal, object_normal) < 0.000001) {
        object_normal = cross(b.position.xyz - a.position.xyz,
                              c.position.xyz - a.position.xyz);
    }
    object_normal = normalize(object_normal);
    mat3 normal_matrix = transpose(mat3(rayQueryGetIntersectionWorldToObjectEXT(query, true)));
    vec3 normal = normalize(normal_matrix * object_normal);
    if (dot(normal, direction) > 0.0) normal = -normal;
    float hit_distance = rayQueryGetIntersectionTEXT(query, true);
    vec3 hit_position = origin + direction * hit_distance;
    vec2 uv = a.uv.xy * weights.x + b.uv.xy * weights.y + c.uv.xy * weights.z;
    if ((material.flags & kMeshWorldUv) != 0u) {
        vec4 object_h = vec4(object_position, 1.0);
        uv = vec2(dot(material.world_uv[0], object_h),
                  dot(material.world_uv[1], object_h));
    }
    uv = MaterialMapUv(normal, hit_position, uv, material.texture_mode == 0u);
    vec3 texel = vec3(1.0);
    vec3 hit_srgb = vec3(1.0);
    if (material.texture_mode != 0u) {
        float distance = hit_distance;
        hit_srgb = clamp(SampleHitTexture(material, uv).rgb, 0.0, 1.0);
        // Existing game textures generally have one mip. A small symmetric hit-texture
        // footprint approximates the widening rough reflection cone without shifting the
        // ray away from its physically correct mirror direction.
        if (kind == 2u && ray_roughness > 0.2) {
            vec2 texel_size = 1.0 / vec2(textureSize(sampler2D(
                u_textures[nonuniformEXT(material.texture_slot)], u_samplers[0]), 0));
            vec2 offset = texel_size * clamp(ray_roughness * distance / 55.0, 0.35, 2.0);
            texel = 0.25 * (
                SrgbToLinear(clamp(SampleHitTexture(material, uv + vec2( offset.x,  offset.y)).rgb, 0.0, 1.0)) +
                SrgbToLinear(clamp(SampleHitTexture(material, uv + vec2(-offset.x,  offset.y)).rgb, 0.0, 1.0)) +
                SrgbToLinear(clamp(SampleHitTexture(material, uv + vec2( offset.x, -offset.y)).rgb, 0.0, 1.0)) +
                SrgbToLinear(clamp(SampleHitTexture(material, uv + vec2(-offset.x, -offset.y)).rgb, 0.0, 1.0)));
        } else {
            texel = SrgbToLinear(hit_srgb);
        }
    }

    float map_lod = clamp(log2(max(hit_distance * max(ray_roughness, 0.2) / 32.0, 1.0)),
                          0.0, 7.0);
    vec3 orm = material.material_maps.y != 0u
        ? SampleMaterialMapHit(material.material_maps.y, uv, map_lod).rgb
        : vec3(1.0, 0.78, 0.0);
    float metalness = MaterialMetalness(orm.b, material.material_maps.z, hit_srgb);
    float roughness = clamp(orm.g, 0.32, 0.95);
    if ((material.flags & kMeshCharacterMatte) != 0u) roughness = max(roughness, 0.85);
    if ((material.flags & kMeshWetTimber) != 0u && normal.y > 0.3)
        roughness = max(roughness, 0.68);
    if (material.material_maps.x != 0u && mc.normal_detail_strength > 0.0) {
        mat4x3 object_to_world = rayQueryGetIntersectionObjectToWorldEXT(query, true);
        vec3 pa = object_to_world * vec4(a.position.xyz, 1.0);
        vec3 pb = object_to_world * vec4(b.position.xyz, 1.0);
        vec3 pc = object_to_world * vec4(c.position.xyz, 1.0);
        vec2 ua = a.uv.xy, ub = b.uv.xy, uc = c.uv.xy;
        if ((material.flags & kMeshWorldUv) != 0u) {
            ua = vec2(dot(material.world_uv[0], a.position), dot(material.world_uv[1], a.position));
            ub = vec2(dot(material.world_uv[0], b.position), dot(material.world_uv[1], b.position));
            uc = vec2(dot(material.world_uv[0], c.position), dot(material.world_uv[1], c.position));
        }
        ua = MaterialMapUv(normal, pa, ua, material.texture_mode == 0u);
        ub = MaterialMapUv(normal, pb, ub, material.texture_mode == 0u);
        uc = MaterialMapUv(normal, pc, uc, material.texture_mode == 0u);
        vec3 edge1 = pb - pa, edge2 = pc - pa;
        vec2 duv1 = ub - ua, duv2 = uc - ua;
        float determinant = duv1.x * duv2.y - duv1.y * duv2.x;
        if (abs(determinant) > 0.0000001) {
            vec3 tangent = (edge1 * duv2.y - edge2 * duv1.y) / determinant;
            vec3 bitangent = (edge2 * duv1.x - edge1 * duv2.x) / determinant;
            tangent -= normal * dot(normal, tangent);
            bitangent -= normal * dot(normal, bitangent);
            if (dot(tangent, tangent) > 0.000001 && dot(bitangent, bitangent) > 0.000001) {
                vec3 mapped = SampleMaterialMapHit(material.material_maps.x, uv, map_lod).xyz * 2.0 - 1.0;
                float strength = clamp(mc.normal_detail_strength, 0.0, 1.0);
                if ((material.flags & kMeshCharacterMatte) != 0u)
                    strength *= mix(0.75, 0.18, smoothstep(0.20, 0.42, hit_srgb.b));
                mapped.xy *= strength;
                normal = normalize(normalize(tangent) * mapped.x +
                                   normalize(bitangent) * mapped.y + normal * mapped.z);
            }
        }
    }
    float hit_wood_upward = (material.flags & kMeshWetTimber) != 0u
        ? smoothstep(0.30, 0.70, normal.y) : 0.0;
    texel = DampWoodAlbedo(texel, hit_wood_upward);
    vec3 base_color = SrgbToLinear(clamp(material.diffuse.rgb, 0.0, 1.0)) * texel;
    vec3 diffuse = base_color * (1.0 - metalness);
    vec3 f0 = mix((material.flags & kMeshCharacterMatte) != 0u ? vec3(0.01) : vec3(0.04),
                  clamp(base_color, vec3(0.02), vec3(0.95)), metalness);
    vec3 result = diffuse;
    if ((material.flags & kMeshLit) != 0u) {
        bool world = (material.flags & kMeshShipwreckWorld) != 0u;
        vec3 ambient_irradiance = world ? vec3(0.48, 0.72, 0.84) : vec3(1.0);
        vec3 direct_irradiance = world ? vec3(0.62, 0.78, 0.90) : vec3(1.0);
        result = SrgbToLinear(clamp(material.ambient.rgb, 0.0, 1.0)) *
                 SrgbToLinear(clamp(material.ambient_material.rgb, 0.0, 1.0)) *
                 texel * (1.0 - metalness) * orm.r * ambient_irradiance;
        result += f0 * metalness * 0.012 * orm.r;
        float alpha = roughness * roughness;
        float alpha2 = alpha * alpha;
        vec3 view = -direction;
        float ndv = max(dot(normal, view), 0.001);
        for (uint i = 0u; i < min(material.light_count, 4u); i++) {
            vec3 light = normalize(-material.light_direction[i].xyz);
            float ndl = max(dot(normal, light), 0.0);
            if (ndl <= 0.0) continue;
            vec3 half_vector = normalize(light + view);
            float ndh = max(dot(normal, half_vector), 0.0);
            float vdh = max(dot(view, half_vector), 0.0);
            vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
            float distribution = alpha2 / (kPi * pow(ndh * ndh * (alpha2 - 1.0) + 1.0, 2.0));
            float k = pow(roughness + 1.0, 2.0) * 0.125;
            float geometry = (ndv / (ndv * (1.0 - k) + k)) *
                             (ndl / (ndl * (1.0 - k) + k));
            vec3 specular = distribution * geometry * fresnel / max(4.0 * ndv * ndl, 0.001);
            if ((material.flags & kMeshCharacterMatte) != 0u) specular *= 0.12;
            if ((material.flags & kMeshWetTimber) != 0u && normal.y > 0.3) specular *= 0.32;
            result += ndl * SrgbToLinear(clamp(material.light_color[i].rgb, 0.0, 1.0)) *
                      direct_irradiance * (diffuse * (3.0 / kPi) + specular * 3.0);
        }
        if ((material.flags & kMeshShipwreckWorld) != 0u) {
            for (uint i = 0u; i < min(u_shipwreck_lights.count.x, 8u); i++) {
                vec3 to_light = u_shipwreck_lights.position_radius[i].xyz - hit_position;
                float radius = u_shipwreck_lights.position_radius[i].w;
                float distance_squared = dot(to_light, to_light);
                if (distance_squared >= radius * radius || distance_squared < 1.0) continue;
                float ndl = max(dot(normal, to_light * inversesqrt(distance_squared)), 0.0);
                float edge = max(1.0 - distance_squared / (radius * radius), 0.0);
                float falloff = edge * edge / (1.0 + distance_squared / 2500.0);
                result += diffuse * ndl * falloff *
                          u_shipwreck_lights.color_intensity[i].rgb *
                          u_shipwreck_lights.color_intensity[i].w * (0.55 / kPi);
            }
        }
    }
    if ((material.flags & kMeshVertexColor) != 0u) {
        result *= SrgbToLinear(clamp(a.color.rgb * weights.x + b.color.rgb * weights.y +
                                     c.color.rgb * weights.z, 0.0, 1.0));
    }
    return clamp(result, vec3(0.0), vec3(2.0));
}

vec3 RayIndirectLighting(vec3 position, vec3 normal, vec3 reflection_normal, vec3 view,
                         float roughness, vec3 f0,
                         vec3 diffuse_albedo, bool reflective_surface, bool wet_face) {
    // One mirror-direction hit cannot integrate a rough timber BRDF: at grazing angles
    // it preserves full character and lantern silhouettes even with a blurred hit texel.
    // Let damp planks use their diffuse bounce and broad direct GGX response instead.
    bool rough_wet_timber = wet_face;
    bool trace_reflection = RayEffect(4u) && reflective_surface && !rough_wet_timber;
    // A dark albedo cannot return visible first-bounce energy at the deliberately restrained
    // GI gain. Avoid tracing a diffuse ray when its maximum useful contribution is tiny.
    bool trace_diffuse = RayEffect(8u) &&
        dot(diffuse_albedo, vec3(0.2126, 0.7152, 0.0722)) > 0.06;
    if (!trace_reflection && !trace_diffuse) return vec3(0.0);
    vec3 n = normalize(normal);
    vec3 rn = normalize(reflection_normal);
    vec3 reflected = reflect(-view, rn);
    float seed = fract(dot(position.xz, vec2(0.0011, 0.0017)));
    vec3 tangent = normalize(cross(abs(n.z) < 0.9 ? vec3(0.0, 0.0, 1.0) :
                                      vec3(0.0, 1.0, 0.0), n));
    vec3 bitangent = cross(n, tangent);
    vec3 reflected_radiance = vec3(0.0);
    vec3 bounce_radiance = vec3(0.0);
    // The old single stochastic offset was a coherent, one-sided mirror displacement across
    // floor strips. Trace the actual reflected direction and blur only the hit footprint.
    if (trace_reflection && dot(reflected, rn) > 0.0)
        reflected_radiance = TraceHitRadiance(position + rn * 0.5, reflected,
                                              1024.0, 2u, roughness);
    // One deterministic cosine-weighted diffuse direction avoids per-frame sparkle.
    // This is a low-energy first-bounce estimate, not a converged GI solution.
    float bounce_radius = sqrt(0.5);
    float bounce_angle = 2.0 * kPi * fract(seed * 7.193);
    vec3 bounce_direction = normalize(n * sqrt(1.0 - bounce_radius * bounce_radius) +
        tangent * (bounce_radius * cos(bounce_angle)) +
        bitangent * (bounce_radius * sin(bounce_angle)));
    if (trace_diffuse)
        bounce_radiance = TraceHitRadiance(position + n * 0.5, bounce_direction,
                                           384.0, 3u, 0.0);
    float ndv = max(dot(rn, view), 0.0);
    vec3 surface_f0 = clamp(f0, vec3(0.005), vec3(0.95));
    vec3 fresnel = surface_f0 + (1.0 - surface_f0) * pow(1.0 - ndv, 5.0);
    // Traced reflections remain on smooth/metal materials; a rough wet plank has no
    // coherent mirror hit to composite. Its low-frequency colour comes from the bounce.
    return fresnel * reflected_radiance * (wet_face ? 0.075 : 0.35) +
           (vec3(1.0) - fresnel) * diffuse_albedo *
           bounce_radiance * 0.045;
}

#endif

vec3 MaterialDetailNormal(vec3 world_normal, vec3 world_position, vec2 uv,
                          vec3 albedo, uint mesh_flags);

vec3 MaterialLighting(float roughness, vec3 n) {
    vec3 v = normalize(mc.camera_position.xyz - v_world_position);
    float ndv = max(dot(n, v), 0.0);
    // Even materials with a zero legacy specular vector retain a subdued dielectric reflection.
    vec3 f0 = clamp(max(mc.specular.rgb, vec3(0.04)), vec3(0.0), vec3(1.0));
    vec3 color = mc.ambient.rgb * mc.ambient_material.rgb;
#ifdef RAY_QUERY_AO
    if (RayEffect(1u)) color *= mix(0.85, 1.0, RayAmbientVisibility(v_world_position, n));
    // One shadow ray follows the strongest light that faces this fragment.
    uint shadow_light = 0u;
    float strongest = 0.0;
    for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
        vec3 direction = normalize(-mc.light_direction[i].xyz);
        float strength = max(dot(n, direction), 0.0) *
                         dot(mc.light_color[i].rgb, vec3(0.2126, 0.7152, 0.0722));
        if (strength > strongest) {
            strongest = strength;
            shadow_light = i;
        }
    }
#endif
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
        vec3 l = normalize(-mc.light_direction[i].xyz);
        float ndl = max(dot(n, l), 0.0);
        if (ndl <= 0.0) continue;
#ifdef RAY_QUERY_AO
        float shadow = 1.0;
        if (RayEffect(2u) && i == shadow_light && strongest > 0.08) {
            shadow = 1.0 - 0.35 * TraceSceneOcclusion(v_world_position + n * 0.5, l, 20.0, 1u);
        }
#endif
        vec3 h = normalize(l + v);
        float ndh = max(dot(n, h), 0.0);
        float vdh = max(dot(v, h), 0.0);
        float d = alpha2 / (kPi * pow(ndh * ndh * (alpha2 - 1.0) + 1.0, 2.0));
        float k = pow(roughness + 1.0, 2.0) * 0.125;
        float g1v = ndv / (ndv * (1.0 - k) + k);
        float g1l = ndl / (ndl * (1.0 - k) + k);
        vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        vec3 specular = d * g1v * g1l * fresnel / max(4.0 * ndv * ndl, 0.001);
        vec3 diffuse = (1.0 - fresnel) * mc.diffuse.rgb;
        color += (diffuse + specular) * ndl * mc.light_color[i].rgb
#ifdef RAY_QUERY_AO
                 * shadow
#endif
                 ;
    }
    if ((v_mesh_flags & kMeshVertexColor) != 0u) color *= v_vertex_color * kModulate;
    return color;
}

// The enhanced display target stores scene radiance. Decode the game's colour inputs before
// evaluating the BRDF, and apply the base texture only to diffuse/ambient energy. Multiplying
// the whole lighting result by the texture would tint dielectric highlights with painted albedo.
vec3 MaterialLightingLinear(vec3 texture_albedo, float roughness, vec3 n,
                            float metalness, float ao) {
    vec3 v = normalize(mc.camera_position.xyz - v_world_position);
    float ndv = max(dot(n, v), 0.001);
    bool character_matte = (v_mesh_flags & kMeshCharacterMatte) != 0u;
    bool shipwreck_world = (v_mesh_flags & kMeshShipwreckWorld) != 0u;
    // Shipwreck's original neutral directional light leaves its warm wood textures lit as if
    // in air. Absorb more red in the nonlocal irradiance; retain the authored grain/pigment and
    // add nearby lantern radiance below this block without the cool filter.
    vec3 ambient_irradiance = shipwreck_world ? vec3(0.48, 0.72, 0.84) : vec3(1.0);
    vec3 direct_irradiance = shipwreck_world ? vec3(0.62, 0.78, 0.90) : vec3(1.0);
    // Only Shipwreck's upward-facing timber receives the damp cool finish. The original
    // texture grain remains visible, while specular radiance stays untinted and directional.
    // Classify the board from its geometric plane. The detail normal may tip individual
    // fibres for lighting, but it must not switch their damp albedo/F0 on and off.
    float wet_upward = (v_mesh_flags & kMeshWetTimber) != 0u
        ? smoothstep(0.30, 0.70, normalize(v_world_normal).y) : 0.0;
    texture_albedo = DampWoodAlbedo(texture_albedo, wet_upward);
    vec3 wet_tint = mix(vec3(1.0), vec3(0.70, 0.82, 0.88), wet_upward);
    vec3 base = SrgbToLinear(clamp(mc.diffuse.rgb, 0.0, 1.0)) * texture_albedo * wet_tint;
    if ((v_mesh_flags & kMeshVertexColor) != 0u) {
        base *= SrgbToLinear(clamp(v_vertex_color * kModulate, 0.0, 1.0));
    }
    vec3 f0 = clamp(mc.specular.rgb, vec3(0.04), vec3(0.10));
    if (character_matte) f0 = vec3(0.01);
    f0 = mix(f0, vec3(0.030), wet_upward);
    f0 = mix(f0, clamp(base, vec3(0.02), vec3(0.95)), metalness);
    // The floor profile's roughness map describes shallow, elongated grain. Keep every
    // timber lobe broad, but give its raised ridges a little more return than the troughs.
    float wet_sheen = mix(0.22, 0.58, 1.0 - smoothstep(0.68, 0.78, roughness));
    float wet_specular_scale = mix(1.0, wet_sheen, wet_upward);
    vec3 color = SrgbToLinear(clamp(mc.ambient.rgb, 0.0, 1.0)) *
                 SrgbToLinear(clamp(mc.ambient_material.rgb, 0.0, 1.0)) *
                 texture_albedo * wet_tint * ambient_irradiance * (1.0 - metalness) * ao;
    // Conductors have no painted diffuse ambient; a tiny coloured environment response
    // keeps a dark blade legible away from a direct light without inventing white fill.
    color += f0 * metalness * 0.012 * ao;
    // Toan's cloth colour lives in the texture. A small neutral fill keeps the orange weave
    // visible under Shipwreck's cool, weak ambient without inventing a glossy white highlight.
    if (character_matte) color += texture_albedo * 0.07;
#ifdef RAY_QUERY_AO
    if (RayEffect(1u)) color *= mix(character_matte ? 0.80 : 0.64, 1.0,
                                   RayAmbientVisibility(v_world_position, n));
    uint shadow_light = 0u;
    float strongest = 0.0;
    for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
        vec3 l = normalize(-mc.light_direction[i].xyz);
        float strength = max(dot(n, l), 0.0) *
            dot(mc.light_color[i].rgb, vec3(0.2126, 0.7152, 0.0722));
        if (strength > strongest) { strongest = strength; shadow_light = i; }
    }
#endif
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
        vec3 l = normalize(-mc.light_direction[i].xyz);
        float ndl = max(dot(n, l), 0.0);
        if (ndl <= 0.0) continue;
        vec3 h = normalize(l + v);
        float ndh = max(dot(n, h), 0.0);
        float vdh = max(dot(v, h), 0.0);
        float d = alpha2 / (kPi * pow(ndh * ndh * (alpha2 - 1.0) + 1.0, 2.0));
        float k = pow(roughness + 1.0, 2.0) * 0.125;
        float g1v = ndv / (ndv * (1.0 - k) + k);
        float g1l = ndl / (ndl * (1.0 - k) + k);
        vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
        vec3 specular = d * g1v * g1l * fresnel / max(4.0 * ndv * ndl, 0.001);
        if (character_matte) specular *= 0.12;
        vec3 diffuse = (1.0 - metalness) * (1.0 - fresnel) * base / kPi;
        // The three legacy directional lights are broad fill in Shipwreck. A uniform GGX
        // response from them reads as a pale sheet at grazing angles. Let only the smoother
        // raised fibres of the wood-floor roughness map catch a broken damp glint;
        // nearby lanterns retain their separate, broader specular response below.
        float floor_ridge = pow(1.0 - smoothstep(0.60, 0.70, roughness), 5.0);
        float directional_wood_sheen = 0.012 + 0.16 * floor_ridge;
        specular *= mix(1.0, directional_wood_sheen, wet_upward);
        float shadow = 1.0;
#ifdef RAY_QUERY_AO
        if (RayEffect(2u) && i == shadow_light && strongest > 0.08) {
            shadow -= 0.35 * TraceSceneOcclusion(v_world_position + n * 0.5, l, 20.0, 1u);
        }
#endif
        color += (diffuse + specular) * ndl *
                 SrgbToLinear(clamp(mc.light_color[i].rgb, 0.0, 1.0)) *
                 direct_irradiance * 3.0 * shadow;
    }
#ifdef RAY_QUERY_AO
    if (RayEffect(16u) &&
        (v_mesh_flags & (kMeshShipwreckWorld | kMeshCharacterMatte)) != 0u) {
        // All nearby emitters contribute direct radiance. Only the strongest light casts a
        // query shadow at this pixel, keeping the contact cue without eight rays per fragment.
        uint shadow_light_index = 8u;
        float shadow_strength = 0.0;
        for (uint i = 0u; i < min(u_shipwreck_lights.count.x, 8u); ++i) {
            vec3 delta = u_shipwreck_lights.position_radius[i].xyz - v_world_position;
            float distance_squared = dot(delta, delta);
            float radius = u_shipwreck_lights.position_radius[i].w;
            if (distance_squared < 1.0 || distance_squared >= radius * radius) continue;
            float ndl = max(dot(n, delta * inversesqrt(distance_squared)), 0.0);
            float edge = max(1.0 - distance_squared / (radius * radius), 0.0);
            float strength = ndl * edge * edge / (1.0 + distance_squared / 2500.0) *
                dot(u_shipwreck_lights.color_intensity[i].rgb, vec3(0.2126, 0.7152, 0.0722)) *
                u_shipwreck_lights.color_intensity[i].w;
            if (strength > shadow_strength) {
                shadow_strength = strength;
                shadow_light_index = i;
            }
        }
        for (uint i = 0u; i < min(u_shipwreck_lights.count.x, 8u); ++i) {
            vec3 delta = u_shipwreck_lights.position_radius[i].xyz - v_world_position;
            float distance = length(delta);
            float radius = u_shipwreck_lights.position_radius[i].w;
            if (distance < 1.0 || distance >= radius) continue;
            vec3 l = delta / distance;
            float ndl = max(dot(n, l), 0.0);
            if (ndl <= 0.0) continue;
            float edge = max(1.0 - (distance * distance) / (radius * radius), 0.0);
            float falloff = edge * edge / (1.0 + distance * distance / 2500.0);
            float visibility = 1.0;
            if (i == shadow_light_index && shadow_strength > 0.02)
                visibility -= 0.8 * TraceSceneOcclusion(
                v_world_position + n * 0.8, l, max(distance - 1.5, 1.0), 4u);
            vec3 h = normalize(l + v);
            float ndh = max(dot(n, h), 0.0);
            float vdh = max(dot(v, h), 0.0);
            vec3 fresnel = f0 + (1.0 - f0) * pow(1.0 - vdh, 5.0);
            float d = alpha2 / (kPi * pow(ndh * ndh * (alpha2 - 1.0) + 1.0, 2.0));
            float k = pow(roughness + 1.0, 2.0) * 0.125;
            float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
            vec3 specular = d * g * fresnel / max(4.0 * ndv * ndl, 0.001);
            if (character_matte) specular *= 0.12;
            vec3 radiance = u_shipwreck_lights.color_intensity[i].rgb *
                            u_shipwreck_lights.color_intensity[i].w * 0.55;
            specular *= wet_specular_scale;
            color += ((1.0 - metalness) * (1.0 - fresnel) * base / kPi + specular) * ndl *
                     radiance * falloff * visibility;
        }
    }
#endif
    return max(color, vec3(0.0));
}

#ifdef RAY_QUERY_AO
// Retain prelit Shipwreck timber's painted diffuse texture and a broad, local,
// shadowed dielectric lobe. A single hit cannot resolve a rough reflection cone.
vec3 ShipwreckPrelitSpecular(vec3 n) {
    if (!RayEffect(32u)) return vec3(0.0);
    if (dot(v_world_normal, v_world_normal) < 0.001) return vec3(0.0);
    vec3 v = normalize(mc.camera_position.xyz - v_world_position);
    float upward = smoothstep(0.45, 0.85, n.y);
    if (upward <= 0.0) return vec3(0.0);
    float roughness = mix(0.72, 0.68, upward);
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float ndv = max(dot(n, v), 0.001);
    vec3 best_light = vec3(0.0);
    vec3 best_direction = n;
    float best_distance = 0.0;
    float best_strength = 0.0;
    for (uint i = 0u; i < min(u_shipwreck_lights.count.x, 8u); ++i) {
        vec3 delta = u_shipwreck_lights.position_radius[i].xyz - v_world_position;
        float distance = length(delta);
        float radius = u_shipwreck_lights.position_radius[i].w;
        if (distance < 1.0 || distance >= radius) continue;
        vec3 l = delta / distance;
        float ndl = max(dot(n, l), 0.0);
        float edge = max(1.0 - distance * distance / (radius * radius), 0.0);
        float strength = ndl * edge * edge / (1.0 + distance * distance / 2500.0);
        if (strength > best_strength) {
            best_strength = strength;
            best_distance = distance;
            best_direction = l;
            best_light = u_shipwreck_lights.color_intensity[i].rgb *
                         u_shipwreck_lights.color_intensity[i].w;
        }
    }
    vec3 result = vec3(0.0);
    if (best_strength > 0.002) {
        vec3 h = normalize(best_direction + v);
        float ndh = max(dot(n, h), 0.0);
        float vdh = max(dot(v, h), 0.0);
        float d = alpha2 / (kPi * pow(ndh * ndh * (alpha2 - 1.0) + 1.0, 2.0));
        float k = pow(roughness + 1.0, 2.0) * 0.125;
        float ndl = max(dot(n, best_direction), 0.001);
        float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
        vec3 fresnel = vec3(0.035) + vec3(0.965) * pow(1.0 - vdh, 5.0);
        float visible = 1.0 - 0.85 * TraceSceneOcclusion(
            v_world_position + n * 0.8, best_direction, max(best_distance - 1.5, 1.0), 4u);
        result += d * g * fresnel / max(4.0 * ndv * ndl, 0.001) *
                  best_light * best_strength * visible * 0.32;
    }
    return min(result, vec3(0.3));
}
#endif

float MaterialFogVisibility() {
    vec3 ray = v_world_position - mc.camera_position.xyz;
    float eye_depth = max(dot(ray, normalize(mc.camera_forward.xyz)), 0.001);
    float distance = length(ray);
    // Match the game's fog visibility on-axis, then extend optical depth along the actual
    // camera-to-fragment path. This keeps its chosen fog range and colour.
    float axial = clamp(mc.fog.x + mc.fog.y / eye_depth, mc.fog.z, mc.fog.w) / 255.0;
    return pow(clamp(axial, 0.0001, 1.0), distance / eye_depth);
}

vec3 ShipwreckAtmosphere(vec3 surface_color) {
    vec3 path = v_world_position - mc.camera_position.xyz;
    float distance = length(path);
    vec3 view_ray = path / max(distance, 0.001);
    // Midpoint integration of an exponential height-density profile relative to the camera.
    // Lower decks grow denser; the nearby character keeps most of its colour and contrast.
    float midpoint_height = mc.camera_position.y + path.y * 0.5;
    float density = clamp(exp(-(midpoint_height - mc.camera_position.y) / 120.0), 0.75, 1.8);
    // Keep nearby characters, torches and foreground deck nearly clear; accumulate optical
    // depth only after the first few metres of the camera ray.
    float visibility = exp(-0.0035 * max(distance - 45.0, 0.0) * density);

    vec3 scattering = vec3(0.045, 0.135, 0.18);
    // The renderer currently exposes directional lights only. Their warm component can tint
    // scattering in the forward direction, but this does not model local lantern volumes.
    const float anisotropy = 0.35;
    for (uint i = 0u; i < min(mc.light_count, 4u); i++) {
        float warmth = max(mc.light_color[i].r - mc.light_color[i].b, 0.0);
        float cosine = clamp(dot(view_ray, normalize(mc.light_direction[i].xyz)), -1.0, 1.0);
        float phase = (1.0 - anisotropy * anisotropy) /
                      pow(1.0 + anisotropy * anisotropy - 2.0 * anisotropy * cosine, 1.5);
        scattering += mc.light_color[i].rgb * warmth * min(phase, 3.0) * 0.025;
    }
    return mix(scattering, surface_color, visibility);
}

#define TEXTURE sampler2D(u_textures[pc.texture_slot], u_samplers[pc.sampler_slot])
#define PALETTE sampler2D(u_textures[pc.palette_slot], u_samplers[0])

// textureGather returns texels (0,1), (1,1), (1,0), (0,0) of the 2x2 footprint.
float Bilerp(vec4 gathered, vec2 f) {
    return mix(mix(gathered.w, gathered.z, f.x), mix(gathered.x, gathered.y, f.x), f.y);
}

vec2 Footprint() {
    return fract(v_uv * vec2(textureSize(TEXTURE, 0)) - 0.5);
}

// The footprint's shared corner. Gathering at v_uv itself lets the hardware's fixed-point pick of
// the 2x2 disagree with Footprint() where a sample lands on a texel centre, which swaps in the
// neighbouring footprint under the old weights.
vec2 FootprintCorner() {
    vec2 size = vec2(textureSize(TEXTURE, 0));
    return (floor(v_uv * size - 0.5) + 1.0) / size;
}

vec4 PaletteEntry(float index) {
    return texelFetch(PALETTE, ivec2(int(index * 255.0 + 0.5), 0), 0);
}

// The GS looks the CLUT up before filtering, so a filtered palette texture blends four entries.
vec4 SamplePalette() {
    if ((pc.flags & kLinear) == 0u) {
        return PaletteEntry(texture(TEXTURE, v_uv).r);
    }
    vec4 index = textureGather(TEXTURE, FootprintCorner(), 0);
    vec2 f     = Footprint();
    return mix(mix(PaletteEntry(index.w), PaletteEntry(index.z), f.x), mix(PaletteEntry(index.x), PaletteEntry(index.y), f.x), f.y);
}

// TEXA expands alpha per texel before filtering too: with AEM a filtered edge between black and
// coloured texels fades out instead of taking TA0 throughout.
vec4 SampleRgba() {
    vec4 t = texture(TEXTURE, v_uv);
    if ((pc.flags & kTexa) != 0u) {
        float ta0 = float(pc.alpha >> 24) / 128.0;
        bool  aem = (pc.flags & kAem) != 0u;
        if (aem && (pc.flags & kLinear) != 0u) {
            vec2 corner = FootprintCorner();
            vec4 sum    = textureGather(TEXTURE, corner, 0) + textureGather(TEXTURE, corner, 1) + textureGather(TEXTURE, corner, 2);
            t.a      = Bilerp(vec4(greaterThan(sum, vec4(0.0))) * ta0, Footprint());
        } else {
            t.a = aem && t.r + t.g + t.b == 0.0 ? 0.0 : ta0;
        }
    }
    return t;
}

#include "material_detail.glsl"

bool AlphaPasses(float alpha) {
    int  value = int(alpha * 128.0 + 0.5);
    int  ref   = int(pc.alpha & 0xFFu);
    uint func  = (pc.alpha >> 8) & 7u;
    switch (func) {
        case 0u:
            return false;
        case 1u:
            return true;
        case 2u:
            return value < ref;
        case 3u:
            return value <= ref;
        case 4u:
            return value == ref;
        case 5u:
            return value >= ref;
        case 6u:
            return value > ref;
        default:
            return value != ref;
    }
}

void main() {
    // Alpha is carried in GS units over 128 (1.0 = 0x80) so the alpha test and blend factor see
    // values above 0x80 before the attachment saturates them.
    vec4 texel = vec4(1.0);
    if (kTextureMode != 0) texel = kTextureMode == 1 ? SampleRgba() : SamplePalette();
    vec4 mesh_color = v_color;
    float roughness = mc.material_roughness;
    float material_ao = 1.0;
    float metalness = 0.0;
    bool enhanced_mesh = (v_mesh_flags & (kMeshLit | kMeshMaterialLighting)) ==
                         (kMeshLit | kMeshMaterialLighting);
    float normal_length_squared = dot(v_world_normal, v_world_normal);
    vec3 shading_normal = normal_length_squared > 0.000001
        ? normalize(v_world_normal) : vec3(0.0, 1.0, 0.0);
    vec2 material_uv = MaterialMapUv(shading_normal, v_world_position, v_uv,
                                     kTextureMode == 0);
    if ((enhanced_mesh || (kHdrOutput && (v_mesh_flags & kMeshShipwreckWorld) != 0u)) &&
        (kTextureMode != 0 || mc.material_maps.x != 0u) &&
        (v_mesh_flags & kMeshShadow) == 0u &&
        normal_length_squared > 0.000001) {
        shading_normal = MaterialDetailNormal(v_world_normal, v_world_position, v_uv,
                                               texel.rgb, v_mesh_flags);
        if (kHdrOutput && mc.material_maps.z == kProfileWoodFloor &&
            (v_mesh_flags & kMeshWetTimber) != 0u) {
            // Increase only the authored board-grain slope. The triangle plane still
            // controls wet classification and long ray directions below.
            vec3 plane = normalize(v_world_normal);
            vec3 detail = shading_normal - plane * dot(shading_normal, plane);
            float floor_weight = smoothstep(0.30, 0.70, plane.y);
            shading_normal = normalize(plane + detail * mix(1.0, 2.0, floor_weight));
        }
    }
    if (enhanced_mesh) {
        roughness = clamp(mc.material_roughness, 0.3, 0.85);
        if (mc.material_maps.y != 0u) {
            vec3 orm = SampleMaterialMap(mc.material_maps.y, material_uv).rgb;
            material_ao = clamp(orm.r, 0.0, 1.0);
            roughness = clamp(orm.g, 0.32, 0.95);
            metalness = MaterialMetalness(orm.b, mc.material_maps.z, texel.rgb);
        }
        if ((v_mesh_flags & kMeshCharacterMatte) != 0u) roughness = max(roughness, 0.85);
        if (kHdrOutput && (v_mesh_flags & kMeshWetTimber) != 0u) {
            float upward = smoothstep(0.30, 0.70, normalize(v_world_normal).y);
            float damp_roughness = 0.72;
            if (mc.material_maps.y != 0u && mc.material_maps.z == kProfileWoodFloor) {
                // Shallow ORM variation marks raised wood fibres. Only those narrow
                // ridges approach 0.60; the troughs remain rough and absorb light.
                float grain = smoothstep(0.655, 0.702, roughness);
                damp_roughness = mix(0.60, 0.82, grain);
                roughness = mix(roughness, damp_roughness, upward);
            } else {
                roughness = mix(roughness, max(roughness, damp_roughness), upward);
            }
        }
        if (!kHdrOutput) {
            mesh_color.rgb = clamp(MaterialLighting(roughness, shading_normal) / kModulate, 0.0, 1.0);
        }
    }
    vec4 color;
    if (kTextureMode == 0) {
        color = vec4(mesh_color.rgb, mesh_color.a * kModulate);
    } else {
        color  = vec4(texel.rgb * mesh_color.rgb * kModulate, texel.a * mesh_color.a * kModulate);
    }
    if (kAlphaTest && !AlphaPasses(color.a)) discard;

    bool linear_rgb = kHdrOutput && enhanced_mesh;
    if (linear_rgb) {
        vec3 texture_albedo = kTextureMode == 0 ? vec3(1.0) :
                              SrgbToLinear(clamp(texel.rgb, 0.0, 1.0));
        color.rgb = MaterialLightingLinear(texture_albedo, roughness, shading_normal,
                                            metalness, material_ao);
    }

#ifdef RAY_QUERY_AO
    bool prelit_shipwreck = kHdrOutput && !enhanced_mesh &&
        (v_mesh_flags & (kMeshShipwreckWorld | kMeshVertexColor | kMeshShadow)) ==
        (kMeshShipwreckWorld | kMeshVertexColor) && color.a >= 0.95;
    if (prelit_shipwreck) {
        vec3 base = clamp(color.rgb, 0.0, 1.0);
        color.rgb = SrgbToLinear(base) + ShipwreckPrelitSpecular(shading_normal);
        linear_rgb = true;
    }
#endif

#ifdef RAY_QUERY_AO
    if (enhanced_mesh) {
        vec3 n = shading_normal;
        vec3 reflection_normal = n;
        if ((v_mesh_flags & kMeshWetTimber) != 0u) {
            // A high-contrast plank texel can tilt the derived micro-normal enough to move
            // a distant mirror hit across several boards. Keep its direct-light grain, but
            // anchor long reflection rays to the actual triangle plane.
            vec3 plane = cross(dFdx(v_world_position), dFdy(v_world_position));
            if (dot(plane, plane) > 0.000001) {
                plane = normalize(plane);
                if (dot(plane, v_world_normal) < 0.0) plane = -plane;
                reflection_normal = normalize(mix(plane, n, 0.12));
            }
        }
        vec3 view = normalize(mc.camera_position.xyz - v_world_position);
        vec3 f0 = (v_mesh_flags & kMeshCharacterMatte) != 0u ? vec3(0.01) :
                  clamp(mc.specular.rgb, vec3(0.04), vec3(0.10));
        if ((v_mesh_flags & kMeshWetTimber) != 0u)
            f0 = mix(f0, vec3(0.035), smoothstep(0.30, 0.70, n.y));
        vec3 metal_base = SrgbToLinear(clamp(mc.diffuse.rgb * texel.rgb, 0.0, 1.0));
        f0 = mix(f0, clamp(metal_base, vec3(0.02), vec3(0.95)), metalness);
        vec3 vertex_modulation = (v_mesh_flags & kMeshVertexColor) != 0u
            ? v_vertex_color * kModulate : vec3(1.0);
        // The map's broad unvarnished vertical timber has no authored specular response.
        // Reserve costly mirror hit queries for upward wet planks and authored shiny props.
        bool wet_face = (v_mesh_flags & kMeshWetTimber) != 0u && reflection_normal.y > 0.30;
        bool authored_specular = metalness > 0.4 ||
            ((v_mesh_flags & kMeshCharacterMatte) == 0u &&
             max(max(mc.specular.r, mc.specular.g), mc.specular.b) > 0.12);
        vec3 indirect = RayIndirectLighting(v_world_position, n, reflection_normal,
            view, roughness, f0,
            SrgbToLinear(clamp(mc.diffuse.rgb * vertex_modulation, 0.0, 1.0)) *
            DampWoodAlbedo(SrgbToLinear(clamp(texel.rgb, 0.0, 1.0)),
                           (v_mesh_flags & kMeshWetTimber) != 0u
                               ? smoothstep(0.30, 0.70, n.y) : 0.0) *
            (1.0 - metalness) * material_ao,
            wet_face || authored_specular, wet_face);
        color.rgb += indirect;
    }
#endif

    if ((pc.flags & kFog) != 0u) {
        bool enhanced_fog = (v_mesh_flags & (kMeshMaterialLighting | kMeshFog | kMeshShadow)) ==
                            (kMeshMaterialLighting | kMeshFog);
        if (enhanced_fog && (v_mesh_flags & kMeshShipwreckFog) != 0u && kHdrOutput) {
            // The display pass applies Shipwreck extinction once, from depth, to every
            // opaque world pixel. Per-material fog here would double it and expose seams.
        } else if (enhanced_fog && (v_mesh_flags & kMeshShipwreckFog) != 0u) {
            color.rgb = ShipwreckAtmosphere(color.rgb);
        } else {
            float visibility = enhanced_fog ? MaterialFogVisibility() : v_fog;
            vec3 fog_color = unpackUnorm4x8(pc.fog_color).rgb;
            color.rgb = mix(linear_rgb ? SrgbToLinear(fog_color) : fog_color,
                            color.rgb, visibility);
        }
    }

    float factor    = (pc.flags & kFactorFix) != 0u ? float((pc.alpha >> 16) & 0xFFu) / 128.0 : color.a;
    uint  transform = (pc.flags >> kTransformShift) & 3u;
    vec3  rgb       = transform == 1u ? color.rgb * (1.0 + factor) : transform == 2u ? vec3(factor) : color.rgb;
    o_color         = vec4(kHdrOutput && !linear_rgb ? SrgbToLinear(max(rgb, vec3(0.0))) : rgb,
                           (pc.flags & kOpaqueTarget) != 0u ? 1.0 : color.a);
    o_factor        = vec4(factor);
}
