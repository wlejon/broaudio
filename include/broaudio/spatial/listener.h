#pragma once

#include "broaudio/types.h"
#include <algorithm>
#include <atomic>
#include <cmath>

namespace broaudio {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
};

// Per-source spatial parameters. Voices and clips each own one of these.
// All fields are atomic for lock-free main -> audio thread access.
struct SpatialSource {
    std::atomic<bool> spatialEnabled{false};
    std::atomic<float> posX{0.0f}, posY{0.0f}, posZ{0.0f};
    std::atomic<float> refDistance{1.0f};    // distance at which gain = 1.0
    std::atomic<float> maxDistance{100.0f};
    std::atomic<float> rolloff{1.0f};
    std::atomic<int> distanceModel{static_cast<int>(DistanceModel::Inverse)};

    Vec3 position() const {
        return {posX.load(std::memory_order_relaxed),
                posY.load(std::memory_order_relaxed),
                posZ.load(std::memory_order_relaxed)};
    }
};

// Listener position and orientation in 3D space.
// Used by the spatial mixer to compute per-source panning and attenuation.
// All fields are atomic for lock-free main→audio thread access.
struct Listener {
    std::atomic<float> posX{0.0f}, posY{0.0f}, posZ{0.0f};
    std::atomic<float> fwdX{0.0f}, fwdY{0.0f}, fwdZ{-1.0f};
    std::atomic<float> upX{0.0f},  upY{1.0f},  upZ{0.0f};

    Vec3 position() const {
        return {posX.load(std::memory_order_relaxed),
                posY.load(std::memory_order_relaxed),
                posZ.load(std::memory_order_relaxed)};
    }
    Vec3 forward() const {
        return {fwdX.load(std::memory_order_relaxed),
                fwdY.load(std::memory_order_relaxed),
                fwdZ.load(std::memory_order_relaxed)};
    }
    Vec3 up() const {
        return {upX.load(std::memory_order_relaxed),
                upY.load(std::memory_order_relaxed),
                upZ.load(std::memory_order_relaxed)};
    }
};

// Result of spatial computation: gain, pan, and directional cues.
struct SpatialResult {
    float gain = 1.0f;          // distance attenuation (0-1)
    float pan = 0.0f;           // stereo pan (-1 left, +1 right)
    float frontBack = 1.0f;     // +1 directly in front, -1 directly behind
    float elevation = 0.0f;     // +1 directly above, -1 directly below
};

// Per-ear spatial filter parameters computed from SpatialResult.
struct HeadParams {
    float coeffL = 0.0f;   // one-pole LP coefficient for left ear
    float coeffR = 0.0f;   // one-pole LP coefficient for right ear
    float gainL  = 1.0f;   // ILD gain for left ear
    float gainR  = 1.0f;   // ILD gain for right ear
};

// Head-shadow filter: independent one-pole lowpass per ear.
// Audio-thread only state — lives alongside each SpatialSource.
struct SpatialFilter {
    float zL = 0.0f;  // left ear filter state
    float zR = 0.0f;  // right ear filter state

    // Process a stereo sample pair in-place with per-ear coefficients and ILD gains.
    void process(float& L, float& R, const HeadParams& hp) {
        // ILD (interaural level difference)
        L *= hp.gainL;
        R *= hp.gainR;
        // ITF (interaural transfer function) — one-pole lowpass per ear
        float aL = 1.0f - hp.coeffL;
        float aR = 1.0f - hp.coeffR;
        zL = zL * hp.coeffL + L * aL;
        zR = zR * hp.coeffR + R * aR;
        L = zL;
        R = zR;
    }
};

// Compute gain, pan, front/back, and elevation from a spatial source relative to a listener.
inline SpatialResult computeSpatial(const Listener& listener, const SpatialSource& src)
{
    SpatialResult result;

    Vec3 lPos = listener.position();
    Vec3 sPos = src.position();
    Vec3 dir = sPos - lPos;
    float dist = dir.length();

    // Distance attenuation
    float refDist = src.refDistance.load(std::memory_order_relaxed);
    float maxDist = src.maxDistance.load(std::memory_order_relaxed);
    float rolloff = src.rolloff.load(std::memory_order_relaxed);
    auto model = static_cast<DistanceModel>(src.distanceModel.load(std::memory_order_relaxed));

    float d = std::max(dist, refDist);
    d = std::min(d, maxDist);

    switch (model) {
        case DistanceModel::Linear:
            if (maxDist > refDist)
                result.gain = 1.0f - rolloff * (d - refDist) / (maxDist - refDist);
            break;
        case DistanceModel::Inverse:
            result.gain = refDist / (refDist + rolloff * (d - refDist));
            break;
        case DistanceModel::Exponential:
            if (refDist > 0.0f)
                result.gain = std::pow(d / refDist, -rolloff);
            break;
    }
    result.gain = std::max(0.0f, std::min(1.0f, result.gain));

    if (dist > 0.0001f) {
        Vec3 fwd = listener.forward();
        Vec3 up = listener.up();
        Vec3 right = fwd.cross(up);
        float rightLen = right.length();
        Vec3 normDir = {dir.x / dist, dir.y / dist, dir.z / dist};

        if (rightLen > 0.0001f) {
            right.x /= rightLen; right.y /= rightLen; right.z /= rightLen;
            result.pan = std::clamp(normDir.dot(right), -1.0f, 1.0f);
        }

        // Front/back: dot with forward vector (relative to gaze direction)
        float fwdLen = std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y + fwd.z * fwd.z);
        if (fwdLen > 0.0001f) {
            result.frontBack = std::clamp(normDir.dot({fwd.x / fwdLen, fwd.y / fwdLen, fwd.z / fwdLen}), -1.0f, 1.0f);
        }

        // Elevation: dot with listener's up vector (relative to gaze, not world Y).
        // Looking down at something below = it's in front, not "below" your ears.
        float upLen = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
        if (upLen > 0.0001f) {
            result.elevation = std::clamp(normDir.dot({up.x / upLen, up.y / upLen, up.z / upLen}), -1.0f, 1.0f);
        }
    }

    return result;
}

