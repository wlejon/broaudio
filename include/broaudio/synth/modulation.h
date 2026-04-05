#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace broaudio {

// --- LFO ---

enum class LfoShape : uint8_t {
    Sine, Triangle, Square, SawUp, SawDown, SampleAndHold
};

// LFO parameters — set from main thread, read from audio thread.
// Shared across all voices (the "patch" definition).
struct LfoParams {
    std::atomic<LfoShape> shape{LfoShape::Sine};
    std::atomic<float> rate{1.0f};       // Hz
    std::atomic<float> depth{1.0f};      // 0-1 output scaling
    std::atomic<float> offset{0.0f};     // DC offset (-1 to 1)
    std::atomic<bool> bipolar{true};     // true: -1..1, false: 0..1
    std::atomic<bool> sync{false};       // true: retrigger on note-on
};

// --- Modulation destinations ---

enum class ModDest : uint8_t {
    Pitch,          // semitones offset
    Gain,           // multiplier (centered at 1.0)
    Pan,            // offset (-1..1)
    FilterFreq,     // multiplier on filter cutoff (0..4, centered at 1.0)
    FilterQ,        // multiplier on filter Q
    PulseWidth,     // for future PWM support
    DelaySend,      // mix level to delay bus
    Count
};

// --- Modulation sources ---

enum class ModSource : uint8_t {
    Lfo1, Lfo2, Lfo3, Lfo4,
    Envelope,       // voice ADSR envelope level
    Velocity,       // note-on velocity
    KeyTracking,    // MIDI note / 127 (0..1)
    ModWheel,       // CC1 value (0..1)
    Aftertouch,     // channel pressure (0..1)
    Count
};

// --- Modulation route ---

struct ModRoute {
    ModSource source = ModSource::Lfo1;
    ModDest dest = ModDest::Pitch;
    float amount = 0.0f;    // scaling factor
    bool enabled = true;
};

// Holds resolved modulation values for one voice for one sample.
// Filled by ModMatrix::process(), consumed by the voice generator.
struct ModValues {
    float pitch = 0.0f;         // semitones offset
    float gain = 1.0f;          // multiplier
    float pan = 0.0f;           // offset
    float filterFreq = 1.0f;    // multiplier
    float filterQ = 1.0f;       // multiplier
    float pulseWidth = 0.5f;    // 0-1
    float delaySend = 0.0f;     // 0-1

    void reset() {
        pitch = 0.0f;
        gain = 1.0f;
        pan = 0.0f;
        filterFreq = 1.0f;
        filterQ = 1.0f;
        pulseWidth = 0.5f;
        delaySend = 0.0f;
    }
};

// --- Per-voice modulation state ---

// Audio-thread-only state that must be independent per voice.
// Each voice owns one of these. The ModMatrix reads shared LfoParams
// but advances phases on this per-voice state.
struct ModState {
    static constexpr int MAX_LFOS = 4;

    // Per-voice LFO phase accumulators
    float lfoPhases[MAX_LFOS] = {};
    float lfoHoldValues[MAX_LFOS] = {};

    // Cached LFO outputs for the current sample
    float lfoOutputs[MAX_LFOS] = {};

    // Per-voice context (set on note-on, constant for voice lifetime)
    int noteNumber = 60;       // MIDI note for key tracking
    float velocity = 1.0f;     // note-on velocity 0-1

    // Reset LFO phases for synced LFOs. Called on note-on.
    void resetSyncedPhases(const LfoParams* params) {
        for (int i = 0; i < MAX_LFOS; i++) {
            if (params[i].sync.load(std::memory_order_relaxed)) {
                lfoPhases[i] = 0.0f;
            }
        }
    }

    // Reset all state (called when voice is allocated to a new note)
    void reset(int note, float vel) {
        noteNumber = note;
        velocity = vel;
        for (int i = 0; i < MAX_LFOS; i++) {
            lfoPhases[i] = 0.0f;
            lfoHoldValues[i] = 0.0f;
            lfoOutputs[i] = 0.0f;
        }
    }
};

// --- Modulation matrix ---

// Owns LFO parameters, routing table, and external source values.
// All per-voice state lives in ModState (on the Voice struct).
// process() advances a ModState's LFO phases and computes ModValues.
class ModMatrix {
public:
    static constexpr int MAX_LFOS = ModState::MAX_LFOS;
    static constexpr int MAX_ROUTES = 16;

    ModMatrix() = default;

    // LFO parameter management (main thread)
    LfoParams& lfoParams(int index);
    const LfoParams& lfoParams(int index) const;
    void setLfoShape(int index, LfoShape shape);
    void setLfoRate(int index, float hz);
    void setLfoDepth(int index, float depth);
    void setLfoOffset(int index, float offset);
    void setLfoBipolar(int index, bool bipolar);
    void setLfoSync(int index, bool sync);

    // Route management (main thread)
    int addRoute(ModSource source, ModDest dest, float amount);
    void removeRoute(int index);
    void setRouteAmount(int index, float amount);
    void setRouteEnabled(int index, bool enabled);
    void clearAllRoutes();
    int routeCount() const { return routeCount_; }

    // External source values (main thread, typically from MIDI)
    void setModWheel(float value) { modWheel_.store(value, std::memory_order_relaxed); }
    void setAftertouch(float value) { aftertouch_.store(value, std::memory_order_relaxed); }

    // Process modulation for one sample of one voice. Audio thread only.
    // Advances state's LFO phases and writes resolved values to `out`.
    void process(ModValues& out, ModState& state, float envelopeLevel, int sampleRate);

    // Access LFO params array (for ModState::resetSyncedPhases)
    const LfoParams* lfoParamsArray() const { return lfos_; }

private:
    float sampleLfo(const LfoParams& params, float& phase, float& holdValue, int sampleRate);
    float getSource(ModSource source, const ModState& state, float envelopeLevel);

    LfoParams lfos_[MAX_LFOS];
    ModRoute routes_[MAX_ROUTES];
    int routeCount_ = 0;

    std::atomic<float> modWheel_{0.0f};
    std::atomic<float> aftertouch_{0.0f};
};

} // namespace broaudio
