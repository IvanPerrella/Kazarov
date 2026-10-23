// audio.hpp on the PSP: a thread pulls the mixer and feeds the sample-rate-converting channel,
// which plays the mixer's 48 kHz as it is. Stereo only.

#include "platform/audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include <pspaudio.h>
#include <pspkernel.h>

#include "kazarov/log.hpp"

namespace {

constexpr int kFrames = 1024;

AudioRenderFn     g_render = nullptr;
void             *g_user = nullptr;
int               g_rate = 0;
SceUID            g_thread = -1;
std::atomic<bool> g_running = false;

std::array<float, kFrames * 2>                       g_mix;
std::array<std::array<std::int16_t, kFrames * 2>, 2> g_out;

int Feed(SceSize, void *) {
    int which = 0;
    while (g_running) {
        g_render(g_user, g_mix.data(), kFrames);
        std::array<std::int16_t, kFrames * 2> &out = g_out[which];
        for (int i = 0; i < kFrames * 2; i++) {
            out[i] = static_cast<std::int16_t>(std::clamp(g_mix[i], -1.0f, 1.0f) * 32767.0f);
        }
        sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, out.data());
        which ^= 1;
    }
    return 0;
}

} // namespace

bool AudioOutputStart(int rate, AudioRenderFn render, void *user) {
    if (g_running) {
        return true;
    }
    if (sceAudioSRCChReserve(kFrames, rate, 2) < 0) {
        kz::Logf(kz::LogLevel::Warning, "audio", "no output channel at {} Hz", rate);
        return false;
    }
    g_render = render;
    g_user = user;
    g_rate = rate;
    g_running = true;
    // Above the game's thread, so a slow tick does not starve the output.
    g_thread = sceKernelCreateThread("audio", Feed, 0x12, 0x8000, PSP_THREAD_ATTR_USER, nullptr);
    if (g_thread < 0 || sceKernelStartThread(g_thread, 0, nullptr) < 0) {
        g_running = false;
        sceAudioSRCChRelease();
        return false;
    }
    return true;
}

void AudioOutputStop() {
    if (!g_running) {
        return;
    }
    g_running = false;
    sceKernelWaitThreadEnd(g_thread, nullptr);
    sceKernelDeleteThread(g_thread);
    g_thread = -1;
    sceAudioSRCChRelease();
}

void AudioSetSurround(bool) {}

bool AudioOutputRunning() { return g_running; }
