#include "raytracing.hpp"
#include "platform/settings.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gfx/context.hpp"

namespace gfx::detail {
namespace {

struct Acceleration {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer buffer;
};

struct HitMeshGeometry {
    std::vector<Vertex3D> vertices;
    std::vector<uint32_t> indices;
};

struct MeshAcceleration {
    std::shared_ptr<Acceleration> blas;
    uint32_t triangles = 0;
    std::shared_ptr<HitMeshGeometry> geometry;
};

struct HitVertex {
    float position[4];
    float normal[4];
    float color[4];
    float uv[4];
};

// Matches std430 RayHitMaterial in enhanced.frag. A hit carries the original mesh's flat material
// and vertex colour, so off-screen surfaces still contribute radiance without frame history.
struct HitMaterial {
    uint32_t vertex_base;
    uint32_t index_base;
    uint32_t flags;
    uint32_t light_count;
    uint32_t primitive_base;
    uint32_t texture_slot;
    uint32_t palette_slot;
    uint32_t sampler_slot;
    uint32_t texture_mode;
    uint32_t pad[3];
    float world_uv[2][4];
    float diffuse[4];
    float ambient[4];
    float ambient_material[4];
    float light_direction[4][4];
    float light_color[4][4];
    uint32_t material_normal_slot;
    uint32_t material_orm_slot;
    uint32_t material_profile;
    uint32_t material_scene;
    float material_parameters[4]; // normal strength, fallback roughness, reserved
    float specular[4];
};

struct PendingDrawMaterial {
    uint32_t first_index;
    uint32_t index_count;
    HitMaterial material;
};

struct PendingRayInstance {
    MeshHandle handle;
    std::shared_ptr<HitMeshGeometry> geometry;
    std::vector<PendingDrawMaterial> draws;
};

struct PendingImmediate {
    size_t instance_index;
    std::shared_ptr<HitMeshGeometry> geometry;
};

struct HitPayload {
    Buffer vertices;
    Buffer indices;
    Buffer materials;
    Buffer primitive_materials;
};

// Matches the std140 local-light block at ray descriptor binding 4.
struct RayLightBlock {
    uint32_t count[4] = {};
    float position_radius[8][4] = {};
    float color_intensity[8][4] = {};
};

static_assert(sizeof(RayLightBlock) == 272);

static_assert(sizeof(HitVertex) == 64);
static_assert(offsetof(HitMaterial, material_parameters) == 272);
static_assert(offsetof(HitMaterial, specular) == 288);
static_assert(sizeof(HitMaterial) == 304);

bool g_enabled = false;
bool g_provisioned = false;
bool g_pending_enabled = false;
bool g_scene_ready = false;
bool g_reported_scene = false;
bool g_reported_bind = false;
bool g_ray_cloth = true;
bool g_reported_cloth_scene = false;
std::unordered_map<MeshHandle, MeshAcceleration> g_meshes;
std::unordered_set<MeshHandle> g_dirty_meshes;
std::vector<VkAccelerationStructureInstanceKHR> g_instances;
std::vector<PendingRayInstance> g_pending_instances;
std::vector<PendingImmediate> g_pending_immediate;
std::vector<VkAccelerationStructureInstanceKHR> g_canonical_instances;
std::vector<PendingRayInstance> g_canonical_pending;
std::vector<std::shared_ptr<Acceleration>> g_canonical_mesh_refs;
std::unordered_set<TextureHandle> g_canonical_textures;
bool g_build_display_scene = false;
std::unordered_set<TextureHandle> g_pending_textures;
std::unordered_set<TextureHandle> g_scene_textures;
std::unordered_set<TextureHandle> g_deferred_texture_destroys;

void FlushDeferredRayTextures() {
    for (auto it = g_deferred_texture_destroys.begin();
         it != g_deferred_texture_destroys.end();) {
        TextureHandle handle = *it;
        if (g_scene_textures.contains(handle) || g_pending_textures.contains(handle)) {
            ++it;
            continue;
        }
        it = g_deferred_texture_destroys.erase(it);
        DestroyDoomedTexture(handle);
    }
}
std::vector<std::shared_ptr<Acceleration>> g_pending_mesh_refs;
std::vector<std::shared_ptr<Acceleration>> g_scene_mesh_refs;
HitPayload g_scene_payload;
Acceleration g_scene;
VkDescriptorSetLayout g_descriptor_layout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptor_pool = VK_NULL_HANDLE;
VkDescriptorSet g_descriptor_sets[kFramesInFlight] = {};
Buffer g_light_buffers[kFramesInFlight] = {};
Buffer g_counter_buffers[kFramesInFlight] = {};
bool g_reported_lights = false;
bool g_profile_enabled = false;
bool g_profile_count_rays = false;
uint32_t g_ray_effects = 0x3fu;

constexpr uint32_t kProfileMaxDraws = 2048;
constexpr uint32_t kProfileTlasStart = kProfileMaxDraws + 1;
constexpr uint32_t kProfileTlasEnd = kProfileMaxDraws + 2;
constexpr uint32_t kProfileQueryCount = kProfileMaxDraws + 3;
struct RayCounters {
    uint32_t ao = 0;
    uint32_t contact = 0;
    uint32_t reflection = 0;
    uint32_t diffuse = 0;
    uint32_t local_shadow = 0;
    uint32_t cloth_hit = 0;
    uint32_t padding[2] = {};
};
struct RayProfileFrame {
    VkQueryPool pool = VK_NULL_HANDLE;
    bool active = false;
    bool pending = false;
    bool ray_started = false;
    bool tlas_built = false;
    bool display = false;
    uint32_t draws = 0;
    uint32_t instances = 0;
    uint32_t blas_updates = 0;
    double cpu_tlas_ms = 0.0;
};
RayProfileFrame g_profile[kFramesInFlight] = {};
bool g_timestamp_supported = false;
uint32_t g_timestamp_valid_bits = 0;

struct ProfileSummary {
    uint32_t frames = 0;
    uint64_t draws = 0;
    uint64_t instances = 0;
    uint64_t blas_updates = 0;
    uint64_t rays[6] = {};
    double cpu_tlas_ms = 0.0;
    double gpu_ray_ms = 0.0;
    double gpu_tlas_ms = 0.0;
};
ProfileSummary g_summary[2] = {};

double TimestampMilliseconds(uint64_t start, uint64_t end) {
    uint64_t delta = end - start;
    if (g_timestamp_valid_bits < 64) delta &= (uint64_t{1} << g_timestamp_valid_bits) - 1;
    return static_cast<double>(delta) * g.properties.limits.timestampPeriod * 1e-6;
}

void CollectProfile(uint32_t slot) {
    RayProfileFrame &frame = g_profile[slot];
    if (!frame.pending) return;
    ProfileSummary &summary = g_summary[frame.display ? 1 : 0];
    summary.frames++;
    summary.draws += frame.draws;
    summary.instances += frame.instances;
    summary.blas_updates += frame.blas_updates;
    summary.cpu_tlas_ms += frame.cpu_tlas_ms;
    const auto &rays = *reinterpret_cast<const RayCounters *>(g_counter_buffers[slot].memory.mapped);
    summary.rays[0] += rays.ao;
    summary.rays[1] += rays.contact;
    summary.rays[2] += rays.reflection;
    summary.rays[3] += rays.diffuse;
    summary.rays[4] += rays.local_shadow;
    summary.rays[5] += rays.cloth_hit;
    if (g_timestamp_supported && frame.ray_started && frame.draws != 0) {
        uint64_t start = 0, end = 0;
        uint32_t last = std::min(frame.draws, kProfileMaxDraws);
        if (vkGetQueryPoolResults(g.device, frame.pool, 0, 1, sizeof(start), &start, sizeof(start),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            vkGetQueryPoolResults(g.device, frame.pool, last, 1, sizeof(end), &end, sizeof(end),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            summary.gpu_ray_ms += TimestampMilliseconds(start, end);
        }
    }
    if (g_timestamp_supported && frame.tlas_built) {
        uint64_t start = 0, end = 0;
        if (vkGetQueryPoolResults(g.device, frame.pool, kProfileTlasStart, 1, sizeof(start), &start,
                                  sizeof(start), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            vkGetQueryPoolResults(g.device, frame.pool, kProfileTlasEnd, 1, sizeof(end), &end,
                                  sizeof(end), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            summary.gpu_tlas_ms += TimestampMilliseconds(start, end);
        }
    }
    if (summary.frames >= 60) {
        double n = static_cast<double>(summary.frames);
        std::fprintf(stderr,
                     "Ray profile %s: %.1f draws/frame, %.1f TLAS instances, %.1f BLAS updates, "
                     "CPU ray-scene prep %.3f ms, GPU ray-draw span %.3f ms, GPU TLAS %.3f ms",
                     frame.display ? "display" : "canonical", summary.draws / n,
                     summary.instances / n, summary.blas_updates / n,
                     summary.cpu_tlas_ms / n, summary.gpu_ray_ms / n, summary.gpu_tlas_ms / n);
        if (g_profile_count_rays) {
            std::fprintf(stderr, "; rays/frame AO %.0f contact %.0f refl %.0f GI %.0f local-shadow %.0f cloth-reflection-hits %.0f",
                         summary.rays[0] / n, summary.rays[1] / n, summary.rays[2] / n,
                         summary.rays[3] / n, summary.rays[4] / n, summary.rays[5] / n);
        }
        std::fputc('\n', stderr);
        summary = {};
    }
    frame.pending = false;
}
VkAccelerationStructureKHR g_described_scene[kFramesInFlight] = {};

PFN_vkCreateAccelerationStructureKHR g_create = nullptr;
PFN_vkDestroyAccelerationStructureKHR g_destroy = nullptr;
PFN_vkGetAccelerationStructureBuildSizesKHR g_sizes = nullptr;
PFN_vkCmdBuildAccelerationStructuresKHR g_build = nullptr;
PFN_vkGetAccelerationStructureDeviceAddressKHR g_address = nullptr;
VkDeviceSize g_scratch_alignment = 256;
uint32_t g_max_instances = 65535;
uint64_t g_max_primitives = UINT64_MAX;

VkDeviceAddress AlignAddress(VkDeviceAddress address) {
    return (address + g_scratch_alignment - 1) / g_scratch_alignment * g_scratch_alignment;
}

VkDeviceAddress BufferAddress(VkBuffer buffer) {
    VkBufferDeviceAddressInfo info = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(g.device, &info);
}

VkDeviceAddress AccelerationAddress(VkAccelerationStructureKHR structure) {
    VkAccelerationStructureDeviceAddressInfoKHR info = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    info.accelerationStructure = structure;
    return g_address(g.device, &info);
}

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 before_stage, VkAccessFlags2 before_access,
             VkPipelineStageFlags2 after_stage, VkAccessFlags2 after_access) {
    VkMemoryBarrier2 memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    memory.srcStageMask = before_stage;
    memory.srcAccessMask = before_access;
    memory.dstStageMask = after_stage;
    memory.dstAccessMask = after_access;
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

Acceleration CreateAcceleration(VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
    Acceleration result;
    result.buffer = CreateBuffer(size, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
    VkAccelerationStructureCreateInfoKHR info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    info.buffer = result.buffer.buffer;
    info.size = size;
    info.type = type;
    Check(g_create(g.device, &info, nullptr, &result.handle), "vkCreateAccelerationStructureKHR");
    return result;
}

void DestroyAcceleration(Acceleration &value) {
    if (value.handle != VK_NULL_HANDLE) {
        g_destroy(g.device, value.handle, nullptr);
    }
    DestroyBuffer(value.buffer);
    value = {};
}

void DestroyHitPayload(HitPayload &payload) {
    DestroyBuffer(payload.vertices);
    DestroyBuffer(payload.indices);
    DestroyBuffer(payload.materials);
    DestroyBuffer(payload.primitive_materials);
    payload = {};
}

template <typename T>
Buffer UploadHitBuffer(const std::vector<T> &data) {
    Buffer buffer = CreateBuffer(data.size() * sizeof(T), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    std::memcpy(buffer.memory.mapped, data.data(), data.size() * sizeof(T));
    return buffer;
}

void BuildMesh(MeshHandle handle, bool update) {
    Mesh *mesh = LookupMesh(handle);
    if (!g_provisioned || mesh == nullptr || mesh->index_count < 3 || mesh->vertex_count == 0) {
        return;
    }
    uint32_t triangles = mesh->index_count / 3;
    if (triangles > g_max_primitives) {
        return;
    }
    MeshAcceleration &entry = g_meshes[handle];
    VkDeviceAddress base = BufferAddress(mesh->buffer.buffer);

    VkAccelerationStructureGeometryTrianglesDataKHR vertices = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    vertices.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    vertices.vertexData.deviceAddress = base;
    vertices.vertexStride = sizeof(Vertex3D);
    vertices.maxVertex = mesh->vertex_count - 1;
    vertices.indexType = VK_INDEX_TYPE_UINT32;
    vertices.indexData.deviceAddress = base + mesh->vertex_count * sizeof(Vertex3D);

    VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles = vertices;
    VkAccelerationStructureBuildGeometryInfoKHR info = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                 VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    info.mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                       : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1;
    info.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes = {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    g_sizes(g.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &triangles, &sizes);
    if (!update) {
        entry.blas = std::shared_ptr<Acceleration>(
            new Acceleration(CreateAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                sizes.accelerationStructureSize)),
            [](Acceleration *acceleration) {
                DestroyAcceleration(*acceleration);
                delete acceleration;
            });
        entry.triangles = triangles;
    }
    info.srcAccelerationStructure = update ? entry.blas->handle : VK_NULL_HANDLE;
    info.dstAccelerationStructure = entry.blas->handle;
    Buffer scratch = CreateBuffer((update ? sizes.updateScratchSize : sizes.buildScratchSize) +
                                      g_scratch_alignment - 1,
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                  false);
    info.scratchData.deviceAddress = AlignAddress(BufferAddress(scratch.buffer));
    VkAccelerationStructureBuildRangeInfoKHR range = {};
    range.primitiveCount = triangles;
    const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
    VkCommandBuffer cmd = CommandsForWrite(mesh->last_draw_use);
    Barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    g_build(cmd, 1, &info, ranges);
    Barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    DeferDestroy([scratch, reference = entry.blas]() mutable {
        DestroyBuffer(scratch);
        reference.reset();
    });
}

} // namespace

bool RayDeferTextureDestroy(TextureHandle handle) {
    if (!g_scene_textures.contains(handle) && !g_pending_textures.contains(handle)) return false;
    g_deferred_texture_destroys.insert(handle);
    return true;
}

bool RayTracingSupported(VkPhysicalDevice physical_device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, extensions.data());
    auto has = [&](const char *name) {
        return std::any_of(extensions.begin(), extensions.end(), [name](const auto &e) {
            return std::strcmp(e.extensionName, name) == 0;
        });
    };
    if (!has(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME) ||
        !has(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !has(VK_KHR_RAY_QUERY_EXTENSION_NAME)) {
        return false;
    }
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    VkPhysicalDeviceVulkan12Features features12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 features = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    features12.pNext = &acceleration;
    acceleration.pNext = &query;
    vkGetPhysicalDeviceFeatures2(physical_device, &features);
    VkFormatProperties format = {};
    vkGetPhysicalDeviceFormatProperties(physical_device, VK_FORMAT_R32G32B32_SFLOAT, &format);
    return features.features.fragmentStoresAndAtomics && features12.bufferDeviceAddress &&
           features12.shaderSampledImageArrayNonUniformIndexing &&
           acceleration.accelerationStructure && query.rayQuery &&
           (format.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR);
}

void SetRayTracingProvisioned(bool available) { g_provisioned = available; }
bool RayTracingProvisioned() { return g_provisioned; }
void SetRayTracingEnabled(bool enabled) {
    g_enabled = g_provisioned && enabled;
    g_pending_enabled = g_enabled;
}
bool RayTracingEnabled() { return g_enabled; }
void QueueRayTracing(bool enabled) { g_pending_enabled = enabled; }
void ApplyPendingRayTracing() {
    bool enabled = g_provisioned && g_pending_enabled;
    if (enabled == g_enabled) return;
    g_enabled = enabled;
    // A disabled interval leaves the last TLAS stale. Re-use the acceleration resources, but
    // wait for a new canonical scene before choosing the ray-enabled display pipeline.
    g_scene_ready = false;
    if (!g_enabled) {
        g_scene_textures.clear();
        g_pending_textures.clear();
        g_canonical_instances.clear();
        g_canonical_pending.clear();
        g_canonical_mesh_refs.clear();
        g_canonical_textures.clear();
        g_pending_immediate.clear();
        FlushDeferredRayTextures();
    }
    std::fprintf(stderr, "Vulkan: ray queries %s at frame boundary\n",
                 g_enabled ? "enabled" : "disabled");
}
bool RayProfileCountersEnabled() { return g_profile_count_rays; }

void InitRayTracing() {
    if (!g_provisioned) {
        return;
    }
    const char *profile_env = SettingsEnv("RAY_PROFILE");
    g_profile_count_rays = profile_env != nullptr && std::strcmp(profile_env, "1") == 0;
    g_profile_enabled = g_profile_count_rays ||
                        (profile_env != nullptr && std::strcmp(profile_env, "timing") == 0);
    g_ray_effects = 0x3fu;
    if (const char *effect_env = SettingsEnv("RAY_EFFECTS")) {
        char *end = nullptr;
        unsigned long parsed = std::strtoul(effect_env, &end, 0);
        if (end != effect_env && *end == '\0') g_ray_effects = static_cast<uint32_t>(parsed) & 0x3fu;
    }
    g_reported_scene = false;
    g_reported_bind = false;
    g_reported_lights = false;
    g_reported_cloth_scene = false;
    const char *cloth_env = SettingsEnv("RAY_CLOTH");
    g_ray_cloth = cloth_env == nullptr || std::strcmp(cloth_env, "0") != 0;
    auto load = [](const char *name) { return vkGetDeviceProcAddr(g.device, name); };
    g_create = reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
    g_destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
    g_sizes = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
        load("vkGetAccelerationStructureBuildSizesKHR"));
    g_build = reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(
        load("vkCmdBuildAccelerationStructuresKHR"));
    g_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        load("vkGetAccelerationStructureDeviceAddressKHR"));
    if (!g_create || !g_destroy || !g_sizes || !g_build || !g_address) {
        Fatal("ray tracing extensions are enabled but an acceleration structure entry point is missing");
    }
    VkPhysicalDeviceAccelerationStructurePropertiesKHR properties = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 device_properties = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    device_properties.pNext = &properties;
    vkGetPhysicalDeviceProperties2(g.physical_device, &device_properties);
    g_scratch_alignment = std::max<VkDeviceSize>(properties.minAccelerationStructureScratchOffsetAlignment, 1);
    g_max_instances = static_cast<uint32_t>(std::min<uint64_t>(properties.maxInstanceCount, 65535));
    g_max_primitives = properties.maxPrimitiveCount;
    VkDescriptorSetLayoutBinding bindings[7] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 1; i < 4; i++) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[5].binding = 5;
    bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[5].descriptorCount = 1;
    bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[6].binding = 6;
    bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[6].descriptorCount = 1;
    bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout.bindingCount = 7;
    layout.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &layout, nullptr, &g_descriptor_layout),
          "vkCreateDescriptorSetLayout(ray scene)");
    VkDescriptorPoolSize sizes[3] = {
        {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * kFramesInFlight},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
    };
    VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 3;
    pool.pPoolSizes = sizes;
    Check(vkCreateDescriptorPool(g.device, &pool, nullptr, &g_descriptor_pool),
          "vkCreateDescriptorPool(ray scene)");
    VkDescriptorSetLayout layouts[kFramesInFlight];
    std::fill(std::begin(layouts), std::end(layouts), g_descriptor_layout);
    VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = g_descriptor_pool;
    allocate.descriptorSetCount = kFramesInFlight;
    allocate.pSetLayouts = layouts;
    Check(vkAllocateDescriptorSets(g.device, &allocate, g_descriptor_sets),
          "vkAllocateDescriptorSets(ray scene)");
    for (Buffer &buffer : g_light_buffers) {
        buffer = CreateBuffer(sizeof(RayLightBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    }
    for (Buffer &buffer : g_counter_buffers) {
        buffer = CreateBuffer(sizeof(RayCounters), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        std::memset(buffer.memory.mapped, 0, sizeof(RayCounters));
    }
    if (g_profile_enabled) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, families.data());
        g_timestamp_supported = g.queue_family < family_count &&
                                families[g.queue_family].timestampValidBits != 0;
        g_timestamp_valid_bits = g_timestamp_supported ?
                                 families[g.queue_family].timestampValidBits : 0;
        if (g_timestamp_supported) {
            VkQueryPoolCreateInfo query = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = kProfileQueryCount;
            for (RayProfileFrame &frame : g_profile) {
                Check(vkCreateQueryPool(g.device, &query, nullptr, &frame.pool),
                      "vkCreateQueryPool(ray profile)");
            }
        }
        std::fprintf(stderr, "Ray profile enabled (%s; GPU timestamps %s)\n",
                     g_profile_count_rays ? "ray counters and timing" : "timing only",
                     g_timestamp_supported ? "available" : "unavailable");
    }
    std::fprintf(stderr, "Vulkan: ray queries %s (effects mask 0x%02x)\n",
                 g_enabled ? "enabled" : "provisioned, currently disabled", g_ray_effects);
}

void ShutdownRayTracing() {
    DestroyAcceleration(g_scene);
    DestroyHitPayload(g_scene_payload);
    g_scene_mesh_refs.clear();
    g_pending_mesh_refs.clear();
    g_pending_instances.clear();
    g_pending_immediate.clear();
    g_canonical_instances.clear();
    g_canonical_pending.clear();
    g_canonical_mesh_refs.clear();
    g_canonical_textures.clear();
    g_pending_textures.clear();
    g_scene_textures.clear();
    g_deferred_texture_destroys.clear();
    g_meshes.clear();
    g_dirty_meshes.clear();
    for (Buffer &buffer : g_light_buffers) DestroyBuffer(buffer);
    for (Buffer &buffer : g_counter_buffers) DestroyBuffer(buffer);
    for (RayProfileFrame &frame : g_profile) {
        if (frame.pool != VK_NULL_HANDLE) vkDestroyQueryPool(g.device, frame.pool, nullptr);
        frame = {};
    }
    g_summary[0] = {};
    g_summary[1] = {};
    g_profile_enabled = false;
    g_profile_count_rays = false;
    g_ray_effects = 0x3fu;
    g_timestamp_supported = false;
    g_timestamp_valid_bits = 0;
    if (g_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g.device, g_descriptor_pool, nullptr);
        g_descriptor_pool = VK_NULL_HANDLE;
    }
    if (g_descriptor_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(g.device, g_descriptor_layout, nullptr);
        g_descriptor_layout = VK_NULL_HANDLE;
    }
    std::fill(std::begin(g_descriptor_sets), std::end(g_descriptor_sets), VK_NULL_HANDLE);
    std::fill(std::begin(g_described_scene), std::end(g_described_scene), VK_NULL_HANDLE);
    g_instances.clear();
    g_enabled = false;
    g_provisioned = false;
    g_pending_enabled = false;
    g_scene_ready = false;
    g_reported_scene = false;
    g_reported_bind = false;
    g_reported_cloth_scene = false;
    g_ray_cloth = true;
}

VkBufferUsageFlags RayMeshBufferUsage() {
    return g_provisioned ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0;
}

void RayMeshCreated(MeshHandle handle, std::span<const Vertex3D> vertices,
                    std::span<const uint32_t> indices) {
    if (!g_provisioned) {
        return;
    }
    BuildMesh(handle, false);
    auto it = g_meshes.find(handle);
    if (it != g_meshes.end()) {
        it->second.geometry = std::make_shared<HitMeshGeometry>();
        it->second.geometry->vertices.assign(vertices.begin(), vertices.end());
        it->second.geometry->indices.assign(indices.begin(), indices.end());
    }
}
void RayMeshUpdated(MeshHandle handle, uint32_t first, std::span<const Vertex3D> vertices) {
    if (g_provisioned && g_meshes.contains(handle)) {
        auto &cache = g_meshes.at(handle).geometry->vertices;
        std::copy(vertices.begin(), vertices.end(), cache.begin() + first);
        g_dirty_meshes.insert(handle);
    }
}

void RayMeshDestroyed(MeshHandle handle) {
    // TLAS instances recorded earlier this frame or in the previous scene retain shared BLAS
    // references until those scenes have finished on the GPU.
    g_meshes.erase(handle);
    g_dirty_meshes.erase(handle);
}

HitMaterial MakeHitMaterial(const MeshConstants &constants, const TextureBinding &binding,
                            const DrawState &state) {
    HitMaterial material = {};
    material.flags = constants.flags;
    material.material_profile = constants.material_profile;
    material.material_scene = constants.material_pad;
    material.material_parameters[0] = constants.normal_detail_strength;
    material.material_parameters[1] = constants.material_roughness;
    std::memcpy(material.specular, constants.specular, sizeof(material.specular));
    // MeshConstants stores bindless slots. Resolve each live slot to its full handle so the
    // texture remains alive for secondary hits, including after the host changes its material.
    auto track_material_slot = [](uint32_t slot) -> uint32_t {
        if (slot < kFirstUserSlot || slot >= g.textures.size()) return 0;
        const Texture &texture = g.textures[slot];
        if (!texture.live) return 0;
        const TextureHandle handle = (static_cast<uint32_t>(texture.generation) << 16) | slot;
        if (LookupTexture(handle) == nullptr) return 0;
        g_pending_textures.insert(handle);
        return slot;
    };
    material.material_normal_slot = track_material_slot(constants.material_normal_slot);
    material.material_orm_slot = track_material_slot(constants.material_orm_slot);
    const bool needs_alpha_test = state.alpha_test && state.alpha_func != AlphaFunc::Always;
    if (needs_alpha_test) {
        material.pad[1] = 1u | (static_cast<uint32_t>(state.alpha_func) << 1u) |
                          (static_cast<uint32_t>(state.alpha_ref) << 8u);
    }
    if (binding.texture != kNullTexture && binding.texture != kPreviousFrame) {
        if (const Texture *texture = LookupTexture(binding.texture)) {
            material.texture_slot = binding.texture & 0xffffu;
            material.texture_mode = texture->desc.format == TextureFormat::Index8 ? 2u : 1u;
            g_pending_textures.insert(binding.texture);
            if (material.texture_mode == 2u && LookupTexture(binding.palette) != nullptr) {
                material.palette_slot = binding.palette & 0xffffu;
                g_pending_textures.insert(binding.palette);
            } else if (material.texture_mode == 2u) {
                material.texture_mode = 0u;
            }
            Filter filter = material.texture_mode == 2u ? Filter::Nearest : binding.filter;
            material.pad[0] = binding.filter == Filter::Linear ? 1u : 0u;
            const bool texa = material.texture_mode == 1u &&
                              (!texture->desc.has_alpha || binding.ignore_alpha);
            if (texa) {
                material.pad[2] = 1u | (state.texa_aem ? 2u : 0u) |
                                  (static_cast<uint32_t>(state.texa_ta0) << 8u);
            }
            material.sampler_slot = SamplerIndex(filter, binding.wrap_u, binding.wrap_v);
            if (material.texture_mode == 1u && filter == Filter::Linear &&
                g.anisotropy_level > 0) {
                material.sampler_slot = kBaseSamplerCount +
                    (g.anisotropy_level - 1) * 4 + (material.sampler_slot & 3u);
            }
        }
    }
    std::memcpy(material.world_uv, constants.world_uv, sizeof(material.world_uv));
    material.light_count = std::min(constants.light_count, 4u);
    std::memcpy(material.diffuse, constants.diffuse, sizeof(material.diffuse));
    std::memcpy(material.ambient, constants.ambient, sizeof(material.ambient));
    std::memcpy(material.ambient_material, constants.ambient_material,
                sizeof(material.ambient_material));
    std::memcpy(material.light_direction, constants.light_direction, sizeof(material.light_direction));
    std::memcpy(material.light_color, constants.light_color, sizeof(material.light_color));
    return material;
}

void RayRecordMesh(MeshHandle handle, uint32_t first_index, uint32_t index_count,
                   const MeshConstants &constants, const TextureBinding &binding,
                   const DrawState &state) {
    if (!g_enabled || g.display_pass || (constants.flags & kMeshShadow) ||
        index_count < 3) {
        return;
    }
    auto it = g_meshes.find(handle);
    if (it == g_meshes.end()) {
        return;
    }
    VkAccelerationStructureInstanceKHR instance = {};
    const float *matrix = constants.model_to_world;
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t col = 0; col < 4; col++) {
            instance.transform.matrix[row][col] = matrix[col * 4 + row];
        }
    }
    size_t instance_index = g_instances.size();
    for (size_t i = 0; i < g_pending_instances.size(); ++i) {
        if (g_pending_instances[i].handle == handle &&
            std::memcmp(g_instances[i].transform.matrix, instance.transform.matrix,
                        sizeof(instance.transform.matrix)) == 0) {
            instance_index = i;
            break;
        }
    }
    if (instance_index == g_instances.size()) {
        if (g_instances.size() >= g_max_instances) return;
        instance.mask = 0xff;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference = AccelerationAddress(it->second.blas->handle);
        g_instances.push_back(instance);
        g_pending_mesh_refs.push_back(it->second.blas);
        g_pending_instances.push_back({handle, it->second.geometry, {}});
    }
    HitMaterial material = MakeHitMaterial(constants, binding, state);
    if ((material.pad[1] & 1u) != 0u)
        g_instances[instance_index].flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    g_pending_instances[instance_index].draws.push_back({first_index, index_count, material});
}

