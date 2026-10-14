#pragma once

#include "gfx/context.hpp"
#include "streamline.hpp"

namespace gfx::detail {

// The camera pass fills a dense motion field from depth, and rigid depth-writing meshes with
// valid history override it with their object motion.
void MotionInit();
void MotionResize();
void MotionDestroyTargets();
void MotionShutdown();
void MotionSetEnabled(bool enabled);
void MotionSetCamera(const StreamlineCamera &camera);
const StreamlineCamera *MotionCamera();
bool MotionBegin();
void MotionDraw(MeshHandle handle, uint32_t first_index, uint32_t index_count,
                const MeshConstants &constants, const DrawState &state);
void MotionEnd();
void MotionDiscard();
const Image *MotionImage();
VkImageView MotionDepthView();

} // namespace gfx::detail
