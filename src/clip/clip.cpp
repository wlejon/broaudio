// AudioClip and ClipPlayback are data structs — playback logic lives in
// engine.cpp. The one shared piece here is the SPSC ring append used by both
// live PCM streams (Engine::pushStreamSamples) and disk-streamed file sources
// (FileStreamRunner).

#include "broaudio/clip/clip.h"

#include <algorithm>

namespace broaudio {

int pushStreamFrames(AudioClip& clip, const float* samples, int numFrames)
{
    if (!samples || numFrames <= 0) return 0;
    if (!clip.streaming || clip.ringFrames <= 0) return 0;

    const int cap = clip.ringFrames;
    const int ch = clip.channels;
    // If a single push exceeds the ring, keep only the most recent capacity.
    if (numFrames > cap) {
        samples += static_cast<size_t>(numFrames - cap) * ch;
        numFrames = cap;
    }

    uint64_t wf = clip.writeFrames.load(std::memory_order_relaxed);
    int startSlot = static_cast<int>(wf % cap);
    int first = std::min(numFrames, cap - startSlot);
    std::copy(samples, samples + static_cast<size_t>(first) * ch,
              clip.samples.begin() + static_cast<size_t>(startSlot) * ch);
    if (numFrames > first) {
        std::copy(samples + static_cast<size_t>(first) * ch,
                  samples + static_cast<size_t>(numFrames) * ch,
                  clip.samples.begin());
    }
    // Publish: the audio thread reads slots strictly below the new writeFrames.
    clip.writeFrames.store(wf + numFrames, std::memory_order_release);
    return numFrames;
}

} // namespace broaudio
