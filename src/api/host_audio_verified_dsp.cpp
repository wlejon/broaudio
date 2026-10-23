#include "host_audio_verified_dsp.h"
#include "broaudio/verified_dsp_kernel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace broaudio::api {

namespace {

HostClass g_verifiedDspNodeClass;

struct HostVerifiedDspNode {
    HostAudioNode base;
    std::shared_ptr<VerifiedDspProcessor> proc;
};

void hostVerifiedDspNodeDtor(void* p) {
    delete static_cast<HostVerifiedDspNode*>(p);
}

std::mutex& dspRegistryMutex() {
    static auto* m = new std::mutex();
    return *m;
}

std::atomic<uint32_t> s_nextDspId{1};

std::unordered_map<uint32_t, CustomDspKernel>& compiledKernelRegistry() {
    static auto* r = new std::unordered_map<uint32_t, CustomDspKernel>();
    return *r;
}

std::unordered_map<uint32_t, std::shared_ptr<VerifiedDspProcessor>>& dspNodeRegistry() {
    static auto* r = new std::unordered_map<uint32_t, std::shared_ptr<VerifiedDspProcessor>>();
    return *r;
}

const CustomDspKernel* lookupCompiledKernel(Value val) {
    if (!ev::isObject(val)) return nullptr;
    Value idVal = ev::getProperty(val, "__broDspKernelId");
    if (!idVal.isNumber()) return nullptr;
    uint32_t id = static_cast<uint32_t>(idVal.asNumber());
    std::lock_guard<std::mutex> lock(dspRegistryMutex());
    auto& reg = compiledKernelRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? &it->second : nullptr;
}

std::shared_ptr<VerifiedDspProcessor> lookupDspProcessor(Value self) {
    if (!ev::isObject(self)) return nullptr;
    if (auto* h = static_cast<HostVerifiedDspNode*>(g_verifiedDspNodeClass.unwrap(self))) {
        if (h->proc) return h->proc;
    }
    Value idVal = ev::getProperty(self, "__broDspNodeId");
    if (!idVal.isNumber()) return nullptr;
    uint32_t id = static_cast<uint32_t>(idVal.asNumber());
    std::lock_guard<std::mutex> lock(dspRegistryMutex());
    auto& reg = dspNodeRegistry();
    auto it = reg.find(id);
    return (it != reg.end()) ? it->second : nullptr;
}

bool extractKernelSourceAndOptions(
    Value fnOrSource,
    Value optsVal,
    std::string& outSource,
    VerifiedDspCompileOptions& outOpts,
    bool& outThrowOnError,
    const CustomDspKernel** outPrecompiled) {

    outSource.clear();
    outThrowOnError = false;
    if (outPrecompiled) *outPrecompiled = nullptr;

    if (const CustomDspKernel* pre = lookupCompiledKernel(fnOrSource)) {
        if (outPrecompiled) *outPrecompiled = pre;
        outSource = pre->source;
        outOpts.maxAmp = pre->maxAmp;
        outOpts.name = pre->kernelName;
    } else if (fnOrSource.isString()) {
        outSource = ev::toUtf8(fnOrSource);
    } else if (ev::isFunction(fnOrSource)) {
        ev::Persistent fnObj(fnOrSource);
        Value nameVal = ev::getProperty(fnObj.get(), "name");
        if (nameVal.isString()) {
            std::string nm = ev::toUtf8(nameVal);
            if (!nm.empty()) outOpts.name = nm;
        }
        Value toStrFn = ev::getProperty(fnObj.get(), "toString");
        if (ev::isFunction(toStrFn)) {
            auto r = ev::call(toStrFn, fnObj.get(), {});
            if (!r.thrown && r.value.isString()) {
                outSource = ev::toUtf8(r.value);
            }
        }
    }

    if (ev::isObject(optsVal) && !ev::isFunction(optsVal)) {
        ev::Persistent opts(optsVal);
        Value maxAmpV = ev::getProperty(opts.get(), "maxAmp");
        if (maxAmpV.isNumber()) {
            float ma = static_cast<float>(maxAmpV.asNumber());
            if (std::isfinite(ma) && ma > 0.0f) outOpts.maxAmp = ma;
        }
        Value superV = ev::getProperty(opts.get(), "superopt");
        if (ev::isBool(superV)) outOpts.enableSuperopt = ev::toBool(superV);
        Value enableSuperV = ev::getProperty(opts.get(), "enableSuperopt");
        if (ev::isBool(enableSuperV)) outOpts.enableSuperopt = ev::toBool(enableSuperV);
        Value nameV = ev::getProperty(opts.get(), "name");
        if (nameV.isString()) outOpts.name = ev::toUtf8(nameV);
        Value kNameV = ev::getProperty(opts.get(), "kernelName");
        if (kNameV.isString()) outOpts.name = ev::toUtf8(kNameV);
        Value throwV = ev::getProperty(opts.get(), "throwOnError");
        if (ev::isBool(throwV)) outThrowOnError = ev::toBool(throwV);
    }

    return !outSource.empty() || (outPrecompiled && *outPrecompiled != nullptr);
}

Value runProcessBlockOnProcessor(const VerifiedDspProcessor& proc, std::span<const Value> args) {
    if (args.empty()) return makeFloat32Array(nullptr, 0);

    // Case 1: processBlock(numSamples, sampleRate = 48000, startTime = 0)
    if (args[0].isNumber()) {
        int n = std::max(0, static_cast<int>(args[0].asNumber()));
        double sr = (args.size() >= 2 && args[1].isNumber()) ? args[1].asNumber() : 48000.0;
        double st = (args.size() >= 3 && args[2].isNumber()) ? args[2].asNumber() : 0.0;
        std::vector<float> out(static_cast<size_t>(n), 0.0f);
        proc.processBlock(nullptr, out.data(), static_cast<uint32_t>(n), st, sr);
        return makeFloat32Array(out);
    }

    // Case 2: processBlock(inputFloat32Array, [outputFloat32Array | sampleRate], [startTime])
    if (ev::isTypedArray(args[0])) {
        auto inInfo = ev::typedArrayInfo(args[0]);
        if (!inInfo.data || inInfo.bytesPerElement != sizeof(float)) {
            ev::throwTypeError("processBlock: input buffer must be a Float32Array");
            return ev::undefined();
        }
        const float* inPtr = reinterpret_cast<const float*>(inInfo.data);
        uint32_t n = static_cast<uint32_t>(inInfo.elementCount);

        if (args.size() >= 2 && ev::isTypedArray(args[1])) {
            auto outInfo = ev::typedArrayInfo(args[1]);
            if (!outInfo.data || outInfo.bytesPerElement != sizeof(float)) {
                ev::throwTypeError("processBlock: output buffer must be a Float32Array");
                return ev::undefined();
            }
            uint32_t count = std::min(n, static_cast<uint32_t>(outInfo.elementCount));
            double sr = (args.size() >= 3 && args[2].isNumber()) ? args[2].asNumber() : 48000.0;
            double st = (args.size() >= 4 && args[3].isNumber()) ? args[3].asNumber() : 0.0;
            proc.processBlock(inPtr, reinterpret_cast<float*>(outInfo.data), count, st, sr);
            return args[1];
        }

        double sr = (args.size() >= 2 && args[1].isNumber()) ? args[1].asNumber() : 48000.0;
        double st = (args.size() >= 3 && args[2].isNumber()) ? args[2].asNumber() : 0.0;
        std::vector<float> out(n, 0.0f);
        proc.processBlock(inPtr, out.data(), n, st, sr);
        return makeFloat32Array(out);
    }

    ev::throwTypeError("processBlock: expected a Float32Array or sample count");
    return ev::undefined();
}

Value implCompileDspKernel(std::span<const Value> args) {
    if (args.empty()) {
        ev::throwTypeError("compileDspKernel requires a JS function or source string");
        return ev::undefined();
    }

    std::string source;
    VerifiedDspCompileOptions opts;
    bool throwOnError = false;
    const CustomDspKernel* precompiled = nullptr;
    Value optsVal = (args.size() >= 2) ? args[1] : ev::undefined();

    if (!extractKernelSourceAndOptions(args[0], optsVal, source, opts, throwOnError, &precompiled)) {
        if (throwOnError) {
            ev::throwTypeError("compileDspKernel: invalid or empty DSP kernel source");
            return ev::undefined();
        }
        ObjectBuilder fail;
        fail.set("ok", ev::fromBool(false));
        fail.set("verified", ev::fromBool(false));
        fail.set("zeroAllocProven", ev::fromBool(false));
        fail.set("nanSafeProven", ev::fromBool(false));
        fail.set("mirEquivProven", ev::fromBool(false));
        fail.set("error", ev::fromUtf8("Invalid or empty DSP kernel source"));
        return fail.get();
    }

    CustomDspKernel kernel = precompiled ? *precompiled : compileVerifiedDspKernel(source, opts);
    if (!kernel.valid && throwOnError) {
        ev::throwTypeError(("compileDspKernel verification failed: " + kernel.errorMessage).c_str());
        return ev::undefined();
    }

    uint32_t kernelId = s_nextDspId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(dspRegistryMutex());
        compiledKernelRegistry()[kernelId] = kernel;
    }

