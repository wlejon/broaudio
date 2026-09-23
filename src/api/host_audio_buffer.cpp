#include "host_audio_internal.h"
#include "host_audio_dsp.h"
#include <broaudio/dsp/convolution_reverb.h>
#include <broaudio/dsp/resampler.h>
#include <broaudio/io/audio_file.h>
#include <cstring>

namespace broaudio::api {

HostClass g_audioBufferClass;
HostClass g_audioBufferSourceNodeClass;

void hostAudioBufferDtor(void* p) {
    delete static_cast<HostAudioBufferHandle*>(p);
}

void hostAudioBufferSourceDtor(void* p) {
    auto* src = static_cast<HostAudioBufferSourceNode*>(p);
    if (src) {
        // Runs in the sweep: never create the default engine from here.
        auto* e = existingAudioEngine();
        if (e) {
            if (src->playbackId >= 0) e->stopPlayback(src->playbackId);
            if (src->clipId >= 0) e->deleteClip(src->clipId);
        }
        delete src;
    }
}

// The node's loop window in clip frames, Web Audio's rules: a window with
// loopStart >= 0, loopEnd > 0 and loopStart < loopEnd loops
// [loopStart, min(loopEnd, duration)); any other loops the whole buffer
// (0, 0 -- the engine's "whole region").
static void loopFrames(const HostAudioBufferSourceNode& src, int frames, int* startF, int* endF) {
    *startF = 0;
    *endF = 0;
    const double sr = src.playSampleRate;
    if (sr <= 0.0 || !(src.loopStart >= 0.0) || !(src.loopEnd > 0.0) || !(src.loopStart < src.loopEnd)) {
        return;
    }
    const double s = std::min(src.loopStart * sr, static_cast<double>(frames));
    const double en = std::min(src.loopEnd * sr, static_cast<double>(frames));
    if (!(s < en)) return;
    *startF = static_cast<int>(std::lround(s));
    *endF = static_cast<int>(std::lround(en));
}

// Push a moved loop window to the playing instance.
static void updateLiveLoop(const HostAudioBufferSourceNode& src) {
    auto* e = existingAudioEngine();
    if (!e || src.playbackId < 0 || !src.buffer) return;
    int s = 0, en = 0;
    loopFrames(src, src.buffer->length, &s, &en);
    e->setPlaybackLoopPoints(src.playbackId, s, en);
}

static HostAudioBufferHandle* bufferHandleOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* h = static_cast<HostAudioBufferHandle*>(ev::handleData(v));
    if (!h || h->tag != kHostAudioBufferTag) return nullptr;
    return h;
}

HostAudioBuffer* hostAudioBufferOf(Value v) {
    HostAudioBufferHandle* h = bufferHandleOf(v);
    return h ? h->buffer.get() : nullptr;
}

BufferRef hostAudioBufferRef(Value v) {
    HostAudioBufferHandle* h = bufferHandleOf(v);
    return h ? h->buffer : nullptr;
}

