// Clip-playback mixing: the source stages (fixed clips and streaming rings)
// and the spatializer that turns a playback's controls into the per-voice
// chain's parameter block (spatial/spatial_chain.h). Audio thread only,
// shared by the realtime and headless render paths.

#include "broaudio/engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace broaudio {

namespace {

inline void deactivate(ClipPlayback& pb) {
    pb.playing.store(false, std::memory_order_relaxed);
    pb.active.store(false, std::memory_order_relaxed);
}

// Time constant of each of the delay line's two cascaded one-poles.
constexpr float kDelaySmoothSeconds = 0.010f;

} // namespace

void Engine::mixPlaybacks(int numFrames, const BusList& buses)
{
    auto currentClips = clips_.load();
    auto currentPlaybacks = playbacks_.load();
    // This block spans absolute samples [blockStart, blockStart+numFrames).
    // generateSamples() already advanced samplesGenerated_ by numFrames, so
    // the counter holds the block's END sample.
    const uint64_t blockEnd = samplesGenerated_.load(std::memory_order_relaxed);
    const uint64_t blockStart = blockEnd >= static_cast<uint64_t>(numFrames)
                                    ? blockEnd - static_cast<uint64_t>(numFrames) : 0;

    const float mpu = metresPerUnit_.load(std::memory_order_relaxed);
    const float c = speedOfSound_.load(std::memory_order_relaxed);
    const float cUnits = c / mpu;
    const float maxDelaySec = maxPropagationDelay_.load(std::memory_order_relaxed);
    const float maxDelaySamples = maxDelaySec * static_cast<float>(sampleRate_);
    const float airStrength = airStrength_.load(std::memory_order_relaxed);
    const float dopplerFactor = dopplerFactor_.load(std::memory_order_relaxed);
    const float delaySmooth = 1.0f - std::exp(-1.0f / (kDelaySmoothSeconds * static_cast<float>(sampleRate_)));
    // One refcount per block, not per playback. The RCU snapshot keeps its own
    // reference, so this copy never frees the table on the audio thread.
    const std::shared_ptr<const AirFilterTable> air = airTable_.load();

    const size_t stride = static_cast<size_t>(chainScratch_.size() / 4);
    float* srcL = chainScratch_.data();
    float* srcR = srcL + stride;
    float* outL = srcR + stride;
    float* outR = outL + stride;

    float* masterBuf = nullptr;
    for (auto& bus : buses) {
        if (bus->id == MASTER_BUS_ID) { masterBuf = bus->buffer.data(); break; }
    }

    for (auto& pbPtr : *currentPlaybacks) {
        ClipPlayback& pb = *pbPtr;
        if (!pb.active.load(std::memory_order_relaxed)) continue;
        if (!pb.playing.load(std::memory_order_relaxed)) continue;

        AudioClip* clip = nullptr;
        for (auto& cl : *currentClips) {
            if (cl->id == pb.clipId) { clip = cl.get(); break; }
        }
        if (!clip) continue;

        const int targetBusId = pb.busId.load(std::memory_order_relaxed);
        float* targetBuf = nullptr;
        for (auto& bus : buses) {
            if (bus->id == targetBusId) { targetBuf = bus->buffer.data(); break; }
        }
        if (!targetBuf) targetBuf = masterBuf;
        if (!targetBuf) continue;

        VoiceChainParams P;
        P.channels = clip->channels == 2 ? 2 : 1;
        P.bus = targetBuf;
        float rate = pb.rate.load(std::memory_order_relaxed);
        P.pan = pb.pan.load(std::memory_order_relaxed);
        P.distanceGain = 1.0f;

        // --- Spatializer: one evaluation per block ---
        if (pb.spatial.spatialEnabled.load(std::memory_order_relaxed)) {
            const SpatialResult sr = computeSpatial(listener_, pb.spatial);
            P.distanceGain = sr.gain;
            P.pan = 0.0f;   // centre — the head stage does L/R
            P.head = computeHeadParams(sr, headModel_, sampleRate_,
                                       pb.spatial.occlusion.load(std::memory_order_relaxed));
            P.stages |= kStageHead;

            const float metres = bromath::vlen(pb.spatial.position() - listener_.position()) * mpu;
            PropagationDelayBuffer* db = pb.delayBuffer.load(std::memory_order_acquire);
            if (db && pb.propagationDelay.load(std::memory_order_relaxed)) {
                P.stages |= kStageDelay;
                P.delayBuf = db;
                P.delayTarget = std::min(metres / c, maxDelaySec) * static_cast<float>(sampleRate_);
                P.delayMax = maxDelaySamples;
                P.delaySmooth = delaySmooth;
            } else {
                // Rate-based Doppler only when the delay line is not producing it.
                const float dop = computeDopplerRatio(listener_, pb.spatial, dopplerFactor, cUnits);
                pb.spatial.lastDopplerRatio.store(dop, std::memory_order_relaxed);
                rate *= dop;
                pb.chain.delayPrimed = false;
            }
            if (air && pb.airAbsorption.load(std::memory_order_relaxed)) {
                P.stages |= kStageAir;
                air->lookup(metres * airStrength, P.airPole, P.airMix);
            } else {
                pb.chain.airPrimed = false;
            }
        } else {
            // Re-enabling spatial later snaps rather than sweeping from stale state.
            pb.chain.delayPrimed = false;
            pb.chain.airPrimed = false;
        }

        // --- Gain and send ramps ---
        pb.chain.gain.update(pb.gain.load(std::memory_order_relaxed), pb.gainRamp, sampleRate_);
        const int sendId = pb.sendBusId.load(std::memory_order_relaxed);
        const float sendAmt = pb.sendAmount.load(std::memory_order_relaxed);
        pb.chain.send.update(sendAmt, pb.sendRamp, sampleRate_);
        if (sendId >= 0 && (sendAmt > 0.0f || pb.chain.send.value != 0.0f || !pb.chain.send.settled())) {
            for (auto& bus : buses) {
                if (bus->id == sendId) { P.send = bus->buffer.data(); break; }
            }
            if (P.send) P.stages |= kStageSend;
        }

        // --- Source stage ---
        bool ended = false;
        if (pb.sourceEnded) {
            std::memset(srcL, 0, static_cast<size_t>(numFrames) * sizeof(float));
            std::memset(srcR, 0, static_cast<size_t>(numFrames) * sizeof(float));
        } else if (clip->streaming) {
            sourceStream(&pb, clip, rate, numFrames, srcL, srcR);
        } else {
            ended = sourceClip(&pb, clip, rate, numFrames, blockStart, srcL, srcR);
        }

        // --- Chain ---
        const bool delayOn = (P.stages & kStageDelay) != 0;
        const bool wasPrimed = pb.chain.delayPrimed && pb.chain.delayBufSeen == P.delayBuf;
        const float delayBefore = pb.chain.delayOut;
        float* ch[2] = {srcL, srcR};
        runVoiceChain(P, pb.chain, ch, outL, outR, numFrames);
        if (delayOn) {
            // The delay line's effective pitch ratio over the block.
            const float dd = wasPrimed ? pb.chain.delayOut - delayBefore : 0.0f;
            pb.spatial.lastDopplerRatio.store(std::clamp(1.0f - dd / static_cast<float>(numFrames), 0.5f, 2.0f),
                                              std::memory_order_relaxed);
        }

        // --- End of a one-shot: drain the delayed tail, then finish ---
        if (ended && !pb.sourceEnded) {
            if (delayOn) {
                pb.sourceEnded = true;
                pb.tailRemaining = static_cast<int>(std::ceil(pb.chain.delayOut)) + 8;
            } else {
                deactivate(pb);
            }
        } else if (pb.sourceEnded) {
            pb.tailRemaining -= numFrames;
            if (pb.tailRemaining <= 0 || !delayOn) deactivate(pb);
        }
    }
}

