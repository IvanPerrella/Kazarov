#pragma once

#include <cstdint>
#include <vector>
#include <vulkan/vulkan.h>

#include "gfx/gfx.hpp"

namespace gfx::detail {

// Optional Streamline interposer. Every call falls through to Vulkan when the
// SDK is unavailable or initialization fails. Init must precede Vulkan setup;
// shutdown must precede destruction of the Vulkan device and instance.
// Offscreen rendering has no swapchain present hook for Streamline's required
// per-frame bookkeeping, so it uses the native Vulkan fallback.
bool StreamlineInit(bool presentation_available, const char *project_id, const char *engine_version);
void StreamlineShutdown();
// Pass to SDL_Vulkan_LoadLibrary after successful init so SDL surface calls
// also pass through the signed interposer.
const char *StreamlineVulkanLoaderPath();
void StreamlineOnDevice(VkPhysicalDevice physical_device);
void StreamlineQueueRequirements(uint32_t &graphics, uint32_t &compute);
void StreamlineAddDeviceRequirements(VkPhysicalDevice physical_device, std::vector<const char *> &extensions,
                                     VkPhysicalDeviceVulkan12Features &features12,
                                     VkPhysicalDeviceVulkan13Features &features13);
bool StreamlineAvailable();
bool StreamlineSupportsSuperResolution();
bool StreamlineOptimalRenderSize(DlssQuality quality, uint32_t output_width, uint32_t output_height,
                                uint32_t &render_width, uint32_t &render_height);
bool StreamlineSupportsFrameGeneration();
bool StreamlineReflexActive();
enum class StreamlineMarker { SimulationStart, SimulationEnd, RenderSubmitStart, RenderSubmitEnd,
                              PresentStart, PresentEnd };
// Use the same monotonically increasing frame index for constants and markers.
// Call Sleep before SimulationStart to let Reflex pace the CPU.
bool StreamlineReflexSleep(uint32_t frame_index);
void StreamlineMarkFrame(uint32_t frame_index, StreamlineMarker marker);

VkResult StreamlineCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                  VkInstance *instance);
VkResult StreamlineCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
                                const VkAllocationCallbacks *allocator, VkDevice *device);
VkResult StreamlineCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                   const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain);
void StreamlineDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator);
VkResult StreamlineGetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count,
                                      VkImage *images);
VkResult StreamlineAcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                    VkSemaphore semaphore, VkFence fence, uint32_t *index);
VkResult StreamlineQueuePresent(VkQueue queue, const VkPresentInfoKHR *info);
VkResult StreamlineDeviceWaitIdle(VkDevice device);

struct StreamlineImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Matrices are row-major and exclude jitter. The motion image stores current
// to previous normalized screen displacement; its declared scale converts it
// to Streamline's normalized [-1, 1] coordinates.
struct StreamlineCamera {
    float view_to_clip[16] = {};
    float clip_to_view[16] = {};
    float clip_to_prev_clip[16] = {};
    float prev_clip_to_clip[16] = {};
    float position[3] = {};
    float up[3] = {};
    float right[3] = {};
    float forward[3] = {};
    float near_plane = 0.0f;
    float far_plane = 0.0f;
    float fov_radians = 0.0f;
    float aspect_ratio = 0.0f;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float mvec_scale_x = 1.0f;
    float mvec_scale_y = 1.0f;
    bool depth_inverted = false;
    bool reset = false;
    bool valid = false;
};

struct StreamlineFrame {
    uint32_t index = 0;
    DlssQuality dlss_quality = DlssQuality::Quality;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    StreamlineCamera camera;
    StreamlineImage scene_color;
    StreamlineImage output_color;
    StreamlineImage depth;
    StreamlineImage motion;
    StreamlineImage hudless_color;
    StreamlineImage ui_color_alpha;
    uint32_t output_width = 0;
    uint32_t output_height = 0;
    uint32_t backbuffer_count = 0;
    bool dense_motion = false;
    bool clean_hudless = false;
    bool valid_ui = false;
    bool reflex_active = false;
    bool hdr_color = false;
};

// Returns true only after a real Streamline evaluation on the supplied frame.
// The caller must submit this command buffer before presenting its swapchain.
bool StreamlineEvaluateSuperResolution(const StreamlineFrame &frame);
// Frame Generation runs at intercepted present; this sets options and tags the
// current frame only when dense motion and a clean HUD/UI split are supplied.
bool StreamlinePrepareFrameGeneration(const StreamlineFrame &frame);
void StreamlineDisableFrameGeneration();

} // namespace gfx::detail
