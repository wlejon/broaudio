#pragma once

#include "api.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace broaudio::api {

// ---------------------------------------------------------------------------
// Argument / Data Helpers
// ---------------------------------------------------------------------------

inline double numAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return 0.0;
    Value v = args[i];
    if (ev::isObject(v)) return 0.0;
    double d = ev::toDouble(v);
    return std::isfinite(d) ? d : 0.0;
}

// Optional trailing rampSeconds (setPlaybackGain / setPlaybackSend /
// setBusGain): absent, negative, NaN or non-finite is 0 (the de-zipper),
// otherwise capped at an hour.
inline float rampAt(std::span<const Value> args, size_t i) {
    double d = numAt(args, i);
    if (!std::isfinite(d) || d <= 0.0) return 0.0f;
    return static_cast<float>(std::min(d, 3600.0));
}

// A JS number truncated to int32, saturating: NaN is 0 and +-Infinity or
// anything past the range lands on the nearest end. A plain static_cast of an
// out-of-range double is undefined behaviour (MSVC yields INT_MIN, so 1e10
// "frames" became a negative count).
inline int32_t saturateI32(double d) {
    if (std::isnan(d)) return 0;
    if (d <= static_cast<double>(INT32_MIN)) return INT32_MIN;
    if (d >= static_cast<double>(INT32_MAX)) return INT32_MAX;
    return static_cast<int32_t>(d);
}

// The same for uint32: negatives are 0.
inline uint32_t saturateU32(double d) {
    if (std::isnan(d) || d <= 0) return 0;
    if (d >= static_cast<double>(UINT32_MAX)) return UINT32_MAX;
    return static_cast<uint32_t>(d);
}

// Longest `_targets` / `_listeners` edge array a node's bookkeeping reads.
// Those arrays are ordinary properties a script can overwrite; a longer one
// is treated as empty instead of sizing a walk or an allocation.
constexpr uint32_t kMaxEdgeList = 1u << 20;

inline int32_t i32At(std::span<const Value> args, size_t i) {
    return saturateI32(numAt(args, i));
}

inline uint32_t u32At(std::span<const Value> args, size_t i) {
    return saturateU32(numAt(args, i));
}

inline bool boolAt(std::span<const Value> args, size_t i) {
    if (i >= args.size()) return false;
    return ev::toBool(args[i]);
}

// Throw Web Audio's InvalidStateError: a DOMException when the realm has
// the constructor (bro installs one), otherwise an Error whose name is
// 'InvalidStateError'. Returns undefined with the exception pending, for a
// native to return straight away.
inline Value throwInvalidStateError(const std::string& message) {
    ev::GlobalValue dom = ev::globalValue("DOMException");
    if (dom.found && ev::isFunction(dom.value)) {
        ev::Persistent ctor(dom.value);
        ev::Persistent msg(ev::fromUtf8(message));
        ev::Persistent name(ev::fromUtf8("InvalidStateError"));
        const Value args[2] = {msg.get(), name.get()};
        ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 2));
        if (!r.thrown) {
            ev::throwValue(r.value);
            return ev::undefined();
        }
    }
    ev::GlobalValue errCtor = ev::globalValue("Error");
    ev::Persistent ctor(errCtor.value);
    ev::Persistent msg(ev::fromUtf8(message));
    const Value arg = msg.get();
    ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(&arg, 1));
    if (r.thrown) return ev::throwError(message);
    ev::Persistent err(r.value);
    ev::Persistent name(ev::fromUtf8("InvalidStateError"));
    ev::setProperty(err.get(), "name", name.get());
    ev::throwValue(err.get());
    return ev::undefined();
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
    // A plain array's length is at most 2^32 - 1, but an array-like's is
    // whatever it says; past this the copy is refused rather than sized to
    // an allocation that throws out of the binding.
    constexpr uint32_t kMaxPlainArray = 1u << 26;
    uint32_t n = saturateU32(ev::toDouble(lenV));
    if (n > kMaxPlainArray) return false;
    storage.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        Value e = ev::getElement(root.get(), i);
        double d = ev::isObject(e) ? 0.0 : ev::toDouble(e);
        storage[i] = !std::isfinite(d) ? T{} : convert(d);
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

} // namespace broaudio::api
