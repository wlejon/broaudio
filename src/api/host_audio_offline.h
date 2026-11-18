#pragma once

// OfflineAudioContext's render: the node graph the context's nodes and their
// connect() edges describe, processed in 128-frame render quanta as Web Audio
// defines it (mixing rules, a-rate / k-rate params, audio-rate param inputs,
// cycles through a DelayNode). The JS half is host_audio_offline.cpp, the
// graph and its quantum loop host_audio_offline_graph.cpp, each node's
// processing host_audio_offline_kernels.cpp.

#include "host_audio_internal.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace broaudio::api {

inline constexpr int kRenderQuantum = 128;

// One output (or input) of a node for one quantum: `channels` planes of
// kRenderQuantum samples.
struct AudioBus {
    int channels = 1;
    std::vector<float> data;  // channels * kRenderQuantum
    float* channel(int c) { return data.data() + static_cast<size_t>(c) * kRenderQuantum; }
    const float* channel(int c) const { return data.data() + static_cast<size_t>(c) * kRenderQuantum; }
    void setSilent(int n) {
        channels = n;
        data.assign(static_cast<size_t>(n) * kRenderQuantum, 0.0f);
    }
};

// Up- or down-mix `in` into `out`'s channel count and add it to `out`
// (Web Audio's speaker rules for 1, 2, 4 and 6 channels, else discrete).
void mixInto(const AudioBus& in, AudioBus& out, bool discrete);

// The render state an OfflineAudioContext owns.
struct OfflineRender {
    int channels = 1;
    int64_t length = 0;
    double sampleRate = 44100.0;
    std::shared_ptr<OfflineClock> clock;
    int64_t frame = 0;                       // frames rendered so far
    std::vector<std::vector<float>> output;  // [channel][length]
};

// Render the graph of OfflineAudioContext `context` (its `_nodes`, their
// `_targets`, its destination and listener) from render.frame up to
// `endFrame`, advancing render.frame and the clock. Reads the JS objects
// first (allocates); the render itself runs no JS.
void renderOfflineGraph(Value context, OfflineRender& render, int64_t endFrame);

// ---- Per-node processing (host_audio_offline_kernels.cpp) -------------------

// A param's values over one quantum: its timeline at each frame's time
// (once for a k-rate param) plus the audio connected to it, clamped to its
// range. `input` may be null (nothing connected).
void computeParam(const HostAudioParam& p, const AudioBus* input, double t0, double sampleRate, float* out);

// What each kind of node does with its (already mixed) inputs and params for
// one quantum. `frame0` is the quantum's first frame. Each keeps what it
// needs across quanta in its node's offlineState.
struct KernelContext {
    double sampleRate = 44100.0;
    int64_t frame0 = 0;
    double time0 = 0.0;
};

void processOscillator(HostOscillatorNode& n, const float* freq, const float* detune, const KernelContext& k,
                       AudioBus& out);
void processConstantSource(HostConstantSourceNode& n, const float* offset, const KernelContext& k, AudioBus& out);
// `data` is the buffer's channels, read when the render starts.
void processBufferSource(HostAudioBufferSourceNode& n, const std::vector<std::vector<float>>& data,
                         double bufferRate, float playbackRate, float detune, const KernelContext& k,
                         AudioBus& out);
void processGain(const AudioBus& in, const float* gain, AudioBus& out);
void processBiquad(HostBiquadFilterNode& n, const AudioBus& in, const float* freq, const float* detune,
                   const float* q, const float* gain, bool perSample, const KernelContext& k, AudioBus& out);
void processIIR(HostIIRFilterNode& n, const AudioBus& in, AudioBus& out);
// The delay line: write the quantum's input, then read the output (a delay
// in a cycle holds at least a quantum).
void delayWrite(HostDelayNode& n, const AudioBus& in, const KernelContext& k);
void delayRead(HostDelayNode& n, const float* delayTime, bool inCycle, const KernelContext& k, AudioBus& out);
void processStereoPanner(const AudioBus& in, const float* pan, AudioBus& out);
struct ListenerState {
    double pos[3] = {0, 0, 0};
    double forward[3] = {0, 0, -1};
    double up[3] = {0, 1, 0};
};
void processPanner(HostPannerNode& n, const AudioBus& in, const ListenerState& listener, const KernelContext& k,
                   AudioBus& out);
void processWaveShaper(const HostWaveShaperNode& n, const AudioBus& in, AudioBus& out);
void processConvolver(HostConvolverNode& n, const std::vector<std::vector<float>>& impulse, double impulseRate,
                      const AudioBus& in, AudioBus& out);
void processCompressor(HostDynamicsCompressorNode& n, const AudioBus& in, const KernelContext& k, AudioBus& out);

}  // namespace broaudio::api
