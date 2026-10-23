// window.hpp on the PSP: the screen is the GE's (gfx_psp), always 480x272, and the only event is
// the HOME menu's quit (platform/psp.cpp) or the game's own.

#include "platform/window.hpp"

namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 272;

bool g_quit = false;

} // namespace

void WindowSetIdentity(const WindowIdentity &) {}
void WindowSetIcon(const std::uint8_t *, int) {}
void WindowInit(const WindowConfig &) {}
void WindowShutdown() {}
SDL_Window *WindowHandle() { return nullptr; }
bool        WindowTakeModeEvent() { return false; }
void        WindowAddEventHook(void (*)(const SDL_Event &)) {}

WindowModeResult WindowSetMode(const WindowConfig &config) {
    // Whatever is asked for is what the screen already is, so nothing waits for a change.
    WindowMode requested{config.fullscreen, config.width, config.height};
    return {requested, requested, requested, std::nullopt};
}

bool WindowCurrentMode(WindowMode &mode) {
    mode = {true, kWidth, kHeight};
    return true;
}

bool WindowSize(int &width, int &height) {
    width = kWidth;
    height = kHeight;
    return true;
}

bool WindowDisplaySize(int &width, int &height, bool) { return WindowSize(width, height); }

std::vector<DisplayModeSize> WindowDisplayModes() { return {{kWidth, kHeight}}; }

bool WindowPollEvents() { return !g_quit; }

void WindowRequestQuit() { g_quit = true; }
