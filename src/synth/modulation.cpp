#include "broaudio/synth/modulation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>

namespace broaudio {

static constexpr float TWO_PI = 2.0f * std::numbers::pi_v<float>;

// ---------------------------------------------------------------------------
// LFO parameter setters
// ---------------------------------------------------------------------------

LfoParams& ModMatrix::lfoParams(int index)
{
    return lfos_[index % MAX_LFOS];
}

const LfoParams& ModMatrix::lfoParams(int index) const
{
    return lfos_[index % MAX_LFOS];
}

void ModMatrix::setLfoShape(int index, LfoShape shape)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].shape.store(shape, std::memory_order_relaxed);
}

void ModMatrix::setLfoRate(int index, float hz)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].rate.store(hz, std::memory_order_relaxed);
}

void ModMatrix::setLfoDepth(int index, float depth)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].depth.store(depth, std::memory_order_relaxed);
}

void ModMatrix::setLfoOffset(int index, float offset)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].offset.store(offset, std::memory_order_relaxed);
}

void ModMatrix::setLfoBipolar(int index, bool bipolar)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].bipolar.store(bipolar, std::memory_order_relaxed);
}

void ModMatrix::setLfoSync(int index, bool sync)
{
    if (index >= 0 && index < MAX_LFOS)
        lfos_[index].sync.store(sync, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Route management
// ---------------------------------------------------------------------------

int ModMatrix::addRoute(ModSource source, ModDest dest, float amount)
{
    if (routeCount_ >= MAX_ROUTES) return -1;
    int idx = routeCount_++;
    routes_[idx].source = source;
    routes_[idx].dest = dest;
    routes_[idx].amount = amount;
    routes_[idx].enabled = true;
    return idx;
}

void ModMatrix::removeRoute(int index)
{
    if (index < 0 || index >= routeCount_) return;
    for (int i = index; i < routeCount_ - 1; i++) {
        routes_[i] = routes_[i + 1];
    }
    routeCount_--;
}

void ModMatrix::setRouteAmount(int index, float amount)
{
    if (index >= 0 && index < routeCount_)
        routes_[index].amount = amount;
}

void ModMatrix::setRouteEnabled(int index, bool enabled)
{
    if (index >= 0 && index < routeCount_)
        routes_[index].enabled = enabled;
}

void ModMatrix::clearAllRoutes()
{
    routeCount_ = 0;
}

// ---------------------------------------------------------------------------
// Per-sample LFO evaluation — operates on per-voice state
// ---------------------------------------------------------------------------

float ModMatrix::sampleLfo(const LfoParams& params, float& phase,
                            float& holdValue, int sampleRate)
{
    LfoShape shape = params.shape.load(std::memory_order_relaxed);
    float rate = params.rate.load(std::memory_order_relaxed);
    float depth = params.depth.load(std::memory_order_relaxed);
    float offset = params.offset.load(std::memory_order_relaxed);
    bool bipolar = params.bipolar.load(std::memory_order_relaxed);

    float raw = 0.0f;

    switch (shape) {
        case LfoShape::Sine:
            raw = std::sin(phase * TWO_PI);
            break;

        case LfoShape::Triangle:
            raw = (phase < 0.5f)
                ? (4.0f * phase - 1.0f)
                : (3.0f - 4.0f * phase);
            break;

        case LfoShape::Square:
            raw = (phase < 0.5f) ? 1.0f : -1.0f;
            break;

        case LfoShape::SawUp:
            raw = 2.0f * phase - 1.0f;
            break;

        case LfoShape::SawDown:
            raw = 1.0f - 2.0f * phase;
            break;

        case LfoShape::SampleAndHold:
            if (phase < rate / static_cast<float>(sampleRate)) {
                holdValue = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX) * 2.0f - 1.0f;
            }
            raw = holdValue;
            break;
    }

    // Advance the per-voice phase
    phase += rate / static_cast<float>(sampleRate);
    if (phase >= 1.0f) phase -= 1.0f;

    if (!bipolar) {
        raw = (raw + 1.0f) * 0.5f;
    }

    return raw * depth + offset;
}

// ---------------------------------------------------------------------------
// Source lookup — reads per-voice state and shared external values
// ---------------------------------------------------------------------------

float ModMatrix::getSource(ModSource source, const ModState& state, float envelopeLevel)
{
    switch (source) {
        case ModSource::Lfo1: return state.lfoOutputs[0];
        case ModSource::Lfo2: return state.lfoOutputs[1];
        case ModSource::Lfo3: return state.lfoOutputs[2];
        case ModSource::Lfo4: return state.lfoOutputs[3];
        case ModSource::Envelope: return envelopeLevel;
        case ModSource::Velocity: return state.velocity;
        case ModSource::KeyTracking: return static_cast<float>(state.noteNumber) / 127.0f;
        case ModSource::ModWheel: return modWheel_.load(std::memory_order_relaxed);
        case ModSource::Aftertouch: return aftertouch_.load(std::memory_order_relaxed);
        default: return 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Process — advance per-voice LFOs, resolve all routes into ModValues
// ---------------------------------------------------------------------------

void ModMatrix::process(ModValues& out, ModState& state, float envelopeLevel, int sampleRate)
{
    out.reset();

    // Advance each LFO using this voice's phase state
    for (int i = 0; i < MAX_LFOS; i++) {
        state.lfoOutputs[i] = sampleLfo(
            lfos_[i], state.lfoPhases[i], state.lfoHoldValues[i], sampleRate);
    }

    // Apply all routes
    for (int i = 0; i < routeCount_; i++) {
        const ModRoute& route = routes_[i];
        if (!route.enabled) continue;

        float sourceVal = getSource(route.source, state, envelopeLevel);
        float modVal = sourceVal * route.amount;

        switch (route.dest) {
            case ModDest::Pitch:
                out.pitch += modVal;
                break;
            case ModDest::Gain:
                out.gain *= (1.0f + modVal);
                break;
            case ModDest::Pan:
                out.pan += modVal;
                break;
            case ModDest::FilterFreq:
                out.filterFreq *= std::exp2(modVal);
                break;
            case ModDest::FilterQ:
                out.filterQ *= (1.0f + modVal);
                break;
            case ModDest::PulseWidth:
                out.pulseWidth += modVal;
                break;
            case ModDest::DelaySend:
                out.delaySend += modVal;
                break;
            default:
                break;
        }
    }

    out.gain = std::max(0.0f, out.gain);
    out.pan = std::clamp(out.pan, -1.0f, 1.0f);
    out.filterFreq = std::max(0.01f, out.filterFreq);
    out.filterQ = std::max(0.1f, out.filterQ);
    out.pulseWidth = std::clamp(out.pulseWidth, 0.01f, 0.99f);
    out.delaySend = std::clamp(out.delaySend, 0.0f, 1.0f);
}

} // namespace broaudio