    ObjectBuilder out;
    out.set("__broDspKernelId", ev::fromDouble(static_cast<double>(kernelId)));
    out.set("ok", ev::fromBool(kernel.valid && kernel.verified));
    out.set("verified", ev::fromBool(kernel.verified));
    out.set("zeroAllocProven", ev::fromBool(kernel.zeroAllocProven));
    out.set("nanSafeProven", ev::fromBool(kernel.nanSafeProven));
    out.set("mirEquivProven", ev::fromBool(kernel.mirEquivProven));
    out.set("maxAmp", ev::fromDouble(static_cast<double>(kernel.maxAmp)));
    out.set("kernelName", ev::fromUtf8(kernel.kernelName));
    out.set("source", ev::fromUtf8(kernel.source));
    out.set("error", ev::fromUtf8(kernel.errorMessage));

    out.def("evalSample", 4, [](Value self, std::span<const Value> a) -> Value {
        const CustomDspKernel* k = lookupCompiledKernel(self);
        if (!k || !k->fn) return ev::fromDouble(0.0);
        float s  = (a.size() >= 1 && a[0].isNumber()) ? static_cast<float>(a[0].asNumber()) : 0.0f;
        float t  = (a.size() >= 2 && a[1].isNumber()) ? static_cast<float>(a[1].asNumber()) : 0.0f;
        float p0 = (a.size() >= 3 && a[2].isNumber()) ? static_cast<float>(a[2].asNumber()) : 0.0f;
        float p1 = (a.size() >= 4 && a[3].isNumber()) ? static_cast<float>(a[3].asNumber()) : 0.0f;
        return ev::fromDouble(static_cast<double>(k->eval(s, t, p0, p1)));
    });
    out.def("eval", 4, [](Value self, std::span<const Value> a) -> Value {
        const CustomDspKernel* k = lookupCompiledKernel(self);
        if (!k || !k->fn) return ev::fromDouble(0.0);
        float s  = (a.size() >= 1 && a[0].isNumber()) ? static_cast<float>(a[0].asNumber()) : 0.0f;
        float t  = (a.size() >= 2 && a[1].isNumber()) ? static_cast<float>(a[1].asNumber()) : 0.0f;
        float p0 = (a.size() >= 3 && a[2].isNumber()) ? static_cast<float>(a[2].asNumber()) : 0.0f;
        float p1 = (a.size() >= 4 && a[3].isNumber()) ? static_cast<float>(a[3].asNumber()) : 0.0f;
        return ev::fromDouble(static_cast<double>(k->eval(s, t, p0, p1)));
    });
    out.def("processBlock", 3, [](Value self, std::span<const Value> a) -> Value {
        const CustomDspKernel* k = lookupCompiledKernel(self);
        if (!k || !k->valid) return makeFloat32Array(nullptr, 0);
        VerifiedDspProcessor tmp;
        tmp.installKernel(*k, false);
        return runProcessBlockOnProcessor(tmp, a);
    });

