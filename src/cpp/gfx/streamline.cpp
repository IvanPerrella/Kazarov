#include "streamline.hpp"
#include "platform/settings.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_helpers_vk.h>
#endif

namespace gfx::detail {

#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
namespace {

struct StreamlineState {
    HMODULE module = nullptr;
    bool initialized = false;
    bool presentation_available = true;
    bool device_ready = false;
    bool super_resolution_supported = false;
    bool frame_generation_supported = false;
    bool frame_generation_enabled = false;
    bool reflex_supported = false;
    bool pcl_supported = false;
    bool reflex_active = false;
    uint64_t fg_real_presented = 0;
    uint64_t fg_total_presented = 0;
    uint32_t fg_last_actual = UINT32_MAX;
    bool present_timing_armed = false;
    uint64_t timed_real_presents = 0;
    uint64_t timed_shown_presents = 0;
    uint64_t present_gaps_over_33ms = 0;
    uint64_t present_gaps_over_50ms = 0;
    double max_present_gap_ms = 0.0;
    std::chrono::steady_clock::time_point first_timed_present{};
    std::chrono::steady_clock::time_point last_timed_present{};
    sl::FeatureRequirements sr_requirements{};
    sl::FeatureRequirements fg_requirements{};
    sl::FeatureRequirements reflex_requirements{};
    sl::FeatureRequirements pcl_requirements{};
    uint32_t cached_frame_index = UINT32_MAX;
    sl::FrameToken *cached_token = nullptr;
    bool constants_set = false;
    std::string loader_path;
    PFun_slInit *init = nullptr;
    PFun_slShutdown *shutdown = nullptr;
    PFun_slGetFeatureRequirements *requirements = nullptr;
    PFun_slIsFeatureSupported *supported = nullptr;
    PFun_slGetFeatureFunction *feature_function = nullptr;
    PFun_slGetNewFrameToken *new_frame_token = nullptr;
    PFun_slSetConstants *set_constants = nullptr;
    PFun_slSetTagForFrame *set_tags = nullptr;
    PFun_slEvaluateFeature *evaluate = nullptr;
    PFun_slSetVulkanInfo *set_vulkan_info = nullptr;
    PFun_slDLSSSetOptions *dlss_options = nullptr;
    PFun_slDLSSGetOptimalSettings *dlss_optimal = nullptr;
    PFun_slDLSSGSetOptions *fg_options = nullptr;
    PFun_slDLSSGGetState *fg_state = nullptr;
    PFun_slReflexSetOptions *reflex_options = nullptr;
    PFun_slReflexSleep *reflex_sleep = nullptr;
    PFun_slPCLSetMarker *pcl_marker = nullptr;
    PFN_vkGetInstanceProcAddr get_instance = nullptr;
    PFN_vkGetDeviceProcAddr get_device = nullptr;
    PFN_vkCreateInstance create_instance = nullptr;
    PFN_vkCreateSwapchainKHR create_swapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkDeviceWaitIdle device_wait_idle = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
};

StreamlineState g_sl;
const sl::ViewportHandle kViewport{0};

sl::DLSSMode SdkQuality(DlssQuality quality) {
    switch (quality) {
    case DlssQuality::Off: return sl::DLSSMode::eOff;
    case DlssQuality::DLAA: return sl::DLSSMode::eDLAA;
    case DlssQuality::Quality: return sl::DLSSMode::eMaxQuality;
    case DlssQuality::Balanced: return sl::DLSSMode::eBalanced;
    case DlssQuality::Performance: return sl::DLSSMode::eMaxPerformance;
    case DlssQuality::UltraPerformance: return sl::DLSSMode::eUltraPerformance;
    }
    return sl::DLSSMode::eOff;
}

bool TrustedModule(const std::wstring &path) {
    HMODULE trust = LoadLibraryExW(L"wintrust.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!trust) return false;
    using Verify = LONG(WINAPI *)(HWND, GUID *, LPVOID);
    auto verify = reinterpret_cast<Verify>(GetProcAddress(trust, "WinVerifyTrust"));
    if (!verify) {
        FreeLibrary(trust);
        return false;
    }
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool trusted = verify(nullptr, &policy, &data) == ERROR_SUCCESS;
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    verify(nullptr, &policy, &data);
    FreeLibrary(trust);
    return trusted;
}

template <typename T> T Load(HMODULE module, const char *name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

// A launcher may retry only a crash during Streamline's device handoff. The
// file is unique to that launch and removed as soon as the handoff returns.
std::wstring DeviceMarkerPath() {
    const std::string key = SettingsEnvName("STREAMLINE_DEVICE_MARKER");
    if (key.empty()) return {};
    std::wstring wide_key(key.begin(), key.end());
    wchar_t path[32768];
    DWORD length = GetEnvironmentVariableW(wide_key.c_str(), path, 32768);
    return length > 0 && length < 32768 ? std::wstring(path, length) : std::wstring{};
}

void SetDeviceMarker(bool pending) {
    std::wstring path = DeviceMarkerPath();
    if (path.empty()) return;
    if (pending) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    } else {
        DeleteFileW(path.c_str());
    }
}

void Log(sl::LogType type, const char *message) {
    if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
        std::fprintf(stderr, "Streamline: %s\n", message);
    }
}

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH] = {};
    DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring result(path, length);
    size_t separator = result.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring{} : result.substr(0, separator);
}

