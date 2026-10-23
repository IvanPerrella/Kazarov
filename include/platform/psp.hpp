#pragma once

#include <cstddef>

// The PSP's system side, kept out of the game's units: the SDK's psptypes.h names u32 and s32
// differently from the game's types.h.

// Calls request_stop when the HOME menu's "quit" is chosen, and ends the program a few seconds
// later if it is still running.
void PspInstallExitCallback(void (*request_stop)());

// Bytes the heap has handed out and not had back.
std::size_t PspHeapUsed();
