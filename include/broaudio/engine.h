#pragma once

#include "broaudio/types.h"
#include "broaudio/core/ring_buffer.h"
#include "broaudio/dsp/params.h"
#include "broaudio/dsp/biquad.h"
#include "broaudio/dsp/compressor.h"
#include "broaudio/dsp/delay.h"
#include "broaudio/dsp/limiter.h"
#include "broaudio/mix/bus.h"
#include "broaudio/synth/modulation.h"
#include "broaudio/synth/voice.h"
#include "broaudio/synth/wavetable.h"
#include "broaudio/clip/clip.h"
#include "broaudio/spatial/listener.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace broaudio {

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool init();
    void shutdown();

    double currentTime() const;
    int sampleRate() const { return sampleRate_; }

    // --- Voices (synthesis) ---

    int createVoice();
    void removeVoice(int id);
    void setWaveform(int id, Waveform wf);
    void setFrequency(int id, float freq);
    void setGain(int id, float gain);
    void setVoicePan(int id, float pan);
    void setVoicePitchBend(int id, float semitones);
    void setVoiceWavetable(int id, std::shared_ptr<const WavetableBank> bank);
    void setAttackTime(int id, float seconds);
    void setDecayTime(int id, float seconds);
    void setSustainLevel(int id, float level);
    void setReleaseTime(int id, float seconds);
    void setVoiceNote(int id, int noteNumber, float velocity);
    void setVoicePersistent(int id, bool persistent);
    void setVoiceFilterEnabled(int id, bool enabled);
    void setVoiceFilterType(int id, BiquadFilter::Type type);
    void setVoiceFilterFrequency(int id, float freq);
    void setVoiceFilterQ(int id, float q);
    void setVoiceUnisonCount(int id, int count);
    void setVoiceUnisonDetune(int id, float semitones);
    void setVoiceUnisonStereoWidth(int id, float width);
    void startVoice(int id, double when);
    void stopVoice(int id, double when);

    // --- Modulation ---

    ModMatrix& modMatrix() { return modMatrix_; }

    // --- Mix buses ---

    static constexpr int MASTER_BUS_ID = 0;

    int createBus();                               // returns bus id, feeds into master
    void deleteBus(int busId);                     // cannot delete master
    void setBusGain(int busId, float gain);
    void setBusPan(int busId, float pan);
    void setBusMuted(int busId, bool muted);

    // Per-bus filter control
    int allocateBusFilterSlot(int busId);
    void releaseBusFilterSlot(int busId, int slot);
    void setBusFilterEnabled(int busId, int slot, bool enabled);
    void setBusFilterType(int busId, int slot, BiquadFilter::Type type);
    void setBusFilterFrequency(int busId, int slot, float freq);
    void setBusFilterQ(int busId, int slot, float q);
    void setBusFilterGain(int busId, int slot, float gainDB);

    // Per-bus delay control
    void setBusDelayEnabled(int busId, bool enabled);
    void setBusDelayTime(int busId, float seconds);
    void setBusDelayFeedback(int busId, float fb);
    void setBusDelayMix(int busId, float mix);

    // Per-bus compressor control
    void setBusCompressorEnabled(int busId, bool enabled);
    void setBusCompressorThreshold(int busId, float threshold);
    void setBusCompressorRatio(int busId, float ratio);
    void setBusCompressorAttack(int busId, float ms);
    void setBusCompressorRelease(int busId, float ms);
    void setBusCompressorSidechain(int busId, int sidechainBusId);

    // Per-bus reverb control
    void setBusReverbEnabled(int busId, bool enabled);
    void setBusReverbRoomSize(int busId, float size);
    void setBusReverbDamping(int busId, float damping);
    void setBusReverbMix(int busId, float mix);

    // Per-bus equalizer control
    void setBusEqEnabled(int busId, bool enabled);
    void setBusEqBandGain(int busId, int band, float gainDB);
    void setBusEqMasterGain(int busId, float gainDB);

    // Per-bus effect chain order
    void setBusEffectOrder(int busId, const EffectSlot* order, int count);

    // Per-bus distortion/waveshaper control
    void setBusDistortionEnabled(int busId, bool enabled);
    void setBusDistortionMode(int busId, DistortionMode mode);
    void setBusDistortionDrive(int busId, float drive);
    void setBusDistortionMix(int busId, float mix);
    void setBusDistortionOutputGain(int busId, float gain);
    void setBusDistortionCrushBits(int busId, float bits);
    void setBusDistortionCrushRate(int busId, float rate);

    // Per-bus chorus/flanger control
    void setBusChorusEnabled(int busId, bool enabled);
    void setBusChorusRate(int busId, float hz);
    void setBusChorusDepth(int busId, float seconds);
    void setBusChorusMix(int busId, float mix);
    void setBusChorusFeedback(int busId, float fb);
    void setBusChorusBaseDelay(int busId, float seconds);

    // Per-bus metering (read from main thread, written by audio thread)
    float getBusPeakL(int busId) const;
    float getBusPeakR(int busId) const;
    float getBusRmsL(int busId) const;
    float getBusRmsR(int busId) const;

    // Voice/clip bus routing
    void setVoiceBus(int voiceId, int busId);
    void setPlaybackBus(int instanceId, int busId);

    // Aux sends (voice, clip, and bus)
    void setVoiceSend(int voiceId, int sendBusId, float amount);
    void setPlaybackSend(int instanceId, int sendBusId, float amount);
    void setBusSend(int busId, int sendBusId, float amount);

    // --- Master output ---

    void setMasterGain(float gain);
    float masterGain() const { return masterGain_.load(std::memory_order_relaxed); }

    // --- Master limiter ---

    void setLimiterEnabled(bool enabled);
    void setLimiterThreshold(float thresholdDb);
    void setLimiterRelease(float releaseMs);

    // --- Filters (master bus shortcuts) ---

    static constexpr int MAX_FILTERS = Bus::MAX_FILTERS;

    int allocateFilterSlot();
    void releaseFilterSlot(int slot);
    void setFilterEnabled(int slot, bool enabled);
    void setFilterType(int slot, BiquadFilter::Type type);
    void setFilterFrequency(int slot, float freq);
    void setFilterQ(int slot, float q);
    void setFilterGain(int slot, float gainDB);

    // --- Delay (master bus shortcuts) ---

    void setDelayEnabled(bool enabled);
    void setDelayTime(float seconds);
    void setDelayFeedback(float fb);
    void setDelayMix(float mix);

    // --- Analysis ---

    AnalysisBuffer& outputBuffer() { return outputBuffer_; }
    const AnalysisBuffer& outputBuffer() const { return outputBuffer_; }
    AnalysisBuffer& micBuffer() { return micBuffer_; }
    const AnalysisBuffer& micBuffer() const { return micBuffer_; }

    // Get spectrum magnitudes from the output analysis buffer.
    // Writes `numBins` magnitude values (linear scale) into `outMagnitudes`.
    // numBins should be a power of 2 (max 8192). Returns actual bins written.
    int getSpectrum(float* outMagnitudes, int numBins) const;

    // --- Microphone ---

    bool startMicCapture();
    void stopMicCapture();
    bool isMicCapturing() const { return micCapturing_; }
    void setMicMuted(bool muted) { micMuted_.store(muted, std::memory_order_relaxed); }
    bool isMicMuted() const { return micMuted_.load(std::memory_order_relaxed); }
    void setMicMonitorGain(float g) { micMonitorGain_.store(g, std::memory_order_relaxed); }
    float micMonitorGain() const { return micMonitorGain_.load(std::memory_order_relaxed); }
    void setMicBus(int busId) { micBusId_.store(busId, std::memory_order_relaxed); }
    int micBus() const { return micBusId_.load(std::memory_order_relaxed); }

    // --- Sample-accurate event scheduling ---

    // Schedule a noteOn/noteOff to be dispatched sample-accurately in the audio callback.
    // `when` is engine time in seconds (from currentTime()).
    void scheduleNoteOn(int voiceId, double when);
    void scheduleNoteOff(int voiceId, double when);

    // --- Offline processing ---

    // Process mono samples through a bus's effect chain offline (non-realtime).
    // Creates a temporary bus with cloned params, processes in chunks, returns mono output.
    // Safe to call from the main thread. Does not affect live audio.
    std::vector<float> processEffectsOffline(int busId, const float* monoInput, int numSamples);

    // --- Recording ---

    void startRecording();
    void stopRecording();
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }
    std::vector<float> getRecordBuffer() const { return recordOutput_; }

    // --- Audio Clips ---

    int createClip(const float* samples, int numSamples, int channels = 1);
    void deleteClip(int clipId);
    int getClipSampleCount(int clipId) const;   // returns frame count
    int getClipChannels(int clipId) const;
    void getClipWaveform(int clipId, float* outMinMax, int numBins) const;

    // --- Clip Playback ---

    int playClip(int clipId, float gain = 1.0f, bool loop = false);
    void stopPlayback(int instanceId);
    void setPlaybackGain(int instanceId, float gain);
    void setPlaybackLoop(int instanceId, bool loop);
    void setPlaybackRegion(int instanceId, int start, int end);
    void setPlaybackPlaying(int instanceId, bool playing);
    void setPlaybackRate(int instanceId, float rate);
    void setPlaybackPan(int instanceId, float pan);
    float getPlaybackPosition(int instanceId) const;

    // --- Spatial (listener) ---

    void setListenerPosition(float x, float y, float z);
    void setListenerOrientation(float fx, float fy, float fz, float ux, float uy, float uz);
    const Listener& listener() const { return listener_; }

    // --- Spatial sources (voice) ---

    void setVoiceSpatialEnabled(int voiceId, bool enabled);
    void setVoiceSpatialPosition(int voiceId, float x, float y, float z);
    void setVoiceSpatialRefDistance(int voiceId, float dist);
    void setVoiceSpatialMaxDistance(int voiceId, float dist);
    void setVoiceSpatialRolloff(int voiceId, float rolloff);
    void setVoiceSpatialDistanceModel(int voiceId, DistanceModel model);

    // --- Spatial sources (clip playback) ---

    void setPlaybackSpatialEnabled(int instanceId, bool enabled);
    void setPlaybackSpatialPosition(int instanceId, float x, float y, float z);
    void setPlaybackSpatialRefDistance(int instanceId, float dist);
    void setPlaybackSpatialMaxDistance(int instanceId, float dist);
    void setPlaybackSpatialRolloff(int instanceId, float rolloff);
    void setPlaybackSpatialDistanceModel(int instanceId, DistanceModel model);