std::string Utf8(const std::wstring &text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr,
                                   nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

bool ImageValid(const StreamlineImage &image) {
    return image.image != VK_NULL_HANDLE && image.view != VK_NULL_HANDLE && image.format != VK_FORMAT_UNDEFINED &&
           image.layout != VK_IMAGE_LAYOUT_UNDEFINED && image.width && image.height;
}

void FillResource(sl::Resource &resource, const StreamlineImage &image) {
    resource.native = reinterpret_cast<void *>(image.image);
    resource.memory = reinterpret_cast<void *>(image.memory);
    resource.view = reinterpret_cast<void *>(image.view);
    resource.state = static_cast<uint32_t>(image.layout);
    resource.width = image.width;
    resource.height = image.height;
    resource.nativeFormat = static_cast<uint32_t>(image.format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = image.usage;
}

void FillMatrix(sl::float4x4 &target, const float source[16]) { std::memcpy(&target, source, sizeof(float) * 16); }

bool CameraValid(const StreamlineCamera &camera) {
    return camera.valid && camera.near_plane > 0.0f && camera.far_plane > camera.near_plane &&
           camera.fov_radians > 0.0f && camera.aspect_ratio > 0.0f && std::isfinite(camera.jitter_x) &&
           std::isfinite(camera.jitter_y) && std::isfinite(camera.mvec_scale_x) &&
           std::isfinite(camera.mvec_scale_y);
}

bool FrameToken(uint32_t frame_index, sl::FrameToken *&token) {
    if (!g_sl.new_frame_token) return false;
    if (g_sl.cached_frame_index != frame_index) {
        g_sl.cached_token = nullptr;
        g_sl.constants_set = false;
        if (g_sl.new_frame_token(g_sl.cached_token, &frame_index) != sl::Result::eOk || !g_sl.cached_token)
            return false;
        g_sl.cached_frame_index = frame_index;
    }
    token = g_sl.cached_token;
    return token != nullptr;
}

bool BeginFrame(const StreamlineFrame &frame, sl::FrameToken *&token) {
    if (!CameraValid(frame.camera) || !g_sl.set_constants || !FrameToken(frame.index, token)) return false;
    if (g_sl.constants_set) return true;
    sl::Constants constants{};
    FillMatrix(constants.cameraViewToClip, frame.camera.view_to_clip);
    FillMatrix(constants.clipToCameraView, frame.camera.clip_to_view);
    FillMatrix(constants.clipToPrevClip, frame.camera.clip_to_prev_clip);
    FillMatrix(constants.prevClipToClip, frame.camera.prev_clip_to_clip);
    constants.jitterOffset = {frame.camera.jitter_x, frame.camera.jitter_y};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.mvecScale = {frame.camera.mvec_scale_x, frame.camera.mvec_scale_y};
    constants.cameraPos = {frame.camera.position[0], frame.camera.position[1], frame.camera.position[2]};
    constants.cameraUp = {frame.camera.up[0], frame.camera.up[1], frame.camera.up[2]};
    constants.cameraRight = {frame.camera.right[0], frame.camera.right[1], frame.camera.right[2]};
    constants.cameraFwd = {frame.camera.forward[0], frame.camera.forward[1], frame.camera.forward[2]};
    constants.cameraNear = frame.camera.near_plane;
    constants.cameraFar = frame.camera.far_plane;
    constants.cameraFOV = frame.camera.fov_radians;
    constants.cameraAspectRatio = frame.camera.aspect_ratio;
    constants.depthInverted = frame.camera.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = frame.camera.reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.motionVectorsJittered = sl::Boolean::eFalse;
    g_sl.constants_set = g_sl.set_constants(constants, *token, kViewport) == sl::Result::eOk;
    return g_sl.constants_set;
}

bool TagFrame(sl::FrameToken &token, VkCommandBuffer cmd, const StreamlineImage &scene,
              const StreamlineImage &output, const StreamlineImage &depth, const StreamlineImage &motion,
              const StreamlineImage *hudless, const StreamlineImage *ui) {
    sl::Resource scene_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource output_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource depth_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource motion_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource hudless_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource ui_resource(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    FillResource(scene_resource, scene);
    FillResource(output_resource, output);
    FillResource(depth_resource, depth);
    FillResource(motion_resource, motion);
    sl::Extent scene_extent{0, 0, scene.width, scene.height};
    sl::Extent output_extent{0, 0, output.width, output.height};
    sl::Extent depth_extent{0, 0, depth.width, depth.height};
    sl::Extent motion_extent{0, 0, motion.width, motion.height};
    sl::ResourceTag tags[] = {
        {&scene_resource, sl::kBufferTypeScalingInputColor, sl::eValidUntilEvaluate, &scene_extent},
        {&output_resource, sl::kBufferTypeScalingOutputColor, sl::eValidUntilEvaluate, &output_extent},
        {&depth_resource, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depth_extent},
        {&motion_resource, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &motion_extent},
    };
    if (g_sl.set_tags(token, kViewport, tags, 4, reinterpret_cast<sl::CommandBuffer *>(cmd)) != sl::Result::eOk)
        return false;
    if (hudless) {
        FillResource(hudless_resource, *hudless);
        sl::Extent hudless_extent{0, 0, hudless->width, hudless->height};
        sl::ResourceTag hudless_tag{&hudless_resource, sl::kBufferTypeHUDLessColor,
                                    sl::eValidUntilPresent, &hudless_extent};
        if (g_sl.set_tags(token, kViewport, &hudless_tag, 1,
                          reinterpret_cast<sl::CommandBuffer *>(cmd)) != sl::Result::eOk) return false;
        if (ui) {
            FillResource(ui_resource, *ui);
            sl::Extent ui_extent{0, 0, ui->width, ui->height};
            sl::ResourceTag ui_tag{&ui_resource, sl::kBufferTypeUIColorAndAlpha,
                                   sl::eValidUntilPresent, &ui_extent};
            if (g_sl.set_tags(token, kViewport, &ui_tag, 1,
                              reinterpret_cast<sl::CommandBuffer *>(cmd)) != sl::Result::eOk) return false;
        }
    }
    return true;
}

bool TagFrameGeneration(sl::FrameToken &token, const StreamlineFrame &frame) {
    sl::Resource depth(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource motion(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    sl::Resource hudless(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
    FillResource(depth, frame.depth);
    FillResource(motion, frame.motion);
    FillResource(hudless, frame.hudless_color);
    sl::Extent depth_extent{0, 0, frame.depth.width, frame.depth.height};
    sl::Extent motion_extent{0, 0, frame.motion.width, frame.motion.height};
    sl::Extent hudless_extent{0, 0, frame.hudless_color.width, frame.hudless_color.height};
    sl::ResourceTag tags[] = {
        {&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depth_extent},
        {&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &motion_extent},
        {&hudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &hudless_extent},
    };
    auto *commands = reinterpret_cast<sl::CommandBuffer *>(frame.command_buffer);
    // Streamline owns the intercepted swapchain. The extent identifies its full
    // presented region without requiring a second backbuffer image handle.
    sl::Extent backbuffer_extent{0, 0, frame.output_width, frame.output_height};
    sl::ResourceTag backbuffer_tag{nullptr, sl::kBufferTypeBackbuffer,
                                   sl::ResourceLifecycle{}, &backbuffer_extent};
    sl::Result result = g_sl.set_tags(token, kViewport, &backbuffer_tag, 1, commands);
    if (result != sl::Result::eOk) {
        std::fprintf(stderr, "Streamline: FG backbuffer extent tag failed (%d)\n",
                     static_cast<int>(result));
        return false;
    }
    result = g_sl.set_tags(token, kViewport, tags, 3, commands);
    if (result != sl::Result::eOk) {
        std::fprintf(stderr, "Streamline: FG resource tags failed (%d)\n", static_cast<int>(result));
        return false;
    }
    if (frame.valid_ui) {
        sl::Resource ui(sl::ResourceType::eTex2d, nullptr, nullptr, nullptr);
        FillResource(ui, frame.ui_color_alpha);
        sl::Extent ui_extent{0, 0, frame.ui_color_alpha.width, frame.ui_color_alpha.height};
        sl::ResourceTag ui_tag{&ui, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &ui_extent};
        result = g_sl.set_tags(token, kViewport, &ui_tag, 1, commands);
        if (result != sl::Result::eOk) {
            std::fprintf(stderr, "Streamline: FG UI tag failed (%d)\n", static_cast<int>(result));
            return false;
        }
    }
    return true;
}

void RecordTimedPresent(uint32_t actually_presented) {
    if (!g_sl.present_timing_armed || SettingsEnv("PRESENT_TIMING") == nullptr) return;
    auto now = std::chrono::steady_clock::now();
    if (g_sl.timed_real_presents == 0) {
        g_sl.first_timed_present = now;
    } else {
        double gap_ms = std::chrono::duration<double, std::milli>(now - g_sl.last_timed_present).count();
        g_sl.max_present_gap_ms = std::max(g_sl.max_present_gap_ms, gap_ms);
        g_sl.present_gaps_over_33ms += gap_ms > 33.333;
        g_sl.present_gaps_over_50ms += gap_ms > 50.0;
    }
    g_sl.last_timed_present = now;
    ++g_sl.timed_real_presents;
    g_sl.timed_shown_presents += actually_presented;
}

} // namespace
#endif

bool StreamlineInit(bool presentation_available, const char *project_id, const char *engine_version) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.initialized) return true;
    if (!project_id || !*project_id || !engine_version || !*engine_version) return false;
    if (!presentation_available) {
        // Even DLSS SR alone requires the common plugin's presentCommon() once
        // per frame for resource retirement. A surfaceless offscreen run has no
        // swapchain presentation to drive that hook; loading the SDK leaks its
        // frame bookkeeping and can crash during shutdown.
        std::fprintf(stderr, "Streamline: offscreen has no present hook; using native Vulkan\n");
        return false;
    }
    if (const char *enabled = SettingsEnv("STREAMLINE"); enabled && std::strcmp(enabled, "0") == 0) {
        std::fprintf(stderr, "Streamline: disabled by STREAMLINE=0; using native Vulkan\n");
        return false;
    }
    std::wstring path = ExecutableDirectory() + L"\\sl.interposer.dll";
    if (!TrustedModule(path)) {
        std::fprintf(stderr, "Streamline: signed interposer unavailable or untrusted\n");
        return false;
    }
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::fprintf(stderr, "Streamline: could not load interposer (Windows error %lu)\n", GetLastError());
        return false;
    }
    g_sl.module = module;
    g_sl.presentation_available = presentation_available;
    g_sl.init = Load<PFun_slInit *>(module, "slInit");
    g_sl.shutdown = Load<PFun_slShutdown *>(module, "slShutdown");
    g_sl.requirements = Load<PFun_slGetFeatureRequirements *>(module, "slGetFeatureRequirements");
    g_sl.supported = Load<PFun_slIsFeatureSupported *>(module, "slIsFeatureSupported");
    g_sl.feature_function = Load<PFun_slGetFeatureFunction *>(module, "slGetFeatureFunction");
    g_sl.new_frame_token = Load<PFun_slGetNewFrameToken *>(module, "slGetNewFrameToken");
    g_sl.set_constants = Load<PFun_slSetConstants *>(module, "slSetConstants");
    g_sl.set_tags = Load<PFun_slSetTagForFrame *>(module, "slSetTagForFrame");
    g_sl.evaluate = Load<PFun_slEvaluateFeature *>(module, "slEvaluateFeature");
    g_sl.set_vulkan_info = Load<PFun_slSetVulkanInfo *>(module, "slSetVulkanInfo");
    g_sl.get_instance = Load<PFN_vkGetInstanceProcAddr>(module, "vkGetInstanceProcAddr");
    g_sl.get_device = Load<PFN_vkGetDeviceProcAddr>(module, "vkGetDeviceProcAddr");
    if (!g_sl.init || !g_sl.shutdown || !g_sl.requirements || !g_sl.supported || !g_sl.feature_function ||
        !g_sl.new_frame_token || !g_sl.set_constants || !g_sl.set_tags || !g_sl.evaluate ||
        !g_sl.set_vulkan_info || !g_sl.get_instance ||
        !g_sl.get_device) {
        FreeLibrary(module);
        g_sl = {};
        return false;
    }
    // Frame Generation requires Reflex; Reflex's latency markers are supplied
    // by the separate PCL plugin, which also works on non-NVIDIA adapters.
    const sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    const char *feature_set = SettingsEnv("STREAMLINE_FEATURES");
    const bool dlss_only = feature_set && std::strcmp(feature_set, "dlss") == 0;
    sl::Preferences preferences{};
    preferences.featuresToLoad = features;
    preferences.numFeaturesToLoad = (dlss_only || !presentation_available) ? 1 : 4;
    preferences.renderAPI = sl::RenderAPI::eVulkan;
    preferences.engine = sl::EngineType::eCustom;
    preferences.engineVersion = engine_version;
    preferences.projectId = project_id;
    preferences.logMessageCallback = Log;
    preferences.flags = sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                        sl::PreferenceFlags::eDisableCLStateTracking;
    sl::Result init_result = g_sl.init(preferences, sl::kSDKVersion);
    if (init_result != sl::Result::eOk) {
        std::fprintf(stderr, "Streamline: initialization failed (%d)\n", static_cast<int>(init_result));
        FreeLibrary(module);
        g_sl = {};
        return false;
    }
    g_sl.initialized = true;
    std::fprintf(stderr, "Streamline: SDK initialized (%s)\n",
                 presentation_available ? "swapchain presentation" : "offscreen SR only");
    g_sl.loader_path = Utf8(path);
    g_sl.create_instance = reinterpret_cast<PFN_vkCreateInstance>(g_sl.get_instance(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!g_sl.create_instance) {
        StreamlineShutdown();
        return false;
    }
    return true;
#else
    (void)presentation_available;
    (void)project_id;
    (void)engine_version;
    return false;
#endif
}

void StreamlineShutdown() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.fg_real_presented)
        std::fprintf(stderr, "Streamline: FG totals real=%llu shown=%llu generated=%llu\n",
                     static_cast<unsigned long long>(g_sl.fg_real_presented),
                     static_cast<unsigned long long>(g_sl.fg_total_presented),
                     static_cast<unsigned long long>(g_sl.fg_total_presented > g_sl.fg_real_presented
                                                         ? g_sl.fg_total_presented - g_sl.fg_real_presented : 0));
    if (g_sl.timed_real_presents > 1) {
        double seconds = std::chrono::duration<double>(g_sl.last_timed_present - g_sl.first_timed_present).count();
        std::fprintf(stderr,
                     "Streamline: present timing %.2fs, real=%llu (%.1f/s), SDK shown=%llu (%.1f/s), "
                     "app gaps >33ms=%llu >50ms=%llu max=%.1fms\n",
                     seconds, static_cast<unsigned long long>(g_sl.timed_real_presents),
                     (g_sl.timed_real_presents - 1) / seconds,
                     static_cast<unsigned long long>(g_sl.timed_shown_presents),
                     (g_sl.timed_shown_presents - 1) / seconds,
                     static_cast<unsigned long long>(g_sl.present_gaps_over_33ms),
                     static_cast<unsigned long long>(g_sl.present_gaps_over_50ms), g_sl.max_present_gap_ms);
    }
    if (g_sl.initialized && g_sl.shutdown) g_sl.shutdown();
    if (g_sl.module) FreeLibrary(g_sl.module);
    g_sl = {};
#endif
}

const char *StreamlineVulkanLoaderPath() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    return g_sl.initialized ? g_sl.loader_path.c_str() : nullptr;
#else
    return nullptr;
#endif
}

