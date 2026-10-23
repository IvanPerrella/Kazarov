// gfx.hpp on the PSP's GE (docs/PSP.md in the game's repository, "Renderer").
//
// Draws go to the GE as they are called: a game tick's "recording" is the GE display list itself,
// its canonical render is the wait for the GE to finish it, and presenting swaps the framebuffers.
// There is no interpolation and no render thread.
//
// Colour stays in GS units throughout, 0x80 being 1.0: the GE's colour doubling (GU_FRAGMENT_2X)
// and its doubled-alpha blend factors are the GS's own conventions, so vertex colours and
// alphas pass through unscaled. Texture alpha arrives in the renderer's units (0xFF opaque), so a
// textured draw's alpha At * Af / 255 is again in GS units.

#include "gfx/gfx.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <psputils.h>

namespace gfx {

struct DisplayList {
    bool                  overlay = false;
    std::vector<Vertex2D> vertices;
    struct Draw {
        Primitive      primitive;
        uint32_t       first;
        uint32_t       count;
        TextureBinding binding;
        DrawState      state;
    };
    std::vector<Draw> draws;
    DisplayListStats  stats{};
};

namespace {

constexpr int      kScreenWidth = 480;
constexpr int      kScreenHeight = 272;
constexpr int      kFrameStride = 512;
constexpr uint32_t kFrameBytes = kFrameStride * kScreenHeight * 4;
constexpr uint32_t kDepthOffset = kFrameBytes * 2;
constexpr uint32_t kTargetPoolStart = kDepthOffset + kFrameStride * kScreenHeight * 2;
constexpr uint32_t kListBytes = 384u << 10;
// Room a draw may need in the list before it is flushed: its commands, vertices and a palette copy.
constexpr uint32_t kListMargin = 64u << 10;
// Vertex data past this size goes to the heap instead of the list, until the GE is done with it.
constexpr uint32_t kSpillBytes = 64u << 10;
constexpr uint32_t kMaxTextureSize = 512;

alignas(64) uint32_t g_list[kListBytes / 4];

struct Texture {
    bool          used = false;
    bool          palette = false;
    bool          render_target = false;
    bool          shares_main_depth = false;
    bool          frame = false;
    bool          vram = false;
    bool          dirty = true;
    TextureDesc   desc;
    uint32_t      pixel_width = 0;  // the stored image
    uint32_t      pixel_height = 0;
    uint32_t      stride = 0;       // texels per stored row
    uint32_t      pot_width = 0;    // what sceGuTexImage is told
    uint32_t      pot_height = 0;
    uint32_t      vram_offset = 0;
    uint8_t      *pixels = nullptr; // RAM, or VRAM's cached mirror for a render target
    uint32_t      bound_serial = 0;
    uint32_t      bytes = 0;        // what it takes of the heap
    // A palette's copy in the open list, so a later update cannot reach draws already queued.
    const void   *clut_copy = nullptr;
    uint32_t      clut_serial = 0;
};

struct GeVertex2D {
    float    u, v;
    uint32_t color;
    float    x, y, z;
};

struct GeVertex3D {
    float    u, v;
    uint32_t color;
    float    nx, ny, nz;
    float    x, y, z;
};

struct Mesh {
    bool                    used = false;
    GeVertex3D             *vertices = nullptr;
    uint32_t                vertex_count = 0;
    std::vector<uint16_t>   indices;
    uint32_t                bound_serial = 0;
};

struct VramBlock {
    uint32_t offset;
    uint32_t bytes;
};

struct State {
    RendererConfig  config;
    FrameLayout     layout;
    PresentMode     present_mode = PresentMode::Fifo;
    std::vector<Texture> textures;
    std::vector<Mesh>    meshes;
    std::unordered_map<std::string, TextureHandle> named;
    std::vector<VramBlock> vram_free;
    uint32_t vram_size = 0;
    uint32_t draw_offset = 0;    // the framebuffer being drawn
    uint32_t display_offset = kFrameBytes;
    bool     list_open = false;
    uint32_t serial = 1;         // which GE list draws belong to; bumped when one finishes
    bool     in_frame = false;   // BeginFrame
    bool     recording = false;  // BeginRecording(Game)
    std::shared_ptr<DisplayList> list; // the list being recorded
    TextureHandle target = kMainTarget;
    UiAnchor      anchor;
    InterpKey     interp_key = 0;
    bool          interp_no_interpolation = false;
    Interp2D      interp_2d;
    bool          warned_prog0 = false;
    bool          up = false;
    std::vector<void *> spills;
    std::size_t         texture_bytes = 0;
    std::size_t         mesh_bytes = 0;
    std::size_t         reported_bytes = 0;
} g;

uint8_t *VramCached(uint32_t offset) {
    return static_cast<uint8_t *>(sceGeEdramGetAddr()) + offset;
}

uint8_t *VramUncached(uint32_t offset) {
    return reinterpret_cast<uint8_t *>(reinterpret_cast<uintptr_t>(VramCached(offset)) | 0x40000000u);
}

uint32_t NextPow2(uint32_t v) {
    uint32_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

uint32_t Pack(const uint8_t c[4]) {
    return static_cast<uint32_t>(c[0]) | static_cast<uint32_t>(c[1]) << 8 | static_cast<uint32_t>(c[2]) << 16 |
           static_cast<uint32_t>(c[3]) << 24;
}

uint8_t Unit(float value, float scale) {
    return static_cast<uint8_t>(std::clamp(value * scale, 0.0f, 255.0f));
}

uint32_t PackFloat(const float c[4], float rgb_scale, float alpha_scale) {
    uint8_t bytes[4] = {Unit(c[0], rgb_scale), Unit(c[1], rgb_scale), Unit(c[2], rgb_scale), Unit(c[3], alpha_scale)};
    return Pack(bytes);
}

// ---- VRAM -------------------------------------------------------------------------------------

bool VramAlloc(uint32_t bytes, uint32_t &offset) {
    bytes = (bytes + 0x1FFF) & ~0x1FFFu;
    for (auto it = g.vram_free.begin(); it != g.vram_free.end(); ++it) {
        if (it->bytes >= bytes) {
            offset = it->offset;
            it->offset += bytes;
            it->bytes -= bytes;
            if (it->bytes == 0) {
                g.vram_free.erase(it);
            }
            return true;
        }
    }
    return false;
}

void VramFree(uint32_t offset, uint32_t bytes) {
    bytes = (bytes + 0x1FFF) & ~0x1FFFu;
    g.vram_free.push_back({offset, bytes});
    std::ranges::sort(g.vram_free, {}, &VramBlock::offset);
    for (size_t i = 1; i < g.vram_free.size();) {
        VramBlock &prev = g.vram_free[i - 1];
        if (prev.offset + prev.bytes == g.vram_free[i].offset) {
            prev.bytes += g.vram_free[i].bytes;
            g.vram_free.erase(g.vram_free.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

// ---- Lookup -----------------------------------------------------------------------------------

Texture *LookupTexture(TextureHandle handle) {
    if (handle < 3 || handle >= g.textures.size() || !g.textures[handle].used) {
        return nullptr;
    }
    return &g.textures[handle];
}

Mesh *LookupMesh(MeshHandle handle) {
    if (handle == kNullMesh || handle >= g.meshes.size() || !g.meshes[handle].used) {
        return nullptr;
    }
    return &g.meshes[handle];
}

TextureHandle NewTextureSlot() {
    if (g.textures.size() < 3) {
        g.textures.resize(3);
    }
    for (TextureHandle i = 3; i < g.textures.size(); i++) {
        if (!g.textures[i].used) {
            g.textures[i] = Texture{};
            g.textures[i].used = true;
            return i;
        }
    }
    g.textures.emplace_back();
    g.textures.back().used = true;
    return static_cast<TextureHandle>(g.textures.size() - 1);
}

// ---- Mapping ----------------------------------------------------------------------------------

LogicalMapping MainMapping() {
    const float scale = std::min(kScreenWidth / g.config.logical_width, kScreenHeight / g.config.logical_height);
    return LogicalMapping{scale,
                          scale,
                          std::floor((kScreenWidth - g.config.logical_width * scale) * 0.5f),
                          std::floor((kScreenHeight - g.config.logical_height * scale) * 0.5f),
                          kScreenWidth,
                          kScreenHeight};
}

float RenderScaleValue() { return kScreenHeight / g.config.logical_height; }

bool Fills() { return g.layout.aspect == AspectMode::Fill; }

LogicalMapping TextureMapping(const Texture &t) {
    LogicalMapping main = MainMapping();
    const float    lw = static_cast<float>(t.desc.logical_width);
    const float    lh = static_cast<float>(t.desc.logical_height);
    if (t.shares_main_depth) {
        return main;
    }
    if (t.frame && Fills()) {
        float rows = t.desc.logical_height == g.config.logical_height ? main.scale_y
                                                                       : static_cast<float>(t.pixel_height) / lh;
        return LogicalMapping{main.scale_x * g.config.logical_width / lw, rows, main.offset_x, 0.0f, t.pixel_width,
                              t.pixel_height};
    }
    return LogicalMapping{static_cast<float>(t.pixel_width) / lw, static_cast<float>(t.pixel_height) / lh, 0.0f, 0.0f,
                          t.pixel_width, t.pixel_height};
}

LogicalMapping MappingOf(TextureHandle handle) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        return MainMapping();
    }
    const Texture *t = LookupTexture(handle);
    return t != nullptr ? TextureMapping(*t) : LogicalMapping{1.0f, 1.0f, 0.0f, 0.0f, 0, 0};
}

// ---- The GE list ------------------------------------------------------------------------------

struct TargetInfo {
    uint32_t vram_offset;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    bool     drawable;
};

TargetInfo TargetOf(TextureHandle handle) {
    if (handle == kMainTarget) {
        return {g.draw_offset, kFrameStride, kScreenWidth, kScreenHeight, true};
    }
    const Texture *t = LookupTexture(handle);
    if (t == nullptr || !t->render_target) {
        return {g.draw_offset, kFrameStride, kScreenWidth, kScreenHeight, false};
    }
    return {t->vram_offset, t->stride, t->pixel_width, t->pixel_height, t->vram};
}

void ApplyTarget() {
    TargetInfo t = TargetOf(g.target);
    sceGuDrawBufferList(GU_PSM_8888, reinterpret_cast<void *>(t.vram_offset), static_cast<int>(t.stride));
    sceGuOffset(2048 - t.width / 2, 2048 - t.height / 2);
    sceGuViewport(2048, 2048, static_cast<int>(t.width), static_cast<int>(t.height));
}

void OpenList() {
    if (g.list_open) {
        return;
    }
    sceGuStart(GU_DIRECT, g_list);
    g.list_open = true;
    ApplyTarget();
}

void CloseList() {
    if (!g.list_open) {
        return;
    }
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    g.list_open = false;
    g.serial++;
    for (void *spill : g.spills) {
        std::free(spill);
    }
    g.spills.clear();
}

// Waits for what the GE has queued, so memory its draws read can change, and goes on in a new list.
void Flush() {
    if (!g.list_open) {
        return;
    }
    CloseList();
    OpenList();
}

// Room in the list for a draw's commands and what of its data stays in the list: reserved before
// its state is sent, so nothing flushes the list between the two.
void EnsureListRoom(uint32_t bytes) {
    if (g.list_open && static_cast<uint32_t>(sceGuCheckList()) + bytes + kListMargin > kListBytes) {
        Flush();
    }
}

// What of a draw's data of this size lands in the list rather than spilling to the heap.
uint32_t InList(uint32_t bytes) { return bytes > kSpillBytes ? 0 : (bytes + 15) & ~15u; }

// Data a reserved draw puts in the list, or on the heap when it is large.
void *ListMemory(uint32_t bytes) {
    bytes = (bytes + 15) & ~15u;
    if (bytes > kSpillBytes) {
        void *spill = memalign(16, bytes);
        if (spill == nullptr) {
            Flush();
            spill = memalign(16, bytes);
        }
        g.spills.push_back(spill);
        // Written through the uncached mirror, as the list is; no stale line may be written over it.
        sceKernelDcacheWritebackInvalidateRange(spill, bytes);
        return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(spill) | 0x40000000u);
    }
    return sceGuGetMemory(static_cast<int>(bytes));
}

void Present() {
    CloseList();
    if (g.present_mode == PresentMode::Fifo) {
        sceDisplayWaitVblankStart();
    }
    g.display_offset = g.draw_offset;
    g.draw_offset = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sceGuSwapBuffers()));
    // The heap is what runs out on a PSP; a move of a megabyte or more is worth a line.
    const std::size_t held = g.texture_bytes + g.mesh_bytes;
    if (held > g.reported_bytes + (1u << 20) || held + (1u << 20) < g.reported_bytes) {
        g.reported_bytes = held;
        std::fprintf(stderr, "gfx: textures %u KiB, meshes %u KiB\n", static_cast<unsigned>(g.texture_bytes >> 10),
                     static_cast<unsigned>(g.mesh_bytes >> 10));
    }
}

// ---- State ------------------------------------------------------------------------------------

int GeAlphaFunc(AlphaFunc f) {
    static constexpr int kMap[] = {GU_NEVER, GU_ALWAYS, GU_LESS, GU_LEQUAL, GU_EQUAL, GU_GEQUAL, GU_GREATER, GU_NOTEQUAL};
    return kMap[static_cast<int>(f)];
}

// GS ((A - B) * C >> 7) + D as the GE's src * Fs (op) dst * Fd. C's units are the GS's (0x80 is
// 1.0), which the doubled factors read directly.
void ApplyBlend(const DrawState &s) {
    if (!s.blend) {
        sceGuDisable(GU_BLEND);
        return;
    }
    const GsBlend &b = s.alpha;
    int          c = GU_DOUBLE_SRC_ALPHA;
    int          one_minus_c = GU_ONE_MINUS_DOUBLE_SRC_ALPHA;
    unsigned int fix = 0;
    unsigned int one_minus_fix = 0;
    if (b.c == 1) {
        c = GU_DOUBLE_DST_ALPHA;
        one_minus_c = GU_ONE_MINUS_DOUBLE_DST_ALPHA;
    } else if (b.c == 2) {
        c = GU_FIX;
        one_minus_c = GU_FIX;
        uint32_t f = std::min<uint32_t>(b.fix * 2u, 255u);
        fix = f | f << 8 | f << 16;
        uint32_t r = 255u - f;
        one_minus_fix = r | r << 8 | r << 16;
    }
    constexpr unsigned int kOne = 0xFFFFFF;
    enum { Cs = 0, Cd = 1, Zero = 2 };
    // Each case is src * Fs (op) dst * Fd; F is a factor code, with GU_FIX taking the value beside it.
    struct Factor {
        int          code;
        unsigned int value;
    };
    const Factor kZero = {GU_FIX, 0};
    const Factor kUnit = {GU_FIX, kOne};
    const Factor kC = {c, fix};
    const Factor kNotC = {one_minus_c, one_minus_fix};
    int    op = GU_ADD;
    Factor src = kZero;
    Factor dst = kZero;
    auto   set = [&](int o, Factor s, Factor d) {
        op = o;
        src = s;
        dst = d;
    };
    // ponytail: the four (A - B) * C + D whose D doubles a term (Cs * (1 + C) and the like) have no GE
    // equivalent and take the nearest case's.
    switch (b.a * 9 + b.b * 3 + b.d) {
        case Cs * 9 + Cs * 3 + Cs:
        case Cd * 9 + Cd * 3 + Cs:
        case Zero * 9 + Zero * 3 + Cs:
            sceGuDisable(GU_BLEND);
            return;
        case Cs * 9 + Cs * 3 + Cd:
        case Cd * 9 + Cd * 3 + Cd:
        case Zero * 9 + Zero * 3 + Cd: set(GU_ADD, kZero, kUnit); break;
        case Cs * 9 + Cs * 3 + Zero:
        case Cd * 9 + Cd * 3 + Zero:
        case Zero * 9 + Zero * 3 + Zero: set(GU_ADD, kZero, kZero); break;
        case Cs * 9 + Cd * 3 + Cd: set(GU_ADD, kC, kNotC); break;         // Cs C + Cd (1 - C)
        case Cs * 9 + Cd * 3 + Cs: set(GU_ADD, kC, kNotC); break;         // ~ Cs (1 + C) - Cd C
        case Cs * 9 + Cd * 3 + Zero: set(GU_SUBTRACT, kC, kC); break;     // Cs C - Cd C
        case Cs * 9 + Zero * 3 + Cd: set(GU_ADD, kC, kUnit); break;       // Cs C + Cd
        case Cs * 9 + Zero * 3 + Cs: set(GU_ADD, kUnit, kZero); break;    // ~ Cs (1 + C)
        case Cs * 9 + Zero * 3 + Zero: set(GU_ADD, kC, kZero); break;     // Cs C
        case Cd * 9 + Cs * 3 + Cs: set(GU_ADD, kNotC, kC); break;         // Cd C + Cs (1 - C)
        case Cd * 9 + Cs * 3 + Cd: set(GU_ADD, kNotC, kC); break;         // ~ Cd (1 + C) - Cs C
        case Cd * 9 + Cs * 3 + Zero: set(GU_REVERSE_SUBTRACT, kC, kC); break; // Cd C - Cs C
        case Cd * 9 + Zero * 3 + Cs: set(GU_ADD, kUnit, kC); break;       // Cs + Cd C
        case Cd * 9 + Zero * 3 + Cd: set(GU_ADD, kZero, kUnit); break;    // ~ Cd (1 + C)
        case Cd * 9 + Zero * 3 + Zero: set(GU_ADD, kZero, kC); break;     // Cd C
        case Zero * 9 + Cs * 3 + Cs: set(GU_ADD, kNotC, kZero); break;    // Cs (1 - C)
        case Zero * 9 + Cs * 3 + Cd: set(GU_REVERSE_SUBTRACT, kC, kUnit); break; // Cd - Cs C
        case Zero * 9 + Cs * 3 + Zero: set(GU_ADD, kZero, kZero); break;  // -Cs C, clamped
        case Zero * 9 + Cd * 3 + Cs: set(GU_SUBTRACT, kUnit, kC); break;  // Cs - Cd C
        case Zero * 9 + Cd * 3 + Cd: set(GU_ADD, kZero, kNotC); break;    // Cd (1 - C)
        default: set(GU_ADD, kZero, kZero); break;                        // -Cd C, clamped
    }
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(op, src.code, dst.code, src.value, dst.value);
}

void SetScissorPixels(int x0, int y0, int x1, int y1) {
    TargetInfo t = TargetOf(g.target);
    x0 = std::clamp(x0, 0, static_cast<int>(t.width));
    y0 = std::clamp(y0, 0, static_cast<int>(t.height));
    x1 = std::clamp(x1, x0, static_cast<int>(t.width));
    y1 = std::clamp(y1, y0, static_cast<int>(t.height));
    if (x1 == x0 || y1 == y0) {
        // An empty scissor: the GE's region is inclusive, so park it off the target.
        x0 = y0 = 1023;
        x1 = y1 = 1024;
    }
    sceGuSendCommandi(212, (y0 << 10) | x0);
    sceGuSendCommandi(213, ((y1 - 1) << 10) | (x1 - 1));
}

// The logical frame on the main target in pixels; under Fill a rect reaching an edge of the frame
// is carried to the target's edge on that side.
void ApplyScissor(const DrawState &s) {
    TargetInfo t = TargetOf(g.target);
    int        x0 = 0;
    int        y0 = 0;
    int        x1 = static_cast<int>(t.width);
    int        y1 = static_cast<int>(t.height);
    LogicalMapping m = MappingOf(g.target);
    const bool main = g.target == kMainTarget;
    if (main && !Fills()) {
        x0 = static_cast<int>(m.offset_x);
        x1 = static_cast<int>(std::ceil(g.config.logical_width * m.scale_x + m.offset_x));
    }
    if (s.scissor) {
        LogicalRect r = s.scissor_rect;
        float       rx1 = std::isinf(r.w) ? g.config.logical_width : r.x + r.w;
        float       ry1 = std::isinf(r.h) ? g.config.logical_height : r.y + r.h;
        int         sx0 = static_cast<int>(std::floor(r.x * m.scale_x + m.offset_x));
        int         sy0 = static_cast<int>(std::floor(r.y * m.scale_y + m.offset_y));
        int         sx1 = static_cast<int>(std::ceil(rx1 * m.scale_x + m.offset_x));
        int         sy1 = static_cast<int>(std::ceil(ry1 * m.scale_y + m.offset_y));
        if (main && Fills() && s.extend_scissor) {
            if (r.x <= 0.0f) {
                sx0 = 0;
            }
            if (rx1 >= g.config.logical_width) {
                sx1 = static_cast<int>(t.width);
            }
        }
        x0 = std::max(x0, sx0);
        y0 = std::max(y0, sy0);
        x1 = std::min(x1, sx1);
        y1 = std::min(y1, sy1);
    }
    SetScissorPixels(x0, y0, x1, y1);
}

void ApplyDepth(const DrawState &s) {
    if (s.depth_test == DepthTest::Always && !s.depth_write) {
        sceGuDisable(GU_DEPTH_TEST);
    } else {
        static constexpr int kMap[] = {GU_NEVER, GU_ALWAYS, GU_GEQUAL, GU_GREATER};
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(kMap[static_cast<int>(s.depth_test)]);
    }
    sceGuDepthMask(s.depth_write ? GU_FALSE : GU_TRUE);
}

void ApplyCommon(const DrawState &s) {
    ApplyBlend(s);
    ApplyScissor(s);
    ApplyDepth(s);
    if (s.alpha_test) {
        sceGuEnable(GU_ALPHA_TEST);
        sceGuAlphaFunc(GeAlphaFunc(s.alpha_func), s.alpha_ref, 0xFF);
    } else {
        sceGuDisable(GU_ALPHA_TEST);
    }
    if (s.cull == CullMode::None) {
        sceGuDisable(GU_CULL_FACE);
    } else {
        // ponytail: the winding the GE keeps is from its documentation, not yet checked on hardware.
        sceGuEnable(GU_CULL_FACE);
        sceGuFrontFace(s.cull == CullMode::Back ? GU_CCW : GU_CW);
    }
    uint32_t keep = 0;
    keep |= (s.color_write_mask & kWriteRed) ? 0 : 0x000000FFu;
    keep |= (s.color_write_mask & kWriteGreen) ? 0 : 0x0000FF00u;
    keep |= (s.color_write_mask & kWriteBlue) ? 0 : 0x00FF0000u;
    keep |= (s.color_write_mask & kWriteAlpha) ? 0 : 0xFF000000u;
    sceGuPixelMask(keep);
    // ponytail: no stencil yet. The GE keeps it in the framebuffer's alpha, which the game reads as
    // destination alpha; the shadow volumes need one or the other moved.
    sceGuDisable(GU_STENCIL_TEST);
}

// What the texture's logical texels are in the bound image's texels.
struct TexelMap {
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
};

bool FrameImage(TextureHandle handle) {
    if (handle == kPreviousFrame) {
        return true;
    }
    const Texture *t = LookupTexture(handle);
    return t != nullptr && (t->frame || t->shares_main_depth);
}

// Binds the texture and says how its logical texels land on the bound image. False when there is
// nothing to bind.
bool BindTexture(const TextureBinding &b, const DrawState &s, bool normalized, TexelMap &map) {
    uint32_t    address = 0;
    uint32_t    stride = 0;
    uint32_t    pot_w = 0;
    uint32_t    pot_h = 0;
    uint32_t    pixel_w = 0;
    uint32_t    pixel_h = 0;
    bool        indexed = false;
    bool        alpha = true;
    LogicalMapping m{};
    if (b.texture == kPreviousFrame || b.texture == kMainTarget) {
        uint32_t offset = b.texture == kPreviousFrame ? g.display_offset : g.draw_offset;
        address = reinterpret_cast<uintptr_t>(VramCached(offset));
        stride = kFrameStride;
        pot_w = kFrameStride;
        pot_h = 512;
        pixel_w = kScreenWidth;
        pixel_h = kScreenHeight;
        m = MainMapping();
        sceGuTexFlush();
    } else {
        Texture *t = LookupTexture(b.texture);
        if (t == nullptr || t->palette) {
            return false;
        }
        address = reinterpret_cast<uintptr_t>(t->pixels);
        stride = t->stride;
        pot_w = t->pot_width;
        pot_h = t->pot_height;
        pixel_w = t->pixel_width;
        pixel_h = t->pixel_height;
        indexed = t->desc.format == TextureFormat::Index8;
        alpha = t->desc.has_alpha;
        m = t->render_target ? TextureMapping(*t)
                             : LogicalMapping{static_cast<float>(t->pixel_width) / t->desc.logical_width,
                                              static_cast<float>(t->pixel_height) / t->desc.logical_height,
                                              0.0f, 0.0f, t->pixel_width, t->pixel_height};
        if (t->dirty || t->render_target) {
            sceGuTexFlush();
            t->dirty = false;
        }
        // The draw's list room was reserved before its state, so nothing below flushes this list.
        t->bound_serial = g.serial;
        if (indexed) {
            Texture *p = LookupTexture(b.palette);
            if (p == nullptr || !p->palette) {
                return false;
            }
            if (p->clut_serial != g.serial || p->clut_copy == nullptr) {
                auto *copy = static_cast<uint8_t *>(ListMemory(1024 + 64));
                copy = reinterpret_cast<uint8_t *>((reinterpret_cast<uintptr_t>(copy) + 63) & ~uintptr_t{63});
                std::memcpy(copy, p->pixels, 1024);
                p->clut_copy = copy;
                p->clut_serial = g.serial;
            }
            sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
            sceGuClutLoad(256 / 8, p->clut_copy);
        }
    }
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(indexed ? GU_PSM_T8 : GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, static_cast<int>(pot_w), static_cast<int>(pot_h), static_cast<int>(stride),
                  reinterpret_cast<const void *>(address));
    // A texture without alpha (or TCC 0) gives TEXA's alpha; TA0 0x80 with AEM's black texels already
    // cleared at upload is what the game uses, which TCC_RGB reads as the fragment's alpha.
    bool texture_alpha = !b.ignore_alpha && (alpha || s.texa_aem);
    sceGuTexFunc(GU_TFX_MODULATE, texture_alpha ? GU_TCC_RGBA : GU_TCC_RGB);
    int filter = b.filter == Filter::Linear ? GU_LINEAR : GU_NEAREST;
    sceGuTexFilter(filter, filter);
    sceGuTexWrap(b.wrap_u == Wrap::Repeat ? GU_REPEAT : GU_CLAMP, b.wrap_v == Wrap::Repeat ? GU_REPEAT : GU_CLAMP);
    if (normalized) {
        sceGuTexScale(static_cast<float>(pixel_w) / pot_w, static_cast<float>(pixel_h) / pot_h);
        sceGuTexOffset(0.0f, 0.0f);
    }
    map = {m.scale_x, m.scale_y, m.offset_x, m.offset_y};
    return true;
}

void ApplyTexture(const TextureBinding &b, const DrawState &s, bool normalized, TexelMap &map) {
    if (b.texture != kNullTexture && BindTexture(b, s, normalized, map)) {
        sceGuEnable(GU_FRAGMENT_2X);
        return;
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_FRAGMENT_2X);
}

// ---- 2D ---------------------------------------------------------------------------------------

float AnchorShift(std::span<const Vertex2D> vertices, const LogicalMapping &m) {
    const UiAnchor &a = g.anchor;
    if (!Fills() || g.target != kMainTarget || (std::isinf(a.left_until) && a.left_until < 0 &&
                                                std::isinf(a.right_from) && a.right_from > 0)) {
        return 0.0f;
    }
    float lo = INFINITY;
    float hi = -INFINITY;
    for (const Vertex2D &v : vertices) {
        lo = std::min(lo, v.x);
        hi = std::max(hi, v.x);
    }
    float centre = (lo + hi) * 0.5f;
    float frame_w = g.config.logical_width * m.scale_x;
    float shown_w = static_cast<float>(kScreenWidth);
    if (a.max_aspect > 0.0f) {
        shown_w = std::min(shown_w, kScreenHeight * a.max_aspect);
    }
    float edge = std::max(0.0f, std::floor((shown_w - frame_w) * 0.5f));
    if (centre < a.left_until) {
        return -edge;
    }
    if (centre >= a.right_from) {
        return edge;
    }
    return 0.0f;
}

void EmitVertex(GeVertex2D &out, const Vertex2D &v, const LogicalMapping &m, const TexelMap &t, float dx) {
    out.u = v.u * t.sx + t.ox;
    out.v = v.v * t.sy + t.oy;
    uint8_t c[4] = {v.color[0], v.color[1], v.color[2], v.color[3]};
    out.color = Pack(c);
    out.x = v.x * m.scale_x + m.offset_x + dx;
    out.y = v.y * m.scale_y + m.offset_y;
    out.z = std::clamp(v.z, 0.0f, 1.0f) * 65535.0f;
}

void Draw2DNow(Primitive primitive, std::span<const Vertex2D> vertices, const TextureBinding &binding,
               const DrawState &state) {
    if (!g.list_open || vertices.empty() || !TargetOf(g.target).drawable) {
        return;
    }
    const uint32_t count = primitive == Primitive::Quads ? static_cast<uint32_t>(vertices.size() / 4 * 6)
                                                         : static_cast<uint32_t>(vertices.size());
    if (count == 0) {
        return;
    }
    EnsureListRoom(InList(count * sizeof(GeVertex2D)) + 4096);
    ApplyCommon(state);
    TexelMap texels;
    ApplyTexture(binding, state, false, texels);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);

