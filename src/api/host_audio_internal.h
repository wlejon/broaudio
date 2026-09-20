#pragma once

#include "api.h"
#include "host_class.h"
#include "object_builder.h"

#include <broaudio/dsp/biquad.h>
#include <broaudio/dsp/delay.h>
#include <broaudio/dsp/distortion.h>
#include <broaudio/dsp/fft.h>
#include <broaudio/dsp/params.h>
#include <broaudio/dsp/resampler.h>
#include <broaudio/engine.h>
#include <broaudio/io/audio_file.h>
#include <broaudio/io/serialization.h>
#include <broaudio/mic_tap.h>
#include <broaudio/midi/midi_input.h>
#include <broaudio/sequencer/sequence.h>
#include <broaudio/synth/modulation.h>
#include <broaudio/synth/voice_allocator.h>
#include <broaudio/synth/wavetable.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace broaudio::api {

inline constexpr uint32_t kHostAudioContextTag       = 0x41435458u; // 'ACTX'
inline constexpr uint32_t kHostAudioNodeTag          = 0x414E4F44u; // 'ANOD'
inline constexpr uint32_t kHostAudioParamTag         = 0x41504152u; // 'APAR'
inline constexpr uint32_t kHostAudioBufferTag        = 0x41425546u; // 'ABUF'
inline constexpr uint32_t kHostPeriodicWaveTag       = 0x50574156u; // 'PWAV'
inline constexpr uint32_t kHostVoiceAllocatorTag     = 0x56414C43u; // 'VALC'
inline constexpr uint32_t kHostModMatrixTag          = 0x4D4F444Du; // 'MODM'
inline constexpr uint32_t kHostMidiInputTag          = 0x4D494449u; // 'MIDI'
inline constexpr uint32_t kHostSequenceTag           = 0x53455155u; // 'SEQU'
inline constexpr uint32_t kHostMediaStreamTag        = 0x4D535452u; // 'MSTR'
inline constexpr uint32_t kHostMediaStreamNodeTag    = 0x4D534E44u; // 'MSND'

struct HostAudioContext {
    uint32_t tag = kHostAudioContextTag;
    std::string state = "running";
    std::vector<int> voiceIds;
};

enum class AudioNodeType : uint8_t {
    Destination = 0,
    Gain,
    Oscillator,
    BiquadFilter,
    Analyser,
    BufferSource,
    Panner,
    StereoPanner,
    Delay,
    DynamicsCompressor,
    WaveShaper,
    Convolver,
    ChannelSplitter,
    ChannelMerger,
    MediaStreamSource,
    Generic,
};

struct HostAudioNode {
    uint32_t tag = kHostAudioNodeTag;
    AudioNodeType nodeType = AudioNodeType::Generic;
    std::vector<ev::Persistent> connectedTargets;

    ~HostAudioNode() {
        for (auto& t : connectedTargets) {
            t.set(ev::undefined());
        }
    }
};

enum class AudioParamTarget : uint8_t {
    Generic = 0,
    Gain,
    VoiceFrequency,
    VoiceDetune,
    VoicePan,
    FilterFrequency,
    FilterQ,
    FilterGain,
    PlaybackRate,
    PlaybackDetune,
    DelayTime,
    Pan,
    PannerPositionX,
    PannerPositionY,
    PannerPositionZ,
    PannerOrientationX,
    PannerOrientationY,
    PannerOrientationZ,
    CompressorThreshold,
    CompressorKnee,
    CompressorRatio,
    CompressorAttack,
    CompressorRelease,
    VoiceAttack,
    VoiceDecay,
    VoiceSustain,
    VoiceRelease,
    VoicePitchBend,
};

enum class ParamEventType : uint8_t {
    SetValue,
    LinearRamp,
    ExponentialRamp,
    SetTarget,
    SetValueCurve,
};

struct ParamTimelineEvent {
    ParamEventType type = ParamEventType::SetValue;
    double time = 0.0;
    double duration = 0.0;
    float value = 0.0f;
    float timeConstant = 0.0f;
    std::vector<float> curve;
};

