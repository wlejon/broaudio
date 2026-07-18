#pragma once

#include "broaudio/types.h"
#include "broaudio/atomic_shared_ptr.h"
#include "broaudio/rcu.h"
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
#include "broaudio/mic_tap.h"
#include "broaudio/spatial/listener.h"
#include "broaudio/io/audio_file.h"
#include "broaudio/io/serialization.h"

#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

struct SDL_AudioStream;

namespace broaudio {

class FileStreamRunner;

// Options for createStreamFromFile. Frame counts are at the ENGINE sample
// rate (the ring holds resampled audio).
struct FileStreamOptions {
    int   ringFrames = 0;       // ring capacity; 0 = ~2 s at the engine rate
    int   prebufferFrames = 0;  // decoded before playback starts; 0 = ~500 ms
    bool  loop = false;         // worker rewinds the decoder at EOF (seamless)
    float gain = 1.0f;
};

// Snapshot of a streaming playback's ring state (disk or live PCM streams).
struct StreamStats {
    uint64_t decodedFrames = 0;   // total frames ever pushed into the ring
    uint64_t playedFrames = 0;    // frames consumed by the mixer
    uint64_t bufferedFrames = 0;  // decoded - played (waiting in the ring)
    uint64_t underrunFrames = 0;  // silent frames emitted while starved
    bool finished = false;        // disk stream: EOF reached and ring drained
    bool valid = false;           // false when the id is not a streaming playback
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool init();
    bool initHeadless();
    void shutdown();

    // Non-realtime maintenance — reaps finished voices, etc. Call from a
    // non-audio thread (e.g., UI tick). Safe to call frequently; each call
    // is cheap when no work is pending.
    void update();

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

    // --- Master pause ---
    // Suspends the engine's output clock: the realtime callback feeds silence
    // to the device without processing the graph, and renderBlock() becomes a
    // no-op — so voices, clip playback, scheduled events, and currentTime()
    // all freeze in place and resume exactly where they stopped. This is a
    // transport suspend, not a mute: nothing advances, nothing is dropped,
    // and playback rate/pitch are untouched on resume. RT-safe (one relaxed
    // atomic read on the audio thread).
    void setMasterPaused(bool paused) { masterPaused_.store(paused, std::memory_order_relaxed); }
    bool masterPaused() const { return masterPaused_.load(std::memory_order_relaxed); }

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

    // Multi-consumer mic-frame dispatch. Each tap describes the rate, frame
    // size, and AGC it wants; the audio thread fans the raw mic chunk out to
    // every active tap (each resampling / AGC-ing / chunking as configured)
    // before the samples are written into the analysis ring and monitor FIFO.
    // This is the sole audio-thread mic hook — very-low-latency consumers
    // (wake-word detectors, ASR frontends, level meters) attach a tap and do
    // real-time-safe work directly in the callback (no allocations after
    // addMicTap, no locks, no blocking).
    //
    // addMicTap / removeMicTap / getMicTapStats are safe to call from any
    // single thread (typically the main thread) but must NOT be called
    // concurrently with each other — the registry is single-writer + multi-
    // reader (RCU). The audio thread reads via a wait-free snapshot.
    //
    // Returns an opaque id; kInvalidMicTapId on failure (callback is null,
    // targetRate < 0, chunkFrames < 0, or the resampler stream could not be
    // created). Pass the id to removeMicTap when the consumer goes away.
    MicTapId addMicTap(const MicTapConfig& cfg, MicTapCallback cb);
    void     removeMicTap(MicTapId id);
    MicTapStats getMicTapStats(MicTapId id) const;

    // Offline / headless seam — fan `numSamples` of mono FP32 audio out to the
    // active mic taps exactly as the recording callback would, but WITHOUT
    // touching the analysis ring or monitor FIFO (so it never races the SPSC
    // publication those rely on). Lets a headless script or unit test drive
    // tap consumers (resample → AGC → chunk → callback) with synthetic audio.
    //
    // MUST NOT be called concurrently with live capture: it mutates the same
    // per-tap resampler / AGC / chunk-buffer state the audio thread owns. Use
    // it only when no recording device is open (the headless default) or while
    // capture is stopped.
    void injectMicSamples(const float* samples, int numSamples);