    LogicalMapping m = MappingOf(g.target);
    float          dx = AnchorShift(vertices, m);
    auto          *out = static_cast<GeVertex2D *>(ListMemory(count * sizeof(GeVertex2D)));
    if (primitive == Primitive::Quads) {
        static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
        for (size_t q = 0; q + 3 < vertices.size(); q += 4) {
            for (int i = 0; i < 6; i++) {
                EmitVertex(out[q / 4 * 6 + i], vertices[q + kOrder[i]], m, texels, dx);
            }
        }
    } else {
        for (size_t i = 0; i < vertices.size(); i++) {
            EmitVertex(out[i], vertices[i], m, texels, dx);
        }
    }
    // The full-frame rule, for the rectangles that need it: one spanning the logical frame's width
    // is carried to the target's edges, a cover stretched, an image of the frame continued.
    const bool rect = (primitive == Primitive::Quads && vertices.size() == 4) ||
                      (primitive == Primitive::TriangleStrip && vertices.size() == 4);
    if (rect && dx == 0.0f && g.target == kMainTarget && Fills()) {
        float lo = INFINITY;
        float hi = -INFINITY;
        for (const Vertex2D &v : vertices) {
            lo = std::min(lo, v.x);
            hi = std::max(hi, v.x);
        }
        if (lo <= 0.0f && hi >= g.config.logical_width && hi > lo) {
            const bool  image = FrameImage(binding.texture);
            const float px_lo = lo * m.scale_x + m.offset_x;
            const float px_hi = hi * m.scale_x + m.offset_x;
            float       u_lo = 0.0f;
            float       u_hi = 0.0f;
            for (uint32_t i = 0; i < count; i++) {
                if (out[i].x == px_lo) {
                    u_lo = out[i].u;
                } else {
                    u_hi = out[i].u;
                }
            }
            const float du = (u_hi - u_lo) / (px_hi - px_lo);
            for (uint32_t i = 0; i < count; i++) {
                float to = out[i].x == px_lo ? std::min(px_lo, 0.0f) : std::max(px_hi, static_cast<float>(kScreenWidth));
                if (image) {
                    out[i].u += (to - out[i].x) * du;
                }
                out[i].x = to;
            }
        }
    }
    static constexpr int kPrim[] = {GU_TRIANGLES, GU_TRIANGLE_STRIP, GU_TRIANGLE_FAN, GU_LINES, GU_LINE_STRIP, GU_TRIANGLES};
    sceGuDrawArray(kPrim[static_cast<int>(primitive)],
                   GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, static_cast<int>(count),
                   nullptr, out);
}

// A rectangle of the bound target in pixels: clears, copies and blits.
void DrawPixelRect(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t color,
                   float z) {
    auto *out = static_cast<GeVertex2D *>(ListMemory(2 * sizeof(GeVertex2D)));
    out[0] = {u0, v0, color, x0, y0, z};
    out[1] = {u1, v1, color, x1, y1, z};
    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, nullptr,
                   out);
}

