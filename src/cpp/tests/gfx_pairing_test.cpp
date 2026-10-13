#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "gfx/displaylist.hpp"
#include "tests/gfx_fixture.hpp"

using namespace dc::test;

TEST(GfxMeshDc2, SkippedAndInsertedPartsResynchronizeSurfacePairing) {
    GfxOptions options;
    options.offscreen = true;
    GfxFixture                      fixture(64, 48, 1, options);
    std::vector<gfx::TextureHandle> textures;
    const uint8_t                   white[4] = {128, 128, 128, 128};
    for (int i = 0; i < 4; ++i) {
        const auto texture = gfx::CreateTexture({1, 1});
        ASSERT_TRUE(gfx::UpdateTexture(texture, 0, 0, 0, 1, 1, white));
        textures.push_back(texture);
    }
    const std::vector<gfx::Vertex3D> vertices = {
        {{-.1f, -.1f, .5f}}, {{.1f, -.1f, .5f}}, {{0, .1f, .5f}}};
    const std::vector<uint32_t> indices{0, 1, 2};
    auto                        record = [&](const std::vector<int> &parts, float x, bool mirrored = false) {
        gfx::BeginRecording();
        gfx::Clear(true, white, true, 0);
        gfx::SetInterpKey(123);
        auto transform = gfx::IdentityMeshTransform();
        transform.model[12] = x;
        if (mirrored) {
            transform.model[0] = -1;
        }
        gfx::MeshConstants constants{};
        std::memcpy(constants.mvp, transform.model, sizeof constants.mvp);
        for (int part : parts) {
            gfx::TextureBinding binding;
            binding.texture = textures[part];
            gfx::DrawMeshImmediate(vertices, indices, constants, binding, {}, &transform);
        }
        return gfx::EndRecording();
    };
    auto before = record({0, 1, 2}, 0);
    for (const auto &parts : {
             std::vector<int>{0, 2},
             std::vector<int>{0, 3, 1, 2},
             std::vector<int>{0, 3, 2},
             std::vector<int>{2, 0, 1}
    }) {
        auto after = record(parts, .1f);
        ASSERT_TRUE(gfx::RenderList(*after, 1, {.canonical = true}));
        ASSERT_TRUE(gfx::RenderList(*after, .5f, {.previous = before.get(), .present = true}));
        ASSERT_EQ(after->cache.match.size(), parts.size());
        for (size_t i = 0; i < parts.size(); ++i) {
            EXPECT_EQ(after->cache.match[i], parts[i] == 3 ? -1 : parts[i]) << "part " << parts[i];
        }
    }
    auto mirrored = record({0, 1, 2}, .1f, true);
    ASSERT_TRUE(gfx::RenderList(*mirrored, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*mirrored, .5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(mirrored->cache.match, (std::vector<int32_t>{-1, -1, -1}));
    auto repeated = record({0, 2, 2}, .1f);
    ASSERT_TRUE(gfx::RenderList(*repeated, 1, {.canonical = true}));
    ASSERT_TRUE(gfx::RenderList(*repeated, .5f, {.previous = before.get(), .present = true}));
    EXPECT_EQ(repeated->cache.match, (std::vector<int32_t>{0, 2, -1}));
    for (auto texture : textures) {
        gfx::DestroyTexture(texture);
    }
}