bool RayBeginDisplayScene() {
    if (!g_enabled || !g_ray_cloth || !g.display_pass || !g_scene_ready ||
        g_canonical_instances.empty())
        return false;
    g_pending_immediate.clear();
    return true;
}

void RayRecordDisplayImmediate(std::span<const Vertex3D> vertices,
                               std::span<const uint32_t> indices,
                               const MeshConstants &constants, const TextureBinding &binding,
                               const DrawState &state) {
    // Deformed matte meshes are emitted as immediate strips. Other transient effects stay raster-only.
    if (!g_enabled || !g.display_pass ||
        (constants.flags & kMeshMatte) == 0 ||
        (constants.flags & kMeshShadow) != 0 ||
        vertices.empty() || indices.size() < 3 ||
        indices.size() / 3 > g_max_primitives ||
        g_canonical_instances.size() + g_pending_immediate.size() >= g_max_instances) return;
    if (g_pending_immediate.empty()) {
        g_instances = g_canonical_instances;
        g_pending_instances = g_canonical_pending;
        g_pending_mesh_refs = g_canonical_mesh_refs;
        g_pending_textures = g_canonical_textures;
    }
    auto geometry = std::make_shared<HitMeshGeometry>();
    geometry->vertices.assign(vertices.begin(), vertices.end());
    geometry->indices.assign(indices.begin(), indices.end());
    VkAccelerationStructureInstanceKHR instance = {};
    for (uint32_t row = 0; row < 3; row++) {
        for (uint32_t col = 0; col < 4; col++) {
            instance.transform.matrix[row][col] = constants.model_to_world[col * 4 + row];
        }
    }
    instance.mask = 0xff;
    instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    HitMaterial material = MakeHitMaterial(constants, binding, state);
    if ((material.pad[1] & 1u) != 0u)
        instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
    const size_t instance_index = g_instances.size();
    g_instances.push_back(instance);
    g_pending_instances.push_back({kNullMesh, geometry,
                                   {{0u, static_cast<uint32_t>(indices.size()), material}}});
    g_pending_immediate.push_back({instance_index, geometry});
}

