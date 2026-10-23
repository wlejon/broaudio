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

    // Reader bookkeeping. Both the active reader count (low 20 bits) and the
    // quiescent generation counter (high 44 bits) live in a single 64-bit
    // atomic word (`state_`) so that:
    //   1. `exitRead()` transitions `(gen, readers=1) -> (gen+1, readers=0)` in
    //      one atomic CAS (preventing a preempted reader's delayed generation
    //      bump from racing with a new reader + retire).
    //   2. `retire()` observes `(gen, readers)` in one atomic `seq_cst` load,
    //      pairing with the `seq_cst` exchange in `AtomicSharedPtr::store`.
    static constexpr uint64_t kGenShift   = 20;
    static constexpr uint64_t kReaderMask = (1ULL << kGenShift) - 1;
    static constexpr uint64_t kGenInc     = 1ULL << kGenShift;

    void enterRead() noexcept {
        state_.fetch_add(1, std::memory_order_seq_cst);
    }
    void exitRead() noexcept {
        uint64_t s = state_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t next = ((s & kReaderMask) == 1) ? (s + kGenInc - 1) : (s - 1);
            if (state_.compare_exchange_weak(s, next,
                                             std::memory_order_seq_cst,
                                             std::memory_order_relaxed)) {
                break;
            }
        }
    }
    uint64_t activeReaders() const noexcept {
        return state_.load(std::memory_order_seq_cst) & kReaderMask;
    }
    uint64_t generation() const noexcept {
        return state_.load(std::memory_order_seq_cst) >> kGenShift;
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

    std::atomic<uint64_t> state_{0};
    std::mutex            retireMutex_;
    Retired*              retired_ = nullptr;
};

} // namespace broaudio