struct HostAudioParam {
    uint32_t tag = kHostAudioParamTag;
    AudioParamTarget target = AudioParamTarget::Generic;
    int targetId = -1;
    float value = 1.0f;
    float defaultValue = 1.0f;
    float minValue = -3.402823466e+38f;
    float maxValue = 3.402823466e+38f;

    std::vector<ParamTimelineEvent> timeline;

    float evaluate(double t) const;
    void addSetValue(float val, double startTime);
    void addLinearRamp(float val, double endTime);
    void addExponentialRamp(float val, double endTime);
    void addSetTarget(float target, double startTime, float timeConstant);
    void addSetValueCurve(const float* data, size_t count, double startTime, double duration);
    void cancelScheduledValues(double cancelTime);
    void cancelAndHoldAtTime(double cancelTime);
};

struct HostGainNode {
    HostAudioNode base;
    HostAudioParam* gainParam = nullptr;
};

struct HostOscillatorNode {
    HostAudioNode base;
    int voiceId = -1;
    std::string type = "sine";
    bool started = false;
    bool stopped = false;
    // The GainNode this oscillator was connect()ed to, read back at start():
    // broaudio has no node graph, so `osc.connect(gain); gain.gain.value = g`
    // reaches the voice as its gain. Undefined until connect, cleared by
    // disconnect.
    ev::Persistent connectedGain;
};

struct HostPeriodicWave {
    uint32_t tag = kHostPeriodicWaveTag;
    std::vector<float> real;
    std::vector<float> imag;
    bool disableNormalization = false;
    std::shared_ptr<broaudio::WavetableBank> wavetable;
};

struct HostBiquadFilterNode {
    HostAudioNode base;
    int slot = -1;
    std::string type = "lowpass";
};

struct HostAnalyserNode {
    HostAudioNode base;
    int fftSize = 2048;
    float minDecibels = -100.0f;
    float maxDecibels = -30.0f;
    float smoothingTimeConstant = 0.8f;
    // What the analyser taps: 0 = engine output, 1 = microphone, 2 = both
    // summed (mic only while it is not muted). A MediaStreamAudioSourceNode
    // connect()ed to the analyser sets 1; the `source` property sets any.
    int source = 0;
    std::vector<float> smoothedMagnitudes;
    std::shared_ptr<broaudio::AnalysisBuffer> inputTapBuffer;
    bool hasConnectedInput = false;
};

struct HostAudioBuffer {
    uint32_t tag = kHostAudioBufferTag;
    int numberOfChannels = 1;
    int length = 0;
    int sampleRate = 44100;
    std::vector<std::vector<float>> channels;
};

struct HostAudioBufferSourceNode {
    HostAudioNode base;
    HostAudioBuffer* buffer = nullptr;
    bool loop = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    int clipId = -1;
    int playbackId = -1;
    bool started = false;
    bool stopped = false;
};

struct HostPannerNode {
    HostAudioNode base;
    HostAudioParam* posParamX = nullptr;
    HostAudioParam* posParamY = nullptr;
    HostAudioParam* posParamZ = nullptr;
    std::string panningModel = "equalpower";
    std::string distanceModel = "inverse";
    float refDistance = 1.0f;
    float maxDistance = 10000.0f;
    float rolloffFactor = 1.0f;
    float coneInnerAngle = 360.0f;
    float coneOuterAngle = 360.0f;
    float coneOuterGain = 0.0f;
    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float orientX = 1.0f, orientY = 0.0f, orientZ = 0.0f;
};

struct HostStereoPannerNode {
    HostAudioNode base;
    HostAudioParam* panParam = nullptr;
    float pan = 0.0f;
};

struct HostDelayNode {
    HostAudioNode base;
    HostAudioParam* delayTimeParam = nullptr;
    double maxDelayTime = 1.0;
};

struct HostDynamicsCompressorNode {
    HostAudioNode base;
    HostAudioParam* thresholdParam = nullptr;
    HostAudioParam* kneeParam = nullptr;
    HostAudioParam* ratioParam = nullptr;
    HostAudioParam* attackParam = nullptr;
    HostAudioParam* releaseParam = nullptr;
    float reduction = 0.0f;
    float envelope = 0.0f;
};

struct HostWaveShaperNode {
    HostAudioNode base;
    std::vector<float> curve;
    std::string oversample = "none";
};