void StreamlineOnDevice(VkPhysicalDevice physical_device) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.initialized || !physical_device) return;
    sl::AdapterInfo adapter{};
    adapter.vkPhysicalDevice = physical_device;
    sl::FeatureRequirements sr_requirements{};
    sl::FeatureRequirements fg_requirements{};
    g_sl.super_resolution_supported =
        g_sl.requirements(sl::kFeatureDLSS, sr_requirements) == sl::Result::eOk &&
        sr_requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported &&
        g_sl.supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk;
    g_sl.frame_generation_supported = g_sl.presentation_available &&
        g_sl.requirements(sl::kFeatureDLSS_G, fg_requirements) == sl::Result::eOk &&
        fg_requirements.flags & sl::FeatureRequirementFlags::eVulkanSupported &&
        g_sl.supported(sl::kFeatureDLSS_G, adapter) == sl::Result::eOk;
    g_sl.reflex_supported = g_sl.presentation_available &&
                            g_sl.supported(sl::kFeatureReflex, adapter) == sl::Result::eOk;
    g_sl.pcl_supported = g_sl.presentation_available &&
                         g_sl.supported(sl::kFeaturePCL, adapter) == sl::Result::eOk;
    if (g_sl.super_resolution_supported) g_sl.sr_requirements = sr_requirements;
    if (g_sl.frame_generation_supported) g_sl.fg_requirements = fg_requirements;
    if (g_sl.reflex_supported) g_sl.requirements(sl::kFeatureReflex, g_sl.reflex_requirements);
    if (g_sl.pcl_supported) g_sl.requirements(sl::kFeaturePCL, g_sl.pcl_requirements);
    std::fprintf(stderr, "Streamline: DLSS SR %s, Frame Generation %s on selected adapter\n",
                 g_sl.super_resolution_supported ? "supported" : "unavailable",
                 g_sl.frame_generation_supported ? "supported" : "unavailable");
    if (g_sl.super_resolution_supported) {
        std::fprintf(stderr, "Streamline: DLSS Vulkan requirements: %u graphics queues, %u compute queues, "
                             "%u device extensions, %u Vulkan 1.2 features, %u Vulkan 1.3 features\n",
                     sr_requirements.vkNumGraphicsQueuesRequired, sr_requirements.vkNumComputeQueuesRequired,
                     sr_requirements.vkNumDeviceExtensions, sr_requirements.vkNumFeatures12,
                     sr_requirements.vkNumFeatures13);
        for (uint32_t i = 0; i < sr_requirements.vkNumDeviceExtensions; ++i)
            std::fprintf(stderr, "Streamline: requires %s\n", sr_requirements.vkDeviceExtensions[i]);
        for (uint32_t i = 0; i < sr_requirements.vkNumFeatures12; ++i)
            std::fprintf(stderr, "Streamline: requires Vulkan 1.2 %s\n", sr_requirements.vkFeatures12[i]);
    }
