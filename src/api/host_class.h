#pragma once

#include "embed/embed.h"
#include "object_builder.h"

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace bronze::embed {
inline bool isDouble(Value v) { return isNumber(v); }
// `val` is rooted before registerGlobal / globalValue, either of which may
// allocate and move it (embed.h GC contract).
inline void setGlobalValue(std::string_view name, Value val) {
    Persistent v(val);
    registerGlobal(name, v.get());
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) {
        Persistent global(g.value);
        setProperty(global.get(), name, v.get());
    }
}
inline void setGlobalFunction(std::string_view name, uint32_t arity, NativeFn fn) {
    setGlobalValue(name, makeFunction(std::move(fn), arity, name));
}
inline void registerFunction(std::string_view name, NativeFn fn, uint32_t arity = 0) {
    setGlobalValue(name, makeFunction(std::move(fn), arity, name));
}
inline Value getGlobal(std::string_view name) {
    auto gv = globalValue(name);
    return gv.found ? gv.value : undefined();
}
} // namespace bronze::embed

namespace broaudio::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

class HostClass {
public:
    struct Slots {
        ev::Persistent* proto = nullptr;
        ev::Persistent* ctor = nullptr;
    };

    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr);

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;

    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    // The payload of a handle THIS class (or a class that inherit()s from it)
    // made; nullptr for anything else, including another class's handle or
    // another library's. ev::handleData answers for ANY handle, so casting
    // it and then reading a tag out of the payload is itself the type
    // confusion; every payload made by make() is registered with its class
    // instead (host_class.cpp, brands). Allocates nothing.
    void* unwrap(Value val) const;
    bool isInstance(Value val) const { return unwrap(val) != nullptr; }

    Value prototype() const;
    Value constructor() const;

    bool installed() const;

private:
    // The class inherit() chained this one onto. The relationship is the
    // same on every thread, so it lives on the process-global class object.
    mutable std::atomic<const HostClass*> base_{nullptr};

    Slots& slots() const;
    const Slots* slotsIfAny() const;
};

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

} // namespace broaudio::api
