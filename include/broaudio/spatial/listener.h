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

// Compute one-pole coefficient from cutoff frequency.
inline float onePoleCoeff(float cutoffHz, int sampleRate) {
    float w = 6.2831853f * cutoffHz / static_cast<float>(sampleRate);
    return std::exp(-w);
}

// Compute per-ear head shadow parameters from a spatial result.
// Models three effects:
//   1. ILD (interaural level difference) — far ear is quieter
//   2. ITF (interaural transfer function) — far ear is lowpass filtered (head shadow)
//   3. Front/back cue — sounds behind are muffled in both ears
//   4. Elevation cue — above is brighter, below is darker
inline HeadParams computeHeadParams(const SpatialResult& sr, int sampleRate) {
    HeadParams hp;

    // pan: -1 = hard left, +1 = hard right
    // |pan| is how far off-center the source is (0 = center, 1 = 90 degrees)
    float absPan = std::abs(sr.pan);

    // ── ILD: interaural level difference ──
    // At 90 degrees off-axis, the far ear loses ~12-15 dB (factor ~0.15-0.25)
    // Near ear is unaffected. Center = both ears equal.
    float shadow = 1.0f - absPan * 0.85f;  // 0.15 at hard left/right
    if (sr.pan > 0.0f) {
        hp.gainL = shadow;
        hp.gainR = 1.0f;
    } else {
        hp.gainL = 1.0f;
        hp.gainR = shadow;
    }

    // Behind penalty: both ears lose gain
    if (sr.frontBack < 0.0f) {
        float behindAtten = 1.0f + sr.frontBack * 0.45f; // down to 0.55 directly behind
        hp.gainL *= behindAtten;
        hp.gainR *= behindAtten;
    }

    // ── ITF: head shadow lowpass ──
    // Near ear cutoff: high (transparent), modulated by front/back
    //   front: 18kHz, behind: 2kHz
    float nearCutoff = 10000.0f + sr.frontBack * 8000.0f;

    // Far ear cutoff: aggressively low — head blocks high frequencies
    //   center: same as near, 90 degrees: ~400 Hz
    float farCutoff = nearCutoff * (1.0f - absPan * 0.95f);

    // Elevation: above = brighter, below = darker
    nearCutoff += sr.elevation * 5000.0f;
    farCutoff  += sr.elevation * 2000.0f;

    nearCutoff = std::max(400.0f, std::min(20000.0f, nearCutoff));
    farCutoff  = std::max(200.0f, std::min(20000.0f, farCutoff));

    float nearCoeff = onePoleCoeff(nearCutoff, sampleRate);
    float farCoeff  = onePoleCoeff(farCutoff, sampleRate);

    if (sr.pan > 0.0f) {
        // Source on right: left ear = far, right ear = near
        hp.coeffL = farCoeff;
        hp.coeffR = nearCoeff;
    } else {
        hp.coeffL = nearCoeff;
        hp.coeffR = farCoeff;
    }

    return hp;
}

} // namespace broaudio
