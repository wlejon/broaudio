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

// A node's native half. It holds no JS values: what a node references in JS
// (its connect() targets, callbacks) lives as properties on its own JS
// object, where the collector traces it, so a cycle through a node (a
// feedback loop, a callback closing over the node) is collectable. See
// connectTargetsOf.
struct HostAudioNode {
    uint32_t tag = kHostAudioNodeTag;
    AudioNodeType nodeType = AudioNodeType::Generic;
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
    FilterDetune,
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

// An AudioParam's state. Shared ownership: the JS AudioParam object's handle
// holds one reference and every node (or live source) that reads the param
// holds another, so a node never reads a param the collector has freed — a
// script can delete or overwrite `node.gain` without leaving the node
// pointing at nothing.
struct HostAudioParam {
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

using ParamRef = std::shared_ptr<HostAudioParam>;

// What an AudioParam JS object's handle carries.
struct HostAudioParamHandle {
    uint32_t tag = kHostAudioParamTag;
    ParamRef param;
};

// ---------------------------------------------------------------------------
// Live params (host_audio_live.cpp)
//
// Engine state that is a function of AudioParams -- a voice's frequency is
// frequency * 2^(detune/1200), its gain the product of its own gain and every
// GainNode on its path, a filter slot's cutoff frequency * 2^(detune/1200) --
// is recomputed from the params whenever one changes and on every tick, so
// value sets and scheduled automation reach sources that are already
// playing. Automation is evaluated at control rate: once per host tick
// (tickAsyncJobs) and once per 128-frame render quantum inside
// ctx.renderBlock; the engine's parameter smoothers interpolate between.
// ---------------------------------------------------------------------------

// A playing (or, for an oscillator, playable) source and the params that
// shape it. Built by start()'s graph walk; the path fields describe the
// nodes downstream of the source (gains multiply, the stereo pan and panner
// position apply). Holds no JS values.
struct LiveSource {
    enum class Kind : uint8_t { Voice, Playback };
    Kind kind = Kind::Voice;
    int id = -1;             // engine voice id / playback id
    ParamRef pitch;          // oscillator frequency / buffer playbackRate
    ParamRef detune;         // cents on top of pitch
    float rateScale = 1.0f;  // playback: buffer sample rate / engine rate
    ParamRef ownGain;        // oscillator's own gain param
    ParamRef ownPan;         // oscillator's own pan param
    std::vector<ParamRef> pathGains;
    ParamRef pathPan;        // a StereoPannerNode's pan on the path
    ParamRef position[3];    // a PannerNode's position on the path
    bool ended = false;
    // Last values written, so a refresh that changes nothing writes nothing.
    float lastGain = -1.0f, lastPitch = -1.0f, lastPan = -2.0f;
    float lastPos[3] = {0.0f, 0.0f, 0.0f};
    bool posWritten = false;
};

// A BiquadFilterNode's slot: cutoff = frequency * 2^(detune/1200).
struct LiveFilter {
    int slot = -1;
    ParamRef frequency;
    ParamRef detune;
    float lastFrequency = -1.0f;
};

// DynamicsCompressorNode.threshold is in dB, as Web Audio defines it; the
// engine's bus compressor takes its threshold as a linear amplitude (0..1).
inline float compressorThresholdLinear(float thresholdDb) {
    return std::pow(10.0f, thresholdDb / 20.0f);
}

// Registration. The registry keeps weak references (the node owns its
// LiveSource / LiveFilter) except for playing buffer sources, below.
void registerLiveSource(const std::shared_ptr<LiveSource>& src);
void registerLiveFilter(const std::shared_ptr<LiveFilter>& filter);
// A param got a timeline: evaluate it every tick until the timeline empties.
void noteAutomatedParam(const ParamRef& param);
// A param's timeline was edited: note it for per-tick evaluation and apply
// its value at the current time. Leaves `param->value` (the value the
// timeline's first ramp starts from) alone.
void paramTimelineChanged(const ParamRef& param);
// A buffer source that started playing: the registry holds it (and roots its
// JS node, as Web Audio keeps a playing source alive) until its playback ends
// or is stopped, then calls the node's `onended` on the next tick.
void holdPlayingSource(const std::shared_ptr<LiveSource>& src, Value node);
// Recompute and write every live source, filter and automated param at
// engine time `t`. Never runs JS.
void refreshLiveParams(double t);
// refreshLiveParams(now), then dispatch `onended` for finished sources
// (runs JS). What the host tick and renderBlock call.
void tickLiveParams();
// Drop the playing holds without running JS (shutdownAudio).
void shutdownLiveParams();
// The engine if one exists, without creating the default one.
broaudio::Engine* existingAudioEngine();

struct HostGainNode {
    HostAudioNode base;
    ParamRef gainParam;
};

struct HostOscillatorNode {
    HostAudioNode base;
    int voiceId = -1;
    std::string type = "sine";
    bool started = false;
    bool stopped = false;
    ParamRef frequencyParam;
    ParamRef detuneParam;
    ParamRef gainParam;
    ParamRef panParam;
    std::shared_ptr<LiveSource> live;  // registered at creation: the voice exists from then
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
    ParamRef frequencyParam;
    ParamRef detuneParam;
    ParamRef qParam;
    ParamRef gainParam;
    std::shared_ptr<LiveFilter> live;
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

// An AudioBuffer's host copy. Shared like HostAudioParam: the JS object's
// handle and any node the buffer is assigned to each hold a reference.
struct HostAudioBuffer {
    int numberOfChannels = 1;
    int length = 0;
    int sampleRate = 44100;
    std::vector<std::vector<float>> channels;
};

using BufferRef = std::shared_ptr<HostAudioBuffer>;

struct HostAudioBufferHandle {
    uint32_t tag = kHostAudioBufferTag;
    BufferRef buffer;
};

struct HostAudioBufferSourceNode {
    HostAudioNode base;
    BufferRef buffer;
    ParamRef playbackRateParam;
    ParamRef detuneParam;
    bool loop = false;
    double loopStart = 0.0;
    double loopEnd = 0.0;
    int clipId = -1;
    int playbackId = -1;
    int playSampleRate = 0;  // the started buffer's rate: loop points are converted with it
    bool started = false;
    bool stopped = false;
    std::shared_ptr<LiveSource> live;  // set by start()
};

struct HostPannerNode {
    HostAudioNode base;
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
    ParamRef positionParams[3];     // positionX / Y / Z
    ParamRef orientationParams[3];  // orientationX / Y / Z
};

struct HostStereoPannerNode {
    HostAudioNode base;
    ParamRef panParam;
    float pan = 0.0f;
};

struct HostDelayNode {
    HostAudioNode base;
    ParamRef delayTimeParam;
    double maxDelayTime = 1.0;
};

struct HostDynamicsCompressorNode {
    HostAudioNode base;
    ParamRef thresholdParam;
    ParamRef kneeParam;
    ParamRef ratioParam;
    ParamRef attackParam;
    ParamRef releaseParam;
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
    BufferRef buffer;
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
};

struct HostModMatrix {
    uint32_t tag = kHostModMatrixTag;
    broaudio::ModMatrix* matrix = nullptr;
};

struct HostMidiInput {
    uint32_t tag = kHostMidiInputTag;
    std::unique_ptr<broaudio::MidiInput> midi;
};

struct HostSequence {
    uint32_t tag = kHostSequenceTag;
    std::unique_ptr<broaudio::Sequence> seq;
    // One stable key per automation lane, in lane order. A lane's JS
    // callback lives on the sequence object as `_laneCbs["k<key>"]` (traced,
    // so a callback closing over its sequence is collectable), and the
    // lane's C++ closure captures only the key -- removing a lane shifts the
    // indices of the ones after it but never re-points them.
    std::vector<int> laneKeys;
    int nextLaneKey = 0;
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
ParamRef hostAudioParamRef(Value v);
HostAudioBuffer* hostAudioBufferOf(Value v);
BufferRef hostAudioBufferRef(Value v);
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
    // Each allocation below may move every Value before it, so the message,
    // the constructor and the error all ride in Persistents.
    ev::Persistent msgVal(ev::fromUtf8(message));
    auto g = ev::globalValue("Error");
    ev::Persistent errObj;
    if (g.found && ev::isFunction(g.value)) {
        ev::Persistent ctor(g.value);
        const Value arg = msgVal.get();
        ev::CallResult res = ev::construct(ctor.get(), std::span<const Value>(&arg, 1));
        errObj.set(res.thrown ? ev::createObject() : res.value);
    } else {
        errObj.set(ev::createObject());
    }
    if (name && name[0]) {
        ev::Persistent nameVal(ev::fromUtf8(name));
        errObj.set(ev::setProperty(errObj.get(), "name", nameVal.get()));
    }
    return errObj.get();
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

// ---------------------------------------------------------------------------
// Typed-array arguments
//
// A parameter documented as a Float32Array takes a Float32Array and nothing
// else: another element kind (an Int16Array, a Uint8Array) is a TypeError,
// never its bytes reinterpreted as floats, and so is a view whose buffer has
// been detached. Some parameters also take a plain array of numbers, or an
// ArrayBuffer read as float32; each call site says which.
//
// Every checker below that fails has already raised the TypeError (pending
// in the runtime): the caller returns ev::undefined() straight away and
// makes no further embed call.
// ---------------------------------------------------------------------------

// A typed array or ArrayBuffer whose buffer has been detached.
inline bool isDetachedBuffer(Value v) {
    return (ev::isTypedArray(v) || ev::isArrayBuffer(v)) && ev::isDetachedArrayBuffer(v);
}

// A live (not detached) typed array of element kind `kind`.
inline bool isTypedArrayOf(Value v, bronze::ElementKind kind) {
    if (!ev::isTypedArray(v) || ev::isDetachedArrayBuffer(v)) return false;
    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
    return info && info.elementKind == kind;
}

inline bool isFloat32Array(Value v) { return isTypedArrayOf(v, ev::elements::Float32); }

// The TypeError the checkers raise: "<what> must be a Float32Array" plus
// what else was acceptable, and why this value was not.
inline void throwArrayTypeError(Value v, const char* what, const char* expected) {
    std::string msg = std::string(what) + " must be " + expected;
    if (isDetachedBuffer(v)) msg += " (its buffer is detached)";
    else if (ev::isTypedArray(v)) msg += " (got a typed array of another element type)";
    ev::throwTypeError(msg);
}

enum class FloatArrayArg : uint8_t {
    Float32Only,     // Float32Array
    Float32OrPlain,  // Float32Array or a plain array of numbers
    Float32OrBuffer, // Float32Array or an ArrayBuffer read as float32
};

// The argument's floats COPIED into `out`. A typed array's bytes live in the
// moving heap, so a pointer into them is stale after the next allocating
// embed call; the copy is what lets a caller read more arguments (a property
// of an options object) before it consumes the data. False with a TypeError
// pending for anything `accept` does not allow.
inline bool readFloatArrayArg(Value v, FloatArrayArg accept, const char* what,
                              std::vector<float>& out) {
    const char* expected = accept == FloatArrayArg::Float32OrPlain  ? "a Float32Array or an array of numbers"
                         : accept == FloatArrayArg::Float32OrBuffer ? "a Float32Array or an ArrayBuffer"
                                                                    : "a Float32Array";
    if (isDetachedBuffer(v)) {
        throwArrayTypeError(v, what, expected);
        return false;
    }
    if (ev::isTypedArray(v)) {
        ev::TypedArrayInfo info = ev::typedArrayInfo(v);
        if (!info || info.elementKind != ev::elements::Float32) {
            throwArrayTypeError(v, what, expected);
            return false;
        }
        out.resize(info.byteLength / sizeof(float));
        if (!out.empty()) std::memcpy(out.data(), info.data, out.size() * sizeof(float));
        return true;
    }
    if (ev::isArrayBuffer(v)) {
        ev::ArrayBufferInfo buf = ev::arrayBufferInfo(v);
        if (accept != FloatArrayArg::Float32OrBuffer || !buf) {
            throwArrayTypeError(v, what, expected);
            return false;
        }
        out.resize(buf.byteLength / sizeof(float));
        if (!out.empty()) std::memcpy(out.data(), buf.data, out.size() * sizeof(float));
        return true;
    }
    if (accept == FloatArrayArg::Float32OrPlain && ev::isObject(v) && !ev::isFunction(v)) {
        const float* data = nullptr;
        size_t count = 0;
        if (plainArrayData<float>(v, out, [](double d) { return static_cast<float>(d); }, &data, &count)) {
            return true;
        }
    }
    throwArrayTypeError(v, what, expected);
    return false;
}

// An OUTPUT typed array of element kind `kind`, written in place: its info,
// or a null info with a TypeError pending. The data pointer is valid only
// until the next allocating embed call, so fetch it right before the write.
inline ev::TypedArrayInfo outArrayArg(Value v, bronze::ElementKind kind, const char* what,
                                      const char* expected) {
    if (!isTypedArrayOf(v, kind)) {
        throwArrayTypeError(v, what, expected);
        return {};
    }
    return ev::typedArrayInfo(v);
}

// A view's or ArrayBuffer's bytes IN PLACE: the pointer is valid only until
// the next allocating embed call, so consume it before making one.
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
// A param's state alone (no JS object yet); a node keeps the ref and hands
// makeAudioParamValue(ref) to the property it exposes the param under.
ParamRef makeAudioParam(AudioParamTarget target, int targetId,
                        float initialVal, float minVal, float maxVal, float defaultVal);
Value makeAudioParamValue(const ParamRef& param);
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
// Pull a PannerNode's position/orientation AudioParams (evaluated at `when`)
// into its HostPannerNode, so `panner.positionX.value = x` (or a scheduled
// ramp) is what a source started through the panner is placed at. Reads
// properties, so it may allocate.
void syncPannerFromParams(Value pannerObj, double when);
// A node's connect() targets, in connection order. They live on the node's
// JS object as the `_targets` array (a traced edge, not a host root); this
// reads them back rooted. Allocates.
std::vector<ev::Persistent> connectTargetsOf(Value nodeObj);
// Queue the audio nodes `nodeObj` is connected to for a start()-time graph
// walk, syncing any PannerNode among them from its params first. Allocates.
void pushConnectedTargets(Value nodeObj, std::vector<ev::Persistent>& queue, double when);

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
// `allocatorObj` is the VoiceAllocator JS object; the sequence keeps it (as
// `_allocator`) so update() can run the allocator's voice-setup callback.
Value makeSequenceValue(Value allocatorObj);
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
// The property name a lane's callback has in the sequence's `_laneCbs`.
inline std::string sequenceLaneKey(int key) { return "k" + std::to_string(key); }

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
