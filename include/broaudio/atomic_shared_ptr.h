#pragma once
// Wait-free RCU snapshot pointer for the audio engine. Replaces the previous
// implementation that wrapped the deprecated free-function
// std::atomic_load(shared_ptr*) — that path falls back to a hidden mutex pool
// on libstdc++ and MSVC STL, which is unsafe for the audio thread.
//
// This implementation is wait-free on the read path on every supported
// platform (libc++ / libstdc++ / MSVC STL):
//
//   load() = atomic-load a Snapshot* + copy a shared_ptr's control-block
//            refcount (lock-free on every standard library).
//
// Reclamation is QSBR (see broaudio/rcu.h). Writers must hold an external
// mutex (single-writer-per-slot — the engine already serialises every store
// via voiceWriteMutex_ / busWriteMutex_ / mediaWriteMutex_), and the audio
// thread must wrap each callback in a RcuDomain::ReadScope.
//
// An AtomicSharedPtr without a bound RcuDomain is single-threaded only:
// store() inline-deletes the previous snapshot. This mode exists so Voice
// (constructed before Engine binds the domain in createVoice) and unit tests
// can default-construct the slot without immediately needing a domain.

#include "broaudio/rcu.h"

#include <atomic>
#include <memory>

namespace broaudio {

template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() = default;

    explicit AtomicSharedPtr(std::shared_ptr<T> initial) noexcept
        : head_(initial ? new Snapshot{std::move(initial)} : nullptr) {}

    AtomicSharedPtr(RcuDomain& domain, std::shared_ptr<T> initial = {}) noexcept
        : head_(initial ? new Snapshot{std::move(initial)} : nullptr),
          domain_(&domain) {}

    ~AtomicSharedPtr() {
        // No readers can race at this point (engine has stopped SDL).
        delete head_.load(std::memory_order_relaxed);
    }

    AtomicSharedPtr(const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator=(const AtomicSharedPtr&) = delete;

    // Bind a domain after construction. Must be called before any concurrent
    // access; not thread-safe with respect to load()/store().
    void setDomain(RcuDomain& domain) noexcept { domain_ = &domain; }

    // Wait-free read. Memory order argument is honoured for the head_ load
    // (acquire is the floor — sequential reads must observe a fully
    // constructed Snapshot). The shared_ptr copy uses the standard library's
    // built-in lock-free control-block refcount path.
    std::shared_ptr<T> load(std::memory_order order = std::memory_order_seq_cst) const noexcept
    {
        std::memory_order load_order =
            (order == std::memory_order_relaxed) ? std::memory_order_acquire : order;
        Snapshot* s = head_.load(load_order);
        if (!s) return {};
        return s->sp;
    }

    // Single-writer publish. Caller must serialise stores externally.
    //
    // The exchange uses seq_cst so it pairs with RcuDomain::observedSeq()'s
    // seq_cst load to give a total order across head_ and readerSeq_ — the
    // inline-free fast path in retire() relies on that order to be safe
    // (otherwise a reader could observe head_=old after a quiescent
    // observedSeq, yielding UAF).
    void store(std::shared_ptr<T> next,
               std::memory_order /*order*/ = std::memory_order_seq_cst) noexcept
    {
        Snapshot* fresh = next ? new Snapshot{std::move(next)} : nullptr;
        Snapshot* old = head_.exchange(fresh, std::memory_order_seq_cst);
        if (!old) return;

        if (domain_) {
            domain_->retire(old, &deleteSnapshot);
        } else {
            // No domain bound — single-threaded use. Free inline.
            delete old;
        }
    }

private:
    struct Snapshot {
        std::shared_ptr<T> sp;
    };

    static void deleteSnapshot(void* p) noexcept {
        delete static_cast<Snapshot*>(p);
    }

    std::atomic<Snapshot*> head_{nullptr};
    RcuDomain*             domain_{nullptr};
};

} // namespace broaudio