    // --- Sample-accurate event scheduling ---

    // Schedule a noteOn/noteOff to be dispatched sample-accurately in the audio callback.
    // `when` is engine time in seconds (from currentTime()).
    void scheduleNoteOn(int voiceId, double when);
    void scheduleNoteOff(int voiceId, double when);

    // --- Offline processing ---

    // Render numFrames of audio without outputting to any device.
    // Drives the full pipeline (voices, clips, bus effects, mix, limiter)
    // and updates metering, analysis buffers, and recording.
    // Use in headless mode (after initHeadless()) to pump the audio engine.
    void renderBlock(int numFrames);

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
    // Sample-accurate scheduled playback: begin the clip when the audio clock
    // reaches `when` seconds (engine time, from currentTime()). A `when` at or
    // before now plays immediately. Use this to queue streamed chunks so they
    // join on the audio thread without main-thread setTimeout jitter or drift.
    int playClipAt(int clipId, double when, float gain = 1.0f, bool loop = false);
    void stopPlayback(int instanceId);

    // --- Streaming PCM source (live voice / network audio) ---
    //
    // A persistent, spatializable playback fed PCM frame-by-frame instead of
    // from a fixed clip. Returns a playback instance id usable with every
    // setPlayback*/setPlaybackSpatial* method (gain, pan, bus, sends, 3D
    // position). The audio thread reads a ring the producer appends to, so a
    // continuous stream plays click-free without per-frame clip churn.
    //
    //   int s = createStream(1);                  // mono voice source
    //   setPlaybackSpatialEnabled(s, true);
    //   setPlaybackSpatialPosition(s, x, y, z);
    //   pushStreamSamples(s, pcm, n);             // per received packet
    //   ...
    //   closeStream(s);
    //
    // ringFrames 0 selects a ~2 s default at the engine rate. Returns -1 on bad
    // args. pushStreamSamples is single-producer (call from one thread); samples
    // must be at the engine sample rate (resample on the producer side).
    int  createStream(int channels = 1, int ringFrames = 0);
    // `numSamples` is the total interleaved sample count (frames * channels).
    // Returns frames written.
    int  pushStreamSamples(int instanceId, const float* samples, int numSamples);
    void closeStream(int instanceId);

    // --- Disk-streamed file playback ---
    //
    // Play a large audio file WITHOUT decoding it into RAM: a cold worker
    // thread decodes the file incrementally (WAV, FLAC, MP3, Ogg Vorbis),
    // resamples to the engine rate, and feeds the same streaming ring a live
    // PCM stream uses. The returned playback instance id works with every
    // setPlayback* control (gain, pan, bus, sends, rate, spatial). Playback
    // starts automatically once the prebuffer is decoded. Looping follows
    // setPlaybackLoop live: the worker rewinds the decoder at EOF, so loops
    // are seamless. There is no seek (matching clip playback). On ring
    // underrun the mixer emits silence and counts it (see getStreamStats).
    //
    // Returns the playback instance id, or -1 on failure with *outError set
    // to an actionable message (unreadable file, unsupported codec, >2
    // channels). Close with closeStream (also stops + joins the worker).
    int createStreamFromFile(const char* path, std::string* outError = nullptr);
    int createStreamFromFile(const char* path, const FileStreamOptions& opts,
                             std::string* outError = nullptr);

    // Ring/underrun statistics for a streaming playback (disk or live PCM).
    // stats.valid is false when the id is not an active streaming playback.
    StreamStats getStreamStats(int instanceId) const;

