#include "platform/arena.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#ifdef __PSP__
#include <malloc.h>
#include <psppower.h>
#include <pspsuspend.h>
#endif

#include "kazarov/log.hpp"
#include "platform/memory.hpp"

namespace {

struct Block {
    const void *owner;
    ArenaMemory memory;
};

std::vector<Block> g_blocks;
// Blocks an owner outgrew stay mapped: retail's memory never went away, and a
// pointer the game kept into an old arena reads zeroes rather than faulting.
std::vector<Block> g_retired;

Block MapBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    return {owner, ArenaMemoryMap(bytes, lead)};
}

bool MapHolds(const std::vector<Block> &blocks, std::uintptr_t address) {
    for (const Block &block : blocks) {
        const auto start = reinterpret_cast<std::uintptr_t>(block.memory.map);
        if (address - start < block.memory.map_size) {
            return true;
        }
    }
    return false;
}

// Every 32-bit truncation the game makes of an arena pointer is widened; one left behind only shows
// when the pointer has bits above 32, so a debug build refuses to run where none would.
void CheckSomeBlockHigh() {
#ifndef NDEBUG
    std::vector<const void *> bases;
    bases.reserve(g_blocks.size());
    for (const Block &block : g_blocks) {
        bases.push_back(block.memory.base);
    }
    ArenaRequireOneAbove4GiB(bases.data(), bases.size());
#endif
}

} // namespace

unsigned char *ArenaBlock(const void *owner, std::size_t bytes, std::size_t lead) {
    for (Block &block : g_blocks) {
        if (block.owner != owner) {
            continue;
        }
        if (block.memory.capacity >= bytes && block.memory.lead >= lead) {
            return block.memory.base;
        }
        ArenaMemoryZero(block.memory);
        g_retired.push_back(block);
        block = MapBlock(owner, bytes, lead);
        CheckSomeBlockHigh();
        return block.memory.base;
    }
    g_blocks.push_back(MapBlock(owner, bytes, lead));
    CheckSomeBlockHigh();
    return g_blocks.back().memory.base;
}

void ArenaClearAll() {
    for (const Block &block : g_blocks) {
        ArenaMemoryZero(block.memory);
    }
}

bool ArenaContains(const void *pointer) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return MapHolds(g_blocks, address) || MapHolds(g_retired, address);
}

void ArenaRequireOneAbove4GiB(const void *const *bases, std::size_t count) {
#ifndef NDEBUG
    for (std::size_t i = 0; i < count; i++) {
        if (IsAbove4GiB(bases[i])) {
            return;
        }
    }
    kz::Abort("arena", "none lies above 4 GiB, where a truncated pointer would show");
#endif
}

void *PortImagePointer(std::int32_t truncated) {
    static const char anchor = 0;
    if (truncated == 0) {
        return nullptr;
    }
    return reinterpret_cast<void *>(WidenNear(reinterpret_cast<std::uintptr_t>(&anchor), static_cast<std::uint32_t>(truncated)));
}

void ArenaOverflow(const void *arena, int used, int limit) {
    // %p, not std::format's {}, so the address reads as the platform's printf spells it.
    char address[32];
    std::snprintf(address, sizeof(address), "%p", arena);
    kz::Abort(std::format("arena {}", address), std::format("allocation overflow, {} of {} quadwords", used, limit));
}

#ifdef __PSP__

namespace {

struct Region {
    unsigned char *base;
    std::size_t    size;
    std::size_t    used;
};

std::vector<Region> g_regions;

// What the pool leaves the rest of the program's heap: the renderer's textures and meshes, the
// sound banks, the game's containers.
// ponytail: a fixed guess until a PSP-1000 run measures it.
constexpr std::size_t kHeapReserve = 5u << 20;
// The most a mode carves: the town, with the port's cut of its DataBuffer__2 (Chronicle's
// dataset.cpp), 1396000 quadwords. Retail's whole GlobalDataBuffer would starve the heap.
constexpr std::size_t kPoolBytes = 1400000u * 16;

void AcquirePool() {
    std::size_t largest = 0;
    std::size_t too_big = 64u << 20;
    while (too_big - largest > (64u << 10)) {
        std::size_t size = (largest + too_big) / 2;
        // memalign, not malloc: GCC drops a malloc freed unread and takes it as having succeeded.
        if (void *block = memalign(64, size)) {
            std::free(block);
            largest = size;
        } else {
            too_big = size;
        }
    }
    std::size_t want = largest > kHeapReserve ? std::min(largest - kHeapReserve, kPoolBytes) : 0;
    std::size_t have = 0;
    if (want < kPoolBytes) {
        // A PSP-1000: its volatile memory too. The system drops that memory's contents when it
        // sleeps, so sleep is held off while it holds the game's arenas.
        void *volatile_memory = nullptr;
        int   volatile_size = 0;
        if (sceKernelVolatileMemLock(0, &volatile_memory, &volatile_size) == 0 && volatile_size > 0) {
            have = static_cast<std::size_t>(volatile_size);
            g_regions.push_back({static_cast<unsigned char *>(volatile_memory), have, 0});
            scePowerLock(0);
        }
    }
    if (want > 0) {
        if (void *block = memalign(64, want)) {
            g_regions.push_back({static_cast<unsigned char *>(block), want, 0});
        }
    }
    std::size_t total = 0;
    for (const Region &region : g_regions) {
        std::memset(region.base, 0, region.size);
        total += region.size;
    }
    kz::Log(kz::LogLevel::Info, "arena", std::format("pool of {} KiB ({} KiB volatile), {} KiB left to the heap", total >> 10, have >> 10,
                                 (largest - want) >> 10));
}

} // namespace

unsigned char *ArenaPoolCarve(std::size_t bytes, std::size_t lead) {
    if (g_regions.empty()) {
        AcquirePool();
    }
    for (Region &region : g_regions) {
        std::size_t at = std::max((region.used + 63) & ~std::size_t{63}, (lead + 63) & ~std::size_t{63});
        if (at + bytes <= region.size) {
            region.used = at + bytes;
            return region.base + at;
        }
    }
    std::string state;
    for (const Region &region : g_regions) {
        state += std::format(" {}/{} KiB", region.used >> 10, region.size >> 10);
    }
    kz::Abort("arena", std::format("the pool has no {} KiB left; its regions hold{}", bytes >> 10, state));
}

void ArenaPoolReset() {
    std::size_t used = 0;
    for (Region &region : g_regions) {
        std::memset(region.base, 0, region.used);
        used += region.used;
        region.used = 0;
    }
    if (used != 0) {
        kz::Log(kz::LogLevel::Info, "arena", std::format("pool high-water {} KiB", used >> 10));
    }
}

#endif

void ArenaOverflow(const char *name, int used, int limit) {
    kz::Abort(std::format("arena {}", name), std::format("allocation overflow, {} of {} quadwords", used, limit));
}
