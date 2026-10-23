#ifdef __PSP__

#include "platform/psp.hpp"

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <new>

#include <pspkernel.h>
#include <psppower.h>

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

std::size_t PspHeapUsed() { return static_cast<std::size_t>(mallinfo().uordblks); }

// Memory is what runs out on a PSP: a failed allocation says how much it wanted and what the heap
// held, and runs the new handler, before it throws as the standard one does.
void *operator new(std::size_t size) {
    for (;;) {
        if (void *block = std::malloc(size == 0 ? 1 : size)) {
            return block;
        }
        std::fprintf(stderr, "memory: no %u bytes left; the heap holds %u KiB\n", static_cast<unsigned>(size),
                     static_cast<unsigned>(PspHeapUsed() >> 10));
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

void *operator new[](std::size_t size) { return operator new(size); }
void  operator delete(void *block) noexcept { std::free(block); }
void  operator delete[](void *block) noexcept { std::free(block); }
void  operator delete(void *block, std::size_t) noexcept { std::free(block); }
void  operator delete[](void *block, std::size_t) noexcept { std::free(block); }

void PspInstallExitCallback(void (*request_stop)()) {
    // The system starts a program at 222 MHz; the game wants all the CPU there is.
    scePowerSetClockFrequency(333, 333, 166);
    g_request_stop = request_stop;
    SceUID thread = sceKernelCreateThread("exit callback", ExitCallbackThread, 0x11, 0x1000, 0, nullptr);
    if (thread >= 0) {
        sceKernelStartThread(thread, 0, nullptr);
    }
}

#endif