    void setPlaybackGain(int instanceId, float gain);
    void setPlaybackLoop(int instanceId, bool loop);
    void setPlaybackRegion(int instanceId, int start, int end);
    void setPlaybackPlaying(int instanceId, bool playing);
    void setPlaybackRate(int instanceId, float rate);
    void setPlaybackPan(int instanceId, float pan);
    float getPlaybackPosition(int instanceId) const;

    // Jump the playback cursor to `seconds`.
    //  - Clip playbacks: seconds are measured from the playback region start
    //    (the whole clip unless setPlaybackRegion narrowed it); clamped to
    //    the region. Takes effect at the next mixed block (within a few ms).
    //  - Disk-streamed playbacks (createStreamFromFile): seconds are file
    //    time; the decode worker seeks the codec, already-buffered audio is
    //    skipped via a lock-free fence, and playback resumes once the worker
    //    has refilled from the new position (silence in between, counted as
    //    underrun). Seeking a finished stream restarts it.
    //  - Live PCM streams (createStream): no backing store — no-op.
    void seekPlayback(int instanceId, double seconds);

    // Playback position in seconds — the seconds-domain counterpart of the
    // normalized getPlaybackPosition. Clip playbacks: seconds from the region
    // start (wraps when looping). Disk streams: current file time (seek-
    // aware). Live PCM streams: seconds of audio consumed since the stream
    // opened. Returns 0 for unknown handles.
    double getPlaybackPositionSeconds(int instanceId) const;

    // --- Spatial (listener) ---

    void setListenerPosition(float x, float y, float z);
    void setListenerOrientation(float fx, float fy, float fz, float ux, float uy, float uz);
    void setListenerVelocity(float x, float y, float z);
    const Listener& listener() const { return listener_; }

    // --- Doppler ---
    //
    // Global pitch-shift from relative listener/source motion, applied to
    // spatialized sources on the render thread: clip playbacks compose the
    // ratio into their resampling rate (multiplies setPlaybackRate), voices
    // fold it into pitch (12·log2(ratio) semitones on top of pitch bend).
    // Model + clamps: see computeDopplerRatio in spatial/listener.h — ratio
    // is clamped to [0.5, 2.0]. factor 0 (or all-zero velocities, the
    // default) disables; 1 is physical; >1 exaggerates. Streaming playbacks
    // (live PCM / disk streams) are NOT Doppler-shifted — their ring mixer
    // has no resampler (same reason setPlaybackRate is a no-op for them).
    void setDopplerFactor(float factor);
    float dopplerFactor() const { return dopplerFactor_.load(std::memory_order_relaxed); }

    // Last Doppler ratio the mixer applied to a spatialized source (1.0
    // until one has been mixed with doppler active). Introspection/testing.
    float getPlaybackDopplerRatio(int instanceId) const;
    float getVoiceDopplerRatio(int voiceId) const;

    // --- Head model (spatial filtering) ---

    HeadModel& headModel() { return headModel_; }
    const HeadModel& headModel() const { return headModel_; }
    void setHeadModelEnabled(bool enabled);
    void setHeadModelIldStrength(float strength);
    void setHeadModelBehindAttenuation(float atten);
    void setHeadModelNearCutoff(float front, float behind);
    void setHeadModelFarCutoffRatio(float ratio);
    void setHeadModelElevation(float nearHz, float farHz);
    void setHeadModelCutoffRange(float minHz, float maxHz);

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

    // --- Audio file I/O ---

    // Create a clip by loading an audio file (WAV, FLAC, MP3, Ogg Vorbis;
    // Ogg Opus with BROAUDIO_OPUS). Returns clip id on success, -1 on failure.
    int createClipFromFile(const char* path);

    // As createClipFromFile, but on failure *outError receives an actionable
    // message (decode error, size cap, resample failure) instead of only a
    // log line. Safe to call from a non-audio background thread — bindings
    // use this to reject an async load with the real reason.
    int createClipFromFileEx(const char* path, std::string* outError);

    // Async version — decodes and resamples on a background thread.
    // Returns a future that resolves to the clip id (-1 on failure).
    std::future<int> createClipFromFileAsync(const char* path);

