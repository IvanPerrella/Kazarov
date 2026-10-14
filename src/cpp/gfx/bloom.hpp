#pragma once

#include "gfx/context.hpp"

namespace gfx::detail {

void BloomInit();
void BloomShutdown();
void BloomDestroyTargets();
void BloomResize();
void BloomResetCapture();
void BloomCaptureScene();
Image &BloomApply(VkCommandBuffer cmd, Image &source);
const Image *BloomHudlessColor();

} // namespace gfx::detail