    return out.get();
}

Value createVerifiedDspNodeValue(Value ctxVal, std::span<const Value> args) {
    if (args.empty()) {
        ev::throwTypeError("createVerifiedDspNode requires a JS function, source string, or compiled kernel");
        return ev::undefined();
    }

    std::string source;
    VerifiedDspCompileOptions opts;
    bool throwOnError = true;
    const CustomDspKernel* precompiled = nullptr;
    Value optsVal = (args.size() >= 2) ? args[1] : ev::undefined();

    bool extracted = extractKernelSourceAndOptions(args[0], optsVal, source, opts, throwOnError, &precompiled);
    // For createVerifiedDspNode, default throwOnError is true unless explicitly set to false in opts
    if (ev::isObject(optsVal) && !ev::isFunction(optsVal)) {
        Value tv = ev::getProperty(optsVal, "throwOnError");
        if (!ev::isBool(tv)) throwOnError = true;
    } else {
        throwOnError = true;
    }

    if (!extracted) {
        ev::throwTypeError("createVerifiedDspNode: expected a JS DSP function or source string");
        return ev::undefined();
    }

    CustomDspKernel kernel = precompiled ? *precompiled : compileVerifiedDspKernel(source, opts);
    if (!kernel.valid || !kernel.verified) {
        if (throwOnError) {
            ev::throwTypeError(("VerifiedDspNode verification failed: " + kernel.errorMessage).c_str());
            return ev::undefined();
        }
    }

    auto proc = std::make_shared<VerifiedDspProcessor>();
    if (kernel.valid && kernel.verified) {
        proc->installKernel(kernel, false);
    }

    // Apply initial params if provided in opts.params = [p0, p1, ...]
    if (ev::isObject(optsVal) && !ev::isFunction(optsVal)) {
        Value p0v = ev::getProperty(optsVal, "p0");
        if (p0v.isNumber()) proc->setParam(0, static_cast<float>(p0v.asNumber()));
        Value p1v = ev::getProperty(optsVal, "p1");
        if (p1v.isNumber()) proc->setParam(1, static_cast<float>(p1v.asNumber()));
    }

    uint32_t nodeId = s_nextDspId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(dspRegistryMutex());
        dspNodeRegistry()[nodeId] = proc;
    }

    // Attach to master bus if Engine exists so real-time audio rendering runs it automatically
    if (auto* e = getAudioEngine()) {
        if (auto* mb = e->masterBus()) {
            mb->customDspProcessor.store(proc, std::memory_order_release);
        }
    }

    auto* hostNode = new HostVerifiedDspNode();
    hostNode->base.nodeType = AudioNodeType::Generic;
    hostNode->proc = proc;

    ObjectBuilder b(g_verifiedDspNodeClass.make(hostNode, hostVerifiedDspNodeDtor));
    b.set("__broDspNodeId", ev::fromDouble(static_cast<double>(nodeId)));
    b.set("nodeType", ev::fromUtf8("verified-dsp"));
    b.set("busId", ev::fromDouble(0.0));
    b.set("numberOfInputs", ev::fromDouble(1.0));
    b.set("numberOfOutputs", ev::fromDouble(1.0));
    b.set("channelCount", ev::fromDouble(2.0));
    b.set("channelCountMode", ev::fromUtf8("max"));
    b.set("channelInterpretation", ev::fromUtf8("speakers"));
    if (ev::isObject(ctxVal)) {
        b.set("context", ctxVal);
    } else if (ev::GlobalValue g = ev::globalValue("__defaultAudioCtx"); g.found && ev::isObject(g.value)) {
        b.set("context", g.value);
    }

    return b.get();
}

