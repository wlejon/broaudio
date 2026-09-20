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
    delete static_cast<HostAudioBuffer*>(p);
}

void hostAudioBufferSourceDtor(void* p) {
    auto* src = static_cast<HostAudioBufferSourceNode*>(p);
    if (src) {
        auto* e = getAudioEngine();
        if (e) {
            if (src->playbackId >= 0) e->stopPlayback(src->playbackId);
            if (src->clipId >= 0) e->deleteClip(src->clipId);
        }
        delete src;
    }
}

HostAudioBuffer* hostAudioBufferOf(Value v) {
    if (!ev::isObject(v)) return nullptr;
    auto* p = static_cast<HostAudioBuffer*>(ev::handleData(v));
    if (!p || p->tag != kHostAudioBufferTag) return nullptr;
    return p;
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
        std::string key = "_ch" + std::to_string(ch);
        Value arr = ev::getProperty(thisValue, key);
        if (ev::isTypedArray(arr)) return arr;

        Value newArr = ev::createTypedArray(ev::elements::Float32, buf->length);
        if (ch < static_cast<int>(buf->channels.size()) && !buf->channels[ch].empty()) {
            std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(buf->channels[ch].data()),
                                           buf->channels[ch].size() * sizeof(float));
            ev::fillTypedArray(newArr, bytes);
        }
        ev::setProperty(thisValue, key, newArr);
        return newArr;
    });

    b.def("copyFromChannel", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBuffer* buf = hostAudioBufferOf(thisValue);
        if (!buf || a.size() < 2) return ev::undefined();
        Value dest = a[0];
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel >= buf->length) return ev::undefined();

        if (ev::isTypedArray(dest)) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(dest);
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
        Value src = a[0];
        int ch = i32At(a, 1);
        int startInChannel = a.size() >= 3 ? i32At(a, 2) : 0;
        if (ch < 0 || ch >= buf->numberOfChannels || startInChannel >= buf->length) return ev::undefined();

        if (ev::isTypedArray(src)) {
            ev::TypedArrayInfo info = ev::typedArrayInfo(src);
            if (info && info.data) {
                const float* srcPtr = reinterpret_cast<const float*>(info.data);
                size_t toCopy = std::min(static_cast<size_t>(info.elementCount),
                                         static_cast<size_t>(buf->length - startInChannel));
                if (ch >= static_cast<int>(buf->channels.size())) {
                    buf->channels.resize(buf->numberOfChannels, std::vector<float>(buf->length, 0.0f));
                }
                std::memcpy(buf->channels[ch].data() + startInChannel, srcPtr, toCopy * sizeof(float));

                std::string key = "_ch" + std::to_string(ch);
                Value cached = ev::getProperty(thisValue, key);
                if (ev::isTypedArray(cached)) {
                    ev::TypedArrayInfo cachedInfo = ev::typedArrayInfo(cached);
                    if (cachedInfo && cachedInfo.data) {
                        float* dstPtr = reinterpret_cast<float*>(cachedInfo.data);
                        size_t cachedLimit = cachedInfo.elementCount > static_cast<size_t>(startInChannel)
                                                 ? cachedInfo.elementCount - startInChannel
                                                 : 0;
                        size_t toWrite = std::min(toCopy, cachedLimit);
                        if (toWrite > 0) {
                            std::memcpy(dstPtr + startInChannel, srcPtr, toWrite * sizeof(float));
                        }
                    } else {
                        ev::setProperty(thisValue, key, ev::undefined());
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

    auto* buf = new HostAudioBuffer();
    buf->numberOfChannels = channels;
    buf->length = length;
    buf->sampleRate = sampleRate;
    buf->channels.resize(channels, std::vector<float>(length, 0.0f));

    return g_audioBufferClass.make(buf, hostAudioBufferDtor);
}

void decorateAudioBufferSourceNodeProto(ObjectBuilder& b) {
    b.accessor("buffer",
               [](Value self_, std::span<const Value>) {
                   return ev::getProperty(self_, "_buffer");
               },
               [](Value self_, std::span<const Value> a) {
                   HostAudioBufferSourceNode* src = bufSrcOf(self_);
                   if (!src) return ev::undefined();
                   Value bufVal = !a.empty() ? a[0] : ev::null();
                   ev::setProperty(self_, "_buffer", bufVal);
                   src->buffer = hostAudioBufferOf(bufVal);
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
                   return ev::undefined();
               });

    b.def("start", 3, [](Value thisValue, std::span<const Value> a) -> Value {
        HostAudioBufferSourceNode* src = bufSrcOf(thisValue);
        if (!src) return ev::undefined();
        auto* e = getAudioEngine();
        if (!e) return ev::undefined();
        if (src->started) return ev::throwError("AudioBufferSourceNode cannot be started more than once");
        src->started = true;

        Value bufVal = ev::getProperty(thisValue, "_buffer");
        HostAudioBuffer* hostBuf = hostAudioBufferOf(bufVal);
        if (hostBuf && hostBuf->length > 0 && hostBuf->numberOfChannels > 0) {
            int channels = hostBuf->numberOfChannels;
            int frames = hostBuf->length;
            std::vector<std::vector<float>> chData(channels);
            for (int c = 0; c < channels; ++c) {
                chData[c].resize(frames, 0.0f);
                std::string key = "_ch" + std::to_string(c);
                Value arr = ev::getProperty(bufVal, key);
                if (ev::isTypedArray(arr)) {
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

            // Traverse downstream connected nodes to apply DSP processing (filters, delays, convolvers, waveshaper, gain, analyser) and spatial/pan settings
            float netGain = 1.0f;
            bool hasPan = false;
            float panVal = 0.0f;
            bool hasSpatial = false;
            HostPannerNode* pannerNode = nullptr;

            double curTime = e->currentTime();
            int sr = e->sampleRate();
            std::vector<HostAudioNode*> queue;
            std::vector<HostAudioNode*> visited;
            for (auto& t : src->base.connectedTargets) {
                HostAudioNode* n = hostAudioNodeOf(t.get());
                if (n) queue.push_back(n);
            }

            while (!queue.empty()) {
                HostAudioNode* cur = queue.back();
                queue.pop_back();

                if (std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
                visited.push_back(cur);

                if (cur->nodeType == AudioNodeType::Gain) {
                    auto* gn = reinterpret_cast<HostGainNode*>(cur);
                    if (gn->gainParam) {
                        netGain *= gn->gainParam->evaluate(curTime);
                    }
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
                    hasPan = true;
                    if (sp->panParam) {
                        panVal = sp->panParam->evaluate(curTime);
                    } else {
                        panVal = sp->pan;
                    }
                } else if (cur->nodeType == AudioNodeType::Panner) {
                    hasSpatial = true;
                    pannerNode = reinterpret_cast<HostPannerNode*>(cur);
                } else if (cur->nodeType == AudioNodeType::DynamicsCompressor) {
                    auto* comp = reinterpret_cast<HostDynamicsCompressorNode*>(cur);
                    processDynamicsCompressor(comp, interleaved.data(), frames, channels, sr, curTime);
                }

                for (auto& t : cur->connectedTargets) {
                    HostAudioNode* next = hostAudioNodeOf(t.get());
                    if (next) queue.push_back(next);
                }
            }

            src->clipId = e->createClip(interleaved.data(), frames * channels, channels);

            double when = numAt(a, 0);
            if (when > 0.0) {
                src->playbackId = e->playClipAt(src->clipId, when, netGain, src->loop);
            } else {
                src->playbackId = e->playClip(src->clipId, netGain, src->loop);
            }

            if (src->playbackId >= 0) {
                if (hasPan) {
                    e->setPlaybackPan(src->playbackId, panVal);
                }
                if (hasSpatial && pannerNode) {
                    float px = pannerNode->posX;
                    float py = pannerNode->posY;
                    float pz = pannerNode->posZ;
                    e->setPlaybackSpatialEnabled(src->playbackId, true);
                    e->setPlaybackSpatialPosition(src->playbackId, px, py, pz);
                    e->setPlaybackSpatialRefDistance(src->playbackId, pannerNode->refDistance);
                    e->setPlaybackSpatialMaxDistance(src->playbackId, pannerNode->maxDistance);
                    e->setPlaybackSpatialRolloff(src->playbackId, pannerNode->rolloffFactor);
                    e->setPlaybackSpatialDistanceModel(src->playbackId, parseDistanceModel(pannerNode->distanceModel));
                }

                Value rateVal = ev::getProperty(thisValue, "playbackRate");
                if (auto* rateParam = hostAudioParamOf(rateVal)) {
                    float r = rateParam->evaluate(curTime);
                    if (r != 1.0f) {
                        e->setPlaybackRate(src->playbackId, r);
                    }
                }

                double offset = numAt(a, 1);
                if (offset > 0.0) {
                    e->seekPlayback(src->playbackId, offset);
                }
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
            src->playbackId = -1;
        }
        src->stopped = true;
        return ev::undefined();
    });
}

Value makeAudioBufferSourceNodeValue() {
    auto* src = new HostAudioBufferSourceNode();
    src->base.nodeType = AudioNodeType::BufferSource;

    ObjectBuilder b(g_audioBufferSourceNodeClass.make(src, hostAudioBufferSourceDtor));
    b.set("playbackRate", makeAudioParamValue(AudioParamTarget::PlaybackRate, -1, 1.0f, 0.0f, 1024.0f, 1.0f));
    b.set("detune", makeAudioParamValue(AudioParamTarget::PlaybackDetune, -1, 0.0f, -153600.0f, 153600.0f, 0.0f));
    return b.get();
}

} // namespace broaudio::api