void PlainState() {
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_LIGHTING);
    sceGuDisable(GU_FOG);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDepthMask(GU_TRUE);
    sceGuPixelMask(0);
    TargetInfo t = TargetOf(g.target);
    SetScissorPixels(0, 0, static_cast<int>(t.width), static_cast<int>(t.height));
}

// ---- 3D ---------------------------------------------------------------------------------------

using Mat4 = std::array<float, 16>; // column-major

Mat4 Mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[c * 4 + k];
            }
            r[c * 4 + row] = sum;
        }
    }
    return r;
}

Mat4 Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

bool InvertAffine(const Mat4 &m, Mat4 &out) {
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) {
        return false;
    }
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float h = m[2], i = m[6], j = m[10];
    float det = a * (e * j - f * i) - b * (d * j - f * h) + c * (d * i - e * h);
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    float inv = 1.0f / det;
    Mat4  r{};
    r[0] = (e * j - f * i) * inv;
    r[4] = (c * i - b * j) * inv;
    r[8] = (b * f - c * e) * inv;
    r[1] = (f * h - d * j) * inv;
    r[5] = (a * j - c * h) * inv;
    r[9] = (c * d - a * f) * inv;
    r[2] = (d * i - e * h) * inv;
    r[6] = (b * h - a * i) * inv;
    r[10] = (a * e - b * d) * inv;
    for (int row = 0; row < 3; row++) {
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    }
    r[15] = 1.0f;
    out = r;
    return true;
}

