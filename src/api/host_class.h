#pragma once

#include "embed/embed.h"
#include "object_builder.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace bronze::embed {
inline bool isDouble(Value v) { return isNumber(v); }
inline void setGlobalValue(std::string_view name, Value val) {
    registerGlobal(name, val);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, val);
}
inline void setGlobalFunction(std::string_view name, uint32_t arity, NativeFn fn) {
    auto f = makeFunction(std::move(fn), arity, name);
    registerGlobal(name, f);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, f);
}
inline void registerFunction(std::string_view name, NativeFn fn, uint32_t arity = 0) {
    auto f = makeFunction(std::move(fn), arity, name);
    registerGlobal(name, f);
    auto g = globalValue("globalThis");
    if (g.found && isObject(g.value)) setProperty(g.value, name, f);
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
    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr);

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;

    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    void* unwrap(Value val) const { return ev::handleData(val); }

    Value prototype() const;
    Value constructor() const;

private:
    ev::Persistent* proto_ = nullptr;
    ev::Persistent* ctor_ = nullptr;
};

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

} // namespace broaudio::api
