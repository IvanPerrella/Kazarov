#include "audio/synth.hpp"

#include <algorithm>
#include <cmath>

namespace audio {

void Envelope::KeyOn(std::uint16_t adsr1, std::uint16_t adsr2) {
    adsr1_ = adsr1;
    adsr2_ = adsr2;
    level_ = 0;
    counter_ = 0;
    phase_ = Phase::Attack;
}

void Envelope::KeyOff() {
    if (phase_ != Phase::Off) {
        phase_ = Phase::Release;
        counter_ = 0;
    }
}

void Envelope::Step(int shift, int step, bool exponential, bool decrease) {
    int cycles = 1 << std::max(0, shift - 11);
    step <<= std::max(0, 11 - shift);
    if (exponential && !decrease && level_ > 0x6000) {
        cycles *= 4;
    }
    if (exponential && decrease) {
        // Floor rather than truncate, so an exponential release still reaches zero.
        step = (step * level_) >> 15;
    }
    if (++counter_ < cycles) {
        return;
    }
    counter_ = 0;
    level_ = std::clamp(level_ + step, 0, kEnvelopeMax);
}

void Envelope::Tick() {
    switch (phase_) {
        case Phase::Off:
            return;
        case Phase::Attack: {
            const int rate = (adsr1_ >> 8) & 0x7F;
            Step(rate >> 2, 7 - (rate & 3), (adsr1_ & 0x8000) != 0, false);
            if (level_ >= kEnvelopeMax) {
                phase_ = Phase::Decay;
                counter_ = 0;
            }
            return;
        }
        case Phase::Decay:
            Step((adsr1_ >> 4) & 0x0F, -8, true, true);
            if (level_ <= SustainLevel()) {
                phase_ = Phase::Sustain;
                counter_ = 0;
            }
            return;
        case Phase::Sustain: {
            const int  rate = (adsr2_ >> 6) & 0x7F;
            const bool decrease = (adsr2_ & 0x4000) != 0;
            Step(rate >> 2, decrease ? -8 + (rate & 3) : 7 - (rate & 3), (adsr2_ & 0x8000) != 0, decrease);
            return;
        }
        case Phase::Release:
            Step(adsr2_ & 0x1F, -8, (adsr2_ & 0x20) != 0, true);
            if (level_ == 0) {
                phase_ = Phase::Off;
            }
            return;
    }
}

Voice &Synth::Allocate(int core) {
    Voice *const first = voices_.data() + core * kCoreVoices;
    Voice *const last = first + kCoreVoices;
    for (Voice *v = first; v != last; v++) {
        if (!v->active) {
            return *v;
        }
    }
    Voice *victim = nullptr;
    for (Voice *v = first; v != last; v++) {
        if (v->released && (victim == nullptr || v->envelope.Level() < victim->envelope.Level())) {
            victim = v;
        }
    }
    if (victim == nullptr) {
        victim = std::min_element(first, last, [](const Voice &a, const Voice &b) { return a.serial < b.serial; });
    }
    return *victim;
}

void Synth::Start(Voice &voice, std::uint16_t adsr1, std::uint16_t adsr2) {
    voice.active = true;
    voice.released = false;
    voice.held = false;
    voice.position = 0.0;
    voice.fresh = true;
    voice.ticks = 0.0;
    voice.serial = ++serial_;
    voice.current_l = voice.current_r = 0.0f;
#ifdef __PSP__
    // The blocks decoded for the voice's last sample are not this one's.
    voice.blocks[0].index = voice.blocks[1].index = -1;
#endif
    voice.envelope.KeyOn(adsr1, adsr2);
}

int Synth::FreeVoices(int core) const {
    const auto first = voices_.begin() + core * kCoreVoices;
    return static_cast<int>(
        std::count_if(first, first + kCoreVoices, [](const Voice &v) { return !v.active; }));
}

int Synth::ActiveVoices() const {
    return static_cast<int>(std::count_if(voices_.begin(), voices_.end(), [](const Voice &v) { return v.active; }));
}

void Synth::Render(int frames, float *dry, float *const send[kCores]) {
    for (Voice &voice : voices_) {
        if (voice.active) {
            RenderVoice(voice, frames, dry, send[voice.Core()]);
        }
    }
}

namespace {

#ifdef __PSP__
using SampleIndex = std::int32_t; // 64-bit division is a library call on 32-bit MIPS
#else
using SampleIndex = std::int64_t;
#endif

float Fetch(Voice &voice, const VagSample &sample, SampleIndex index) {
    const auto size = static_cast<SampleIndex>(sample.Frames());
    if (index < 0) {
        return 0.0f;
    }
    if (index >= size) {
        if (!sample.loops) {
            return 0.0f;
        }
        const SampleIndex length = size - sample.loop_start;
        index = sample.loop_start + (index - sample.loop_start) % length;
    }
#ifdef __PSP__
    constexpr SampleIndex kBlockSamples = 28;
    const SampleIndex     block = index / kBlockSamples;
    for (const Voice::Block &cached : voice.blocks) {
        if (cached.index == block) {
            return cached.samples[index % kBlockSamples] * (1.0f / 32768.0f);
        }
    }
    Voice::Block &fresh = voice.blocks[voice.next_block];
    voice.next_block ^= 1;
    sample.DecodeBlock(static_cast<std::size_t>(block), fresh.samples);
    fresh.index = block;
    return fresh.samples[index % kBlockSamples] * (1.0f / 32768.0f);
#else
    (void)voice;
    return sample.pcm[index] * (1.0f / 32768.0f);
#endif
}

} // namespace

void Synth::RenderVoice(Voice &voice, int frames, float *dry, float *send) {
    const VagSample &sample = *voice.sample;
    const auto       size = static_cast<double>(sample.Frames());
    const double     loop = sample.loops ? size - sample.loop_start : 0.0;
    const double     tick = static_cast<double>(kSpuRate) / rate_;
    if (voice.fresh) {
        voice.current_l = voice.gain_l;
        voice.current_r = voice.gain_r;
        voice.fresh = false;
    }
    // A one-pole glide of a few milliseconds keeps volume and pan changes from clicking.
    const float glide = std::min(1.0f, 240.0f / rate_);
    const float dry_l = (voice.mix & kMixDryLeft) ? 1.0f : 0.0f;
    const float dry_r = (voice.mix & kMixDryRight) ? 1.0f : 0.0f;
    const float wet_l = (voice.mix & kMixWetLeft) ? 1.0f : 0.0f;
    const float wet_r = (voice.mix & kMixWetRight) ? 1.0f : 0.0f;

#ifdef __PSP__
    // The PSP's FPU is single precision and it has no 64-bit divide, so double arithmetic for every
    // sample of every voice took the whole CPU. The voice steps through its sample in 16.16 fixed
    // point (the SPU's own pitch is 4.12) and counts envelope ticks in a float; the double position
    // is read and written once a block.
    const auto    length = static_cast<std::uint32_t>(sample.Frames());
    const auto    loop_start = static_cast<std::uint32_t>(sample.loop_start);
    const auto    loop_length = sample.loops && loop_start < length ? length - loop_start : 0u;
    std::uint32_t whole = static_cast<std::uint32_t>(voice.position);
    std::uint32_t fraction = static_cast<std::uint32_t>((voice.position - whole) * 65536.0);
    const auto    step = static_cast<std::uint32_t>(voice.step * 65536.0 + 0.5);
    const auto    tick_f = static_cast<float>(tick);
    float         ticks = static_cast<float>(voice.ticks);
    for (int i = 0; i < frames; i++) {
        ticks += tick_f;
        while (ticks >= 1.0f) {
            voice.envelope.Tick();
            ticks -= 1.0f;
        }
        if (voice.envelope.CurrentPhase() == Envelope::Phase::Off) {
            voice.active = false;
            return;
        }

        // Linear interpolation: two reads a sample rather than the cubic's four.
        const auto  index = static_cast<std::int32_t>(whole);
        const float t = static_cast<float>(fraction) * (1.0f / 65536.0f);
        const float p1 = Fetch(voice, sample, index);
        const float p2 = Fetch(voice, sample, index + 1);
        const float value = p1 + t * (p2 - p1);
        const float level = voice.envelope.Level() * (1.0f / kEnvelopeMax) * value;

        voice.current_l += (voice.gain_l - voice.current_l) * glide;
        voice.current_r += (voice.gain_r - voice.current_r) * glide;
        const float left = level * voice.current_l;
        const float right = level * voice.current_r;
        dry[i * 2] += left * dry_l;
        dry[i * 2 + 1] += right * dry_r;
        send[i * 2] += left * wet_l;
        send[i * 2 + 1] += right * wet_r;

        fraction += step;
        whole += fraction >> 16;
        fraction &= 0xFFFF;
        if (whole >= length) {
            if (loop_length == 0) {
                // The SPU mutes a voice that reaches an end block without the repeat flag.
                voice.active = false;
                voice.envelope.Kill();
                return;
            }
            whole = loop_start + (whole - loop_start) % loop_length;
        }
    }
    voice.position = whole + fraction * (1.0 / 65536.0);
    voice.ticks = ticks;
    (void)size;
    (void)loop;
#else
    for (int i = 0; i < frames; i++) {
        voice.ticks += tick;
        while (voice.ticks >= 1.0) {
            voice.envelope.Tick();
            voice.ticks -= 1.0;
        }
        if (voice.envelope.CurrentPhase() == Envelope::Phase::Off) {
            voice.active = false;
            return;
        }

        const auto  index = static_cast<std::int64_t>(voice.position);
        const auto  t = static_cast<float>(voice.position - index);
        const float p0 = Fetch(voice, sample, index - 1);
        const float p1 = Fetch(voice, sample, index);
        const float p2 = Fetch(voice, sample, index + 1);
        const float p3 = Fetch(voice, sample, index + 2);
        const float value = p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
        const float level = voice.envelope.Level() * (1.0f / kEnvelopeMax) * value;

        voice.current_l += (voice.gain_l - voice.current_l) * glide;
        voice.current_r += (voice.gain_r - voice.current_r) * glide;
        const float left = level * voice.current_l;
        const float right = level * voice.current_r;
        dry[i * 2] += left * dry_l;
        dry[i * 2 + 1] += right * dry_r;
        send[i * 2] += left * wet_l;
        send[i * 2 + 1] += right * wet_r;

        voice.position += voice.step;
        if (voice.position >= size) {
            if (!sample.loops || loop <= 0.0) {
                // The SPU mutes a voice that reaches an end block without the repeat flag.
                voice.active = false;
                voice.envelope.Kill();
                return;
            }
            voice.position = sample.loop_start + std::fmod(voice.position - sample.loop_start, loop);
        }
    }
#endif
}

} // namespace audio
