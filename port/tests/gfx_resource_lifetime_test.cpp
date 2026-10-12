#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "gfx/displaylist.hpp"
#include "gfx_fixture.hpp"

using namespace dc::test;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 48;
const std::array<uint8_t, 4> kClear = {0, 0, 0, 0x80};
const std::vector<uint32_t> kTriangle = {0, 1, 2};
// Small enough that filling it is cheap on a software rasterizer; the fixture's own textures take
// the first kFirstUserSlot slots of the table.
constexpr uint32_t kSmallTable = 16;
// Above the small table's size; reaching it means the table never filled.
constexpr uint32_t kSafetyCap = 64;

void Identity(float (&m)[16]) {
    std::memset(m, 0, sizeof m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

std::vector<gfx::Vertex3D> RedTriangle() {
    auto vertex = [](float x, float y) {
        return gfx::Vertex3D{{x, y, 0.5f}, {0, 0, 1}, {0.0f, 0.0f}, {255, 0, 0, 255}};
    };
    return {vertex(-0.5f, -0.5f), vertex(0.5f, -0.5f), vertex(0.0f, 0.5f)};
}

gfx::MeshConstants PlainConstants() {
    gfx::MeshConstants constants = {};
    Identity(constants.mvp);
    for (float &channel : constants.diffuse) {
        channel = 1.0f;
    }
    return constants;
}

int RedPixels(GfxFixture &fixture) {
    EXPECT_TRUE(gfx::ReadbackFrame(fixture.pixels, fixture.width, fixture.height));
    int count = 0;
    for (uint32_t y = 0; y < fixture.height; y++) {
        for (uint32_t x = 0; x < fixture.width; x++) {
            count += fixture.Pixel(x, y)[0] > 100;
        }
    }
    return count;
}

GfxOptions SmallTables() {
    GfxOptions options;
    options.texture_slots = kSmallTable;
    options.mesh_slots = kSmallTable;
    return options;
}

std::vector<gfx::TextureHandle> FillTextureSlots() {
    std::vector<gfx::TextureHandle> handles;
    for (uint32_t i = 0; i < kSafetyCap; i++) {
        gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{1, 1});
        if (texture == gfx::kNullTexture) {
            break;
        }
        handles.push_back(texture);
    }
    return handles;
}

std::vector<gfx::MeshHandle> FillMeshSlots() {
    std::vector<gfx::MeshHandle> handles;
    for (uint32_t i = 0; i < kSafetyCap; i++) {
        gfx::MeshHandle mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
        if (mesh == gfx::kNullMesh) {
            break;
        }
        handles.push_back(mesh);
    }
    return handles;
}

} // namespace

TEST(GfxMeshDc2, DoomedMeshDrawIsRecorded) {
    GfxFixture          fixture(kWidth, kHeight);
    gfx::MeshHandle     mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(mesh, gfx::kNullMesh);
    gfx::MeshConstants  constants = PlainConstants();
    fixture.Frame(kClear, [&] { gfx::DrawMesh(mesh, 0, 3, constants, {}, gfx::DrawState{}); });
    ASSERT_GT(RedPixels(fixture), 0) << "the undoomed draw does not paint red";

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::Clear(true, kClear.data(), true, 0.0f);
    gfx::DestroyMesh(mesh);
    gfx::DrawMesh(mesh, 0, 3, constants, {}, gfx::DrawState{});
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).mesh_draws, 1u);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    ASSERT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
    EXPECT_GT(RedPixels(fixture), 0);
}

TEST(GfxMeshDc2, DoomedTextureCopyIsRecorded) {
    GfxFixture      fixture(kWidth, kHeight);
    gfx::TextureHandle a = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    gfx::TextureHandle b = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    ASSERT_NE(a, gfx::kNullTexture);
    ASSERT_NE(b, gfx::kNullTexture);

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::DestroyTexture(b);
    EXPECT_TRUE(gfx::CopyTexture(b, gfx::Rect{0, 0, 4, 4}, a, 0, 0));
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).stateful, 1u);

    gfx::RenderOptions canonical;
    canonical.canonical = true;
    EXPECT_TRUE(gfx::RenderList(*list, 1.0f, canonical));
    list.reset();
    gfx::DestroyTexture(a);
}

TEST(GfxMeshDc2, DoomedTextureUpdateIsRecorded) {
    GfxFixture      fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{2, 2});
    ASSERT_NE(texture, gfx::kNullTexture);
    const uint8_t texel[4] = {0, 255, 0, 255};

    gfx::DisplayListRef list;
    gfx::BeginRecording();
    gfx::DestroyTexture(texture);
    EXPECT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, texel));
    list = gfx::EndRecording();
    ASSERT_TRUE(list != nullptr);
    EXPECT_EQ(gfx::ListStats(*list).stateful, 1u);
    list.reset();
}

TEST(GfxMeshDc2, SecondDestroyOfDoomedHandleIsHarmless) {
    GfxFixture      fixture(kWidth, kHeight);
    gfx::TextureHandle texture = gfx::CreateTexture(gfx::TextureDesc{4, 4});
    gfx::MeshHandle    mesh = gfx::CreateMesh(RedTriangle(), kTriangle);
    ASSERT_NE(texture, gfx::kNullTexture);
    ASSERT_NE(mesh, gfx::kNullMesh);

    {
        gfx::DisplayListRef list;
        gfx::BeginRecording();
        gfx::DestroyTexture(texture);
        gfx::DestroyTexture(texture);
        gfx::DestroyMesh(mesh);
        gfx::DestroyMesh(mesh);
        list = gfx::EndRecording();
        ASSERT_TRUE(list != nullptr);
    }
    fixture.Frame(kClear, [] {});
    fixture.Frame(kClear, [] {});
}

TEST(GfxMeshDc2, CreateAtTheSlotLimitRetiresDestroyedSlots) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::TextureHandle> handles = FillTextureSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the slot table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::TextureHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyTexture(freed);
    gfx::TextureHandle again = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    EXPECT_NE(again, gfx::kNullTexture);
    if (again != gfx::kNullTexture) {
        handles.push_back(again);
    }
    for (gfx::TextureHandle texture : handles) {
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxMeshDc2, DestroyedSlotsReturnAfterTwoFrames) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::TextureHandle> handles = FillTextureSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the slot table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::TextureHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyTexture(freed);
    fixture.Frame(kClear, [] {});
    fixture.Frame(kClear, [] {});
    gfx::TextureHandle again = gfx::CreateTexture(gfx::TextureDesc{1, 1});
    EXPECT_NE(again, gfx::kNullTexture);
    if (again != gfx::kNullTexture) {
        handles.push_back(again);
    }
    for (gfx::TextureHandle texture : handles) {
        gfx::DestroyTexture(texture);
    }
}

TEST(GfxMeshDc2, CreateMeshAtTheSlotLimitRetiresDestroyedSlots) {
    GfxFixture fixture(kWidth, kHeight, 1.0f, SmallTables());
    std::vector<gfx::MeshHandle> handles = FillMeshSlots();
    ASSERT_LT(handles.size(), kSafetyCap) << "the mesh table never reported full";
    ASSERT_FALSE(handles.empty());

    gfx::MeshHandle freed = handles.back();
    handles.pop_back();
    gfx::DestroyMesh(freed);
    gfx::MeshHandle again = gfx::CreateMesh(RedTriangle(), kTriangle);
    EXPECT_NE(again, gfx::kNullMesh);
    if (again != gfx::kNullMesh) {
        handles.push_back(again);
    }
    for (gfx::MeshHandle mesh : handles) {
        gfx::DestroyMesh(mesh);
    }
}