ScePspFMatrix4 ToGe(const Mat4 &m) {
    ScePspFMatrix4 r;
    std::memcpy(&r, m.data(), sizeof(r));
    return r;
}

// Vulkan's clip space (y down, z / w in [0, 1] with near at 1) to the GE's (y up, z / w in [-1, 1]
// with near at -1, which sceGuDepthRange(65535, 0) puts at 65535).
Mat4 VulkanToGe() { return {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, -2, 0, 0, 0, 1, 1}; }

// Sets the GE's matrices and lighting for a mesh draw. The GE lights in its world space and fogs by
// its view-space z, so world is the draw's object-to-lighting matrix, view puts the eye's depth
// (clip w) in z, and projection undoes both on the way to clip space.
void ApplyMeshTransform(const MeshConstants &k, const DrawState &s) {
    Mat4 mvp;
    std::memcpy(mvp.data(), k.mvp, sizeof(k.mvp));
    const bool lit = (k.flags & kMeshLit) != 0 && (k.flags & kMeshShadow) == 0;
    const bool vertex_color = (k.flags & kMeshVertexColor) != 0 && (k.flags & kMeshShadow) == 0;

    Mat4 world = Identity();
    Mat4 world_inv = Identity();
    bool lighting = lit;
    if (lit) {
        for (int c = 0; c < 3; c++) {
            for (int row = 0; row < 3; row++) {
                world[c * 4 + row] = k.normal_matrix[c * 4 + row];
            }
        }
        if (!InvertAffine(world, world_inv)) {
            world = Identity();
            world_inv = Identity();
            lighting = false;
        }
    }
    Mat4 to_clip = Mul(mvp, world_inv); // world space to clip

    Mat4 view = Identity();
    Mat4 view_inv = Identity();
    bool fog = false;
    float fog_near = 0.0f;
    float fog_far = 0.0f;
    if (s.fog && (k.flags & kMeshFog) != 0 && k.fog[1] != 0.0f) {
        // F = clamp(fog0 + fog1 / w, fog2, fog3) / 255 against the GE's (far - w) / (far - near):
        // the same at the depths where F reaches its top and its floor.
        float top = std::min(k.fog[3], 255.0f);
        float floor = std::max(k.fog[2], 0.0f);
        float w_near = top - k.fog[0] != 0.0f ? k.fog[1] / (top - k.fog[0]) : -1.0f;
        float w_far = floor - k.fog[0] != 0.0f ? k.fog[1] / (floor - k.fog[0]) : -1.0f;
        if (w_near > 0.0f && w_far > w_near) {
            float rest = floor / 255.0f;
            fog_near = w_near;
            fog_far = rest < 1.0f ? (w_far - rest * w_near) / (1.0f - rest) : w_far;
            // View z = -w: the clip w row, in the place of whichever world axis it leans on most.
            float r[4] = {to_clip[3], to_clip[7], to_clip[11], to_clip[15]};
            int   k_axis = 0;
            for (int a = 1; a < 3; a++) {
                if (std::fabs(r[a]) > std::fabs(r[k_axis])) {
                    k_axis = a;
                }
            }
            if (std::fabs(r[k_axis]) > 1e-12f) {
                int others[2];
                int n = 0;
                for (int a = 0; a < 3; a++) {
                    if (a != k_axis) {
                        others[n++] = a;
                    }
                }
                view = Mat4{};
                view[others[0] * 4 + 0] = 1.0f;
                view[others[1] * 4 + 1] = 1.0f;
                for (int a = 0; a < 3; a++) {
                    view[a * 4 + 2] = -r[a];
                }
                view[12 + 2] = -r[3];
                view[15] = 1.0f;
                fog = InvertAffine(view, view_inv);
                if (!fog) {
                    view = Identity();
                    view_inv = Identity();
                }
            }
        }
    }
    Mat4 projection = Mul(VulkanToGe(), Mul(to_clip, view_inv));
    ScePspFMatrix4 p = ToGe(projection);
    ScePspFMatrix4 v = ToGe(view);
    ScePspFMatrix4 w = ToGe(world);
    sceGuSetMatrix(GU_PROJECTION, &p);
    sceGuSetMatrix(GU_VIEW, &v);
    sceGuSetMatrix(GU_MODEL, &w);

    if (fog) {
        uint8_t c[4] = {s.fog_color[0], s.fog_color[1], s.fog_color[2], 0};
        sceGuFog(fog_near, fog_far, Pack(c));
        sceGuEnable(GU_FOG);
    } else {
        sceGuDisable(GU_FOG);
    }

    // The GE's lighting unit computes every mesh colour. Its products are of 0..1 factors, and the
    // result must come out in GS units: a factor of 1.0 (GS 0x80) is 128 / 255 on one side of each
    // product and 1.0 on the other. Material values ride on the lights' side, where they are folded in.
    sceGuEnable(GU_LIGHTING);
    sceGuLightMode(GU_SINGLE_COLOR);
    const float side = vertex_color ? 255.0f : 128.0f;
    float       ambient[4] = {0, 0, 0, 0};
    if (lighting) {
        for (int i = 0; i < 4; i++) {
            ambient[i] = k.ambient[i] * k.ambient_material[i];
        }
    } else {
        // Unlit: the material diffuse alone.
        for (int i = 0; i < 3; i++) {
            ambient[i] = k.diffuse[i];
        }
    }
    uint8_t ambient_bytes[4] = {Unit(ambient[0], side), Unit(ambient[1], side), Unit(ambient[2], side),
                                Unit(vertex_color ? k.diffuse[3] : 1.0f, 255.0f)};
    sceGuAmbient(Pack(ambient_bytes));
    // Without vertex colours the material is white with the draw's alpha (GS units).
    uint8_t material[4] = {255, 255, 255, Unit(k.diffuse[3], 128.0f)};
    sceGuColorMaterial(vertex_color ? GU_AMBIENT_AND_DIFFUSE : 0);
    sceGuModelColor(0, Pack(material), Pack(material), 0);
    // sceGuModelColor leaves the material's alpha, which the lit alpha is made from; this sets it.
    sceGuAmbientColor(Pack(material));
    const uint32_t lights = lighting ? std::min<uint32_t>(k.light_count, 4) : 0;
    for (uint32_t i = 0; i < 4; i++) {
        if (i >= lights) {
            sceGuDisable(GU_LIGHT0 + static_cast<int>(i));
            continue;
        }
        ScePspFVector3 towards = {-k.light_direction[i][0], -k.light_direction[i][1], -k.light_direction[i][2]};
        float          colour[3];
        for (int c = 0; c < 3; c++) {
            colour[c] = k.light_color[i][c] * k.diffuse[c];
        }
        uint8_t bytes[4] = {Unit(colour[0], side), Unit(colour[1], side), Unit(colour[2], side), 255};
        sceGuEnable(GU_LIGHT0 + static_cast<int>(i));
        sceGuLight(static_cast<int>(i), GU_DIRECTIONAL, GU_DIFFUSE, &towards);
        sceGuLightColor(static_cast<int>(i), GU_DIFFUSE, Pack(bytes));
    }
}