void decorateAudioBufferProto(ObjectBuilder& b) {
    b.accessor("numberOfChannels", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->numberOfChannels);
    }, nullptr);

    b.accessor("length", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->length);
    }, nullptr);

    b.accessor("sampleRate", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->sampleRate);
    }, nullptr);

    b.accessor("duration", [](Value self_, std::span<const Value>) {
        HostAudioBuffer* buf = hostAudioBufferOf(self_);
        if (!buf) return ev::undefined();
        return ev::fromDouble(buf->sampleRate > 0 ? static_cast<double>(buf->length) / buf->sampleRate : 0.0);
    }, nullptr);

    b.def("getChannelData", 1, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf) return ev::undefined();
        int ch = i32At(a, 0);
        if (ch < 0 || ch >= buf->numberOfChannels) {
            return ev::throwRangeError("AudioBuffer.getChannelData: channel index out of range");
        }
        // thisValue is a plain copy: root it before the first allocation.
        ev::Persistent self(thisValue);
        std::string key = "_ch" + std::to_string(ch);
        Value arr = ev::getProperty(self.get(), key);
        // The cached view, unless its buffer was detached (transferred):
        // then a fresh view is made from the host copy.
        if (isFloat32Array(arr)) return arr;

        ev::Persistent newArr(ev::createTypedArray(ev::elements::Float32, buf->length));
        if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
            std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(buf->channels[ch].data()),
                                           buf->channels[ch].size() * sizeof(float));
            ev::fillTypedArray(newArr.get(), bytes);
        }
        ev::setProperty(self.get(), key, newArr.get());
        return newArr.get();
    });

    b.def("copyFromChannel", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf || a.size() < 2) return ev::undefined();
        if (!isFloat32Array(a[0])) {
            throwArrayTypeError(a[0], "AudioBuffer.copyFromChannel: destination", "a Float32Array");
            return ev::undefined();
        }
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 ||
            startInChannel >= buf->length) {
            return ev::undefined();
        }

        // A getChannelData() view is the channel's live storage (the script
        // may have written into it); fold it into the host copy first. The
        // read may allocate, so the destination's bytes are looked up after.
        {
            ev::Persistent self(thisValue);
            Value cached = ev::getProperty(self.get(), "_ch" + std::to_string(ch));
            ev::TypedArrayInfo cachedInfo =
                isFloat32Array(cached) ? ev::typedArrayInfo(cached) : ev::TypedArrayInfo{};
            if (cachedInfo && ch < static_cast<int>(buf->channels.size())) {
                auto& chan = buf->channels[ch];
                size_t n = std::min(chan.size(), static_cast<size_t>(cachedInfo.elementCount));
                if (n > 0) std::memcpy(chan.data(), cachedInfo.data, n * sizeof(float));
            }
        }

        if (ev::isTypedArray(a[0])) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
            if (info && info.data) {
                float* dst = reinterpret_cast<float*>(info.data);
                size_t toCopy = std::min(static_cast<size_t>(info.elementCount),
                                         static_cast<size_t>(buf->length - startInChannel));
                if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
                    std::memcpy(dst, buf->channels[ch].data() + startInChannel, toCopy * sizeof(float));
                }
            }
        }
        return ev::undefined();
    });

    b.def("copyToChannel", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf || a.size() < 2) return ev::undefined();
        if (!isFloat32Array(a[0])) {
            throwArrayTypeError(a[0], "AudioBuffer.copyToChannel: source", "a Float32Array");
            return ev::undefined();
        }
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel < 0 ||
            startInChannel >= buf->length) {
            return ev::undefined();
        }

        if (ev::isTypedArray(a[0])) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(a[0]);
            if (info && info.data) {
                const float* srcPtr = reinterpret_cast<const float*>(info.data);
                size_t toCopy = std::min(static_cast<size_t>(info.elementCount),
                                         static_cast<size_t>(buf->length - startInChannel));
                if (ch >= static_cast<int>(buf->channels.size())) {
                    buf->channels.resize(buf->numberOfChannels, std::vector<float>(buf->length, 0.0f));
                }
                std::memcpy(buf->channels[ch].data() + startInChannel, srcPtr, toCopy * sizeof(float));

                // The read below may allocate: srcPtr is dead after it, so
                // the cache is refreshed from the host copy just written, and
                // thisValue is rooted for the write-back.
                ev::Persistent self(thisValue);
                std::string key = "_ch" + std::to_string(ch);
                Value cached = ev::getProperty(self.get(), key);
                if (ev::isTypedArray(cached)) {
                    ev::TypedArrayInfo cachedInfo =
                        isFloat32Array(cached) ? ev::typedArrayInfo(cached) : ev::TypedArrayInfo{};
                    if (cachedInfo && cachedInfo.data) {
                        float* dstPtr = reinterpret_cast<float*>(cachedInfo.data);
                        size_t cachedLimit = cachedInfo.elementCount > static_cast<size_t>(startInChannel)
                                                 ? cachedInfo.elementCount - startInChannel
                                                 : 0;
                        size_t toWrite = std::min(toCopy, cachedLimit);
                        if (toWrite > 0) {
                            std::memcpy(dstPtr + startInChannel,
                                        buf->channels[ch].data() + startInChannel,
                                        toWrite * sizeof(float));
                        }
                    } else {
                        ev::setProperty(self.get(), key, ev::undefined());
                    }
                }
            }
        }
        return ev::undefined();
    });
}