#else
    (void)physical_device;
#endif
}

void StreamlineQueueRequirements(uint32_t &graphics, uint32_t &compute) {
    graphics = compute = 0;
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.initialized) return;
    const sl::FeatureRequirements *requirements[] = {
        g_sl.super_resolution_supported ? &g_sl.sr_requirements : nullptr,
        g_sl.frame_generation_supported ? &g_sl.fg_requirements : nullptr,
        g_sl.reflex_supported ? &g_sl.reflex_requirements : nullptr,
        g_sl.pcl_supported ? &g_sl.pcl_requirements : nullptr,
    };
    for (const sl::FeatureRequirements *required : requirements) {
        if (!required) continue;
        graphics += required->vkNumGraphicsQueuesRequired;
        compute += required->vkNumComputeQueuesRequired;
    }
    std::fprintf(stderr, "Streamline: requesting %u additional graphics and %u compute queues\n",
                 graphics, compute);
#endif
}

void StreamlineAddDeviceRequirements(VkPhysicalDevice physical_device, std::vector<const char *> &extensions,
                                     VkPhysicalDeviceVulkan12Features &features12,
                                     VkPhysicalDeviceVulkan13Features &features13) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.initialized) return;
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, available.data());
    std::vector<const char *> names12, names13;
    const sl::FeatureRequirements *requirements[] = {
        g_sl.super_resolution_supported ? &g_sl.sr_requirements : nullptr,
        g_sl.frame_generation_supported ? &g_sl.fg_requirements : nullptr,
        g_sl.reflex_supported ? &g_sl.reflex_requirements : nullptr,
        g_sl.pcl_supported ? &g_sl.pcl_requirements : nullptr,
    };
    for (const sl::FeatureRequirements *required : requirements) {
        if (!required) continue;
        for (uint32_t i = 0; i < required->vkNumDeviceExtensions; ++i) {
            const char *name = required->vkDeviceExtensions[i];
            bool present = std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties &entry) {
                return std::strcmp(entry.extensionName, name) == 0;
            });
            if (!present) {
                std::fprintf(stderr, "Streamline: required Vulkan extension %s is unavailable\n", name);
            } else if (std::none_of(extensions.begin(), extensions.end(), [name](const char *entry) {
                           return std::strcmp(entry, name) == 0;
                       })) {
                extensions.push_back(name);
            }
        }
        if (required->vkNumFeatures12)
            names12.insert(names12.end(), required->vkFeatures12,
                           required->vkFeatures12 + required->vkNumFeatures12);
        if (required->vkNumFeatures13)
            names13.insert(names13.end(), required->vkFeatures13,
                           required->vkFeatures13 + required->vkNumFeatures13);
    }
    const auto required12 = sl::getVkPhysicalDeviceVulkan12Features(
        static_cast<uint32_t>(names12.size()), names12.data());
    const auto required13 = sl::getVkPhysicalDeviceVulkan13Features(
        static_cast<uint32_t>(names13.size()), names13.data());
    auto merge = [](void *target, const void *needed, size_t start, size_t bytes) {
        auto *to = static_cast<unsigned char *>(target);
        auto *from = static_cast<const unsigned char *>(needed);
        for (size_t offset = start; offset + sizeof(VkBool32) <= bytes; offset += sizeof(VkBool32)) {
            VkBool32 a, b;
            std::memcpy(&a, to + offset, sizeof(a));
            std::memcpy(&b, from + offset, sizeof(b));
            a |= b;
            std::memcpy(to + offset, &a, sizeof(a));
        }
    };
    merge(&features12, &required12, offsetof(VkPhysicalDeviceVulkan12Features, samplerMirrorClampToEdge),
          sizeof(features12));
    merge(&features13, &required13, offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess),
          sizeof(features13));