bool Engine::sourceClip(ClipPlayback* pb, AudioClip* clip, float rate, int numFrames,
                        uint64_t blockStart, float* L, float* R)
{
    std::memset(L, 0, static_cast<size_t>(numFrames) * sizeof(float));
    std::memset(R, 0, static_cast<size_t>(numFrames) * sizeof(float));

    const int start = pb->regionStart.load(std::memory_order_relaxed);
    int end = pb->regionEnd.load(std::memory_order_relaxed);
    end = end > 0 ? end : clip->numFrames();
    const int len = end - start;
    if (len <= 0) return false;
    const int ch = clip->channels;
    const bool looping = pb->looping.load(std::memory_order_relaxed);

    // Loop window, clamped into the region; a window that is empty, inverted
    // or starts before 0 loops the whole region (Web Audio's actualLoopStart /
    // actualLoopEnd fallback).
    int ls = pb->loopStart.load(std::memory_order_relaxed);
    int le = pb->loopEnd.load(std::memory_order_relaxed);
    if (le > len) le = len;
    if (ls < 0 || le <= ls) { ls = 0; le = len; }

    constexpr int FRAC_BITS = 16;
    constexpr uint64_t FRAC_MASK = (1ULL << FRAC_BITS) - 1;
    const uint64_t increment = static_cast<uint64_t>(rate * (1 << FRAC_BITS) + 0.5f);
    const uint64_t loopStartF = static_cast<uint64_t>(ls) << FRAC_BITS;
    const uint64_t loopEndF = static_cast<uint64_t>(le) << FRAC_BITS;
    const uint64_t loopLenF = loopEndF - loopStartF;
    const uint64_t budget = pb->durationFixed.load(std::memory_order_relaxed);

    // Sample-accurate scheduled start: stay silent until the audio clock
    // reaches startSample, then begin mid-block at the exact frame. 0 (or any
    // past sample) starts immediately. Sample-accurate scheduled stop
    // (stopPlaybackAt): the source ends at stopSample. A stop at or before
    // the start ends it, silent, in the block that holds the stop.
    const uint64_t blockEndS = blockStart + static_cast<uint64_t>(numFrames);
    const uint64_t stopS = pb->stopSample.load(std::memory_order_relaxed);
    const uint64_t startS = pb->startSample.load(std::memory_order_relaxed);
    if (stopS < blockEndS && stopS <= std::max(blockStart, startS)) return true;
    const int endFrame = stopS < blockEndS ? static_cast<int>(stopS - blockStart) : numFrames;

    int startFrame = 0;
    if (startS > blockStart) {
        const uint64_t off = startS - blockStart;
        if (off >= static_cast<uint64_t>(numFrames)) return false;  // starts in a later block
        startFrame = static_cast<int>(off);
    }

    bool ended = false;
    uint64_t pos = pb->playPos.load(std::memory_order_relaxed);
    uint64_t consumed = pb->consumedFixed;
    const float* s = clip->samples.data();
    for (int i = startFrame; i < endFrame; i++) {
        if (looping && pos >= loopEndF) pos = loopStartF + (pos - loopStartF) % loopLenF;
        const int intPos = static_cast<int>(pos >> FRAC_BITS);
        if ((!looping && intPos >= len) || consumed >= budget) { ended = true; break; }
        const float frac = static_cast<float>(pos & FRAC_MASK) / (1 << FRAC_BITS);
        int nextIdx = intPos + 1;
        if (looping && intPos < le && nextIdx >= le) nextIdx = ls;
        else if (nextIdx >= len) nextIdx = intPos;

        if (ch == 2) {
            const int i0 = (start + intPos) * 2;
            const int i1 = (start + nextIdx) * 2;
            L[i] = s[i0] + frac * (s[i1] - s[i0]);
            R[i] = s[i0 + 1] + frac * (s[i1 + 1] - s[i0 + 1]);
        } else {
            const float s0 = s[start + intPos];
            const float s1 = s[start + nextIdx];
            L[i] = s0 + frac * (s1 - s0);
        }
        pos += increment;
        consumed += increment;
    }
    pb->playPos.store(pos, std::memory_order_relaxed);
    pb->consumedFixed = consumed;
    return ended || endFrame < numFrames;
}