void BuildDisplayImmediate(VkCommandBuffer cmd) {
    for (const PendingImmediate &pending : g_pending_immediate) {
        const HitMeshGeometry &mesh = *pending.geometry;
        VkDeviceSize vertex_bytes = mesh.vertices.size() * sizeof(Vertex3D);
        VkDeviceSize index_bytes = mesh.indices.size() * sizeof(uint32_t);
        Buffer input = CreateBuffer(vertex_bytes + index_bytes,
                                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                    true);
        std::memcpy(input.memory.mapped, mesh.vertices.data(), vertex_bytes);
        std::memcpy(static_cast<uint8_t *>(input.memory.mapped) + vertex_bytes,
                    mesh.indices.data(), index_bytes);
        VkDeviceAddress address = BufferAddress(input.buffer);
        VkAccelerationStructureGeometryTrianglesDataKHR triangles = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
        triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        triangles.vertexData.deviceAddress = address;
        triangles.vertexStride = sizeof(Vertex3D);
        triangles.maxVertex = static_cast<uint32_t>(mesh.vertices.size() - 1);
        triangles.indexType = VK_INDEX_TYPE_UINT32;
        triangles.indexData.deviceAddress = address + vertex_bytes;
        VkAccelerationStructureGeometryKHR geometry = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = triangles;
        VkAccelerationStructureBuildGeometryInfoKHR info = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        uint32_t count = static_cast<uint32_t>(mesh.indices.size() / 3);
        VkAccelerationStructureBuildSizesInfoKHR sizes = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        g_sizes(g.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
        auto acceleration = std::shared_ptr<Acceleration>(
            new Acceleration(CreateAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                                sizes.accelerationStructureSize)),
            [](Acceleration *value) { DestroyAcceleration(*value); delete value; });
        Buffer scratch = CreateBuffer(sizes.buildScratchSize + g_scratch_alignment - 1,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
        info.dstAccelerationStructure = acceleration->handle;
        info.scratchData.deviceAddress = AlignAddress(BufferAddress(scratch.buffer));
        VkAccelerationStructureBuildRangeInfoKHR range = {};
        range.primitiveCount = count;
        const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
        Barrier(cmd, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        g_build(cmd, 1, &info, ranges);
        g_instances[pending.instance_index].accelerationStructureReference =
            AccelerationAddress(acceleration->handle);
        g_pending_mesh_refs.push_back(std::move(acceleration));
        DeferDestroy([input, scratch]() mutable {
            DestroyBuffer(input);
            DestroyBuffer(scratch);
        });
    }
    g_pending_immediate.clear();
}

void RayFinishDisplayScene(VkCommandBuffer cmd) {
    if (!g_enabled || !g.display_pass || g_pending_immediate.empty()) return;
    g_build_display_scene = true;
    FinishRayScene(cmd);
    g_build_display_scene = false;
}

void FinishRayScene(VkCommandBuffer cmd) {
    if (!g_enabled) {
        return;
    }
    RayProfileFrame &profile = g_profile[g.frame_slot];
    if (g_profile_enabled && profile.active) {
        profile.display = g.display_pass;
        profile.pending = true;
    }
    if (g.display_pass && !g_build_display_scene) return;
    const auto cpu_begin = std::chrono::steady_clock::now();
    const uint32_t blas_updates = g_build_display_scene
        ? static_cast<uint32_t>(g_pending_immediate.size())
        : static_cast<uint32_t>(g_dirty_meshes.size());
    // Animated meshes may receive several partial vertex uploads in a tick. Refit once, after
    // all uploads and draws, for the scene consumed by the following frame.
    if (g_build_display_scene) {
        BuildDisplayImmediate(cmd);
    } else {
        for (MeshHandle handle : g_dirty_meshes) BuildMesh(handle, true);
        g_dirty_meshes.clear();
    }
    Acceleration previous = g_scene;
    HitPayload previous_payload = g_scene_payload;
    auto previous_mesh_refs = std::move(g_scene_mesh_refs);
    g_scene_mesh_refs = std::move(g_pending_mesh_refs);
    g_scene = {};
    g_scene_payload = {};
    if (!g_instances.empty()) {
        std::vector<HitVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<HitMaterial> materials;
        std::vector<uint32_t> primitive_materials;
        std::unordered_map<const HitMeshGeometry *, std::pair<uint32_t, uint32_t>> offsets;
        vertices.reserve(65536);
        indices.reserve(65536);
        for (size_t draw = 0; draw < g_pending_instances.size(); draw++) {
            const PendingRayInstance &pending = g_pending_instances[draw];
            const HitMeshGeometry &geometry = *pending.geometry;
            auto [it, inserted] = offsets.try_emplace(pending.geometry.get());
            if (inserted) {
                it->second = {static_cast<uint32_t>(vertices.size()),
                              static_cast<uint32_t>(indices.size())};
                for (const Vertex3D &source : geometry.vertices) {
                    HitVertex vertex = {};
                    std::copy_n(source.position, 3, vertex.position);
                    std::copy_n(source.normal, 3, vertex.normal);
                    std::copy_n(source.uv, 2, vertex.uv);
                    for (uint32_t c = 0; c < 4; c++) {
                        vertex.color[c] = static_cast<float>(source.color[c]) / 128.0f;
                    }
                    vertices.push_back(vertex);
                }
                indices.insert(indices.end(), geometry.indices.begin(), geometry.indices.end());
            }
            if (pending.draws.empty()) continue;
            const uint32_t first_material = static_cast<uint32_t>(materials.size());
            const uint32_t primitive_base = static_cast<uint32_t>(primitive_materials.size());
            const uint32_t triangle_count = static_cast<uint32_t>(geometry.indices.size() / 3);
            primitive_materials.resize(primitive_materials.size() + triangle_count, 0xffffffffu);
            for (const PendingDrawMaterial &draw_material : pending.draws) {
                HitMaterial material = draw_material.material;
                material.vertex_base = it->second.first;
                material.index_base = it->second.second;
                material.primitive_base = primitive_base;
                const uint32_t material_index = static_cast<uint32_t>(materials.size());
                materials.push_back(material);
                // Draw ranges are index ranges into the same BLAS geometry. Later draws win
                // when the game's strips intentionally overlap; each ray sees the material
                // used by the raster draw that owns its triangle.
                const uint32_t first_triangle = std::min(draw_material.first_index / 3, triangle_count);
                const uint32_t end_triangle = std::min((draw_material.first_index +
                                                        draw_material.index_count) / 3, triangle_count);
                std::fill(primitive_materials.begin() + primitive_base + first_triangle,
                          primitive_materials.begin() + primitive_base + end_triangle,
                          material_index);
            }
            if (std::find(primitive_materials.begin() + primitive_base,
                          primitive_materials.begin() + primitive_base + triangle_count,
                          0xffffffffu) != primitive_materials.begin() + primitive_base + triangle_count) {
                // Some meshes are drawn as selected strips. Unsubmitted triangles remain in the
                // whole-mesh BLAS but must not cast shadows or appear in secondary hits.
                g_instances[draw].flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
            }
            g_instances[draw].instanceCustomIndex = first_material;
        }
        VkPhysicalDeviceProperties device_properties = {};
        vkGetPhysicalDeviceProperties(g.physical_device, &device_properties);
        const VkDeviceSize max_range = device_properties.limits.maxStorageBufferRange;
        if (vertices.size() * sizeof(HitVertex) > max_range ||
            indices.size() * sizeof(uint32_t) > max_range ||
            materials.size() * sizeof(HitMaterial) > max_range ||
            primitive_materials.size() * sizeof(uint32_t) > max_range) {
            std::fprintf(stderr, "Vulkan: ray scene hit payload exceeds storage buffer range; skipping scene\n");
            g_instances.clear();
            g_scene_mesh_refs.clear();
        } else {
            g_scene_payload.vertices = UploadHitBuffer(vertices);
            g_scene_payload.indices = UploadHitBuffer(indices);
            g_scene_payload.materials = UploadHitBuffer(materials);
            g_scene_payload.primitive_materials = UploadHitBuffer(primitive_materials);
        }
    }
    if (!g_instances.empty()) {
        Buffer instances = CreateBuffer(g_instances.size() * sizeof(VkAccelerationStructureInstanceKHR),
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
                                        true);
        std::memcpy(instances.memory.mapped, g_instances.data(),
                    g_instances.size() * sizeof(VkAccelerationStructureInstanceKHR));
        VkAccelerationStructureGeometryInstancesDataKHR data = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        data.data.deviceAddress = BufferAddress(instances.buffer);
        VkAccelerationStructureGeometryKHR geometry = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = data;
        VkAccelerationStructureBuildGeometryInfoKHR info = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1;
        info.pGeometries = &geometry;
        uint32_t count = static_cast<uint32_t>(g_instances.size());
        VkAccelerationStructureBuildSizesInfoKHR sizes = {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        g_sizes(g.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes);
        g_scene = CreateAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                     sizes.accelerationStructureSize);
        Buffer scratch = CreateBuffer(sizes.buildScratchSize + g_scratch_alignment - 1,
                                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                      false);
        info.dstAccelerationStructure = g_scene.handle;
        info.scratchData.deviceAddress = AlignAddress(BufferAddress(scratch.buffer));
        VkAccelerationStructureBuildRangeInfoKHR range = {};
        range.primitiveCount = count;
        const VkAccelerationStructureBuildRangeInfoKHR *ranges[] = {&range};
        Barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
        if (g_profile_enabled && profile.active && g_timestamp_supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                 profile.pool, kProfileTlasStart);
        }
        g_build(cmd, 1, &info, ranges);
        Barrier(cmd, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR);
        if (g_profile_enabled && profile.active) {
            profile.tlas_built = true;
            profile.instances = count;
            if (g_timestamp_supported) {
                vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                     profile.pool, kProfileTlasEnd);
            }
        }
        if (!g_reported_scene) {
            std::fprintf(stderr, "Vulkan: ray scene TLAS built with %u mesh instances, %zu hit vertices\n",
                         count, static_cast<size_t>(g_scene_payload.vertices.size / sizeof(HitVertex)));
            g_reported_scene = true;
        }
        if (g_build_display_scene && !g_reported_cloth_scene) {
            std::fprintf(stderr,
                         "Vulkan: display ray scene contains %zu current-pose cloth BLAS instances\n",
                         static_cast<size_t>(blas_updates));
            g_reported_cloth_scene = true;
        }
        DeferDestroy([instances, scratch]() mutable {
            DestroyBuffer(instances);
            DestroyBuffer(scratch);
        });
    }
    if (previous.handle != VK_NULL_HANDLE || !previous_mesh_refs.empty() ||
        previous_payload.vertices.buffer != VK_NULL_HANDLE) {
        DeferDestroy([previous, previous_payload, previous_mesh_refs = std::move(previous_mesh_refs)]() mutable {
            DestroyAcceleration(previous);
            DestroyHitPayload(previous_payload);
            previous_mesh_refs.clear();
        });
    }
    std::fill(std::begin(g_described_scene), std::end(g_described_scene), VK_NULL_HANDLE);
    g_scene_ready = g_scene.handle != VK_NULL_HANDLE;
    if (g_profile_enabled && profile.active) {
        profile.blas_updates = blas_updates;
        profile.cpu_tlas_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpu_begin).count();
    }
    g_scene_textures = std::move(g_pending_textures);
    g_pending_textures.clear();
    FlushDeferredRayTextures();
    if (!g_build_display_scene) {
        g_canonical_instances = g_instances;
        g_canonical_pending = g_pending_instances;
        g_canonical_mesh_refs = g_scene_mesh_refs;
        g_canonical_textures = g_scene_textures;
    }
    g_pending_immediate.clear();
    g_instances.clear();
    g_pending_instances.clear();
}