#else
    (void)physical_device;
    (void)extensions;
    (void)features12;
    (void)features13;
#endif
}

bool StreamlineAvailable() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    return g_sl.device_ready;
#else
    return false;
#endif
}

bool StreamlineSupportsSuperResolution() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    return g_sl.super_resolution_supported;
#else
    return false;
#endif
}

bool StreamlineOptimalRenderSize(DlssQuality quality, uint32_t output_width, uint32_t output_height,
                                uint32_t &render_width, uint32_t &render_height) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.dlss_optimal || !output_width || !output_height || quality == DlssQuality::Off) return false;
    if (quality == DlssQuality::DLAA) {
        render_width = output_width;
        render_height = output_height;
        return true;
    }
    sl::DLSSOptions options{};
    options.mode = SdkQuality(quality);
    options.outputWidth = output_width;
    options.outputHeight = output_height;
    sl::DLSSOptimalSettings settings{};
    if (g_sl.dlss_optimal(options, settings) != sl::Result::eOk ||
        !settings.optimalRenderWidth || !settings.optimalRenderHeight) return false;
    render_width = settings.optimalRenderWidth;
    render_height = settings.optimalRenderHeight;
    return true;
#else
    (void)quality;
    (void)output_width;
    (void)output_height;
    (void)render_width;
    (void)render_height;
    return false;