// Configurable head shadow model parameters.
// All fields are atomic for lock-free main→audio thread access.
// Set from main thread, read from audio thread during spatial processing.
struct HeadModel {
    // ILD: far-ear gain reduction at 90 degrees (0 = silent, 1 = no reduction)
    std::atomic<float> ildStrength{0.85f};      // far ear gain = 1 - |pan| * ildStrength

    // Behind attenuation: both-ear gain reduction when source is directly behind
    std::atomic<float> behindAttenuation{0.45f}; // gain = 1 - behindAttenuation when directly behind

    // Near ear cutoff range (Hz): modulated by front/back
    std::atomic<float> nearCutoffFront{18000.0f};  // cutoff when source is directly in front
    std::atomic<float> nearCutoffBehind{2000.0f};   // cutoff when source is directly behind

    // Far ear shadow: how much the far-ear cutoff drops relative to near ear at 90 degrees
    std::atomic<float> farCutoffRatio{0.95f};     // far cutoff = near * (1 - |pan| * ratio)

    // Elevation influence on cutoff (Hz shift)
    std::atomic<float> elevationNear{5000.0f};    // near ear cutoff += elevation * this
    std::atomic<float> elevationFar{2000.0f};     // far ear cutoff += elevation * this

    // Cutoff clamps (Hz)
    std::atomic<float> minCutoff{200.0f};
    std::atomic<float> maxCutoff{20000.0f};

    // Master enable — when false, no head filtering is applied (but distance/pan still work)
    std::atomic<bool> enabled{true};
};

// Compute one-pole coefficient from cutoff frequency.
inline float onePoleCoeff(float cutoffHz, int sampleRate) {
    float w = 6.2831853f * cutoffHz / static_cast<float>(sampleRate);
    return std::exp(-w);
}

// Compute per-ear head shadow parameters from a spatial result and head model config.
inline HeadParams computeHeadParams(const SpatialResult& sr, const HeadModel& hm, int sampleRate) {
    HeadParams hp;

    if (!hm.enabled.load(std::memory_order_relaxed)) return hp;

    float absPan = std::abs(sr.pan);
    float ildStr = hm.ildStrength.load(std::memory_order_relaxed);
    float behindAtt = hm.behindAttenuation.load(std::memory_order_relaxed);
    float nearFront = hm.nearCutoffFront.load(std::memory_order_relaxed);
    float nearBehind = hm.nearCutoffBehind.load(std::memory_order_relaxed);
    float farRatio = hm.farCutoffRatio.load(std::memory_order_relaxed);
    float elevNear = hm.elevationNear.load(std::memory_order_relaxed);
    float elevFar = hm.elevationFar.load(std::memory_order_relaxed);
    float minCut = hm.minCutoff.load(std::memory_order_relaxed);
    float maxCut = hm.maxCutoff.load(std::memory_order_relaxed);

    // ── ILD: interaural level difference ──
    float shadow = 1.0f - absPan * ildStr;
    if (sr.pan > 0.0f) {
        hp.gainL = shadow;
        hp.gainR = 1.0f;
    } else {
        hp.gainL = 1.0f;
        hp.gainR = shadow;
    }

    // Behind penalty
    if (sr.frontBack < 0.0f) {
        float behindGain = 1.0f + sr.frontBack * behindAtt;
        hp.gainL *= behindGain;
        hp.gainR *= behindGain;
    }

    // ── ITF: head shadow lowpass ──
    float nearRange = (nearFront - nearBehind) * 0.5f;
    float nearMid = (nearFront + nearBehind) * 0.5f;
    float nearCutoff = nearMid + sr.frontBack * nearRange;
    float farCutoff = nearCutoff * (1.0f - absPan * farRatio);

    nearCutoff += sr.elevation * elevNear;
    farCutoff  += sr.elevation * elevFar;

    nearCutoff = std::max(minCut, std::min(maxCut, nearCutoff));
    farCutoff  = std::max(minCut, std::min(maxCut, farCutoff));

    float nearCoeff = onePoleCoeff(nearCutoff, sampleRate);
    float farCoeff  = onePoleCoeff(farCutoff, sampleRate);

    if (sr.pan > 0.0f) {
        hp.coeffL = farCoeff;
        hp.coeffR = nearCoeff;
    } else {
        hp.coeffL = nearCoeff;
        hp.coeffR = farCoeff;
    }

    return hp;
}

} // namespace broaudio
