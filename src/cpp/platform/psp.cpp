#ifdef __PSP__

#include "platform/psp.hpp"

#include <pspkernel.h>

// EBOOT.PBP's module header. The heap takes all the memory but what the kernel needs for the
// threads' stacks: the game's own, SDL's audio thread and the exit callback's.
PSP_MODULE_INFO("kazarov", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-4096);

namespace {

void (*g_request_stop)() = nullptr;

int ExitRequested(int, int, void *) {
    if (g_request_stop != nullptr) {
        g_request_stop();
    }
    // One stuck in a load is given a few seconds to see the request.
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitGame();
    return 0;
}

int ExitCallbackThread(SceSize, void *) {
    sceKernelRegisterExitCallback(sceKernelCreateCallback("exit", ExitRequested, nullptr));
    sceKernelSleepThreadCB();
    return 0;
}

} // namespace

void PspInstallExitCallback(void (*request_stop)()) {
    g_request_stop = request_stop;
    SceUID thread = sceKernelCreateThread("exit callback", ExitCallbackThread, 0x11, 0x1000, 0, nullptr);
    if (thread >= 0) {
        sceKernelStartThread(thread, 0, nullptr);
    }
}

#endif
