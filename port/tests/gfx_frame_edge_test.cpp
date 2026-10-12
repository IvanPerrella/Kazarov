#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

#include "gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr float kLogicalWidth = 512.0f;
constexpr float kLogicalHeight = 480.0f;
const std::array<uint8_t, 4> kBlue = {0, 0, 255, 0x80};
const std::array<uint8_t, 4> kRed = {255, 0, 0, 0x80};
const std::array<uint8_t, 4> kBlack = {0, 0, 0, 0x80};

struct FramePixels {
    int x;
    int y;
    int w;
    int h;
};

// The frame's pixels under MainMapping: the logical 4:3 frame is shown at the narrower of the window's
// width and its 4:3 height, centred.
FramePixels FrameOf(int width, int height) {
    int shown_w = std::min(width, height * 4 / 3);
    int shown_h = shown_w * 3 / 4;
    return {(width - shown_w) / 2, (height - shown_h) / 2, shown_w, shown_h};
}

void DrawFrameWithOutline(GfxFixture &fixture) {
    const std::array<gfx::Vertex2D, 8> outline = {
        Vertex(0, 0, 0, 0, kBlack),     Vertex(511, 0, 0, 0, kBlack),   Vertex(511, 0, 0, 0, kBlack),
        Vertex(511, 479, 0, 0, kBlack), Vertex(511, 479, 0, 0, kBlack), Vertex(0, 479, 0, 0, kBlack),
        Vertex(0, 479, 0, 0, kBlack),   Vertex(0, 0, 0, 0, kBlack),
    };
    fixture.Frame(kBlue, [&] {
        auto red = Quad(0, 0, 512, 480, kRed);
        gfx::Draw2D(gfx::Primitive::Quads, red, {}, gfx::DrawState{});
        gfx::Draw2D(gfx::Primitive::Lines, outline, {}, gfx::DrawState{});
    });
}

// The first pixel along the span that is not the wanted colour, or -1.
int FirstOffColour(const GfxFixture &fixture, int from, int to, bool horizontal, int fixed, int r, int g, int b) {
    for (int i = from; i < to; i++) {
        bool same = horizontal ? fixture.PixelNear(i, fixed, r, g, b) : fixture.PixelNear(fixed, i, r, g, b);
        if (!same) {
            return i;
        }
    }
    return -1;
}

bool AnyBlack(const GfxFixture &fixture, int from, int to, bool horizontal, int fixed) {
    for (int i = from; i < to; i++) {
        std::array<uint8_t, 4> p = horizontal ? fixture.Pixel(i, fixed) : fixture.Pixel(fixed, i);
        if (p[0] <= 2 && p[1] <= 2 && p[2] <= 2) {
            return true;
        }
    }
    return false;
}

void CheckFillFrameEdges(int width, int height) {
    SCOPED_TRACE(testing::Message() << width << "x" << height);
    GfxFixture fixture(width, height, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    DrawFrameWithOutline(fixture);

    const FramePixels frame = FrameOf(width, height);
    const int         mid_row = height / 2;
    const int         mid_col = width / 2;
    ASSERT_GT(frame.w, 0);
    ASSERT_GT(frame.h, 0);

    if (frame.x > 0) {
        EXPECT_TRUE(fixture.PixelNear(frame.x - 1, mid_row, 255, 0, 0));
        EXPECT_TRUE(fixture.PixelNear(frame.x + frame.w, mid_row, 255, 0, 0));
    }
    if (frame.y > 0) {
        EXPECT_TRUE(fixture.PixelNear(mid_col, frame.y - 1, 255, 0, 0));
        EXPECT_TRUE(fixture.PixelNear(mid_col, frame.y + frame.h, 255, 0, 0));
    }

    EXPECT_EQ(FirstOffColour(fixture, 0, frame.x, true, mid_row, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, frame.x + frame.w, width, true, mid_row, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, 0, frame.y, false, mid_col, 255, 0, 0), -1);
    EXPECT_EQ(FirstOffColour(fixture, frame.y + frame.h, height, false, mid_col, 255, 0, 0), -1);

    // The logical right edge (511 of 512) lands scale-dependent pixels short of the frame's far edge.
    const int scale = (frame.w + 511) / 512;
    EXPECT_TRUE(AnyBlack(fixture, frame.x + frame.w - scale - 1, frame.x + frame.w, true, mid_row));
}

} // namespace

TEST(GfxMeshDc2, FillOutlineInWiderAndTallerWindowsStaysInsideTheFrame) {
    CheckFillFrameEdges(1280, 720);
    CheckFillFrameEdges(1366, 768);
    CheckFillFrameEdges(1920, 1080);
    CheckFillFrameEdges(2560, 1440);
    CheckFillFrameEdges(800, 1200);
}

TEST(GfxMeshDc2, FillOutlineFourByThreeKeepsItsRightEdge) {
    SCOPED_TRACE("1024x768");
    GfxFixture fixture(1024, 768, 1.0f, true, kLogicalWidth, kLogicalHeight);
    gfx::SetFrameLayout(gfx::FrameLayout{gfx::AspectMode::Fill, 1.0f});
    DrawFrameWithOutline(fixture);

    const FramePixels frame = FrameOf(1024, 768);
    ASSERT_EQ(frame.x, 0);
    ASSERT_EQ(frame.w, 1024);
    EXPECT_TRUE(AnyBlack(fixture, frame.x + frame.w - 3, frame.x + frame.w, true, 384));
}