    // Maximum decoded clip size in bytes. Files exceeding this after decode
    // are rejected (createClipFromFile returns -1). Default ~200 MB.
    // Set to 0 to disable the limit.
    void setMaxClipDecodedBytes(size_t bytes) { maxClipDecodedBytes_ = bytes; }
    size_t maxClipDecodedBytes() const { return maxClipDecodedBytes_; }

    // Export the current recording buffer to a WAV file.
    // Returns true on success. Call after stopRecording().
    bool exportRecordingToWav(const char* path);

    // --- Preset application ---

    void applyVoicePreset(int voiceId, const VoicePreset& preset);
    void applyBusPreset(int busId, const BusPreset& preset);
    void applyModPreset(const ModPreset& preset);
    void applyEnginePreset(const EnginePreset& preset);

private:
    // Type aliases (must precede method declarations that use them)
    using VoiceList = std::vector<std::shared_ptr<Voice>>;
    using ClipList = std::vector<std::shared_ptr<AudioClip>>;
    using PlaybackList = std::vector<std::shared_ptr<ClipPlayback>>;
    using BusList = std::vector<std::shared_ptr<Bus>>;

    static void audioCallback(void* userdata, SDL_AudioStream* stream,
                              int additional_amount, int total_amount);
    void processOutputChunk(SDL_AudioStream* stream, int numFrames);
    void renderInternal(int numFrames);
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
    // Ring-read mix path for streaming playbacks (clip->streaming). Shared by the
    // headless and realtime clip loops. Outputs silence on underrun, skips ahead
    // on overrun.
    void mixStreamPlayback(ClipPlayback* pb, AudioClip* clip, float* targetBuf,
                           float* clipSendBuf, float clipSendAmt,
                           bool spatialFilterActive, const HeadParams& headParams,
                           int numFrames, int startFrame);

    static void micCallback(void* userdata, SDL_AudioStream* stream,
                            int additional_amount, int total_amount);
    void processMicSamples(const float* samples, int numSamples);

    // RCU reclamation domain — must be declared before any AtomicSharedPtr
    // member so the slots can bind to it in their initializers.
    RcuDomain rcu_;

    // RCU voice list
    AtomicSharedPtr<const VoiceList> voices_{rcu_, std::make_shared<const VoiceList>()};
    std::mutex voiceWriteMutex_;
    int nextVoiceId_ = 1;
    // Set by audio thread when one-shot voices finish; cleared by update().
    std::atomic<bool> voiceCleanupNeeded_{false};
    void reapFinishedVoicesLocked();

    Voice* findVoice(int id);

    // RCU clip list
    AtomicSharedPtr<const ClipList> clips_{rcu_, std::make_shared<const ClipList>()};
    std::mutex mediaWriteMutex_;

    // RCU playback list
    AtomicSharedPtr<const PlaybackList> playbacks_{rcu_, std::make_shared<const PlaybackList>()};

    int nextClipId_ = 1;
    int nextPlaybackId_ = 1;

    AudioClip* findClip(int clipId) const;
    ClipPlayback* findPlayback(int instanceId) const;

    // Create a streaming clip + persistent playback pair (shared by
    // createStream and createStreamFromFile). Returns the playback id.
    int createStreamPlayback(int channels, int ringFrames, bool startPlaying,
                             float gain, bool loop,
                             std::shared_ptr<AudioClip>& outClip,
                             std::shared_ptr<ClipPlayback>& outPb);

    // Disk-stream workers, keyed by playback id. Control plane only
    // (create/close/shutdown on the main thread) — the audio thread never
    // touches this list; it sees only the clip ring.
    std::vector<std::unique_ptr<FileStreamRunner>> fileStreams_;
    mutable std::mutex fileStreamsMutex_;
    void stopFileStream(int instanceId);   // stop + join + erase, if present
    void stopAllFileStreams();

