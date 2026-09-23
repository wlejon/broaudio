#include "host_class.h"
#include "object_builder.h"

#include <memory>
#include <mutex>
#include <unordered_map>

namespace broaudio::api {

namespace {

std::unordered_map<const HostClass*, HostClass::Slots>& threadSlots() {
    static thread_local std::unordered_map<const HostClass*, HostClass::Slots> t;
    return t;
}

// ── Brands ──────────────────────────────────────────────────────────────────
// Every payload make() hands out is registered with the class that made it,
// and unwrap() answers only for that class and the classes it inherits
// from. The table is process-wide (a Worker's handles live on its own
// thread but the payload addresses are unique) and outlives every sweep.
struct Brand {
    const HostClass* cls;
    ev::HandleDestructor dtor;
};

std::mutex& brandMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<const void*, Brand>& brands() {
    static auto* m = new std::unordered_map<const void*, Brand>();
    return *m;
}

// The destructor every branded handle carries: unregister, then run the
// class's own destructor.
void brandedDestroy(void* data) {
    ev::HandleDestructor dtor = nullptr;
    {
        std::lock_guard<std::mutex> lk(brandMutex());
        auto& m = brands();
        auto it = m.find(data);
        if (it != m.end()) {
            dtor = it->second.dtor;
            m.erase(it);
        }
    }
    if (dtor) dtor(data);
}

} // namespace

void* HostClass::unwrap(Value val) const {
    void* data = ev::handleData(val);
    if (!data) return nullptr;
    const HostClass* cls = nullptr;
    {
        std::lock_guard<std::mutex> lk(brandMutex());
        auto& m = brands();
        auto it = m.find(data);
        if (it == m.end()) return nullptr;
        cls = it->second.cls;
    }
    for (; cls; cls = cls->base_.load(std::memory_order_acquire)) {
        if (cls == this) return data;
    }
    return nullptr;
}

HostClass::Slots& HostClass::slots() const {
    return threadSlots()[this];
}

const HostClass::Slots* HostClass::slotsIfAny() const {
    auto& t = threadSlots();
    auto it = t.find(this);
    return it == t.end() ? nullptr : &it->second;
}

bool HostClass::installed() const {
    const Slots* s = slotsIfAny();
    return s && s->ctor;
}

void HostClass::install(const char* name, uint32_t arity, ev::NativeFn body,
                        const std::function<void(ObjectBuilder&)>& decorate) {
    ev::NativeFn ctorBody = body;
    if (!ctorBody) {
        std::string msg = std::string("TypeError: ") + name + " is not a constructor";
        ctorBody = [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); };
    }

    Slots& s = slots();
    ev::Persistent ctor(ev::makeFunction(std::move(ctorBody), arity, name));
    s.ctor = new ev::Persistent(ctor.get());

    {
        ObjectBuilder proto(ev::getProperty(ctor.get(), "prototype"));
        proto.set("constructor", ctor.get());
        if (decorate) decorate(proto);
        s.proto = new ev::Persistent(proto.get());
    }

    ev::setGlobalValue(name, s.ctor->get());
}

void HostClass::alias(const char* name) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    ev::setGlobalValue(name, s->ctor->get());
}

void HostClass::inherit(const HostClass& base) const {
    base_.store(&base, std::memory_order_release);
    const Slots* s = slotsIfAny();
    const Slots* b = base.slotsIfAny();
    if (!s || !s->proto || !b || !b->proto) return;
    ev::GlobalValue objectCtor = ev::globalValue("Object");
    if (!objectCtor.found) return;
    ev::Persistent objectNs(objectCtor.value);
    ev::Persistent setProto(ev::getProperty(objectNs.get(), "setPrototypeOf"));
    if (!ev::isFunction(setProto.get())) return;
    const Value args[2] = {s->proto->get(), b->proto->get()};
    ev::call(setProto.get(), ev::undefined(), std::span<const Value>(args, 2));
}

Value HostClass::make(void* data, ev::HandleDestructor dtor, ev::Finalize when) const {
    if (data) {
        std::lock_guard<std::mutex> lk(brandMutex());
        brands()[data] = Brand{this, dtor};
    }
    const Slots* s = slotsIfAny();
    if (!s || !s->proto) return ev::makeHandle(data, brandedDestroy, when);
    return ev::makeHandle(data, brandedDestroy, when, s->proto->get());
}

void HostClass::setStatic(const char* name, Value v) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    s->ctor->set(ev::setProperty(s->ctor->get(), name, v));
}

Value HostClass::prototype() const {
    const Slots* s = slotsIfAny();
    return (s && s->proto) ? s->proto->get() : ev::undefined();
}

Value HostClass::constructor() const {
    const Slots* s = slotsIfAny();
    return (s && s->ctor) ? s->ctor->get() : ev::undefined();
}

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
    Value arr = ev::makeArray(static_cast<uint32_t>(count));
    if (!ev::isObject(arr)) {
        return ev::undefined();
    }
    ev::Persistent persistentArr(arr);
    for (size_t i = 0; i < count; ++i) {
        Value v = make(i);
        persistentArr.set(ev::setElement(persistentArr.get(), static_cast<uint32_t>(i), v));
    }
    return persistentArr.get();
}

} // namespace broaudio::api
