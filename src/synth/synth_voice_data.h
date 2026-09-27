#pragma once

// A synthesis voice's state (synth/synth_graph.h's SynthVoice): the resolved
// parameter values, each layer's constants, carried words and memory, the
// envelope runs, and the end detection. synth_voice.cpp runs it;
// synth_loop.cpp drives it to cut loops.

#include "synth_plan.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace broaudio {

struct SynthEnvRun {
    const EnvDef* def = nullptr;
    int word = 0;        // y
    int kBase = 0;       // m, c
    int seg = 0;
    int64_t remaining = 0;
    int64_t gateAt = -1;
    bool holding = false;
    bool done = false;
    bool released = false;
};

struct SynthLayerRun {
    const LayerData* L = nullptr;
    const SynthShape* shape = nullptr;
    std::vector<float> k;
    std::vector<int32_t> ki;
    std::vector<uint32_t> words;
    std::vector<std::vector<float>> bufMem;
    std::vector<float*> bufs;
    std::vector<SynthEnvRun> envs;
    int64_t offset = 0;
    int64_t time = 0;
    bool started = false;
};

struct SynthVoiceData {
    int fs = 48000;
    std::vector<float> values;
    std::vector<SynthLayerRun> layers;
    int64_t pos = 0;
    int64_t end = -1;
    int64_t duration = -1;
    bool done = false;
    // Never ends: no duration, no end of the envelopes, no silence floor
    // (a loop's source).
    bool endless = false;
    int ampPending = 0;
    bool ampDone = false;
    int64_t ampDoneAt = -1;
    float winPeak = 0.0f;
    int winFill = 0;

    // Resolve the trigger's values (snapped to whole cycles over
    // `snapFrames` when > 0) and build every layer's state.
    void init(const SynthGraphData& g, int sampleRate, const SynthTrigger& t, int64_t snapFrames);
    // The next n frames into out (overwriting), as SynthVoice::render.
    bool render(float* out, int n, bool compiled);
    // Note-off for every envelope, at each layer's current time.
    void releaseAll();
    // An independent copy (its delay lines its own) that continues exactly
    // as this one would.
    std::unique_ptr<SynthVoiceData> clone() const;

    float level(int param) const { return param >= 0 ? values[param] : 0.0f; }
    int64_t frames(int param) const;

    void finish(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    void enter(SynthLayerRun& lr, SynthEnvRun& e, int s, int64_t lt);
    void release(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    void advance(SynthLayerRun& lr, SynthEnvRun& e, int64_t lt);
    bool runLayer(SynthLayerRun& lr, float* out, int n, bool compiled);
    void snap(const SynthGraphData& g, int64_t loopFrames);
};

// A looping voice's loop, its release tail and where it is in them.
struct SynthLoopData {
    std::vector<float> loop;
    std::vector<float> tail;      // the voice released at the loop's seam
    std::vector<float> fadeIn;    // release crossfade gains, fade frames
    std::vector<float> fadeOut;
    int64_t pos = 0;              // in the loop
    int64_t rel = -1;             // frames since the release, -1 while looping
    int64_t relFrom = 0;          // loop position of the release
    int64_t played = 0;           // frames rendered
    int64_t end = -1;
    bool done = false;
};

// Cut the loop from `v` (initialised with snapFrames = the loop's length when
// snapping, not yet rendered); `v` is left spent.
std::unique_ptr<SynthLoopData> buildSynthLoop(SynthVoiceData& v, const SynthGraphData& g,
                                              const SynthLoopOptions& o, bool compiled);

// The loop's length in frames at `fs` (what init's snapFrames must be).
int64_t synthLoopFrames(const SynthLoopOptions& o, int fs);

} // namespace broaudio