    // RCU bus list — master bus is always id 0
    AtomicSharedPtr<const BusList> buses_{rcu_, std::make_shared<const BusList>()};
    std::mutex busWriteMutex_;
    int nextBusId_ = 1;   // 0 is reserved for master

    Bus* findBus(int busId) const;

    // Modulation
    ModMatrix modMatrix_;

    // Spatial
    Listener listener_;
    HeadModel headModel_;

    SDL_AudioStream* stream_ = nullptr;
    SDL_AudioStream* micStream_ = nullptr;
    std::atomic<uint64_t> samplesGenerated_{0};
    int sampleRate_ = 44100;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> micCapturing_{false};

    AnalysisBuffer outputBuffer_{16384};
    AnalysisBuffer micBuffer_{16384};
    // micBuffer_ uses AnalysisBuffer's atomic writePos_ (acquire/release) for
    // SPSC publication; no lock is required between the recording callback
    // (writer) and the analysis readers on the main thread.

    // Multi-consumer mic-tap registry. Each tap owns its own SDL_AudioStream
    // (when resampling) + Agc + chunk-buffer state. The audio thread loads a
    // wait-free snapshot of the list and dispatches to each tap in order.
    struct MicTap {
        MicTapId         id          = kInvalidMicTapId;
        MicTapConfig     cfg{};
        MicTapCallback   callback;
        SDL_AudioStream* resampler   = nullptr;   // nullptr when targetRate==0 or matches engine rate
        int              effectiveRate = 0;       // rate samples arrive at the callback
        Agc              agc{};
        std::vector<float> chunkBuf;              // accumulator when cfg.chunkFrames > 0
        std::vector<float> scratch;               // resample / per-call temp
        // Stats — audio-thread writer, main-thread reader; relaxed is fine
        // because the read is purely diagnostic.
        std::atomic<std::uint64_t> framesDelivered{0};
        std::atomic<std::uint64_t> samplesDelivered{0};
        std::atomic<int>           rollingPeakX10000{0};

        MicTap() = default;
        ~MicTap();
        MicTap(const MicTap&) = delete;
        MicTap& operator=(const MicTap&) = delete;
    };
    using MicTapList = std::vector<std::shared_ptr<MicTap>>;
    AtomicSharedPtr<const MicTapList> micTaps_{rcu_, std::make_shared<const MicTapList>()};
    // Single-writer-per-slot — callers must serialise addMicTap / removeMicTap
    // externally. nextMicTapId_ is only touched by those calls.
    MicTapId nextMicTapId_ = 1;

    // Dispatch a raw mic chunk to every active tap. Called by processMicSamples
    // on the audio thread, before the legacy callback and ring writes.
    void dispatchMicTaps(const float* samples, int numSamples);
    void deliverTapChunk(MicTap& tap, const float* samples, int numSamples);

    static constexpr int MIC_FIFO_SIZE = 22050;
    std::vector<float> micPlayback_ = std::vector<float>(MIC_FIFO_SIZE, 0.0f);
    std::atomic<uint64_t> micPlaybackWritePos_{0};
    std::atomic<uint64_t> micPlaybackReadPos_{0};

    std::atomic<float> masterGain_{0.5f};
    std::atomic<bool> masterPaused_{false};
    Smoother smoothMasterGain_;
    Limiter masterLimiter_{44100, 2};
    std::atomic<bool> micMuted_{true};
    std::atomic<float> micMonitorGain_{0.5f};
    std::atomic<int> micBusId_{-1};  // -1 = direct-to-output (legacy), >= 0 = route through bus

    static constexpr int RECORD_RING_SIZE = 44100 * 60;
    std::vector<float> recordRing_ = std::vector<float>(RECORD_RING_SIZE, 0.0f);
    std::atomic<uint64_t> recordWritePos_{0};
    std::atomic<uint64_t> recordStartPos_{0};
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

    // Max decoded clip size (bytes). 0 = unlimited.
    size_t maxClipDecodedBytes_ = 200 * 1024 * 1024;  // 200 MB
};

} // namespace broaudio