GeVertex3D ToGe(const Vertex3D &v) {
    return {v.uv[0], v.uv[1], Pack(v.color), v.normal[0], v.normal[1], v.normal[2],
            v.position[0], v.position[1], v.position[2]};
}

constexpr int kMeshVertexType =
    GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_NORMAL_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D;

// The caller has reserved kMeshCommandBytes in the list.
constexpr uint32_t kMeshCommandBytes = 8192;

void DrawMeshNow(const GeVertex3D *vertices, const uint16_t *indices, uint32_t count, const MeshConstants &k,
                 const TextureBinding &binding, const DrawState &state) {
    if (!g.list_open || count == 0 || !TargetOf(g.target).drawable) {
        return;
    }
    ApplyCommon(state);
    TexelMap texels;
    ApplyTexture(binding, state, true, texels);
    ApplyMeshTransform(k, state);
    sceGuDrawArray(GU_TRIANGLES, kMeshVertexType, static_cast<int>(count), indices, vertices);
}

// ---- CPU copies -------------------------------------------------------------------------------

// The texel at (x, y) of a stored image, RGBA8 or one index byte.
const uint8_t *TexelAt(TextureHandle handle, uint32_t x, uint32_t y, uint32_t &bytes) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        uint32_t offset = handle == kPreviousFrame ? g.display_offset : g.draw_offset;
        bytes = 4;
        return VramUncached(offset) + (y * kFrameStride + x) * 4;
    }
    Texture *t = LookupTexture(handle);
    bytes = t->desc.format == TextureFormat::Index8 ? 1 : 4;
    const uint8_t *base = t->vram ? VramUncached(t->vram_offset) : t->pixels;
    return base + (y * t->stride + x) * bytes;
}

uint32_t PixelWidth(TextureHandle handle) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        return kScreenWidth;
    }
    const Texture *t = LookupTexture(handle);
    return t != nullptr ? t->pixel_width : 0;
}

uint32_t PixelHeight(TextureHandle handle) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        return kScreenHeight;
    }
    const Texture *t = LookupTexture(handle);
    return t != nullptr ? t->pixel_height : 0;
}

// Nearest-texel copy between two images by their logical rects, through each one's mapping. For a
// destination the GE cannot draw into (a texture in RAM).
bool CpuBlit(TextureHandle src, Rect sr, TextureHandle dst, Rect dr) {
    Texture *d = LookupTexture(dst);
    if (d == nullptr || (src != kMainTarget && src != kPreviousFrame && LookupTexture(src) == nullptr)) {
        return false;
    }
    Flush();
    LogicalMapping sm = MappingOf(src);
    LogicalMapping dm = MappingOf(dst);
    uint32_t       sw = PixelWidth(src);
    uint32_t       sh = PixelHeight(src);
    int            x0 = static_cast<int>(std::lround(std::min(dr.x, dr.x + dr.w) * dm.scale_x + dm.offset_x));
    int            x1 = static_cast<int>(std::lround(std::max(dr.x, dr.x + dr.w) * dm.scale_x + dm.offset_x));
    int            y0 = static_cast<int>(std::lround(std::min(dr.y, dr.y + dr.h) * dm.scale_y + dm.offset_y));
    int            y1 = static_cast<int>(std::lround(std::max(dr.y, dr.y + dr.h) * dm.scale_y + dm.offset_y));
    x0 = std::clamp(x0, 0, static_cast<int>(d->pixel_width));
    x1 = std::clamp(x1, 0, static_cast<int>(d->pixel_width));
    y0 = std::clamp(y0, 0, static_cast<int>(d->pixel_height));
    y1 = std::clamp(y1, 0, static_cast<int>(d->pixel_height));
    const uint32_t dbytes = d->desc.format == TextureFormat::Index8 ? 1 : 4;
    uint8_t       *dbase = d->vram ? VramUncached(d->vram_offset) : d->pixels;
    for (int y = y0; y < y1; y++) {
        float fy = (static_cast<float>(y) + 0.5f - dm.offset_y) / dm.scale_y; // logical in dst
        float ty = dr.h != 0 ? (fy - dr.y) / static_cast<float>(dr.h) : 0.0f;
        float ly = static_cast<float>(sr.y) + ty * static_cast<float>(sr.h);
        int   sy = std::clamp(static_cast<int>(ly * sm.scale_y + sm.offset_y), 0, static_cast<int>(sh) - 1);
        for (int x = x0; x < x1; x++) {
            float    fx = (static_cast<float>(x) + 0.5f - dm.offset_x) / dm.scale_x;
            float    tx = dr.w != 0 ? (fx - dr.x) / static_cast<float>(dr.w) : 0.0f;
            float    lx = static_cast<float>(sr.x) + tx * static_cast<float>(sr.w);
            int      sx = std::clamp(static_cast<int>(lx * sm.scale_x + sm.offset_x), 0, static_cast<int>(sw) - 1);
            uint32_t sbytes = 0;
            const uint8_t *s = TexelAt(src, static_cast<uint32_t>(sx), static_cast<uint32_t>(sy), sbytes);
            if (sbytes != dbytes) {
                return false;
            }
            std::memcpy(dbase + (static_cast<uint32_t>(y) * d->stride + static_cast<uint32_t>(x)) * dbytes, s, dbytes);
        }
    }
    if (!d->vram) {
        sceKernelDcacheWritebackRange(d->pixels, d->stride * d->pot_height * dbytes);
    }
    d->dirty = true;
    return true;
}

// A copy or blit drawn by the GE into a render target or the frame.
bool GpuBlit(TextureHandle src, Rect sr, TextureHandle dst, Rect dr, Filter filter) {
    EnsureListRoom(4096);
    TextureHandle previous = g.target;
    if (previous != dst) {
        g.target = dst;
        ApplyTarget();
    }
    PlainState();
    TextureBinding binding;
    binding.texture = src;
    binding.filter = filter;
    DrawState plain;
    TexelMap  texels;
    bool      ok = BindTexture(binding, plain, false, texels);
    if (ok) {
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuDisable(GU_FRAGMENT_2X);
        LogicalMapping dm = MappingOf(dst);
        DrawPixelRect(dr.x * dm.scale_x + dm.offset_x, dr.y * dm.scale_y + dm.offset_y,
                      (dr.x + dr.w) * dm.scale_x + dm.offset_x, (dr.y + dr.h) * dm.scale_y + dm.offset_y,
                      sr.x * texels.sx + texels.ox, sr.y * texels.sy + texels.oy,
                      (sr.x + sr.w) * texels.sx + texels.ox, (sr.y + sr.h) * texels.sy + texels.oy, 0xFFFFFFFF,
                      0.0f);
    }
    if (previous != dst) {
        g.target = previous;
        ApplyTarget();
    }
    return ok;
}

bool Blit(TextureHandle src, Rect sr, TextureHandle dst, Rect dr, Filter filter) {
    if (src == dst) {
        return false;
    }
    TargetInfo t = TargetOf(dst);
    if (g.list_open && (dst == kMainTarget || (LookupTexture(dst) != nullptr && LookupTexture(dst)->render_target && t.drawable))) {
        return GpuBlit(src, sr, dst, dr, filter);
    }
    return CpuBlit(src, sr, dst, dr);
}

} // namespace

// ---- Lifecycle --------------------------------------------------------------------------------

std::optional<RenderThreadMode> ParseRenderThreadMode(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "off") {
        return RenderThreadMode::Off;
    }
    if (lower == "lockstep") {
        return RenderThreadMode::Lockstep;
    }
    if (lower == "async") {
        return RenderThreadMode::Async;
    }
    return std::nullopt;
}