#endif
}

bool StreamlineSupportsFrameGeneration() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    return g_sl.frame_generation_supported;
#else
    return false;
#endif
}

bool StreamlineReflexActive() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    return g_sl.reflex_active;
#else
    return false;
#endif
}

bool StreamlineReflexSleep(uint32_t frame_index) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.reflex_active || !g_sl.reflex_sleep) return false;
    sl::FrameToken *token = nullptr;
    return FrameToken(frame_index, token) && g_sl.reflex_sleep(*token) == sl::Result::eOk;
#else
    (void)frame_index;
    return false;
#endif
}

void StreamlineMarkFrame(uint32_t frame_index, StreamlineMarker marker) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.pcl_marker) return;
    sl::FrameToken *token = nullptr;
    if (!FrameToken(frame_index, token)) return;
    sl::PCLMarker pcl = sl::PCLMarker::eSimulationStart;
    switch (marker) {
    case StreamlineMarker::SimulationStart: pcl = sl::PCLMarker::eSimulationStart; break;
    case StreamlineMarker::SimulationEnd: pcl = sl::PCLMarker::eSimulationEnd; break;
    case StreamlineMarker::RenderSubmitStart: pcl = sl::PCLMarker::eRenderSubmitStart; break;
    case StreamlineMarker::RenderSubmitEnd: pcl = sl::PCLMarker::eRenderSubmitEnd; break;
    case StreamlineMarker::PresentStart: pcl = sl::PCLMarker::ePresentStart; break;
    case StreamlineMarker::PresentEnd: pcl = sl::PCLMarker::ePresentEnd; break;
    }
    g_sl.pcl_marker(pcl, *token);
#else
    (void)frame_index;
    (void)marker;
#endif
}

VkResult StreamlineCreateInstance(const VkInstanceCreateInfo *info, const VkAllocationCallbacks *allocator,
                                  VkInstance *instance) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.initialized && g_sl.create_instance) {
        VkResult result = g_sl.create_instance(info, allocator, instance);
        if (result == VK_SUCCESS) g_sl.instance = *instance;
        return result;
    }
#endif
    return vkCreateInstance(info, allocator, instance);
}

#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
void BindStreamlineDeviceFunctions(VkDevice device) {
    g_sl.create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
        g_sl.get_device(device, "vkCreateSwapchainKHR"));
    g_sl.destroy_swapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(
        g_sl.get_device(device, "vkDestroySwapchainKHR"));
    g_sl.get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
        g_sl.get_device(device, "vkGetSwapchainImagesKHR"));
    g_sl.acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(g_sl.get_device(device, "vkAcquireNextImageKHR"));
    g_sl.present = reinterpret_cast<PFN_vkQueuePresentKHR>(g_sl.get_device(device, "vkQueuePresentKHR"));
    g_sl.device_wait_idle = reinterpret_cast<PFN_vkDeviceWaitIdle>(g_sl.get_device(device, "vkDeviceWaitIdle"));
    void *function = nullptr;
    if (g_sl.super_resolution_supported &&
        g_sl.feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", function) == sl::Result::eOk)
        g_sl.dlss_options = reinterpret_cast<PFun_slDLSSSetOptions *>(function);
    function = nullptr;
    if (g_sl.super_resolution_supported &&
        g_sl.feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function) == sl::Result::eOk)
        g_sl.dlss_optimal = reinterpret_cast<PFun_slDLSSGetOptimalSettings *>(function);
    function = nullptr;
    if (g_sl.frame_generation_supported &&
        g_sl.feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) == sl::Result::eOk)
        g_sl.fg_options = reinterpret_cast<PFun_slDLSSGSetOptions *>(function);
    function = nullptr;
    if (g_sl.frame_generation_supported &&
        g_sl.feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", function) == sl::Result::eOk)
        g_sl.fg_state = reinterpret_cast<PFun_slDLSSGGetState *>(function);
    function = nullptr;
    if (g_sl.reflex_supported &&
        g_sl.feature_function(sl::kFeatureReflex, "slReflexSetOptions", function) == sl::Result::eOk)
        g_sl.reflex_options = reinterpret_cast<PFun_slReflexSetOptions *>(function);
    function = nullptr;
    if (g_sl.reflex_supported &&
        g_sl.feature_function(sl::kFeatureReflex, "slReflexSleep", function) == sl::Result::eOk)
        g_sl.reflex_sleep = reinterpret_cast<PFun_slReflexSleep *>(function);
    function = nullptr;
    if (g_sl.pcl_supported &&
        g_sl.feature_function(sl::kFeaturePCL, "slPCLSetMarker", function) == sl::Result::eOk)
        g_sl.pcl_marker = reinterpret_cast<PFun_slPCLSetMarker *>(function);
    if (g_sl.reflex_options && g_sl.reflex_sleep && g_sl.pcl_marker) {
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;
        g_sl.reflex_active = g_sl.reflex_options(reflex) == sl::Result::eOk;
        std::fprintf(stderr, "Streamline: Reflex %s\n", g_sl.reflex_active ? "active" : "unavailable");
    }
}
#endif

