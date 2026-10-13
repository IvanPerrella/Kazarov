#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>

#include "audio/mixer.hpp"
#include "audio/track_cache.hpp"

namespace {
using namespace std::chrono_literals;

bool Wait(const std::shared_ptr<const audio::PendingTrack> &request) {
    if (!request) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!request->ready.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    return request->ready.load();
}

std::shared_ptr<audio::PcmTrack> Track(size_t samples = 64) {
    auto track = std::make_shared<audio::PcmTrack>();
    track->samples.assign(samples, 8192);
    return track;
}
} // namespace

TEST(AudioTrackCache, GameAndMixerDoNotWaitForBlockedDecode) {
    std::promise<void> entered, release;
    auto               released = release.get_future().share();
    audio::TrackCache  cache([&](const std::string &) {
        entered.set_value();
        released.wait();
        return Track();
    });
    const auto         start = std::chrono::steady_clock::now();
    const auto         request = cache.Request("song");
    const auto         request_time = std::chrono::steady_clock::now() - start;
    const bool         worker_entered = entered.get_future().wait_for(1s) == std::future_status::ready;
    audio::Mixer       mixer;
    mixer.SetPendingStream(0, request);
    mixer.SetVolume(0, 256);
    mixer.Play(0);
    float      samples[128]{};
    const auto tick_start = std::chrono::steady_clock::now();
    for (int tick = 0; tick < 10; ++tick) {
        mixer.Render(samples, 64);
    }
    const auto ticks = std::chrono::steady_clock::now() - tick_start;
    const bool still_pending = !request->ready.load();
    release.set_value(); // Always release before any assertion can exit the test.
    EXPECT_TRUE(worker_entered);
    EXPECT_TRUE(still_pending);
    EXPECT_LT(request_time, 100ms);
    EXPECT_LT(ticks, 100ms);
    ASSERT_TRUE(Wait(request));
    mixer.Render(samples, 64);
    EXPECT_GT(samples[0], .1f);
}

TEST(AudioTrackCache, EvictsByEntryCountAndBytesWhilePlaybackRetainsItsTrack) {
    audio::TrackCache cache([](const std::string &key) { return Track(key == "large" ? 96 : 64); }, 2, 256);
    auto              first = cache.Request("one");
    ASSERT_TRUE(Wait(first));
    auto                                 playing = first->Get();
    std::weak_ptr<const audio::PcmTrack> old = playing;
    first.reset();
    auto second = cache.Request("two");
    ASSERT_TRUE(Wait(second));
    second.reset();
    auto third = cache.Request("three");
    ASSERT_TRUE(Wait(third));
    EXPECT_LE(cache.CachedBytes(), 256u);
    EXPECT_FALSE(old.expired());
    playing.reset();
    EXPECT_TRUE(old.expired());
    third.reset();
    auto large = cache.Request("large");
    ASSERT_TRUE(Wait(large));
    EXPECT_EQ(cache.CachedBytes(), 192u);
    for (int song = 0; song < 20; ++song) {
        auto request = cache.Request(std::to_string(song));
        ASSERT_TRUE(Wait(request));
        EXPECT_LE(cache.CachedBytes(), 256u);
    }
}

TEST(AudioTrackCache, ReloadDiscardsInFlightResultAndBoundsQueue) {
    std::promise<void> entered, release;
    auto               released = release.get_future().share();
    audio::TrackCache  cache([&](const std::string &) {
        entered.set_value();
        released.wait();
        return Track();
    });
    auto               old = cache.Request("old");
    const bool         worker_entered = entered.get_future().wait_for(1s) == std::future_status::ready;
    size_t             accepted = 0;
    for (int i = 0; i < 30; ++i) {
        accepted += cache.Request(std::to_string(i)) != nullptr;
    }
    const auto start = std::chrono::steady_clock::now();
    cache.Reset([](const std::string &) { return Track(128); });
    const auto reset_time = std::chrono::steady_clock::now() - start;
    auto       fresh = cache.Request("new");
    release.set_value();
    EXPECT_TRUE(worker_entered);
    EXPECT_LE(accepted, 16u);
    EXPECT_LT(reset_time, 100ms);
    ASSERT_TRUE(Wait(old));
    EXPECT_EQ(old->Get(), nullptr);
    ASSERT_TRUE(Wait(fresh));
    ASSERT_NE(fresh->Get(), nullptr);
    EXPECT_EQ(fresh->Get()->samples.size(), 128u);
}

TEST(AudioTrackCache, LateDecodeDoesNotRestartStoppedOrReplacedPlayback) {
    audio::Mixer mixer;
    auto         request = std::make_shared<audio::PendingTrack>();
    mixer.SetPendingStream(0, request);
    mixer.SetVolume(0, 256);
    mixer.Play(0);
    mixer.Stop(0);
    request->track = Track();
    request->ready.store(true);
    float samples[128]{};
    mixer.Render(samples, 64);
    EXPECT_EQ(samples[0], 0);
    EXPECT_FALSE(mixer.IsPlaying(0));
    mixer.Play(0);
    mixer.Render(samples, 64);
    EXPECT_GT(samples[0], .1f);
    mixer.SetPendingStream(0, request);
    mixer.SetSequence(0, nullptr);
    mixer.Play(0);
    mixer.Render(samples, 64);
    EXPECT_EQ(samples[0], 0);
}
