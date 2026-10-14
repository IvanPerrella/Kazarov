#pragma once

#include "gfx/requirements.hpp"
#include "gfx/gfx.hpp"

namespace gfx::detail {

// Capability is provisioned on supported devices so the effect can be switched at frame
// boundaries without rebuilding the Vulkan device or pipeline layout.
bool RayTracingSupported(VkPhysicalDevice physical_device);
void SetRayTracingEnabled(bool enabled);
bool RayTracingEnabled();
void SetRayTracingProvisioned(bool available);
bool RayTracingProvisioned();
void QueueRayTracing(bool enabled);
void ApplyPendingRayTracing();
bool RayProfileCountersEnabled();
void InitRayTracing();
void ShutdownRayTracing();

VkBufferUsageFlags RayMeshBufferUsage();
void RayMeshCreated(MeshHandle handle, std::span<const Vertex3D> vertices,
                    std::span<const uint32_t> indices);
void RayMeshUpdated(MeshHandle handle, uint32_t first, std::span<const Vertex3D> vertices);
void RayMeshDestroyed(MeshHandle handle);
void RayRecordMesh(MeshHandle handle, uint32_t first_index, uint32_t index_count,
                   const MeshConstants &constants, const TextureBinding &binding,
                   const DrawState &state);
// Before a display replay, add its interpolated immediate cloth vertices to a fresh scene.
bool RayBeginDisplayScene();
void RayRecordDisplayImmediate(std::span<const Vertex3D> vertices,
                               std::span<const uint32_t> indices,
                               const MeshConstants &constants, const TextureBinding &binding,
                               const DrawState &state);
void RayFinishDisplayScene(VkCommandBuffer cmd);
// Keep bindless texture slots alive while a scene still uses them for secondary hit shading.
bool RayDeferTextureDestroy(TextureHandle handle);
// Called after the frame's mesh draws, outside dynamic rendering. Queries use the last completed
// scene while this builds the acceleration structure for the following frame.
void FinishRayScene(VkCommandBuffer cmd);
VkAccelerationStructureKHR RayScene();
VkDescriptorSetLayout RayDescriptorSetLayout();
void RayBindScene(VkCommandBuffer cmd);
void RayNoteMeshDraw(VkCommandBuffer cmd);
// Optional DC_RAY_PROFILE instrumentation. Call immediately after beginning an
// otherwise empty frame command buffer, before dynamic rendering begins.
void RayProfileBeginFrame(VkCommandBuffer cmd);
uint32_t RayPipelineIndex(uint32_t base_index);

} // namespace gfx::detail
