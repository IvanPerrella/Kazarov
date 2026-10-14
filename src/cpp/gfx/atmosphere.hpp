#pragma once

#include "gfx/context.hpp"
#include <span>

namespace gfx::detail {

inline constexpr uint32_t kMaxLocalLights = 8;

struct LocalLightBlock {
    uint32_t count[4] = {};
    float position_radius[kMaxLocalLights][4] = {};
    float color_intensity[kMaxLocalLights][4] = {};
};

// Depth sampling is optional: the base renderer still accepts devices whose selected
// depth/stencil format cannot be sampled.
bool AtmosphereDepthSupported();
void AtmosphereInit();
void AtmosphereResize();
void AtmosphereDestroyTarget();
void AtmosphereShutdown();

// Called on replayed world draws, after interpolation has rebuilt MeshConstants.
void AtmosphereTrackCamera(const MeshConstants &constants);
// One elevation per grid cell anchors the lower medium in world space.
void AtmosphereSetDeckField(std::span<const float, 400> cell_heights);
void AtmosphereSetLights(std::span<const LocalLight> lights);
const LocalLightBlock &AtmosphereLights();
void AtmosphereSetSettings(const AtmosphereSettings &settings);
bool AtmosphereEnabled();

// Returns source unchanged when the current scene has no atmosphere.
Image &AtmosphereApply(VkCommandBuffer cmd, Image &source, Image &depth);

} // namespace gfx::detail