void Engine::sourceStream(ClipPlayback* pb, AudioClip* clip, float rate, int numFrames,
                          float* L, float* R)
{
    std::memset(L, 0, static_cast<size_t>(numFrames) * sizeof(float));
    std::memset(R, 0, static_cast<size_t>(numFrames) * sizeof(float));
    const int cap = clip->ringFrames;
    const int ch = clip->channels;
    if (cap <= 0) return;

    // A disk-stream seek the worker has not applied yet: what the ring holds
    // is pre-seek audio the caller already seeked away from. Hold the cursor
    // and stay silent (not starvation) until the fence below is published.
    if (clip->streamSeekApplied.load(std::memory_order_acquire) !=
        clip->streamSeekRequested.load(std::memory_order_relaxed))
        return;

    const uint64_t wf = clip->writeFrames.load(std::memory_order_acquire);
    uint64_t rf = pb->playPos.load(std::memory_order_relaxed);
    // Seek fence: everything below streamFlushFrames is pre-seek audio.
    const uint64_t ff = clip->streamFlushFrames.load(std::memory_order_acquire);
    if (ff > rf) { rf = ff; pb->streamFrac = 0.0f; }
    // Overrun: more than a full ring behind — drop the oldest audio.
    if (wf > rf + static_cast<uint64_t>(cap))
        rf = wf - static_cast<uint64_t>(cap) / 2;

    // Starvation accounting, batched into one relaxed add after the loop.
    int underruns = 0;
    const bool ended = clip->streamEnded.load(std::memory_order_acquire);

    // Playback rate: fractional cursor + linear interpolation, tape-style
    // (pitch follows speed), like clip playback.
    if (!(rate > 0.0f)) rate = 1.0f;
    const bool resampling = (rate < 0.999f || rate > 1.001f);
    const float* s = clip->samples.data();

    for (int i = 0; i < numFrames; i++) {
        if (!resampling) {
            if (rf < wf) {   // else underrun → silence, hold the cursor
                const int slot = static_cast<int>(rf % cap);
                if (ch == 2) { L[i] = s[slot * 2]; R[i] = s[slot * 2 + 1]; }
                else L[i] = s[slot];
                rf++;
            } else if (!ended) {
                underruns++;
            }
        } else if (rf + 1 < wf) {
            // Two neighbours are needed to interpolate, so this runs one frame
            // shallower into the ring than the rate-1 path.
            const int s0 = static_cast<int>(rf % cap);
            const int s1 = static_cast<int>((rf + 1) % cap);
            const float t = pb->streamFrac;
            if (ch == 2) {
                L[i] = s[s0 * 2] + (s[s1 * 2] - s[s0 * 2]) * t;
                R[i] = s[s0 * 2 + 1] + (s[s1 * 2 + 1] - s[s0 * 2 + 1]) * t;
            } else {
                L[i] = s[s0] + (s[s1] - s[s0]) * t;
            }
            float frac = t + rate;
            const int whole = static_cast<int>(frac);   // rate > 1 consumes > 1 frame
            frac -= static_cast<float>(whole);
            rf += static_cast<uint64_t>(whole);
            pb->streamFrac = frac;
        } else if (!ended) {
            underruns++;
        }
    }
    pb->playPos.store(rf, std::memory_order_relaxed);
    if (underruns > 0)
        clip->streamUnderrunFrames.fetch_add(static_cast<uint64_t>(underruns),
                                             std::memory_order_relaxed);
}

} // namespace broaudio