struct HostConvolverNode {
    HostAudioNode base;
    HostAudioBuffer* buffer = nullptr;
    bool normalize = true;
};

struct HostChannelSplitterNode {
    HostAudioNode base;
    int numberOfOutputs = 6;
};

struct HostChannelMergerNode {
    HostAudioNode base;
    int numberOfInputs = 6;
};

struct HostVoiceAllocator {
    uint32_t tag = kHostVoiceAllocatorTag;
    std::unique_ptr<broaudio::VoiceAllocator> allocator;
    ev::Persistent voiceSetupCallback;
};

struct HostModMatrix {
    uint32_t tag = kHostModMatrixTag;
    broaudio::ModMatrix* matrix = nullptr;
};

struct HostMidiInput {
    uint32_t tag = kHostMidiInputTag;
    std::unique_ptr<broaudio::MidiInput> midi;
    ev::Persistent pitchBendCb;
    ev::Persistent rawCb;
    std::vector<ev::Persistent> ccCallbacks = std::vector<ev::Persistent>(128);
};

struct HostSequence {
    uint32_t tag = kHostSequenceTag;
    std::unique_ptr<broaudio::Sequence> seq;
    std::vector<ev::Persistent> automationCallbacks;
};

struct HostMediaStream {
    uint32_t tag = kHostMediaStreamTag;
};

struct HostMediaStreamAudioSourceNode {
    HostAudioNode base;
};

// ---------------------------------------------------------------------------
// HostClass Declarations (extern)
// ---------------------------------------------------------------------------

extern HostClass g_audioNodeClass;
extern HostClass g_audioDestinationNodeClass;
extern HostClass g_audioParamClass;
extern HostClass g_audioContextClass;
extern HostClass g_audioBufferClass;
extern HostClass g_gainNodeClass;
extern HostClass g_oscillatorNodeClass;
extern HostClass g_periodicWaveClass;
extern HostClass g_biquadFilterNodeClass;
extern HostClass g_analyserNodeClass;
extern HostClass g_audioBufferSourceNodeClass;
extern HostClass g_pannerNodeClass;
extern HostClass g_stereoPannerNodeClass;
extern HostClass g_delayNodeClass;
extern HostClass g_dynamicsCompressorNodeClass;
extern HostClass g_waveShaperNodeClass;
extern HostClass g_convolverNodeClass;
extern HostClass g_channelSplitterNodeClass;
extern HostClass g_channelMergerNodeClass;
extern HostClass g_voiceAllocatorClass;
extern HostClass g_modMatrixClass;
extern HostClass g_midiInputClass;
extern HostClass g_sequenceClass;
extern HostClass g_mediaStreamClass;
extern HostClass g_mediaStreamAudioSourceNodeClass;

// ---------------------------------------------------------------------------
// Destructors
// ---------------------------------------------------------------------------

void hostAudioContextDtor(void* p);
void hostAudioNodeDtor(void* p);
void hostAudioParamDtor(void* p);
void hostAudioBufferDtor(void* p);
void hostGainDtor(void* p);
void hostOscillatorDtor(void* p);
void hostPeriodicWaveDtor(void* p);
void hostBiquadFilterDtor(void* p);
void hostAnalyserDtor(void* p);
void hostAudioBufferSourceDtor(void* p);
void hostPannerDtor(void* p);
void hostStereoPannerDtor(void* p);
void hostDelayDtor(void* p);
void hostDynamicsCompressorDtor(void* p);
void hostWaveShaperDtor(void* p);
void hostConvolverDtor(void* p);
void hostChannelSplitterDtor(void* p);
void hostChannelMergerDtor(void* p);
void hostVoiceAllocatorDtor(void* p);
void hostModMatrixDtor(void* p);
void hostMidiInputDtor(void* p);
void hostSequenceDtor(void* p);
void hostMediaStreamDtor(void* p);
void hostMediaStreamAudioSourceNodeDtor(void* p);

// ---------------------------------------------------------------------------
// Unwrap Helpers
// ---------------------------------------------------------------------------

HostAudioContext* hostAudioContextOf(Value v);
HostAudioNode* hostAudioNodeOf(Value v);
HostAudioParam* hostAudioParamOf(Value v);
HostAudioBuffer* hostAudioBufferOf(Value v);
HostPeriodicWave* hostPeriodicWaveOf(Value v);