VkResult StreamlineCreateDevice(VkPhysicalDevice physical_device, const VkDeviceCreateInfo *info,
                                const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkResult result = vkCreateDevice(physical_device, info, allocator, device);
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    uint32_t required_graphics_queues = 0, required_compute_queues = 0;
    StreamlineQueueRequirements(required_graphics_queues, required_compute_queues);
    if (result == VK_SUCCESS && g_sl.initialized &&
        info->pQueueCreateInfos[0].queueCount >= 1 + required_graphics_queues + required_compute_queues) {
        sl::VulkanInfo vulkan{};
        vulkan.instance = g_sl.instance;
        vulkan.physicalDevice = physical_device;
        vulkan.device = *device;
        vulkan.graphicsQueueFamily = info->pQueueCreateInfos[0].queueFamilyIndex;
        vulkan.computeQueueFamily = vulkan.graphicsQueueFamily;
        vulkan.graphicsQueueIndex = 1;
        vulkan.computeQueueIndex = 1 + required_graphics_queues;
        SetDeviceMarker(true);
        auto set_result = g_sl.set_vulkan_info(vulkan);
        SetDeviceMarker(false);
        if (set_result == sl::Result::eOk) {
            g_sl.device_ready = true;
            BindStreamlineDeviceFunctions(*device);
            std::fprintf(stderr, "Streamline: Vulkan device ready (DLSS SR %s, Frame Generation %s)\n",
                         g_sl.dlss_options ? "ready" : "unavailable", g_sl.fg_options ? "ready" : "unavailable");
        } else {
            std::fprintf(stderr, "Streamline: Vulkan device handoff failed (%d); using native Vulkan\n",
                         static_cast<int>(set_result));
        }
    }
#endif
    return result;
}

VkResult StreamlineCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                   const VkAllocationCallbacks *allocator, VkSwapchainKHR *swapchain) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.create_swapchain) return g_sl.create_swapchain(device, info, allocator, swapchain);
#endif
    return vkCreateSwapchainKHR(device, info, allocator, swapchain);
}

void StreamlineDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *allocator) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.destroy_swapchain) return g_sl.destroy_swapchain(device, swapchain, allocator);
#endif
    vkDestroySwapchainKHR(device, swapchain, allocator);
}

VkResult StreamlineGetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, uint32_t *count, VkImage *images) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.get_swapchain_images) return g_sl.get_swapchain_images(device, swapchain, count, images);
#endif
    return vkGetSwapchainImagesKHR(device, swapchain, count, images);
}

VkResult StreamlineAcquireNextImage(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                    VkSemaphore semaphore, VkFence fence, uint32_t *index) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.acquire) return g_sl.acquire(device, swapchain, timeout, semaphore, fence, index);
#endif
    return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
}

VkResult StreamlineQueuePresent(VkQueue queue, const VkPresentInfoKHR *info) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.present) {
        VkResult result = g_sl.present(queue, info);
        uint32_t actually_presented = 1;
        if (g_sl.frame_generation_enabled && g_sl.fg_state) {
            sl::DLSSGState state{};
            sl::Result query = g_sl.fg_state(kViewport, state, nullptr);
            if (query == sl::Result::eOk) {
                actually_presented = state.numFramesActuallyPresented;
                ++g_sl.fg_real_presented;
                g_sl.fg_total_presented += state.numFramesActuallyPresented;
                if (g_sl.fg_real_presented <= 20 || g_sl.fg_real_presented % 120 == 0 ||
                    g_sl.fg_last_actual != state.numFramesActuallyPresented)
                    std::fprintf(stderr, "Streamline: FG present real=%llu shown=%llu latest=%u status=0x%x\n",
                                 static_cast<unsigned long long>(g_sl.fg_real_presented),
                                 static_cast<unsigned long long>(g_sl.fg_total_presented),
                                 state.numFramesActuallyPresented, static_cast<unsigned>(state.status));
                g_sl.fg_last_actual = state.numFramesActuallyPresented;
            } else if (g_sl.fg_real_presented < 20) {
                std::fprintf(stderr, "Streamline: FG state query failed (%d)\n", static_cast<int>(query));
            }
        }
        if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) RecordTimedPresent(actually_presented);
        return result;
    }
#endif
    return vkQueuePresentKHR(queue, info);
}