void RendererInit(SDL_Window *, const RendererConfig &config) {
    g.config = config;
    g.layout = config.layout;
    g.present_mode = config.present_mode;
    g.textures.assign(3, Texture{});
    g.meshes.assign(1, Mesh{});
    g.vram_size = sceGeEdramGetSize();
    g.vram_free = {{kTargetPoolStart, g.vram_size - kTargetPoolStart}};

    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, reinterpret_cast<void *>(0), kFrameStride);
    sceGuDispBuffer(kScreenWidth, kScreenHeight, reinterpret_cast<void *>(kFrameBytes), kFrameStride);
    sceGuDepthBuffer(reinterpret_cast<void *>(kDepthOffset), kFrameStride);
    sceGuOffset(2048 - kScreenWidth / 2, 2048 - kScreenHeight / 2);
    sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_CLIP_PLANES);
    sceGuShadeModel(GU_SMOOTH);
    sceGuClearColor(0xFF000000);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    g.draw_offset = 0;
    g.display_offset = kFrameBytes;
    g.up = true;
    std::fprintf(stderr, "gfx: GE renderer, %u KiB of VRAM for render targets\n",
                 (g.vram_size - kTargetPoolStart) >> 10);
}

RendererFeatures ActiveRendererFeatures() { return {0, false, true, false, false, false}; }
bool             HeadlessSurfaceAvailable() { return false; }

void RendererShutdown() {
    g.up = false;
    CloseList();
    sceGuTerm();
}

RenderThreadMode ActiveRenderThreadMode() { return RenderThreadMode::Off; }
bool             OnRenderThread() { return true; }
RenderLease::RenderLease() = default;
RenderLease::~RenderLease() = default;
void SetRenderIdleHook(RenderIdleHook, void *) {}
void WakeRenderThread() {}
void RendererResize() {}
void SetPresentMode(PresentMode mode) { g.present_mode = mode; }
void SetAnisotropy(int) {}

bool BeginFrame() {
    if (g.recording || g.in_frame) {
        return false;
    }
    g.target = kMainTarget;
    OpenList();
    g.in_frame = true;
    return true;
}

void EndFrame() {
    if (!g.in_frame) {
        return;
    }
    g.in_frame = false;
    Present();
}

bool        InFrame() { return g.in_frame || g.recording || (g.list && g.list->overlay); }
uint32_t    ValidationMessageCount() { return 0; }
// Callers read a count above zero as "the renderer is up"; the GE has one fixed pipeline.
uint32_t    PipelineCount() { return g.up ? 1 : 0; }
double      PipelineCompileSeconds() { return 0.0; }
float       RenderScale() { return RenderScaleValue(); }
void        SetRenderScale(float) {}
void        SetFrameLayout(const FrameLayout &layout) { g.layout = layout; }
FrameLayout CurrentFrameLayout() { return g.layout; }

// ---- Textures ---------------------------------------------------------------------------------

TextureHandle CreateTexture(const TextureDesc &desc) {
    if (desc.width == 0 || desc.height == 0) {
        return kNullTexture;
    }
    TextureHandle handle = NewTextureSlot();
    Texture      &t = g.textures[handle];
    t.desc = desc;
    t.desc.logical_width = desc.logical_width != 0 ? desc.logical_width : desc.width;
    t.desc.logical_height = desc.logical_height != 0 ? desc.logical_height : desc.height;
    // ponytail: past 512 texels the GE cannot address an image, so it is kept at half size (and
    // so on); splitting the draws would keep its detail.
    uint32_t w = desc.width;
    uint32_t h = desc.height;
    while (w > kMaxTextureSize || h > kMaxTextureSize) {
        w = std::max<uint32_t>(1, w / 2);
        h = std::max<uint32_t>(1, h / 2);
    }
    t.pixel_width = w;
    t.pixel_height = h;
    const uint32_t bytes = desc.format == TextureFormat::Index8 ? 1 : 4;
    t.pot_width = NextPow2(w);
    t.pot_height = NextPow2(h);
    t.stride = std::max<uint32_t>(t.pot_width, 16);
    const uint32_t size = t.stride * t.pot_height * bytes;
    t.pixels = static_cast<uint8_t *>(memalign(64, size));
    t.bytes = size;
    g.texture_bytes += size;
    if (t.pixels == nullptr) {
        std::fprintf(stderr, "gfx: no memory for a %ux%u texture\n", desc.width, desc.height);
        t.used = false;
        return kNullTexture;
    }
    std::memset(t.pixels, 0, size);
    sceKernelDcacheWritebackRange(t.pixels, size);
    return handle;
}

TextureHandle CreatePalette() {
    TextureHandle handle = NewTextureSlot();
    Texture      &t = g.textures[handle];
    t.palette = true;
    t.desc.width = 256;
    t.desc.height = 1;
    t.desc.logical_width = 256;
    t.desc.logical_height = 1;
    t.pixel_width = 256;
    t.pixel_height = 1;
    t.pixels = static_cast<uint8_t *>(memalign(64, 1024));
    std::memset(t.pixels, 0, 1024);
    return handle;
}

TextureHandle CreateRenderTarget(uint32_t logical_width, uint32_t logical_height, bool has_alpha,
                                 bool share_main_depth, bool frame_target) {
    if (logical_width == 0 || logical_height == 0) {
        return kNullTexture;
    }
    LogicalMapping main = MainMapping();
    uint32_t       w;
    uint32_t       h;
    if (share_main_depth || (frame_target && Fills() && logical_height == g.config.logical_height)) {
        w = kScreenWidth;
        h = kScreenHeight;
    } else if (frame_target && Fills()) {
        w = kScreenWidth;
        h = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(logical_height * main.scale_y)));
    } else {
        w = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(logical_width * RenderScaleValue())));
        h = std::max<uint32_t>(1, static_cast<uint32_t>(std::ceil(logical_height * RenderScaleValue())));
    }
    w = std::min<uint32_t>(w, kFrameStride);
    h = std::min<uint32_t>(h, kFrameStride);
    TextureHandle handle = NewTextureSlot();
    Texture      &t = g.textures[handle];
    t.render_target = true;
    t.shares_main_depth = share_main_depth;
    t.frame = frame_target;
    t.desc.width = logical_width;
    t.desc.height = logical_height;
    t.desc.logical_width = logical_width;
    t.desc.logical_height = logical_height;
    t.desc.has_alpha = has_alpha;
    t.pixel_width = w;
    t.pixel_height = h;
    t.stride = std::max<uint32_t>(64, NextPow2(w));
    t.pot_width = t.stride;
    t.pot_height = NextPow2(h);
    const uint32_t size = t.stride * h * 4;
    t.vram = VramAlloc(size, t.vram_offset);
    if (t.vram) {
        t.pixels = VramCached(t.vram_offset);
        auto *pixels = reinterpret_cast<uint32_t *>(VramUncached(t.vram_offset));
        std::fill(pixels, pixels + t.stride * h, 0x80000000u);
    } else {
        // ponytail: out of VRAM the target is an image in RAM, which copies reach but draws do not.
        std::fprintf(stderr, "gfx: no VRAM for a %ux%u render target; it is drawn into only by copies\n", w, h);
        t.pixels = static_cast<uint8_t *>(memalign(64, size));
        if (t.pixels == nullptr) {
            std::fprintf(stderr, "gfx: no memory for a %ux%u render target either\n", w, h);
            t = Texture{};
            return kNullTexture;
        }
        t.bytes = size;
        g.texture_bytes += size;
        auto *pixels = reinterpret_cast<uint32_t *>(t.pixels);
        std::fill(pixels, pixels + t.stride * h, 0x80000000u);
        sceKernelDcacheWritebackRange(t.pixels, size);
    }
    return handle;
}

TextureHandle NamedRenderTarget(std::string_view name, uint32_t logical_width, uint32_t logical_height,
                                bool has_alpha, bool share_main_depth, bool frame_target) {
    TextureHandle existing = FindNamedRenderTarget(name);
    if (existing != kNullTexture) {
        const Texture &t = g.textures[existing];
        bool same_size = share_main_depth ||
                         (t.desc.logical_width == logical_width && t.desc.logical_height == logical_height);
        bool same_shape = t.shares_main_depth == share_main_depth && (share_main_depth || t.frame == frame_target);
        if (same_size && same_shape && t.desc.has_alpha == has_alpha) {
            return existing;
        }
        DestroyTexture(existing);
    }
    TextureHandle created = CreateRenderTarget(logical_width, logical_height, has_alpha, share_main_depth, frame_target);
    if (created != kNullTexture) {
        g.named.emplace(std::string(name), created);
    }
    return created;
}

TextureHandle FindNamedRenderTarget(std::string_view name) {
    auto it = g.named.find(std::string(name));
    return it == g.named.end() ? kNullTexture : it->second;
}

void DestroyTexture(TextureHandle handle) {
    Texture *t = LookupTexture(handle);
    if (t == nullptr) {
        return;
    }
    std::erase_if(g.named, [handle](const auto &entry) { return entry.second == handle; });
    if (g.target == handle) {
        g.target = kMainTarget;
        if (g.list_open) {
            ApplyTarget();
        }
    }
    if (g.list_open && t->bound_serial == g.serial) {
        Flush();
    }
    if (t->vram) {
        VramFree(t->vram_offset, t->stride * t->pixel_height * 4);
    } else {
        std::free(t->pixels);
        g.texture_bytes -= t->bytes;
    }
    *t = Texture{};
}

