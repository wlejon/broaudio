#pragma once
// Quiescent-state-based reclamation (QSBR) for the audio engine's RCU
// snapshots (AtomicSharedPtr). One RcuDomain is shared by all RCU slots that
// participate in the same reader/writer protocol; readers bracket each
// critical section with a RcuDomain::ReadScope, and writers retire old
// snapshots to the domain. Reclamation runs from a non-RT thread, typically
// Engine::update().
//
// Reader: wait-free. ReadScope ctor/dtor are atomic fetch_add/fetch_sub on a
// reader-count, plus a fetch_add on a generation counter when the count
// transitions to zero. All snapshot loads inside the scope are unprotected;
// the scope guarantees no writer will free what's been published.
//
// Writer (under external single-writer mutex): publishes a new snapshot,
// then retires the old one. If no reader is currently in a scope, the old
// snapshot is freed inline. Otherwise it is queued with the current
// generation; reclaim() later frees it once the generation has advanced
// (which happens only when the reader count transitions to zero, i.e., all
// readers in flight at retire-time have exited).

#include <atomic>
#include <cstdint>
#include <mutex>

namespace broaudio {

class RcuDomain {
public:
    RcuDomain() = default;
    ~RcuDomain();

    RcuDomain(const RcuDomain&) = delete;
    RcuDomain& operator=(const RcuDomain&) = delete;

    // Reader bookkeeping. The seq_cst here pairs with the seq_cst exchange
    // in AtomicSharedPtr::store and the seq_cst loads in retire/reclaim, so
    // that "publish then observe count" gives a consistent total order with
    // "enter scope then load head" — the inline-free fast path relies on it.
    void enterRead() noexcept {
        activeReaders_.fetch_add(1, std::memory_order_seq_cst);
    }
    void exitRead() noexcept {
        // If we're the last reader leaving, bump the generation so writers
        // can recognise the transition through zero — only that transition
        // proves all readers in flight at any earlier retire-time have
        // exited.
        if (activeReaders_.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            generation_.fetch_add(1, std::memory_order_seq_cst);
        }
    }
    uint64_t activeReaders() const noexcept {
        return activeReaders_.load(std::memory_order_seq_cst);
    }
    uint64_t generation() const noexcept {
        return generation_.load(std::memory_order_seq_cst);
    }

    using Deleter = void (*)(void*) noexcept;
    void retire(void* node, Deleter deleter) noexcept;
    void reclaim() noexcept;

    class ReadScope {
    public:
        explicit ReadScope(RcuDomain& d) noexcept : d_(d) { d_.enterRead(); }
        ~ReadScope() noexcept { d_.exitRead(); }
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;
    private:
        RcuDomain& d_;
    };

private:
    struct Retired {
        Deleter   deleter;
        void*     node;
        uint64_t  gen;     // generation observed at retire time
        Retired*  next;
    };

    std::atomic<uint64_t> activeReaders_{0};
    std::atomic<uint64_t> generation_{0};
    std::mutex            retireMutex_;
    Retired*              retired_ = nullptr;
};

} // namespace broaudio