VkAccelerationStructureKHR RayScene() {
    return g_enabled && g_scene_ready ? g_scene.handle : VK_NULL_HANDLE;
}

VkDescriptorSetLayout RayDescriptorSetLayout() { return g_descriptor_layout; }

void RayProfileBeginFrame(VkCommandBuffer cmd) {
    if (!g_enabled || !g_profile_enabled) return;
    RayProfileFrame &profile = g_profile[g.frame_slot];
    CollectProfile(g.frame_slot);
    if (g_timestamp_supported) {
        vkCmdResetQueryPool(cmd, profile.pool, 0, kProfileQueryCount);
    }
    std::memset(g_counter_buffers[g.frame_slot].memory.mapped, 0, sizeof(RayCounters));
    VkQueryPool pool = profile.pool;
    profile = {};
    profile.pool = pool;
    profile.active = true;
}

void RayNoteMeshDraw(VkCommandBuffer cmd) {
    if (!g_enabled || !g_profile_enabled || g_scene.handle == VK_NULL_HANDLE) return;
    RayProfileFrame &profile = g_profile[g.frame_slot];
    if (!profile.active || !profile.ray_started) return;
    ++profile.draws;
    if (g_timestamp_supported && profile.draws <= kProfileMaxDraws) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             profile.pool, profile.draws);
    }
}

