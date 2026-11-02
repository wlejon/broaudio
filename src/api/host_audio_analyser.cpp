#include "host_audio_internal.h"

namespace broaudio::api {

HostClass g_analyserNodeClass;

void hostAnalyserDtor(void* p) {
    delete static_cast<HostAnalyserNode*>(p);
}

// The latest `n` samples of what the analyser taps (source: 0 output,
// 1 mic, 2 both), into `dst`.
static void readAnalyserSource(const HostAnalyserNode* analyser, float* dst, int n) {
    auto* e = getAudioEngine();
    if (!e || n <= 0) return;
    if (analyser->source == 2) {
        if (analyser->hasConnectedInput && analyser->inputTapBuffer) {
            analyser->inputTapBuffer->readLatest(dst, n);
        } else {
            e->outputBuffer().readLatest(dst, n);
        }
        if (!e->isMicMuted()) {
            std::vector<float> mic(static_cast<size_t>(n), 0.0f);
            e->micBuffer().readLatest(mic.data(), n);
            for (int i = 0; i < n; i++) dst[i] += mic[i];
        }
        return;
    }
    if (analyser->source == 1) {
        e->micBuffer().readLatest(dst, n);
        return;
    }
    if (analyser->hasConnectedInput && analyser->inputTapBuffer) {
        analyser->inputTapBuffer->readLatest(dst, n);
    } else {
        e->outputBuffer().readLatest(dst, n);
    }
}

void decorateMediaStreamSourceNodeProto(ObjectBuilder&) {
    // connect(analyser) -> analyser.source = 1 is handled by the shared
    // AudioNode.connect (see decorateAudioNodeProto).
}

void decorateAnalyserNodeProto(ObjectBuilder& b) {
    b.accessor("source",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->source);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   int v = i32At(a, 0);
                   analyser->source = (v == 2) ? 2 : (v == 1) ? 1 : 0;
                   return ev::undefined();
               });

    b.accessor("fftSize",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->fftSize);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   int sz = i32At(a, 0);
                   if (sz >= 32 && sz <= 32768 && (sz & (sz - 1)) == 0) {
                       analyser->fftSize = sz;
                   } else {
                       ev::throwRangeError("AnalyserNode.fftSize must be a power of 2 between 32 and 32768");
                   }
                   return ev::undefined();
               });

    b.accessor("frequencyBinCount", [](Value self_, std::span<const Value>) {
        HostAnalyserNode* analyser = analyserOf(self_);
        return ev::fromDouble(analyser ? analyser->fftSize / 2 : 1024);
    }, nullptr);

    b.accessor("minDecibels",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->minDecibels);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->minDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("maxDecibels",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->maxDecibels);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->maxDecibels = static_cast<float>(numAt(a, 0));
                   return ev::undefined();
               });

    b.accessor("smoothingTimeConstant",
               [](Value self_, std::span<const Value>) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   return ev::fromDouble(analyser->smoothingTimeConstant);
               },
               [](Value self_, std::span<const Value> a) {
                   HostAnalyserNode* analyser = analyserOf(self_);
                   if (!analyser) return ev::undefined();
                   analyser->smoothingTimeConstant = static_cast<float>(std::clamp(numAt(a, 0), 0.0, 1.0));
                   return ev::undefined();
               });

    b.def("getFloatFrequencyData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        ev::TypedArrayInfo info = outArrayArg(a.empty() ? ev::undefined() : a[0], ev::elements::Float32,
                                              "AnalyserNode.getFloatFrequencyData: array", "a Float32Array");
        if (!info) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        std::vector<float> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            outData[i] = analyser->smoothedMagnitudes[i];
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteFrequencyData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        ev::TypedArrayInfo info = outArrayArg(a.empty() ? ev::undefined() : a[0], ev::elements::Uint8,
                                              "AnalyserNode.getByteFrequencyData: array", "a Uint8Array");
        if (!info) return ev::undefined();

        int n = analyser->fftSize;
        int halfN = n / 2;
        std::vector<float> real(n, 0.0f), imag(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        for (int i = 0; i < n; i++) {
            float w = 0.42f - 0.5f * std::cos(2.0f * static_cast<float>(M_PI) * i / (n - 1))
                            + 0.08f * std::cos(4.0f * static_cast<float>(M_PI) * i / (n - 1));
            real[i] *= w;
        }

        broaudio::fft(real.data(), imag.data(), n);

        if (analyser->smoothedMagnitudes.size() != static_cast<size_t>(halfN)) {
            analyser->smoothedMagnitudes.assign(halfN, -100.0f);
        }

        float sm = std::clamp(analyser->smoothingTimeConstant, 0.0f, 1.0f);
        float minDb = analyser->minDecibels;
        float maxDb = analyser->maxDecibels;
        float range = (maxDb > minDb) ? (maxDb - minDb) : 1.0f;

        std::vector<uint8_t> outData(halfN);
        for (int i = 0; i < halfN; i++) {
            float mag = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) / (n / 2.0f);
            float db = (mag > 1e-6f) ? 20.0f * std::log10(mag) : -100.0f;
            analyser->smoothedMagnitudes[i] = sm * analyser->smoothedMagnitudes[i] + (1.0f - sm) * db;
            float norm = (analyser->smoothedMagnitudes[i] - minDb) / range;
            norm = std::clamp(norm, 0.0f, 1.0f);
            outData[i] = static_cast<uint8_t>(norm * 255.0f);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(halfN));
        std::memcpy(info.data, outData.data(), count);
        return ev::undefined();
    });

    b.def("getFloatTimeDomainData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        ev::TypedArrayInfo info = outArrayArg(a.empty() ? ev::undefined() : a[0], ev::elements::Float32,
                                              "AnalyserNode.getFloatTimeDomainData: array", "a Float32Array");
        if (!info) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, real.data(), count * sizeof(float));
        return ev::undefined();
    });

    b.def("getByteTimeDomainData", 1, [](Value self_, std::span<const Value> a) -> Value {
        HostAnalyserNode* analyser = analyserOf(self_);
        if (!analyser) return ev::undefined();
        ev::TypedArrayInfo info = outArrayArg(a.empty() ? ev::undefined() : a[0], ev::elements::Uint8,
                                              "AnalyserNode.getByteTimeDomainData: array", "a Uint8Array");
        if (!info) return ev::undefined();

        int n = analyser->fftSize;
        std::vector<float> real(n, 0.0f);
        readAnalyserSource(analyser, real.data(), n);

        std::vector<uint8_t> outData(n);
        for (int i = 0; i < n; i++) {
            float s = std::clamp(real[i], -1.0f, 1.0f);
            outData[i] = static_cast<uint8_t>((s * 0.5f + 0.5f) * 255.0f);
        }

        size_t count = std::min(static_cast<size_t>(info.elementCount), static_cast<size_t>(n));
        std::memcpy(info.data, outData.data(), count);
        return ev::undefined();
    });
}

Value makeAnalyserNodeValue() {
    auto* analyser = new HostAnalyserNode();
    analyser->base.nodeType = AudioNodeType::Analyser;
    analyser->inputTapBuffer = std::make_shared<broaudio::AnalysisBuffer>(16384);
    return g_analyserNodeClass.make(analyser, hostAnalyserDtor);
}

} // namespace broaudio::api