void decorateVerifiedDspNodeProto(ObjectBuilder& b) {
    b.accessor("verified", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromBool(p ? p->isVerified() : false);
    }, nullptr);

    b.accessor("zeroAllocProven", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromBool(p ? p->isZeroAllocProven() : false);
    }, nullptr);

    b.accessor("nanSafeProven", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromBool(p ? p->isNanSafeProven() : false);
    }, nullptr);

    b.accessor("mirEquivProven", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromBool(p ? p->isMirEquivProven() : false);
    }, nullptr);

    b.accessor("hotSwapCount", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromDouble(p ? static_cast<double>(p->hotSwapCount()) : 0.0);
    }, nullptr);

    b.accessor("maxAmp", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromDouble(p ? static_cast<double>(p->maxAmp()) : 1.0);
    }, nullptr);

    b.accessor("kernelName", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromUtf8(p ? p->kernelName() : "");
    }, nullptr);

    b.accessor("source", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromUtf8(p ? p->currentSource() : "");
    }, nullptr);

    b.accessor("lastError", [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        return ev::fromUtf8(p ? p->lastError() : "");
    }, nullptr);

    b.def("setParam", 2, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (p && a.size() >= 2 && a[0].isNumber() && a[1].isNumber()) {
            int idx = static_cast<int>(a[0].asNumber());
            if (idx >= 0) {
                p->setParam(static_cast<size_t>(idx), static_cast<float>(a[1].asNumber()));
            }
        }
        return self;
    });

    b.def("getParam", 1, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (p && !a.empty() && a[0].isNumber()) {
            int idx = static_cast<int>(a[0].asNumber());
            if (idx >= 0) {
                return ev::fromDouble(static_cast<double>(p->getParam(static_cast<size_t>(idx))));
            }
        }
        return ev::fromDouble(0.0);
    });

    b.def("evalSample", 4, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (!p) return ev::fromDouble(0.0);
        float s = (a.size() >= 1 && a[0].isNumber()) ? static_cast<float>(a[0].asNumber()) : 0.0f;
        float t = (a.size() >= 2 && a[1].isNumber()) ? static_cast<float>(a[1].asNumber()) : 0.0f;
        if (a.size() >= 3 && a[2].isNumber()) {
            float p0 = static_cast<float>(a[2].asNumber());
            float p1 = (a.size() >= 4 && a[3].isNumber()) ? static_cast<float>(a[3].asNumber()) : p->getParam(1);
            return ev::fromDouble(static_cast<double>(p->evalSampleWithParams(s, t, p0, p1)));
        }
        return ev::fromDouble(static_cast<double>(p->evalSample(s, t)));
    });

    b.def("processBlock", 3, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (!p) return makeFloat32Array(nullptr, 0);
        return runProcessBlockOnProcessor(*p, a);
    });

    b.def("hotSwap", 2, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (!p || a.empty()) return ev::fromBool(false);

        std::string source;
        VerifiedDspCompileOptions opts;
        opts.maxAmp = p->maxAmp();
        bool throwOnError = false;
        const CustomDspKernel* precompiled = nullptr;
        Value optsVal = (a.size() >= 2) ? a[1] : ev::undefined();

        if (!extractKernelSourceAndOptions(a[0], optsVal, source, opts, throwOnError, &precompiled)) {
            if (throwOnError) {
                ev::throwTypeError("hotSwap: invalid DSP kernel source");
                return ev::undefined();
            }
            return ev::fromBool(false);
        }

        if (precompiled) {
            if (!precompiled->valid || !precompiled->verified) {
                if (throwOnError) {
                    ev::throwTypeError(("hotSwap verification failed: " + precompiled->errorMessage).c_str());
                    return ev::undefined();
                }
                return ev::fromBool(false);
            }
            return ev::fromBool(p->installKernel(*precompiled, true));
        }

        std::string err;
        bool ok = p->hotSwap(source, opts, &err);
        if (!ok && throwOnError) {
            ev::throwTypeError(("hotSwap verification failed: " + err).c_str());
            return ev::undefined();
        }
        return ev::fromBool(ok);
    });

    b.def("connect", 1, [](Value self, std::span<const Value> a) -> Value {
        auto p = lookupDspProcessor(self);
        if (p) {
            if (auto* e = getAudioEngine()) {
                if (auto* mb = e->masterBus()) {
                    mb->customDspProcessor.store(p, std::memory_order_release);
                }
            }
        }
        return (!a.empty() && ev::isObject(a[0])) ? a[0] : self;
    });

    b.def("disconnect", 0, [](Value self, std::span<const Value>) -> Value {
        auto p = lookupDspProcessor(self);
        if (p) {
            if (auto* e = getAudioEngine()) {
                if (auto* mb = e->masterBus()) {
                    auto cur = mb->customDspProcessor.load(std::memory_order_acquire);
                    if (cur.get() == p.get()) {
                        mb->customDspProcessor.store(nullptr, std::memory_order_release);
                    }
                }
            }
        }
        return ev::undefined();
    });
}

} // namespace