Value makeAudioBufferValue(int channels, int length, int sampleRate) {
    if (channels <= 0) channels = 1;
    if (channels > 32) channels = 32;
    if (length < 0) length = 0;
    if (sampleRate <= 0) sampleRate = 44100;

    auto* h = new HostAudioBufferHandle();
    h->buffer = std::make_shared<HostAudioBuffer>();
    h->buffer->numberOfChannels = channels;
    h->buffer->length = length;
    h->buffer->sampleRate = sampleRate;
    h->buffer->channels.resize(channels, std::vector<float>(length, 0.0f));

    return g_audioBufferClass.make(h, hostAudioBufferDtor);
}

void decorateAudioBufferSourceNodeProto(ObjectBuilder& b) {
    b.accessor("buffer",
               [](Value self_, std::span<const Value>) {
                   Value v = ev::getProperty(self_, "_buffer");
                   return ev::isUndefined(v) ? ev::null() : v;
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   const bool clear = a.empty() || ev::isNull(a[0]) || ev::isUndefined(a[0]);
                   if (!clear && !hostAudioBufferOf(a[0])) {
                       return ev::throwTypeError("AudioBufferSourceNode.buffer must be an AudioBuffer or null");
                   }
                   // Unwrap before the write: setProperty moves the heap.
                   src->buffer = clear ? nullptr : hostAudioBufferRef(a[0]);
                   ev::setProperty(self_, "_buffer", clear ? ev::null() : a[0]);
                   return ev::undefined();
               });

    b.accessor("loop",
               [](Value self_, std::span<const Value>) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   return ev::fromBool(src->loop);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   src->loop = boolAt(a, 0);
                   auto* e = getAudioEngine();
                   if (e && src->playbackId >= 0) {
                       e->setPlaybackLoop(src->playbackId, src->loop);
                   }
                   return ev::undefined();
               });

    b.accessor("loopStart",
               [](Value self_, std::span<const Value>) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   return ev::fromDouble(src->loopStart);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   src->loopStart = numAt(a, 0);
                   updateLiveLoop(*src);
                   return ev::undefined();
               });

    b.accessor("loopEnd",
               [](Value self_, std::span<const Value>) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   return ev::fromDouble(src->loopEnd);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   src->loopEnd = numAt(a, 0);
                   updateLiveLoop(*src);
                   return ev::undefined();
               });

    b.def("start", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBufferSourceNode* src = bufSrcOf(thisValue);
        if (!src) return ev::undefined();
        auto* e = getAudioEngine();
        if (!e) return ev::undefined();
        if (src->started) return ev::throwError("AudioBufferSourceNode cannot be started more than once");

        // start(when = 0, offset = 0, duration): seconds, as Web Audio
        // defines them -- `when` on the context clock (past = now), `offset`
        // into the buffer, `duration` of buffer content to play (loops
        // included) before the source ends.
        const double when = hasArg(a, 0) ? numAt(a, 0) : 0.0;
        const double offset = hasArg(a, 1) ? numAt(a, 1) : 0.0;
        const bool hasDuration = hasArg(a, 2);
        const double duration = hasDuration ? numAt(a, 2) : 0.0;
        if (!(when >= 0.0)) return ev::throwRangeError("start: when must be a non-negative number");
        if (!(offset >= 0.0)) return ev::throwRangeError("start: offset must be a non-negative number");
        if (hasDuration && !(duration >= 0.0)) {
            return ev::throwRangeError("start: duration must be a non-negative number");
        }
        src->started = true;

        // Every getProperty below may allocate: the node and its buffer are
        // read back through Persistents, never through a held Value.
        ev::Persistent self(thisValue);
        ev::Persistent bufVal(ev::getProperty(self.get(), "_buffer"));
        HostAudioBuffer* hostBuf = hostAudioBufferOf(bufVal.get());
        if (hostBuf && hostBuf->length > 0 && hostBuf->numberOfChannels > 0) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(bufVal.get(), key);
                if (isFloat32Array(arr)) {
                    ev::TypedArrayInfo info = ev::typedArrayInfo(arr);
                    if (info && info.data) {
                        size_t count = std::min(static_cast<size_t>(frames), static_cast<size_t>(info.elementCount));
                        std::memcpy(chData[c].data(), info.data, count * sizeof(float));
                    }
                } else if (c < static_cast<int>(hostBuf->channels.size())) {
                    chData[c] = hostBuf->channels[c];
                }
            }

            std::vector<float> interleaved(frames * channels);
            for (int f = 0; f < frames; ++f) {
                for (int c = 0; c < channels; ++c) {
                    interleaved[f * channels + c] = chData[c][f];
                }
            }

            // Traverse downstream connected nodes. Filters, delays,
            // convolvers, the waveshaper and compressor are rendered into the
            // clip here; gain, stereo pan and panner position stay live on
            // the playback's LiveSource (host_audio_live.cpp).
            auto live = std::make_shared<LiveSource>();
            live->kind = LiveSource::Kind::Playback;
            live->pitch = src->playbackRateParam;
            live->detune = src->detuneParam;
            bool hasSpatial = false;
            HostPannerNode* pannerNode = nullptr;

            double curTime = e->currentTime();
            int sr = e->sampleRate();
            std::vector<ev::Persistent> queue;
            std::vector<HostAudioNode*> visited;
            pushConnectedTargets(self.get(), queue, curTime);

            while (!queue.empty()) {
                ev::Persistent curObj = std::move(queue.back());
                queue.pop_back();
                HostAudioNode* cur = hostAudioNodeOf(curObj.get());

                if (!cur || std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
                visited.push_back(cur);

                if (cur->nodeType == AudioNodeType::Gain) {
                    auto* gn = reinterpret_cast<HostGainNode*>(cur);
                    if (gn->gainParam) live->pathGains.push_back(gn->gainParam);
                } else if (cur->nodeType == AudioNodeType::BiquadFilter) {
                    auto* bf = reinterpret_cast<HostBiquadFilterNode*>(cur);
                    broaudio::BiquadFilter bq;
                    if (bf->slot >= 0) {
                        bq.type = e->getBusFilterType(0, bf->slot);
                        bq.frequency = e->getBusFilterFrequency(0, bf->slot);
                        bq.Q = e->getBusFilterQ(0, bf->slot);
                        bq.gainDB = e->getBusFilterGain(0, bf->slot);
                    } else {
                        bq.type = parseFilterType(bf->type);
                    }
                    bq.computeCoefficients(sr);
                    bq.snapToTarget();
                    for (int f = 0; f < frames; ++f) {
                        for (int c = 0; c < channels; ++c) {
                            interleaved[f * channels + c] = bq.process(interleaved[f * channels + c], c % 2);
                        }
                    }
                } else if (cur->nodeType == AudioNodeType::Delay) {
                    auto* dn = reinterpret_cast<HostDelayNode*>(cur);
                    float dt = dn->delayTimeParam ? dn->delayTimeParam->evaluate(curTime) : 0.0f;
                    if (dt > 0.0f) {
                        int delaySamples = static_cast<int>(dt * sr);
                        if (delaySamples > 0 && delaySamples < frames) {
                            std::vector<float> delayed(frames * channels, 0.0f);
                            for (int f = delaySamples; f < frames; ++f) {
                                for (int c = 0; c < channels; ++c) {
                                    delayed[f * channels + c] = interleaved[(f - delaySamples) * channels + c];
                                }
                            }
                            interleaved = std::move(delayed);
                        }
                    }
                } else if (cur->nodeType == AudioNodeType::Convolver) {
                    auto* conv = reinterpret_cast<HostConvolverNode*>(cur);
                    if (conv->buffer && conv->buffer->length > 0 && conv->buffer->numberOfChannels > 0) {
                        int irChannels = conv->buffer->numberOfChannels;
                        int irFrames = conv->buffer->length;
                        std::vector<float> irInterleaved(irFrames * irChannels);
                        for (int f = 0; f < irFrames; ++f) {
                            for (int c = 0; c < irChannels; ++c) {
                                irInterleaved[f * irChannels + c] = (c < static_cast<int>(conv->buffer->channels.size()) && f < static_cast<int>(conv->buffer->channels[c].size()))
                                    ? conv->buffer->channels[c][f] : 0.0f;
                            }
                        }
                        broaudio::ConvolutionReverb cr;
                        cr.init(sr, 256);
                        cr.normalize = conv->normalize;
                        if (cr.loadImpulseResponse(irInterleaved.data(), irFrames, irChannels, sr, true)) {
                            cr.mix = 1.0f;
                            cr.gain = 1.0f;
                            if (channels == 1) {
                                std::vector<float> stereo(frames * 2);
                                for (int f = 0; f < frames; ++f) {
                                    stereo[f * 2] = interleaved[f];
                                    stereo[f * 2 + 1] = interleaved[f];
                                }
                                cr.processStereo(stereo.data(), frames);
                                interleaved = std::move(stereo);
                                channels = 2;
                            } else if (channels == 2) {
                                cr.processStereo(interleaved.data(), frames);
                            }
                        }
                    }
                } else if (cur->nodeType == AudioNodeType::WaveShaper) {
                    auto* ws = reinterpret_cast<HostWaveShaperNode*>(cur);
                    if (!ws->curve.empty()) {
                        size_t N = ws->curve.size();
                        for (float& s : interleaved) {
                            float norm = std::clamp(s, -1.0f, 1.0f);
                            float idx = (norm * 0.5f + 0.5f) * static_cast<float>(N - 1);
                            size_t i0 = static_cast<size_t>(idx);
                            size_t i1 = std::min(i0 + 1, N - 1);
                            float frac = idx - static_cast<float>(i0);
                            s = ws->curve[i0] + frac * (ws->curve[i1] - ws->curve[i0]);
                        }
                    }
                } else if (cur->nodeType == AudioNodeType::Analyser) {
                    auto* an = reinterpret_cast<HostAnalyserNode*>(cur);
                    if (an->inputTapBuffer) {
                        if (channels == 1) {
                            an->inputTapBuffer->write(interleaved.data(), frames);
                        } else {
                            std::vector<float> mono(frames);
                            for (int f = 0; f < frames; ++f) {
                                mono[f] = 0.5f * (interleaved[f * channels] + interleaved[f * channels + 1]);
                            }
                            an->inputTapBuffer->write(mono.data(), frames);
                        }
                        an->hasConnectedInput = true;
                    }
                } else if (cur->nodeType == AudioNodeType::StereoPanner) {
                    auto* sp = reinterpret_cast<HostStereoPannerNode*>(cur);
                    if (sp->panParam) live->pathPan = sp->panParam;
                } else if (cur->nodeType == AudioNodeType::Panner) {
                    hasSpatial = true;
                    pannerNode = reinterpret_cast<HostPannerNode*>(cur);
                    for (int i = 0; i < 3; ++i) live->position[i] = pannerNode->positionParams[i];
                } else if (cur->nodeType == AudioNodeType::DynamicsCompressor) {
                    auto* comp = reinterpret_cast<HostDynamicsCompressorNode*>(cur);
                    processDynamicsCompressor(comp, interleaved.data(), frames, channels, sr, curTime);
                }

                pushConnectedTargets(curObj.get(), queue, curTime);
            }

            src->clipId = e->createClip(interleaved.data(), frames * channels, channels);

            // The clip holds the buffer's frames at the buffer's own rate:
            // the engine steps through it at rateScale per output frame on
            // top of the computed playbackRate * 2^(detune / 1200).
            const int bufSR = hostBuf->sampleRate > 0 ? hostBuf->sampleRate : sr;
            src->playSampleRate = bufSR;
            live->rateScale = static_cast<float>(bufSR) / static_cast<float>(sr);
            const float pitch = live->pitch->evaluate(curTime) *
                                std::pow(2.0f, live->detune->evaluate(curTime) / 1200.0f);

            float gain = 1.0f;
            for (const auto& g : live->pathGains) gain *= g->evaluate(curTime);

            broaudio::Engine::ClipPlayOptions opts;
            opts.gain = gain;
            opts.loop = src->loop;
            opts.rate = pitch * live->rateScale;
            live->lastGain = opts.gain;
            live->lastPitch = opts.rate;
            opts.when = when;
            opts.offsetFrames = std::min(offset * bufSR, static_cast<double>(frames));
            loopFrames(*src, frames, &opts.loopStartFrame, &opts.loopEndFrame);
            if (hasDuration) opts.durationFrames = duration * bufSR;
            src->playbackId = e->playClip(src->clipId, opts);

            if (src->playbackId >= 0) {
                if (hasSpatial && pannerNode) {
                    e->setPlaybackSpatialEnabled(src->playbackId, true);
                    e->setPlaybackSpatialRefDistance(src->playbackId, pannerNode->refDistance);
                    e->setPlaybackSpatialMaxDistance(src->playbackId, pannerNode->maxDistance);
                    e->setPlaybackSpatialRolloff(src->playbackId, pannerNode->rolloffFactor);
                    e->setPlaybackSpatialDistanceModel(src->playbackId, parseDistanceModel(pannerNode->distanceModel));
                }
                live->id = src->playbackId;
                src->live = live;
                registerLiveSource(live);
                // Web Audio keeps a playing source alive; the hold also fires
                // `onended` once the playback finishes or is stopped.
                holdPlayingSource(live, self.get());
                refreshLiveParams(curTime);
            }
        }
        return ev::undefined();
    });

    b.def("stop", 1, [](Value self_, std::span<const Value>) -> Value {
        HostAudioBufferSourceNode* src = bufSrcOf(self_);
        if (!src) return ev::undefined();
        auto* e = getAudioEngine();
        if (e && src->playbackId >= 0) {
            e->stopPlayback(src->playbackId);
            // The playing hold sees the playback gone on the next tick and
            // fires `onended`, as Web Audio does for stop().
            src->playbackId = -1;
        }
        src->stopped = true;
        return ev::undefined();
    });
}

Value makeAudioBufferSourceNodeValue() {
    auto* src = new HostAudioBufferSourceNode();
    src->base.nodeType = AudioNodeType::BufferSource;

    src->playbackRateParam = makeAudioParam(AudioParamTarget::PlaybackRate, -1, 1.0f, 0.0f, 1024.0f, 1.0f);
    src->detuneParam = makeAudioParam(AudioParamTarget::PlaybackDetune, -1, 0.0f, -153600.0f, 153600.0f, 0.0f);

    ObjectBuilder b(g_audioBufferSourceNodeClass.make(src, hostAudioBufferSourceDtor));
    b.set("playbackRate", makeAudioParamValue(src->playbackRateParam));
    b.set("detune", makeAudioParamValue(src->detuneParam));
    return b.get();
}

} // namespace broaudio::api
