#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace broaudio {

// --- LFO ---

enum class LfoShape : uint8_t {
    Sine, Triangle, Square, SawUp, SawDown, SampleAndHold
};

struct Lfo {
    int id = 0;

    // Parameters (main thread → audio thread)
    std::atomic<LfoShape> shape{LfoShape::Sine};
    std::atomic<float> rate{1.0f};       // Hz
    std::atomic<float> depth{1.0f};      // 0-1 output scaling
    std::atomic<float> offset{0.0f};     // DC offset (-1 to 1)
    std::atomic<bool> bipolar{true};     // true: -1..1, false: 0..1
    std::atomic<bool> sync{false};       // true: retrigger on note-on

    // Audio-thread-only state
    float phase = 0.0f;
    float holdValue = 0.0f;  // for SampleAndHold
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

// Holds resolved modulation values for one voice during a sample block.
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

// --- Modulation matrix ---

// Manages LFOs, routing table, and per-sample modulation computation.
// Designed to be owned by the Engine. LFO state is audio-thread-only;
// routes and LFO params are set from the main thread.
class ModMatrix {
public:
    static constexpr int MAX_LFOS = 4;
    static constexpr int MAX_ROUTES = 16;

    ModMatrix();

    // LFO management
    Lfo& lfo(int index);
    const Lfo& lfo(int index) const;
    void setLfoShape(int index, LfoShape shape);
    void setLfoRate(int index, float hz);
    void setLfoDepth(int index, float depth);
    void setLfoOffset(int index, float offset);
    void setLfoBipolar(int index, bool bipolar);
    void setLfoSync(int index, bool sync);

    // Route management
    int addRoute(ModSource source, ModDest dest, float amount);  // returns route index
    void removeRoute(int index);
    void setRouteAmount(int index, float amount);
    void setRouteEnabled(int index, bool enabled);
    void clearAllRoutes();
    int routeCount() const { return routeCount_; }

    // Set external source values (called from main thread, typically from MIDI)
    void setModWheel(float value) { modWheel_.store(value, std::memory_order_relaxed); }
    void setAftertouch(float value) { aftertouch_.store(value, std::memory_order_relaxed); }

    // Process modulation for one sample. Called once per sample from the audio thread.
    // envelopeLevel: current ADSR level of the voice
    // velocity: note-on velocity (0-1)
    // noteNumber: MIDI note (0-127) for key tracking
    // sampleRate: for LFO phase advancement
    // Returns modulated values in `out`.
    void process(ModValues& out, float envelopeLevel, float velocity,
                 int noteNumber, int sampleRate);

    // Reset all LFO phases (call on note-on for synced LFOs)
    void resetSyncedLfos();

private:
    float lfoSample(Lfo& lfo, int sampleRate);
    float getSource(ModSource source, float envelopeLevel, float velocity, int noteNumber);

    Lfo lfos_[MAX_LFOS];
    ModRoute routes_[MAX_ROUTES];
    int routeCount_ = 0;

    std::atomic<float> modWheel_{0.0f};
    std::atomic<float> aftertouch_{0.0f};

    // Cached LFO outputs for the current sample
    float lfoOutputs_[MAX_LFOS] = {};
};

} // namespace broaudio
