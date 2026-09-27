#pragma once

#include "broaudio/dsp/param_ramp.h"
#include "broaudio/spatial/air_absorption.h"
#include "broaudio/spatial/listener.h"

#include <cstdint>
#include <vector>

namespace broaudio {

// The per-voice (per-playback) signal chain, as explicit stages over block
// buffers:
//
//   [source] -> air -> delay -> gain/pan -> head+occlusion -> bus / send taps
//
// The spatializer (Engine, once per block per playback) fills a plain
// VoiceChainParams; runVoiceChain walks the stages the `stages` mask names.
// The mask is the chain's shape: compiled kernels (spatial/voice_jit.h), an
// air kernel over batches of voices and a chain kernel per shape for the
// stages after it, read and write VoiceChainState in place and replace
// runVoiceChain bit for bit. A change to any stage below must be mirrored in
// src/spatial/voice_jit_builder.cpp.
//
// The source stage: a playback driven by a synthesis graph (synth/
// synth_graph.h) renders its voice into ch[0] at the head of the chain (mono).
// Clip and stream playbacks have no source stage; the mixer's readers fill
// the input buffers before the chain runs. The synthesis layers have their
// own compiled kernels (one per layer shape), so the chain kernel of a shape
// with a source is the one of the same shape without it.
class SynthVoice;

enum SpatialStage : uint32_t {
    kStageSource = 1u << 0,   // synthesis voice, see above
    kStageAir    = 1u << 1,   // ISO 9613-1 air absorption (AirFilterTable)
    kStageDelay  = 1u << 2,   // propagation delay line (Doppler falls out of it)
    kStageHead   = 1u << 3,   // head shadow ILD + lowpass, and occlusion
    kStageSend   = 1u << 4,   // aux send tap
};

// Propagation delay memory for one playback: planar channels, power-of-two
// capacity. Allocated on the control thread, handed to the audio thread by
// pointer, replaced only through an RCU retire.
struct PropagationDelayBuffer {
    int channels = 1;
    int capacity = 0;   // power of two
    int mask = 0;
    std::vector<float> data;   // [channel][capacity]
    float* channel(int c) { return data.data() + static_cast<size_t>(c) * capacity; }
};

// Per-block parameters. Targets are block-end values; the stages interpolate
// or smooth toward them per sample.
struct VoiceChainParams {
    uint32_t stages = 0;
    int channels = 1;                   // source channels, 1 or 2

    // Source: the voice renders block frames [sourceFrom, sourceTo) (a
    // scheduled start or stop inside the block); the rest is silence.
    SynthVoice* source = nullptr;
    int sourceFrom = 0;
    int sourceTo = 0;

    // Air: section pole and mix targets (AirFilterTable::lookup).
    float airPole[kAirSections] = {};
    float airMix[kAirSections] = {};

    // Delay: target in samples, the cap (buffer bound), and the smoothing
    // coefficient of each of the two cascaded one-poles the delay follows.
    PropagationDelayBuffer* delayBuf = nullptr;
    float delayTarget = 0.0f;
    float delayMax = 0.0f;
    float delaySmooth = 0.0f;

    // Gain/pan: spatial distance gain (de-zippered here; the playback's own
    // gain is a ParamRamp in the state) and pan / stereo balance.
    float distanceGain = 1.0f;
    float pan = 0.0f;

    HeadParams head;

    float* bus = nullptr;               // interleaved stereo accumulation target
    float* send = nullptr;              // interleaved stereo send target, or null
};

// Audio-thread state of one voice's chain.
struct VoiceChainState {
    // Air
    bool airPrimed = false;
    float airPole[kAirSections] = {};
    float airMix[kAirSections] = {};
    float airZ[2][kAirSections] = {};   // lowpass outputs
    float airX[2][kAirSections] = {};   // previous section inputs

    // Delay
    const PropagationDelayBuffer* delayBufSeen = nullptr;
    bool delayPrimed = false;
    int writePos = 0;
    float delayS1 = 0.0f, delayS2 = 0.0f, delayOut = 0.0f;

    // Gain / pan / send
    ParamRamp gain;          // playback gain: ramp or de-zipper
    ParamRamp distanceGain;  // spatial distance gain: de-zipper
    ParamRamp pan;
    ParamRamp send;          // send amount: ramp or de-zipper
    float panL = 0.70710678f, panR = 0.70710678f, panCached = 0.0f;
    bool panValid = false;

    SpatialFilter head;
};

// --- Stages. `ch` are the planar source channels (1 or 2), processed in place.

// The source stage into ch0 (n frames). `compiled`: the voice's layers run
// their kernels where published. Returns whether every layer ran compiled.
bool chainSource(const VoiceChainParams& p, float* ch0, int n, bool compiled);

// Poles and mixes move linearly from the previous block's values to these.
void chainAir(VoiceChainState& s, const float* poleTarget, const float* mixTarget,
              float* const* ch, int nch, int n);

// Returns the delay (samples) reached at the end of the block.
float chainDelay(VoiceChainState& s, PropagationDelayBuffer& buf, float target,
                 float maxDelay, float smooth, float* const* ch, int nch, int n);

// Source channels -> stereo L/R with playback gain x distance gain and pan
// (equal-power for mono, balance for stereo, as the mixer always did).
void chainGainPan(VoiceChainState& s, float distanceGain, float pan,
                  const float* const* ch, int nch, float* L, float* R, int n);

void chainHead(VoiceChainState& s, const HeadParams& hp, float* L, float* R, int n);

// bus += L/R; send += L/R * send amount (per-sample ramp) when send != null.
void chainTaps(VoiceChainState& s, const float* L, const float* R,
               float* bus, float* send, int n);

// Whole chain for one block. `ch` holds the source block (modified); L/R are
// n-frame scratch buffers.
void runVoiceChain(const VoiceChainParams& p, VoiceChainState& s,
                   float* const* ch, float* L, float* R, int n);

} // namespace broaudio