template <typename T>
T* nodeOfKind(Value v, AudioNodeType kind) {
    HostAudioNode* n = hostAudioNodeOf(v);
    if (!n || n->nodeType != kind) return nullptr;
    return reinterpret_cast<T*>(n);
}

inline HostOscillatorNode* oscOf(Value v) {
    return nodeOfKind<HostOscillatorNode>(v, AudioNodeType::Oscillator);
}
inline HostBiquadFilterNode* filterOf(Value v) {
    return nodeOfKind<HostBiquadFilterNode>(v, AudioNodeType::BiquadFilter);
}
inline HostAnalyserNode* analyserOf(Value v) {
    return nodeOfKind<HostAnalyserNode>(v, AudioNodeType::Analyser);
}
inline HostAudioBufferSourceNode* bufSrcOf(Value v) {
    return nodeOfKind<HostAudioBufferSourceNode>(v, AudioNodeType::BufferSource);
}
inline HostPannerNode* pannerOf(Value v) {
    return nodeOfKind<HostPannerNode>(v, AudioNodeType::Panner);
}
inline HostStereoPannerNode* stereoPannerOf(Value v) {
    return nodeOfKind<HostStereoPannerNode>(v, AudioNodeType::StereoPanner);
}
inline HostDelayNode* delayOf(Value v) {
    return nodeOfKind<HostDelayNode>(v, AudioNodeType::Delay);
}
inline HostDynamicsCompressorNode* compressorOf(Value v) {
    return nodeOfKind<HostDynamicsCompressorNode>(v, AudioNodeType::DynamicsCompressor);
}
inline HostWaveShaperNode* waveShaperOf(Value v) {
    return nodeOfKind<HostWaveShaperNode>(v, AudioNodeType::WaveShaper);
}
inline HostConvolverNode* convolverOf(Value v) {
    return nodeOfKind<HostConvolverNode>(v, AudioNodeType::Convolver);
}
inline HostChannelSplitterNode* channelSplitterOf(Value v) {
    return nodeOfKind<HostChannelSplitterNode>(v, AudioNodeType::ChannelSplitter);
}
inline HostChannelMergerNode* channelMergerOf(Value v) {
    return nodeOfKind<HostChannelMergerNode>(v, AudioNodeType::ChannelMerger);
}

HostVoiceAllocator* hostVoiceAllocatorOf(Value v);
HostModMatrix* hostModMatrixOf(Value v);
HostMidiInput* hostMidiInputOf(Value v);
HostSequence* hostSequenceOf(Value v);
HostMediaStream* hostMediaStreamOf(Value v);
HostMediaStreamAudioSourceNode* hostMediaStreamNodeOf(Value v);

// ---------------------------------------------------------------------------
// Argument / Data Helpers
// ---------------------------------------------------------------------------

inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isnan(d) ? 0.0 : d;
}

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return static_cast<int32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return static_cast<uint32_t>(static_cast<int64_t>(numAt(args, i)));
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return ev::toBool(args[i]);
}

inline bool hasArg(std::span<const Value> args, size_t i) {
    return i < args.size() && !ev::isUndefined(args[i]);
}

inline Value hostMakeDomError(const char* name, const std::string& message) {
    auto g = ev::globalValue("Error");
    Value errObj;
    Value msgVal = ev::fromUtf8(message);
    if (g.found) {
        ev::CallResult res = ev::construct(g.value, std::span<const Value>(&msgVal, 1));
        errObj = res.thrown ? ev::createObject() : res.value;
    } else {
        errObj = ev::createObject();
    }
    if (name && name[0]) {
        ev::setProperty(errObj, "name", ev::fromUtf8(name));
    }
    return errObj;
}

template <typename T, typename Convert>
inline bool plainArrayData(Value v, std::vector<T>& storage, Convert convert,
                           const T** outData, size_t* outCount) {
    if (!ev::isObject(v)) return false;
    ev::Persistent root(v);
    Value lenV = ev::getProperty(root.get(), "length");
    if (ev::isUndefined(lenV) || ev::isObject(lenV)) return false;
    uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
    storage.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = ev::isObject(e) ? 0.0 : ev::toDouble(e);
        storage[i] = std::isnan(d) ? T{} : convert(d);
    }
    *outData = storage.data();
    *outCount = n;
    return true;
}

