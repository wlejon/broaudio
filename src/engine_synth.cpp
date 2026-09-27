// Synthesis playbacks: a ClipPlayback whose source stage is a SynthVoice
// (synth/synth_graph.h) instead of a clip. Control thread; the mixer side is
// in engine_playback_mix.cpp.

#include "broaudio/engine.h"

#include <algorithm>

namespace broaudio {

int Engine::playSynth(std::shared_ptr<const SynthGraph> graph, const SynthPlayOptions& opts)
{
    if (!graph) return -1;
    // Ask the compile workers now rather than on the first mixed block.
    graph->requestKernels();
    // The voice and its memory (delay lines, state) are allocated here, off
    // the audio thread and outside the lock.
    // A looping voice renders its loop here, through whatever kernels are
    // already published (the samples do not depend on it).
    auto voice = opts.loop.enabled()
                     ? std::make_shared<SynthVoice>(graph, sampleRate_, opts.trigger, opts.loop, true)
                     : std::make_shared<SynthVoice>(graph, sampleRate_, opts.trigger);

    uint64_t startSample = 0;
    const double s = opts.when * static_cast<double>(sampleRate_);
    if (s > 0.0) startSample = static_cast<uint64_t>(s + 0.5);

    std::lock_guard<std::mutex> lock(mediaWriteMutex_);
    auto pb = std::make_shared<ClipPlayback>();
    pb->id = nextPlaybackId_++;
    pb->clipId = 0;
    pb->synth = std::move(voice);
    pb->gain.store(opts.gain, std::memory_order_relaxed);
    pb->playing.store(true, std::memory_order_relaxed);
    pb->active.store(true, std::memory_order_relaxed);
    pb->startSample.store(startSample, std::memory_order_relaxed);
    pb->pan.store(std::clamp(opts.pan, -1.0f, 1.0f), std::memory_order_relaxed);
    pb->busId.store(opts.busId, std::memory_order_relaxed);
    if (opts.spatial) {
        pb->spatial.posX.store(opts.position[0], std::memory_order_relaxed);
        pb->spatial.posY.store(opts.position[1], std::memory_order_relaxed);
        pb->spatial.posZ.store(opts.position[2], std::memory_order_relaxed);
        pb->spatial.spatialEnabled.store(true, std::memory_order_relaxed);
    }

    const int id = pb->id;
    auto newList = std::make_shared<PlaybackList>(*playbacks_.load());
    newList->push_back(std::move(pb));
    playbacks_.store(std::move(newList));
    return id;
}

void Engine::releaseSynth(int instanceId)
{
    if (ClipPlayback* pb = findPlayback(instanceId)) {
        if (pb->synth) pb->synth->release();
    }
}

} // namespace broaudio