void RayBindScene(VkCommandBuffer cmd) {
    if (!g_enabled || g_scene.handle == VK_NULL_HANDLE) {
        return;
    }
    uint32_t slot = g.frame_slot;
    RayLightBlock lights;
    lights.count[0] = static_cast<uint32_t>(std::min<size_t>(g.local_lights.size(), 8));
    for (uint32_t i = 0; i < lights.count[0]; ++i) {
        std::copy_n(g.local_lights[i].position_radius, 4, lights.position_radius[i]);
        std::copy_n(g.local_lights[i].color_intensity, 4, lights.color_intensity[i]);
    }
    lights.count[1] = g_ray_effects;
    // Dense output pixels carry the same low-energy AO with one stable ray; this halves the
    // dominant query category at 1440p and 4K without a temporally varying checker pattern.
    lights.count[2] = static_cast<uint64_t>(g.display_color.width) * g.display_color.height >=
                              2'000'000u ? 1u : 0u;
    lights.count[3] = g_profile_count_rays ? 1u : 0u;
    std::memcpy(g_light_buffers[slot].memory.mapped, &lights, sizeof(lights));
    RayProfileFrame &profile = g_profile[slot];
    if (g_profile_enabled && profile.active && !profile.ray_started) {
        profile.ray_started = true;
        if (g_timestamp_supported) {
            vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, profile.pool, 0);
        }
    }
    if (!g_reported_lights && lights.count[0] != 0) {
        std::fprintf(stderr, "Vulkan: ray mesh local-light UBO bound with %u lights\n",
                     lights.count[0]);
        g_reported_lights = true;
    }
    if (g_described_scene[slot] != g_scene.handle) {
        VkWriteDescriptorSetAccelerationStructureKHR acceleration = {
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        acceleration.accelerationStructureCount = 1;
        acceleration.pAccelerationStructures = &g_scene.handle;
        VkDescriptorBufferInfo buffers[4] = {
            {g_scene_payload.vertices.buffer, 0, VK_WHOLE_SIZE},
            {g_scene_payload.indices.buffer, 0, VK_WHOLE_SIZE},
            {g_scene_payload.materials.buffer, 0, VK_WHOLE_SIZE},
            {g_scene_payload.primitive_materials.buffer, 0, VK_WHOLE_SIZE},
        };
        VkDescriptorBufferInfo light_buffer = {g_light_buffers[slot].buffer, 0,
                                               sizeof(RayLightBlock)};
        VkDescriptorBufferInfo counter_buffer = {g_counter_buffers[slot].buffer, 0,
                                                 sizeof(RayCounters)};
        VkWriteDescriptorSet writes[7] = {};
        for (uint32_t binding = 0; binding < 4; binding++) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = g_descriptor_sets[slot];
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            if (binding == 0) {
                writes[binding].pNext = &acceleration;
            } else {
                writes[binding].pBufferInfo = &buffers[binding - 1];
            }
        }
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = g_descriptor_sets[slot];
        writes[4].dstBinding = 4;
        writes[4].descriptorCount = 1;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].pBufferInfo = &light_buffer;
        writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[5].dstSet = g_descriptor_sets[slot];
        writes[5].dstBinding = 5;
        writes[5].descriptorCount = 1;
        writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[5].pBufferInfo = &counter_buffer;
        writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6].dstSet = g_descriptor_sets[slot];
        writes[6].dstBinding = 6;
        writes[6].descriptorCount = 1;
        writes[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[6].pBufferInfo = &buffers[3];
        vkUpdateDescriptorSets(g.device, 7, writes, 0, nullptr);
        g_described_scene[slot] = g_scene.handle;
    }
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.ray_pipeline_layout, 3, 1,
                            &g_descriptor_sets[slot], 0, nullptr);
    if (!g_reported_bind) {
        std::fprintf(stderr, "Vulkan: ray query mesh pipeline bound with TLAS descriptor\n");
        g_reported_bind = true;
    }
}

uint32_t RayPipelineIndex(uint32_t base_index) {
    const uint32_t mesh_blended = g.blend_slot_count * kTextureModeCount * 2;
    const uint32_t families = g.config.prog0_meshes ? kFamilyCount : kFamilyMesh + 1;
    const uint32_t all_blended = families * mesh_blended;
    const uint32_t mesh_no_color = kTextureModeCount * 2;
    const uint32_t base_total = all_blended + (g.dynamic_color_write_mask ? 0 : families * mesh_no_color);
    if (base_index < all_blended) {
        return base_total + base_index - kFamilyMesh * mesh_blended;
    }
    return base_total + mesh_blended + base_index - all_blended - kFamilyMesh * mesh_no_color;
}

} // namespace gfx::detail
