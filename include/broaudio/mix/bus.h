#pragma once

#include "broaudio/types.h"
#include "broaudio/dsp/params.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/chorus.h"
#include "broaudio/dsp/compressor.h"
#include "broaudio/dsp/delay.h"
#include "broaudio/dsp/equalizer.h"
#include "broaudio/dsp/reverb.h"

#include <atomic>
#include <cstring>
#include <vector>

namespace broaudio {

// A mix bus aggregates audio from voices, clips, or child buses,
// applies its own effect chain, and feeds into a parent bus.
// Parameters are atomic (main thread writes, audio thread reads).
// Effect state (filter z-values, delay buffer) is audio-thread-only.
struct Bus {
    static constexpr int MAX_FILTERS = 4;
    static constexpr int NUM_EFFECT_SLOTS = static_cast<int>(EffectSlot::Count);

    int id = 0;

    // Parameters (main thread → audio thread)
    std::atomic<float> gain{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<bool> muted{false};
    std::atomic<int> parentId{-1};   // -1 = master (no parent), 0+ = parent bus id
    std::atomic<int> sendBusId{-1};  // aux send target (-1 = none)
    std::atomic<float> sendAmount{0.0f}; // aux send level (0-1, post-fader)

    // Per-bus effect parameters
    FilterParams filterParams[MAX_FILTERS];
    DelayParams delayParams;
    CompressorParams compressorParams;
    ReverbParams reverbParams;
    ChorusParams chorusParams;
    EqualizerParams eqParams;

    // Effect processing order (main thread writes, audio thread reads)
    std::atomic<uint8_t> effectOrder[NUM_EFFECT_SLOTS] = {
        static_cast<uint8_t>(EffectSlot::Filter),
        static_cast<uint8_t>(EffectSlot::Delay),
        static_cast<uint8_t>(EffectSlot::Compressor),
        static_cast<uint8_t>(EffectSlot::Chorus),
        static_cast<uint8_t>(EffectSlot::Reverb),
        static_cast<uint8_t>(EffectSlot::Equalizer)
    };
    std::atomic<uint32_t> effectOrderVersion{0};

    // Audio-thread-only effect state — never touched by main thread
    std::vector<float> buffer;       // stereo interleaved scratch, sized at init
    BiquadFilter filters[MAX_FILTERS];
    uint32_t filterVersions[MAX_FILTERS] = {};
    DelayEffect delay;
    uint32_t delayVersion = 0;
    Compressor compressor;
    uint32_t compressorVersion = 0;
    Reverb reverb;
    uint32_t reverbVersion = 0;
    Chorus chorus;
    uint32_t chorusVersion = 0;
    Equalizer equalizer;
    uint32_t eqVersion = 0;
    uint8_t effectOrderCache[NUM_EFFECT_SLOTS] = {0, 1, 2, 3, 4, 5};
    uint32_t effectOrderVersionSeen = 0;

    // Metering (audio thread writes, main thread reads)
    std::atomic<float> peakL{0.0f};
    std::atomic<float> peakR{0.0f};
    std::atomic<float> rmsL{0.0f};
    std::atomic<float> rmsR{0.0f};

    void initAudioState(int sampleRate, int maxFrames) {
        buffer.resize(static_cast<size_t>(maxFrames) * 2, 0.0f);
        delay.init(sampleRate * 2);
        compressor.init(sampleRate);
        reverb.init(sampleRate);
        chorus.init(sampleRate);
    }

    void clearBuffer(int numFrames) {
        std::memset(buffer.data(), 0,
                    static_cast<size_t>(numFrames) * 2 * sizeof(float));
    }
};

} // namespace broaudio