namespace {

bool WriteTexels(TextureHandle handle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void *pixels,
                 uint32_t row_length) {
    Texture *t = LookupTexture(handle);
    if (t == nullptr || t->palette || pixels == nullptr) {
        return false;
    }
    if (g.list_open && t->bound_serial == g.serial) {
        Flush();
    }
    const uint32_t bytes = t->desc.format == TextureFormat::Index8 ? 1 : 4;
    const uint32_t row = (row_length != 0 ? row_length : w) * bytes;
    const auto    *src = static_cast<const uint8_t *>(pixels);
    LogicalMapping m = t->render_target ? TextureMapping(*t)
                                        : LogicalMapping{static_cast<float>(t->pixel_width) / t->desc.logical_width,
                                                         static_cast<float>(t->pixel_height) / t->desc.logical_height,
                                                         0.0f, 0.0f, t->pixel_width, t->pixel_height};
    // The update is in the texture's own texels (desc.width x desc.height), which may be stored smaller.
    const float to_logical_x = static_cast<float>(t->desc.logical_width) / t->desc.width;
    const float to_logical_y = static_cast<float>(t->desc.logical_height) / t->desc.height;
    const float sx = m.scale_x * to_logical_x;
    const float sy = m.scale_y * to_logical_y;
    int px0 = static_cast<int>(std::floor(x * sx + m.offset_x));
    int py0 = static_cast<int>(std::floor(y * sy + m.offset_y));
    int px1 = static_cast<int>(std::ceil((x + w) * sx + m.offset_x));
    int py1 = static_cast<int>(std::ceil((y + h) * sy + m.offset_y));
    px0 = std::clamp(px0, 0, static_cast<int>(t->pixel_width));
    py0 = std::clamp(py0, 0, static_cast<int>(t->pixel_height));
    px1 = std::clamp(px1, px0, static_cast<int>(t->pixel_width));
    py1 = std::clamp(py1, py0, static_cast<int>(t->pixel_height));
    uint8_t *base = t->vram ? VramUncached(t->vram_offset) : t->pixels;
    const bool one_to_one = sx == 1.0f && sy == 1.0f && m.offset_x == 0.0f && m.offset_y == 0.0f;
    for (int py = py0; py < py1; py++) {
        uint32_t sy_texel = one_to_one ? static_cast<uint32_t>(py) - y
                                       : std::min(h - 1, static_cast<uint32_t>(std::max(0.0f, (py + 0.5f - m.offset_y) / sy - y)));
        uint8_t *dst = base + (static_cast<uint32_t>(py) * t->stride + static_cast<uint32_t>(px0)) * bytes;
        const uint8_t *src_row = src + sy_texel * row;
        if (one_to_one) {
            std::memcpy(dst, src_row + (static_cast<uint32_t>(px0) - x) * bytes, static_cast<size_t>(px1 - px0) * bytes);
        } else {
            for (int px = px0; px < px1; px++) {
                uint32_t sx_texel = std::min(w - 1, static_cast<uint32_t>(std::max(0.0f, (px + 0.5f - m.offset_x) / sx - x)));
                std::memcpy(dst, src_row + sx_texel * bytes, bytes);
                dst += bytes;
            }
        }
        if (bytes == 4 && !t->desc.has_alpha && !t->render_target) {
            // TEXA with AEM: black texels are transparent, the rest take TA0 0x80, here 0xFF.
            auto *texels = reinterpret_cast<uint32_t *>(base + (static_cast<uint32_t>(py) * t->stride + static_cast<uint32_t>(px0)) * 4);
            for (int px = px0; px < px1; px++, texels++) {
                *texels = (*texels & 0x00FFFFFFu) == 0 ? 0 : (*texels | 0xFF000000u);
            }
        }
    }
    if (!t->vram) {
        sceKernelDcacheWritebackRange(t->pixels, t->stride * t->pot_height * bytes);
    }
    t->dirty = true;
    return true;
}

} // namespace

bool UpdateTexture(TextureHandle texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                   const void *pixels, uint32_t row_length) {
    if (mip != 0) {
        return true; // ponytail: the GE samples level 0 only; mip chains would cut texture aliasing.
    }
    return WriteTexels(texture, x, y, w, h, pixels, row_length);
}

bool UpdateTextureNow(TextureHandle texture, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      const void *pixels, uint32_t row_length) {
    return UpdateTexture(texture, mip, x, y, w, h, pixels, row_length);
}

bool UpdatePalette(TextureHandle palette, const uint32_t *rgba, uint32_t first, uint32_t count) {
    Texture *t = LookupTexture(palette);
    if (t == nullptr || !t->palette || first >= 256) {
        return false;
    }
    count = std::min(count, 256 - first);
    std::memcpy(t->pixels + first * 4, rgba, count * 4);
    // Draws already queued keep the copy they were given (BindTexture).
    t->clut_copy = nullptr;
    return true;
}

std::optional<TextureInfo> GetTextureInfo(TextureHandle handle) {
    if (handle == kMainTarget || handle == kPreviousFrame) {
        return TextureInfo{static_cast<uint32_t>(g.config.logical_width), static_cast<uint32_t>(g.config.logical_height),
                           kScreenWidth, kScreenHeight, TextureFormat::Rgba8, 1, true, true, false, false};
    }
    const Texture *t = LookupTexture(handle);
    if (t == nullptr) {
        return std::nullopt;
    }
    return TextureInfo{t->desc.logical_width, t->desc.logical_height, t->pixel_width, t->pixel_height, t->desc.format,
                       1, t->desc.has_alpha, t->render_target, t->shares_main_depth, t->frame};
}

void ConvertPs2Alpha(uint32_t *rgba, size_t count) {
    uint8_t *bytes = reinterpret_cast<uint8_t *>(rgba);
    for (size_t i = 0; i < count; i++) {
        uint8_t &alpha = bytes[i * 4 + 3];
        alpha = static_cast<uint8_t>(std::min(alpha * 2, 255));
    }
}

// ---- Meshes -----------------------------------------------------------------------------------

MeshHandle CreateMesh(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices) {
    if (vertices.empty() || indices.empty() || vertices.size() > 65536) {
        std::fprintf(stderr, "gfx: mesh of %zu vertices not drawn\n", vertices.size());
        return kNullMesh;
    }
    MeshHandle handle = kNullMesh;
    for (MeshHandle i = 1; i < g.meshes.size(); i++) {
        if (!g.meshes[i].used) {
            handle = i;
            break;
        }
    }
    if (handle == kNullMesh) {
        g.meshes.emplace_back();
        handle = static_cast<MeshHandle>(g.meshes.size() - 1);
    }
    Mesh &m = g.meshes[handle];
    m = Mesh{};
    m.used = true;
    m.vertex_count = static_cast<uint32_t>(vertices.size());
    m.vertices = static_cast<GeVertex3D *>(memalign(16, vertices.size() * sizeof(GeVertex3D)));
    if (m.vertices == nullptr) {
        m.used = false;
        return kNullMesh;
    }
    for (size_t i = 0; i < vertices.size(); i++) {
        m.vertices[i] = ToGe(vertices[i]);
    }
    m.indices.assign(indices.begin(), indices.end());
    g.mesh_bytes += vertices.size() * sizeof(GeVertex3D) + m.indices.size() * sizeof(uint16_t);
    sceKernelDcacheWritebackRange(m.vertices, vertices.size() * sizeof(GeVertex3D));
    sceKernelDcacheWritebackRange(m.indices.data(), m.indices.size() * sizeof(uint16_t));
    return handle;
}

bool UpdateMeshVertices(MeshHandle handle, uint32_t first, std::span<const Vertex3D> vertices) {
    Mesh *m = LookupMesh(handle);
    if (m == nullptr || first + vertices.size() > m->vertex_count) {
        return false;
    }
    if (g.list_open && m->bound_serial == g.serial) {
        Flush();
    }
    for (size_t i = 0; i < vertices.size(); i++) {
        m->vertices[first + i] = ToGe(vertices[i]);
    }
    sceKernelDcacheWritebackRange(m->vertices + first, vertices.size() * sizeof(GeVertex3D));
    return true;
}

void DestroyMesh(MeshHandle handle) {
    Mesh *m = LookupMesh(handle);
    if (m == nullptr) {
        return;
    }
    if (g.list_open && m->bound_serial == g.serial) {
        Flush();
    }
    g.mesh_bytes -= m->vertex_count * sizeof(GeVertex3D) + m->indices.size() * sizeof(uint16_t);
    std::free(m->vertices);
    *m = Mesh{};
}

// ---- Logical space ----------------------------------------------------------------------------

float       LogicalWidth() { return g.config.logical_width; }
float       LogicalHeight() { return g.config.logical_height; }
LogicalRect LogicalFrame() { return {0.0f, 0.0f, g.config.logical_width, g.config.logical_height}; }

MeshTransform IdentityMeshTransform() {
    MeshTransform transform;
    const Mat4    identity = Identity();
    for (float *m : {transform.projection, transform.view, transform.middle, transform.model, transform.local}) {
        std::memcpy(m, identity.data(), sizeof(float) * 16);
    }
    return transform;
}

bool InvertAffineTransform(const float matrix[16], float inverse[16]) {
    Mat4 in;
    Mat4 out;
    std::memcpy(in.data(), matrix, sizeof(float) * 16);
    if (!InvertAffine(in, out)) {
        return false;
    }
    std::memcpy(inverse, out.data(), sizeof(float) * 16);
    return true;
}

// ---- Drawing ----------------------------------------------------------------------------------

void Draw2D(Primitive primitive, std::span<const Vertex2D> vertices, const TextureBinding &texture,
            const DrawState &state, const Presentation2D *) {
    if (g.list && g.list->overlay) {
        DisplayList &list = *g.list;
        list.draws.push_back({primitive, static_cast<uint32_t>(list.vertices.size()),
                              static_cast<uint32_t>(vertices.size()), texture, state});
        list.vertices.insert(list.vertices.end(), vertices.begin(), vertices.end());
        list.stats.draws_2d++;
        return;
    }
    if (g.list) {
        g.list->stats.draws_2d++;
    }
    Draw2DNow(primitive, vertices, texture, state);
}