VkResult StreamlineDeviceWaitIdle(VkDevice device) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (g_sl.device_wait_idle) return g_sl.device_wait_idle(device);
#endif
    return vkDeviceWaitIdle(device);
}

bool StreamlineEvaluateSuperResolution(const StreamlineFrame &frame) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    static bool attempted = false;
    if (!attempted) {
        std::fprintf(stderr, "Streamline: DLSS evaluation requested (%ux%u to %ux%u)\n",
                     frame.scene_color.width, frame.scene_color.height,
                     frame.output_color.width, frame.output_color.height);
        attempted = true;
    }
    if (!g_sl.super_resolution_supported || !g_sl.dlss_options ||
        frame.dlss_quality == DlssQuality::Off || !frame.dense_motion ||
        !ImageValid(frame.scene_color) || !ImageValid(frame.output_color) || !ImageValid(frame.depth) ||
        !ImageValid(frame.motion) || !frame.output_width || !frame.output_height || !frame.command_buffer ||
        frame.scene_color.width > frame.output_color.width ||
        frame.scene_color.height > frame.output_color.height ||
        frame.depth.width != frame.scene_color.width || frame.depth.height != frame.scene_color.height ||
        frame.motion.width != frame.scene_color.width || frame.motion.height != frame.scene_color.height ||
        (frame.output_color.usage & VK_IMAGE_USAGE_STORAGE_BIT) == 0)
        return false;
    sl::FrameToken *token = nullptr;
    if (!BeginFrame(frame, token)) return false;
    sl::DLSSOptions options{};
    options.mode = SdkQuality(frame.dlss_quality);
    options.outputWidth = frame.output_width;
    options.outputHeight = frame.output_height;
    options.colorBuffersHDR = frame.hdr_color ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    options.useAutoExposure = sl::Boolean::eTrue;
    sl::Result result = g_sl.dlss_options(kViewport, options);
    if (result != sl::Result::eOk) {
        std::fprintf(stderr, "Streamline: DLSS options failed (%d)\n", static_cast<int>(result));
        return false;
    }
    if (!TagFrame(*token, frame.command_buffer, frame.scene_color, frame.output_color, frame.depth, frame.motion,
                  nullptr, nullptr)) return false;
    const sl::BaseStructure *evaluate_inputs[] = {&kViewport};
    result = g_sl.evaluate(sl::kFeatureDLSS, *token, evaluate_inputs, 1,
                           reinterpret_cast<sl::CommandBuffer *>(frame.command_buffer));
    if (result != sl::Result::eOk)
        std::fprintf(stderr, "Streamline: DLSS evaluation failed (%d)\n", static_cast<int>(result));
    else {
        g_sl.present_timing_armed = true;
        static bool succeeded = false;
        if (!succeeded) {
            std::fprintf(stderr, "Streamline: DLSS evaluation succeeded\n");
            succeeded = true;
        }
    }
    return result == sl::Result::eOk;
#else
    (void)frame;
    return false;
#endif
}

bool StreamlinePrepareFrameGeneration(const StreamlineFrame &frame) {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.frame_generation_supported || !g_sl.fg_options || !g_sl.fg_state || !frame.dense_motion ||
        !frame.clean_hudless || !frame.reflex_active || frame.camera.reset ||
        !ImageValid(frame.depth) || !ImageValid(frame.motion) || !ImageValid(frame.hudless_color) ||
        (frame.valid_ui && !ImageValid(frame.ui_color_alpha)) || !frame.command_buffer ||
        !frame.backbuffer_count || !frame.output_width || !frame.output_height ||
        frame.hudless_color.width != frame.output_width || frame.hudless_color.height != frame.output_height) {
        StreamlineDisableFrameGeneration();
        return false;
    }
    sl::FrameToken *token = nullptr;
    if (!BeginFrame(frame, token)) return false;
    if (!TagFrameGeneration(*token, frame)) return false;
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    options.numBackBuffers = frame.backbuffer_count;
    options.mvecDepthWidth = frame.motion.width;
    options.mvecDepthHeight = frame.motion.height;
    options.colorWidth = frame.output_width;
    options.colorHeight = frame.output_height;
    options.colorBufferFormat = static_cast<uint32_t>(frame.output_color.format);
    options.mvecBufferFormat = static_cast<uint32_t>(frame.motion.format);
    options.depthBufferFormat = static_cast<uint32_t>(frame.depth.format);
    options.hudLessBufferFormat = static_cast<uint32_t>(frame.hudless_color.format);
    options.uiBufferFormat = frame.valid_ui ? static_cast<uint32_t>(frame.ui_color_alpha.format) : 0;
    sl::Result result = g_sl.fg_options(kViewport, options);
    g_sl.frame_generation_enabled = result == sl::Result::eOk;
    static bool reported = false;
    if (!reported || result != sl::Result::eOk) {
        std::fprintf(stderr, "Streamline: FG options %s (%d)\n",
                     g_sl.frame_generation_enabled ? "enabled" : "failed", static_cast<int>(result));
        reported = true;
    }
    return g_sl.frame_generation_enabled;
#else
    (void)frame;
    return false;
#endif
}

void StreamlineDisableFrameGeneration() {
#if defined(_WIN32) && defined(KZ_HAS_STREAMLINE)
    if (!g_sl.frame_generation_enabled || !g_sl.fg_options) return;
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOff;
    g_sl.fg_options(kViewport, options);
    g_sl.frame_generation_enabled = false;
#endif
}

} // namespace gfx::detail