void attachAudioContextVerifiedDspMethods(ObjectBuilder& b) {
    b.def("createVerifiedDspNode", 2, [](Value self, std::span<const Value> a) -> Value {
        return createVerifiedDspNodeValue(self, a);
    });
    b.def("compileDspKernel", 2, [](Value, std::span<const Value> a) -> Value {
        return implCompileDspKernel(a);
    });
}

void attachBroAudioVerifiedDspMethods(ObjectBuilder& b) {
    b.def("createVerifiedDspNode", 2, [](Value, std::span<const Value> a) -> Value {
        return createVerifiedDspNodeValue(ev::undefined(), a);
    });
    b.def("compileDspKernel", 2, [](Value, std::span<const Value> a) -> Value {
        return implCompileDspKernel(a);
    });
}

void installAudioVerifiedDspGlobals() {
    if (!g_verifiedDspNodeClass.installed()) {
        g_verifiedDspNodeClass.install("VerifiedDspNode", 2,
            [](Value, std::span<const Value> a) -> Value {
                if (a.size() >= 2 && ev::isObject(a[0]) && !ev::isFunction(a[0]) &&
                    lookupCompiledKernel(a[0]) == nullptr) {
                    return createVerifiedDspNodeValue(a[0], a.subspan(1));
                }
                return createVerifiedDspNodeValue(ev::undefined(), a);
            },
            decorateVerifiedDspNodeProto);
        g_verifiedDspNodeClass.inherit(g_audioNodeClass);
    }

    // Ensure global bro.audio exists and attach compileDspKernel & createVerifiedDspNode.
    // Mutating the existing `bro` object in-place preserves any existing host global slot
    // while setting it on `globalThis` avoids locking `registerGlobal("bro")` prematurely
    // before `globalThis.bro = ...` in standalone tests.
    ev::GlobalValue broGlobal = ev::globalValue("bro");
    bool hadBroGlobal = broGlobal.found && ev::isObject(broGlobal.value);
    ObjectBuilder broObj = hadBroGlobal ? ObjectBuilder(broGlobal.value) : ObjectBuilder();
    Value audioVal = ev::getProperty(broObj.get(), "audio");
    ObjectBuilder audioObj = ev::isObject(audioVal) ? ObjectBuilder(audioVal) : ObjectBuilder();
    attachBroAudioVerifiedDspMethods(audioObj);
    broObj.set("audio", audioObj.get());
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        ev::Persistent global(g.value);
        ev::setProperty(global.get(), "bro", broObj.get());
    }
}

} // namespace broaudio::api