private:
    // Type aliases (must precede method declarations that use them)
    using VoiceList = std::vector<std::shared_ptr<Voice>>;
    using ClipList = std::vector<std::shared_ptr<AudioClip>>;
    using PlaybackList = std::vector<std::shared_ptr<ClipPlayback>>;
    using BusList = std::vector<std::shared_ptr<Bus>>;

    static void audioCallback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int total_amount);
    void generateSamples(int numFrames, const BusList& buses);
    void processBusEffects(Bus& bus, int numFrames);
    void processBusFilters(Bus& bus, float* buf, int numFrames);
    void processBusDelay(Bus& bus, float* buf, int numFrames);
    void processBusCompressor(Bus& bus, float* buf, int numFrames);
    void processBusChorus(Bus& bus, float* buf, int numFrames);
    void processBusDistortion(Bus& bus, float* buf, int numFrames);
    void processBusReverb(Bus& bus, float* buf, int numFrames);
    void processBusEqualizer(Bus& bus, float* buf, int numFrames);
    void updateBusMeters(Bus& bus, int numFrames);
    void mixBusIntoParent(Bus& child, Bus& parent, int numFrames);

    static void micCallback(void* userdata, SDL_AudioStream* stream,
                            int additional_amount, int total_amount);

    // RCU voice list
    std::atomic<std::shared_ptr<const VoiceList>> voices_{std::make_shared<const VoiceList>()};
    std::mutex voiceWriteMutex_;
    int nextVoiceId_ = 1;

    Voice* findVoice(int id);

    // RCU clip list
    std::atomic<std::shared_ptr<const ClipList>> clips_{std::make_shared<const ClipList>()};
    std::mutex mediaWriteMutex_;

    // RCU playback list
    std::atomic<std::shared_ptr<const PlaybackList>> playbacks_{std::make_shared<const PlaybackList>()};

    int nextClipId_ = 1;
    int nextPlaybackId_ = 1;

    AudioClip* findClip(int clipId) const;
    ClipPlayback* findPlayback(int instanceId) const;

    // RCU bus list — master bus is always id 0
    std::atomic<std::shared_ptr<const BusList>> buses_{std::make_shared<const BusList>()};
    std::mutex busWriteMutex_;
    int nextBusId_ = 1;   // 0 is reserved for master

    Bus* findBus(int busId) const;

    // Modulation
    ModMatrix modMatrix_;

    // Spatial
    Listener listener_;

    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioStream* micStream_ = nullptr;
    std::atomic<uint64_t> samplesGenerated_{0};
    int sampleRate_ = 44100;
    bool initialized_ = false;
    bool micCapturing_ = false;

    AnalysisBuffer outputBuffer_{16384};
    AnalysisBuffer micBuffer_{16384};
    std::mutex micMutex_;

    static constexpr int MIC_FIFO_SIZE = 22050;
    std::vector<float> micPlayback_ = std::vector<float>(MIC_FIFO_SIZE, 0.0f);
    std::atomic<uint64_t> micPlaybackWritePos_{0};
    uint64_t micPlaybackReadPos_ = 0;

    std::atomic<float> masterGain_{0.5f};
    Limiter masterLimiter_{44100, 2};
    std::atomic<bool> micMuted_{true};
    std::atomic<float> micMonitorGain_{0.5f};
    std::atomic<int> micBusId_{-1};  // -1 = direct-to-output (legacy), >= 0 = route through bus

    static constexpr int RECORD_RING_SIZE = 44100 * 60;
    std::vector<float> recordRing_ = std::vector<float>(RECORD_RING_SIZE, 0.0f);
    std::atomic<uint64_t> recordWritePos_{0};
    uint64_t recordStartPos_ = 0;
    std::atomic<bool> recording_{false};
    std::vector<float> recordOutput_;

    // Sample-accurate scheduled events (main thread writes, audio thread reads)
    struct ScheduledEvent {
        enum class Type : uint8_t { NoteOn, NoteOff };
        Type type;
        int voiceId;
        double when; // engine time in seconds
    };
    static constexpr int EVENT_RING_SIZE = 4096;
    ScheduledEvent eventRing_[EVENT_RING_SIZE];
    alignas(64) std::atomic<uint32_t> eventWrite_{0};
    alignas(64) std::atomic<uint32_t> eventRead_{0};

    // Pre-allocated scratch buffers for audio callbacks (avoids heap allocs on audio thread)
    std::vector<float> outputScratch_;
    std::vector<float> micScratch_;
};

} // namespace broaudio