inline bool floatData(Value v, std::vector<float>& storage,
                      const float** outData, size_t* outCount) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = reinterpret_cast<const float*>(info.data);
        *outCount = info.byteLength / sizeof(float);
        return true;
    }
    return plainArrayData<float>(
        v, storage, [](double d) { return static_cast<float>(d); }, outData, outCount);
}

inline bool bufferBytes(Value v, const uint8_t** outData, size_t* outLen,
                        size_t* outElemSize) {
    if (auto info = ev::typedArrayInfo(v)) {
        *outData = info.data;
        *outLen = info.byteLength;
        *outElemSize = info.bytesPerElement ? info.bytesPerElement : 1;
        return true;
    }
    if (auto buf = ev::arrayBufferInfo(v)) {
        *outData = buf.data;
        *outLen = buf.byteLength;
        *outElemSize = 1;
        return true;
    }
    return false;
}

inline Value makeFloat32Array(const float* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Float32, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data), count * sizeof(float));
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

inline Value makeFloat32Array(const std::vector<float>& vec) {
    return makeFloat32Array(vec.data(), vec.size());
}

inline Value makeUint8Array(const uint8_t* data, size_t count) {
    Value arr = ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(count));
    if (data && count > 0) {
        std::span<const uint8_t> bytes(data, count);
        ev::fillTypedArray(arr, bytes);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// Helpers & Node Value Creators
// ---------------------------------------------------------------------------

void syncAudioParamValue(HostAudioParam* p, float val);
Value makeAudioParamValue(AudioParamTarget target, int targetId,
                          float initialVal, float minVal, float maxVal, float defaultVal);

// Core node creators & decorators (host_audio_nodes.cpp)
void decorateAudioNodeProto(ObjectBuilder& b);
void decorateAudioParamProto(ObjectBuilder& b);
void decorateOscillatorNodeProto(ObjectBuilder& b);
void decoratePeriodicWaveProto(ObjectBuilder& b);
void decorateBiquadFilterNodeProto(ObjectBuilder& b);
void decorateAnalyserNodeProto(ObjectBuilder& b);
void decorateMediaStreamSourceNodeProto(ObjectBuilder& b);
Value makeGainNodeValue();
Value makeOscillatorNodeValue();
Value makePeriodicWaveValue(const float* real, const float* imag, int count, bool disableNorm);
Value makeBiquadFilterNodeValue();
Value makeAnalyserNodeValue();

// Buffer creators & decorators (host_audio_buffer.cpp)
void decorateAudioBufferProto(ObjectBuilder& b);
void decorateAudioBufferSourceNodeProto(ObjectBuilder& b);
Value makeAudioBufferValue(int channels, int length, int sampleRate);
Value makeAudioBufferSourceNodeValue();

// Context creators & decorators (host_audio_context.cpp)
void decorateAudioContextProto(ObjectBuilder& b);
Value makeAudioContextValue();

// Spatial creators & decorators (host_audio_spatial.cpp)
void decoratePannerNodeProto(ObjectBuilder& b);
void decorateStereoPannerNodeProto(ObjectBuilder& b);
void decorateAudioDestinationNodeProto(ObjectBuilder& b);
Value makeDestinationNodeValue();
Value makeListenerValue();
Value makePannerNodeValue();
Value makeStereoPannerNodeValue();

// DSP creators & decorators (host_audio_dsp.cpp)
void decorateDelayNodeProto(ObjectBuilder& b);
void decorateDynamicsCompressorNodeProto(ObjectBuilder& b);
void decorateWaveShaperNodeProto(ObjectBuilder& b);
void decorateConvolverNodeProto(ObjectBuilder& b);
void decorateChannelSplitterNodeProto(ObjectBuilder& b);
void decorateChannelMergerNodeProto(ObjectBuilder& b);
Value makeDelayNodeValue(double maxDelayTime = 1.0);
Value makeDynamicsCompressorNodeValue();
Value makeWaveShaperNodeValue();
Value makeConvolverNodeValue();
Value makeChannelSplitterNodeValue(int numberOfOutputs = 6);
Value makeChannelMergerNodeValue(int numberOfInputs = 6);
void registerAudioContextBuses(ObjectBuilder& b);

// Synth creators & globals (host_audio_synth.cpp)
void installAudioSynthGlobals();
void installAudioSequencerGlobals();
Value makeVoiceAllocatorValue(int maxVoices);
Value makeModMatrixValue();
Value makeMidiInputValue();
Value makeSequenceValue(HostVoiceAllocator* va);
Value makeMediaStreamValue();
Value makeMediaStreamAudioSourceNodeValue();
void registerAudioContextVoice(ObjectBuilder& b);
void registerAudioContextClips(ObjectBuilder& b);
// The rest of the AudioContext surface, one file each: short ADSR names,
// unison, per-voice spatial, bus routing, note scheduling
// (host_audio_voice_ext.cpp); clip introspection, playback transport,
// per-instance spatial, streams (host_audio_playback.cpp); chorus feedback,
// EQ, distortion, effect order, offline processing (host_audio_bus_fx.cpp).
void registerAudioContextVoiceExt(ObjectBuilder& b);
void registerAudioContextPlayback(ObjectBuilder& b);
void registerAudioContextBusFx(ObjectBuilder& b);
// getModMatrix, wavetables, getSpectrum, renderBlock
// (host_audio_synth_ext.cpp).
void registerAudioContextSynthExt(ObjectBuilder& b);
// The four preset families (voice / bus / mod / engine) as plain JS objects:
// toJson / fromJson / apply, plus savePreset / loadPreset
// (host_audio_presets.cpp).
void registerAudioContextPresets(ObjectBuilder& b);
// Sequence automation lanes/points beyond addAutomationLane
// (host_audio_sequence_ext.cpp); called from decorateSequenceProto.
void decorateSequenceAutomation(ObjectBuilder& b);

// ---------------------------------------------------------------------------
// File paths and background work (host_audio_io.cpp)
// ---------------------------------------------------------------------------

// A file path as the host's resolver sees it (api.h setPathResolver), or as
// given when no resolver is set.
std::string resolveAudioPath(const std::string& path);
// The same for a file about to be written: the parent directory resolves
// (it exists), the file name is appended.
std::string resolveAudioWritePath(const std::string& path);

// createClipFromFileAsync's worker: decodes `resolvedPath` on a background
// thread and returns the promise that tickAsyncJobs() (api.h) later settles
// on the JS thread — resolved with the clip id, or rejected with an Error
// whose message is "<path>: <decoder's reason>".
Value launchClipLoad(const std::string& resolvedPath);
// Join every outstanding job without touching JS (shutdownAudio).
void shutdownAsyncJobs();

// Wavetable bank registry helpers
std::shared_ptr<broaudio::WavetableBank> findWavetable(int id);
int registerWavetable(std::shared_ptr<broaudio::WavetableBank> bank);
void deleteWavetable(int id);

// String/Enum parsers & formatters
broaudio::BiquadFilter::Type parseFilterType(const std::string& str);
const char* filterTypeToString(broaudio::BiquadFilter::Type type);
broaudio::Waveform parseWaveform(const std::string& str);
const char* waveformToString(broaudio::Waveform wf);
broaudio::DistortionMode parseDistortionMode(const std::string& str);
const char* distortionModeToString(broaudio::DistortionMode mode);
broaudio::LfoShape parseLfoShape(const std::string& str);
const char* lfoShapeToString(broaudio::LfoShape shape);
broaudio::ModSource parseModSource(const std::string& str);
const char* modSourceToString(broaudio::ModSource src);
broaudio::ModDest parseModDest(const std::string& str);
const char* modDestToString(broaudio::ModDest dst);
broaudio::DistanceModel parseDistanceModel(const std::string& str);
const char* distanceModelToString(broaudio::DistanceModel model);
broaudio::EffectSlot parseEffectSlot(const std::string& str, broaudio::EffectSlot def);
const char* effectSlotToString(broaudio::EffectSlot slot);
broaudio::StealPolicy parseStealPolicy(const std::string& str);

void installAudioGlobals();

} // namespace broaudio::api
