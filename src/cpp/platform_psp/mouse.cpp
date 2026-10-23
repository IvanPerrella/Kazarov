// mouse.hpp on the PSP, which has no mouse.

#include "platform/mouse.hpp"

void          MouseConfigure(bool, std::span<const int>) {}
void          MouseStart() {}
void          MouseStop() {}
bool          MouseHandleEvent(const SDL_Event &) { return false; }
bool          MouseCaptured() { return false; }
bool          MouseInputLive() { return false; }
bool          MouseOwnsCursor() { return false; }
std::uint32_t MouseButtons() { return 0; }
void          MouseSetPointerCursor(bool) {}
bool          MousePointerCursorHidden(bool, bool, bool) { return true; }
float         MouseTakeWheel() { return 0.0f; }

void MouseTakeMotion(float &dx, float &dy) {
    dx = 0.0f;
    dy = 0.0f;
}

void MousePeekMotion(float &dx, float &dy) {
    dx = 0.0f;
    dy = 0.0f;
}
