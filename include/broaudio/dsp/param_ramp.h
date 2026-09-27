#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace broaudio {

// Control-thread half of a rampable parameter. The target itself lives in the
// owner's existing std::atomic<float> (Bus::gain, ClipPlayback::gain, ...) so
// every reader of the target keeps working; this carries how to get there.
//
// set(): stores the ramp length, the value the ramp starts from when the audio
// thread has not mixed the owner yet (the previous target), then bumps `seq`
// with release. The audio thread picks the change up once per block
// (ParamRamp::update). Two sets racing one block collapse into the latest,
// which is what a caller moving a parameter every frame wants.
struct RampControl {
    std::atomic<float> seconds{0.0f};
    std::atomic<float> from{0.0f};
    std::atomic<uint32_t> seq{0};

    void set(std::atomic<float>& target, float value, float rampSeconds) {
        from.store(target.load(std::memory_order_relaxed), std::memory_order_relaxed);
        seconds.store(rampSeconds > 0.0f ? rampSeconds : 0.0f, std::memory_order_relaxed);
        target.store(value, std::memory_order_relaxed);
        seq.fetch_add(1, std::memory_order_release);
    }
};

// Audio-thread half: a per-sample parameter trajectory.
//   - A set with rampSeconds > 0 is a linear ramp from the current value that
//     lands exactly on the target after round(rampSeconds * sampleRate)
//     samples.
//   - A set without one, and any target that moves without a set (a spatial
//     distance gain recomputed every block), is de-zippered: a one-pole
//     approach (~5 ms to 95%), the smoothing the engine always applied.
// The first update snaps to the target, or starts the pending ramp from the
// value that preceded it, so a playback created at gain 0 and immediately
// ramped to 1 fades in instead of starting at 1.
//
// Plain data on purpose: a JIT kernel can take {value, start, step,
// remaining, total, target, coeff} as parameters.
struct ParamRamp {
    float value = 0.0f;
    float target = 0.0f;
    float start = 0.0f;       // linear ramp: value = start + step * k
    float step = 0.0f;
    int remaining = 0;
    int total = 0;
    float coeff = 0.014f;     // one-pole de-zipper, ~5 ms at 44.1 kHz
    bool primed = false;
    uint32_t seqSeen = 0;

    // De-zipper time constant for `sampleRate` (matches Smoother::init).
    void init(int sampleRate, float timeMs = 5.0f) {
        float samples = timeMs * 0.001f * static_cast<float>(sampleRate);
        coeff = (samples > 0.0f) ? 1.0f - std::pow(0.05f, 1.0f / samples) : 1.0f;
    }

    void snap(float v) { value = target = v; remaining = 0; primed = true; }

    // Block-rate pickup of a controlled parameter.
    void update(float newTarget, const RampControl& rc, int sampleRate) {
        const uint32_t s = rc.seq.load(std::memory_order_acquire);
        if (!primed) {
            primed = true;
            seqSeen = s;
            const float sec = rc.seconds.load(std::memory_order_relaxed);
            if (s != 0 && sec > 0.0f) {
                value = rc.from.load(std::memory_order_relaxed);
                startRamp(newTarget, sec, sampleRate);
            } else {
                value = target = newTarget;
                remaining = 0;
            }
            return;
        }
        if (s != seqSeen) {
            seqSeen = s;
            const float sec = rc.seconds.load(std::memory_order_relaxed);
            if (sec > 0.0f) { startRamp(newTarget, sec, sampleRate); return; }
            remaining = 0;
        }
        if (remaining == 0) target = newTarget;
    }

    // Block-rate pickup of an uncontrolled target (de-zipper only).
    void follow(float newTarget) {
        if (!primed) { snap(newTarget); return; }
        target = newTarget;
    }

    bool settled() const { return remaining == 0 && value == target; }

    float next() {
        if (remaining > 0) {
            // Evaluated from the start rather than accumulated, so a long
            // ramp does not drift and then step onto its target.
            --remaining;
            value = remaining == 0 ? target : start + step * static_cast<float>(total - remaining);
        } else if (value != target) {
            float d = target - value;
            value += coeff * d;
            if (std::fabs(d) < 1e-7f) value = target;
        }
        return value;
    }

private:
    void startRamp(float newTarget, float seconds, int sampleRate) {
        target = newTarget;
        remaining = static_cast<int>(std::lround(static_cast<double>(seconds) * sampleRate));
        if (remaining <= 0) { value = target; remaining = 0; step = 0.0f; return; }
        total = remaining;
        start = value;
        step = (target - value) / static_cast<float>(remaining);
    }
};

} // namespace broaudio