void DrawMesh(MeshHandle handle, uint32_t first_index, uint32_t index_count, const MeshConstants &constants,
              const TextureBinding &texture, const DrawState &state, const MeshTransform *) {
    Mesh *m = LookupMesh(handle);
    if (m == nullptr || first_index + index_count > m->indices.size()) {
        return;
    }
    if (g.list) {
        g.list->stats.mesh_draws++;
    }
    EnsureListRoom(kMeshCommandBytes);
    m->bound_serial = g.serial;
    DrawMeshNow(m->vertices, m->indices.data() + first_index, index_count, constants, texture, state);
}

void DrawMeshImmediate(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices,
                       const MeshConstants &constants, const TextureBinding &texture, const DrawState &state,
                       const MeshTransform *) {
    if (!g.list_open || vertices.empty() || indices.empty() || vertices.size() > 65536) {
        return;
    }
    if (g.list) {
        g.list->stats.mesh_draws++;
    }
    const uint32_t vertex_bytes = static_cast<uint32_t>(vertices.size() * sizeof(GeVertex3D));
    const uint32_t index_bytes = static_cast<uint32_t>((indices.size() * sizeof(uint16_t) + 15) & ~size_t{15});
    EnsureListRoom(InList(vertex_bytes) + InList(index_bytes) + kMeshCommandBytes);
    auto *v = static_cast<GeVertex3D *>(ListMemory(vertex_bytes));
    auto *i = static_cast<uint16_t *>(ListMemory(index_bytes));
    for (size_t n = 0; n < vertices.size(); n++) {
        v[n] = ToGe(vertices[n]);
    }
    for (size_t n = 0; n < indices.size(); n++) {
        i[n] = static_cast<uint16_t>(indices[n]);
    }
    DrawMeshNow(v, i, static_cast<uint32_t>(indices.size()), constants, texture, state);
}

void DrawMeshProg0(MeshHandle mesh, uint32_t first_index, uint32_t index_count, const MeshConstants &constants,
                   const Prog0Constants &, const TextureBinding &texture, const DrawState &state,
                   const MeshTransform *transform, const Prog0Palette *) {
    if (!g.warned_prog0) {
        g.warned_prog0 = true;
        std::fprintf(stderr, "gfx: Vu_prog0 draws are drawn as plain meshes on the PSP\n");
    }
    DrawMesh(mesh, first_index, index_count, constants, texture, state, transform);
}

void DrawMeshImmediateProg0(std::span<const Vertex3D> vertices, std::span<const uint32_t> indices,
                            const MeshConstants &constants, const Prog0Constants &, const TextureBinding &texture,
                            const DrawState &state, const MeshTransform *transform, const Prog0Palette *) {
    DrawMeshImmediate(vertices, indices, constants, texture, state, transform);
}

// ---- Targets, copies, blits -------------------------------------------------------------------

void SetRenderTarget(TextureHandle handle) {
    if (handle == g.target) {
        return;
    }
    if (handle != kMainTarget) {
        Texture *t = LookupTexture(handle);
        if (t == nullptr || !t->render_target) {
            return;
        }
    }
    g.target = handle;
    if (g.list_open) {
        ApplyTarget();
    }
}

TextureHandle  CurrentRenderTarget() { return g.target; }
LogicalMapping GetLogicalMapping(TextureHandle target) { return MappingOf(target); }
LogicalMapping GetUiMapping(TextureHandle target) { return MappingOf(target); }
void           SetUiAnchor(const UiAnchor &anchor) { g.anchor = anchor; }
UiAnchor       CurrentUiAnchor() { return g.anchor; }

LogicalRect VisibleLogicalRect(TextureHandle target) {
    LogicalRect frame = LogicalFrame();
    if (!Fills() || (target != kMainTarget && !FrameImage(target))) {
        return frame;
    }
    LogicalMapping m = MappingOf(target);
    float          left = -m.offset_x / m.scale_x;
    float          right = (static_cast<float>(m.pixel_width) - m.offset_x) / m.scale_x;
    return {left, frame.y, right - left, frame.h};
}

void Clear(bool clear_color, const uint8_t color[4], bool clear_depth, float depth, const LogicalRect *rect,
           const LogicalRect *) {
    if (!g.list_open || (!clear_color && !clear_depth) || !TargetOf(g.target).drawable) {
        return;
    }
    EnsureListRoom(1024);
    PlainState();
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_FRAGMENT_2X);
    if (clear_depth) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(GU_ALWAYS);
        sceGuDepthMask(GU_FALSE);
    }
    sceGuPixelMask(clear_color ? 0 : 0xFFFFFFFFu);
    TargetInfo     t = TargetOf(g.target);
    float          x0 = 0.0f;
    float          y0 = 0.0f;
    float          x1 = static_cast<float>(t.width);
    float          y1 = static_cast<float>(t.height);
    if (rect != nullptr) {
        LogicalMapping m = MappingOf(g.target);
        x0 = std::floor(rect->x * m.scale_x + m.offset_x);
        y0 = std::floor(rect->y * m.scale_y + m.offset_y);
        x1 = std::ceil((rect->x + rect->w) * m.scale_x + m.offset_x);
        y1 = std::ceil((rect->y + rect->h) * m.scale_y + m.offset_y);
        if (g.target == kMainTarget && Fills()) {
            if (rect->x <= 0.0f) {
                x0 = 0.0f;
            }
            if (rect->x + rect->w >= g.config.logical_width) {
                x1 = static_cast<float>(t.width);
            }
        }
    }
    uint8_t c[4] = {color[0], color[1], color[2], color[3]};
    DrawPixelRect(x0, y0, x1, y1, 0, 0, 0, 0, Pack(c), std::clamp(depth, 0.0f, 1.0f) * 65535.0f);
}

void ClearStencil(uint8_t, const LogicalRect *) {}

bool CopyTexture(TextureHandle src, Rect src_rect, TextureHandle dst, int32_t dst_x, int32_t dst_y) {
    return Blit(src, src_rect, dst, {dst_x, dst_y, src_rect.w, src_rect.h}, Filter::Nearest);
}

bool BlitTexture(TextureHandle src, Rect src_rect, TextureHandle dst, Rect dst_rect, Filter filter) {
    return Blit(src, src_rect, dst, dst_rect, filter);
}

bool SnapshotFrame(TextureHandle dst) {
    const Texture *t = LookupTexture(dst);
    if (t == nullptr) {
        return false;
    }
    const Rect frame = {0, 0, static_cast<int32_t>(g.config.logical_width), static_cast<int32_t>(g.config.logical_height)};
    return Blit(kMainTarget, frame, dst,
                {0, 0, static_cast<int32_t>(t->desc.logical_width), static_cast<int32_t>(t->desc.logical_height)},
                Filter::Linear);
}

// ---- Readback ---------------------------------------------------------------------------------

// ponytail: depth reads (the lens flare's occlusion) answer nothing yet; the GE's depth buffer
// would have to be read through its linear VRAM mirror after each frame.
void                 ReadDepth(uint32_t, float, float, float, float) {}
std::optional<float> DepthResult(uint32_t) { return std::nullopt; }

bool ReadbackFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) {
    return ReadbackTexture(kPreviousFrame, rgba, width, height);
}

bool ReadbackTexture(TextureHandle texture, std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height) {
    if (g.list_open || (texture != kMainTarget && texture != kPreviousFrame && LookupTexture(texture) == nullptr)) {
        return false;
    }
    width = PixelWidth(texture);
    height = PixelHeight(texture);
    uint32_t bytes = 0;
    TexelAt(texture, 0, 0, bytes);
    pixels.resize(static_cast<size_t>(width) * height * bytes);
    for (uint32_t y = 0; y < height; y++) {
        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * bytes, TexelAt(texture, 0, y, bytes), width * bytes);
    }
    return true;
}

bool WritePng(const std::filesystem::path &, const uint8_t *, uint32_t, uint32_t) { return false; }

// ---- Display lists ----------------------------------------------------------------------------

void SetInterpKey(InterpKey key, bool no_interpolation, float, bool) {
    g.interp_key = key;
    g.interp_no_interpolation = no_interpolation;
}

InterpKey CurrentInterpKey() { return g.interp_key; }
bool      CurrentNoInterpolation() { return g.interp_no_interpolation; }
void      SetInterp2D(Interp2D interpolation) { g.interp_2d = interpolation; }
Interp2D  CurrentInterp2D() { return g.interp_2d; }

void BeginRecording(RecordingKind kind) {
    if (g.list || g.in_frame) {
        return;
    }
    g.list = std::make_shared<DisplayList>();
    g.list->overlay = kind == RecordingKind::Overlay;
    if (!g.list->overlay) {
        g.recording = true;
        g.target = kMainTarget;
        OpenList();
    }
}

DisplayListRef EndRecording() {
    std::shared_ptr<DisplayList> list = std::move(g.list);
    g.list.reset();
    if (list && !list->overlay) {
        g.recording = false;
        CloseList();
    }
    return list;
}

bool Recording() { return g.list != nullptr; }
void CutInterpolation() {}
void CutCameraInterpolation() {}

bool RenderList(const DisplayList &list, float, const RenderOptions &options) {
    if (options.canonical || g.in_frame || g.recording) {
        return !g.in_frame && !g.recording;
    }
    // A display render: the canonical image is still in the draw buffer, so a host overlay is drawn
    // over it before it is shown.
    const DisplayList *overlay = list.overlay ? &list : options.overlay;
    if (overlay != nullptr && !overlay->draws.empty()) {
        g.target = kMainTarget;
        OpenList();
        for (const DisplayList::Draw &draw : overlay->draws) {
            Draw2DNow(draw.primitive, std::span(overlay->vertices).subspan(draw.first, draw.count), draw.binding,
                      draw.state);
        }
        CloseList();
    }
    if (options.present) {
        Present();
    }
    return true;
}

bool PresentCanonical() {
    if (g.in_frame || g.recording) {
        return false;
    }
    Present();
    return true;
}

DisplayListStats ListStats(const DisplayList &list) { return list.stats; }

} // namespace gfx
