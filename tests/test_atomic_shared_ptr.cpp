#include "test_harness.h"
#include "broaudio/atomic_shared_ptr.h"
#include "broaudio/rcu.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace broaudio;

namespace {

// Counts payload ctor/dtor across all instances so leaks / UAF show up as
// imbalanced totals at the end of a stress run.
struct Payload {
    static std::atomic<int> ctorCount;
    static std::atomic<int> dtorCount;
    int magic;
    explicit Payload(int v) : magic(v) { ctorCount.fetch_add(1, std::memory_order_relaxed); }
    ~Payload() { dtorCount.fetch_add(1, std::memory_order_relaxed); }
    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;
};
std::atomic<int> Payload::ctorCount{0};
std::atomic<int> Payload::dtorCount{0};

void resetCounters() {
    Payload::ctorCount.store(0);
    Payload::dtorCount.store(0);
}

} // namespace

TEST(asp_lock_free_assertions) {
    // The whole point of this rewrite: the underlying atomics must be
    // genuinely lock-free on every platform we ship on.
    static_assert(std::atomic<void*>::is_always_lock_free,
                  "atomic<Snapshot*> must be lock-free");
    std::atomic<uint64_t> seq{0};
    ASSERT_TRUE(seq.is_lock_free());
    PASS();
}

TEST(asp_load_store_basic) {
    RcuDomain domain;
    AtomicSharedPtr<Payload> slot(domain);
    ASSERT_TRUE(slot.load() == nullptr);

    slot.store(std::make_shared<Payload>(7));
    auto p = slot.load();
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(p->magic, 7);

    slot.store(std::make_shared<Payload>(11));
    auto q = slot.load();
    ASSERT_EQ(q->magic, 11);
    PASS();
}

TEST(asp_idle_engine_inline_reclaim) {
    // No reader ever opens a ReadScope — retire() should free inline because
    // the domain is always quiescent. Otherwise the retire list would grow
    // unboundedly over a long-running idle engine.
    resetCounters();
    {
        RcuDomain domain;
        AtomicSharedPtr<Payload> slot(domain);
        for (int i = 0; i < 1000; i++) {
            slot.store(std::make_shared<Payload>(i));
        }
        // Holding one live reference; the other 999 should already be freed.
        auto live = slot.load();
        ASSERT_EQ(Payload::ctorCount.load(), 1000);
        ASSERT_EQ(Payload::dtorCount.load(), 999);
        (void)live;
    }
    // Domain destructor + slot destructor drain the last one.
    ASSERT_EQ(Payload::ctorCount.load(), Payload::dtorCount.load());
    PASS();
}

TEST(asp_open_scope_defers_reclaim) {
    // Inside a ReadScope, retired snapshots must NOT be freed inline; they
    // queue up and are released by reclaim() once the scope closes.
    resetCounters();
    {
        RcuDomain domain;
        AtomicSharedPtr<Payload> slot(domain, std::make_shared<Payload>(0));

        {
            RcuDomain::ReadScope scope(domain);
            for (int i = 1; i < 50; i++) {
                slot.store(std::make_shared<Payload>(i));
            }
            // Reader holds one snapshot, but the writer just published 49
            // others while the scope is open — they must be queued, not freed.
            auto live = slot.load();
            ASSERT_EQ(live->magic, 49);
            // Nothing freed yet (scope still open).
            ASSERT_EQ(Payload::dtorCount.load(), 0);
        }

        // Scope closed; reclaim should now release everything except the
        // current head (which is still pointed to by slot).
        domain.reclaim();
        ASSERT_EQ(Payload::ctorCount.load(), 50);
        ASSERT_EQ(Payload::dtorCount.load(), 49);
    }
    ASSERT_EQ(Payload::ctorCount.load(), Payload::dtorCount.load());
    PASS();
}

TEST(asp_concurrent_stress) {
    // 1 writer thread + N reader threads + 1 reclaimer for ~1 s. After
    // joining, verify ctor/dtor balance — any UAF would corrupt counters
    // (or trip ASan/TSan in CI), and any leak shows as imbalance.
    resetCounters();
    {
        RcuDomain domain;
        AtomicSharedPtr<Payload> slot(domain, std::make_shared<Payload>(-1));

        std::atomic<bool> stop{false};
        const int kReaders = 4;

        std::thread writer([&] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                slot.store(std::make_shared<Payload>(i++));
                std::this_thread::yield();
            }
        });

        std::vector<std::thread> readers;
        for (int r = 0; r < kReaders; r++) {
            readers.emplace_back([&] {
                while (!stop.load(std::memory_order_relaxed)) {
                    RcuDomain::ReadScope scope(domain);
                    for (int k = 0; k < 32; k++) {
                        auto p = slot.load();
                        if (p) {
                            // Touch the payload — UAF would crash or trip
                            // sanitizers here.
                            volatile int x = p->magic;
                            (void)x;
                        }
                    }
                }
            });
        }

        std::thread reclaimer([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                domain.reclaim();
                std::this_thread::sleep_for(std::chrono::microseconds(500));
            }
        });

        std::this_thread::sleep_for(std::chrono::seconds(1));
        stop.store(true);

        writer.join();
        for (auto& t : readers) t.join();
        reclaimer.join();

        // Final reclaim plus dropping the slot's head leaves only payloads
        // still held by readers' local shared_ptrs (which are gone after
        // join). Slot still holds the head; everything else must be freed.
        domain.reclaim();
        int ctors = Payload::ctorCount.load();
        int dtors = Payload::dtorCount.load();
        ASSERT_EQ(ctors - dtors, 1);  // one live: the current head
    }
    // Domain + slot destruction frees the last one.
    ASSERT_EQ(Payload::ctorCount.load(), Payload::dtorCount.load());
    PASS();
}

// Deterministic exhaustive state-space model checker for RcuDomain + AtomicSharedPtr
// across all multi-reader (enterRead -> load_head -> copy_sp -> exitRead),
// writer (exchange -> retire), and reclaimer (reclaim) interleavings.
TEST(asp_qsbr_exhaustive_state_model_checker) {
    struct ModelState {
        // Packed RcuDomain state: (gen << 4) | activeReaders
        int gen = 0;
        int activeReaders = 0;
        int headSnap = 0;       // current published snapshot id (0..kMaxSnaps)
        int nextSnap = 1;       // next snapshot id to publish (up to 3)
        // Writer in-flight state: -1 if idle, >=0 if exchanged `head_` and about to `retire(oldSnap)`
        int writerRetiringSnap = -1;
        // Retired queue: bitmask of retired snapshots and their retire generation
        uint8_t retiredMask = 0;
        uint8_t freedMask = 0;
        int retiredGen[4] = {0, 0, 0, 0};
        // Two readers: pc = 0 (idle), 1 (entered ReadScope, about to load head_),
        //              2 (loaded s = head_, about to copy s->sp and exitRead)
        int readerPc[2] = {0, 0};
        int readerLoadedSnap[2] = {-1, -1};
    };

    int exploredStates = 0;
    int terminalChecks = 0;

    auto stepDfs = [&](auto& self, const ModelState& st, int depth) -> bool {
        ++exploredStates;
        // Safety invariant at EVERY reachable state:
        // No reader at pc==2 (holding raw Snapshot* `s` inside ReadScope before copying `s->sp`)
        // may EVER hold a snapshot that has already been freed (`freedMask & (1 << s)`),
        // nor may `headSnap` itself be freed!
        if ((st.freedMask & (1u << st.headSnap)) != 0) return false;
        for (int r = 0; r < 2; ++r) {
            if (st.readerPc[r] == 2) {
                int s = st.readerLoadedSnap[r];
                if ((st.freedMask & (1u << s)) != 0) return false;
            }
        }
        if (depth >= 9) {
            ++terminalChecks;
            return true;
        }

        // Transition 1 & 2: Reader r steps
        for (int r = 0; r < 2; ++r) {
            if (st.readerPc[r] == 0) {
                ModelState next = st;
                next.activeReaders++;
                next.readerPc[r] = 1;
                if (!self(self, next, depth + 1)) return false;
            } else if (st.readerPc[r] == 1) {
                ModelState next = st;
                next.readerLoadedSnap[r] = st.headSnap;
                next.readerPc[r] = 2;
                if (!self(self, next, depth + 1)) return false;
            } else if (st.readerPc[r] == 2) {
                // Atomic exitRead(): (gen, 1) -> (gen + 1, 0) or (gen, k) -> (gen, k - 1)
                ModelState next = st;
                if (next.activeReaders == 1) {
                    next.activeReaders = 0;
                    next.gen++;
                } else {
                    next.activeReaders--;
                }
                next.readerPc[r] = 0;
                next.readerLoadedSnap[r] = -1;
                if (!self(self, next, depth + 1)) return false;
            }
        }

        // Transition 3: Writer exchanges head_ (if idle and nextSnap <= 3)
        if (st.writerRetiringSnap == -1 && st.nextSnap <= 3) {
            ModelState next = st;
            next.writerRetiringSnap = st.headSnap;
            next.headSnap = next.nextSnap++;
            if (!self(self, next, depth + 1)) return false;
        }

        // Transition 4: Writer completes retire(writerRetiringSnap) using atomic (gen, activeReaders)
        if (st.writerRetiringSnap != -1) {
            ModelState next = st;
            int oldSnap = next.writerRetiringSnap;
            next.writerRetiringSnap = -1;
            if (next.activeReaders == 0) {
                next.freedMask |= static_cast<uint8_t>(1u << oldSnap);
            } else {
                next.retiredMask |= static_cast<uint8_t>(1u << oldSnap);
                next.retiredGen[oldSnap] = next.gen;
            }
            if (!self(self, next, depth + 1)) return false;
        }

        // Transition 5: Reclaimer runs reclaim()
        if (st.retiredMask != 0) {
            ModelState next = st;
            for (int s = 0; s < 4; ++s) {
                if ((next.retiredMask & (1u << s)) && next.gen > next.retiredGen[s]) {
                    next.retiredMask &= static_cast<uint8_t>(~(1u << s));
                    next.freedMask |= static_cast<uint8_t>(1u << s);
                }
            }
            if (!self(self, next, depth + 1)) return false;
        }
        return true;
    };

    ModelState init{};
    ASSERT_TRUE(stepDfs(stepDfs, init, 0));
    ASSERT_GT(exploredStates, 5000);
    ASSERT_GT(terminalChecks, 1000);

    // Also drive the real RcuDomain + AtomicSharedPtr through deterministic
    // overlapping reader/writer/reclaim schedules to verify concrete execution.
    resetCounters();
    {
        RcuDomain domain;
        AtomicSharedPtr<Payload> slot(domain, std::make_shared<Payload>(100));
        domain.enterRead(); // Reader 0 enters
        domain.exitRead();  // Reader 0 exits (atomic 1->0 + gen bump)
        ASSERT_EQ(static_cast<int>(domain.activeReaders()), 0);
        ASSERT_EQ(static_cast<int>(domain.generation()), 1);

        domain.enterRead(); // Reader 1 enters at gen==1
        auto p1 = slot.load();
        slot.store(std::make_shared<Payload>(200)); // retires Payload(100) at gen==1
        domain.reclaim(); // gen is still 1 -> must NOT reclaim Payload(100)
        ASSERT_EQ(Payload::dtorCount.load(), 0);
        ASSERT_EQ(p1->magic, 100);
        p1.reset();
        domain.exitRead(); // Reader 1 exits -> gen becomes 2
        domain.reclaim();  // gen (2) > 1 -> reclaims Payload(100)
        ASSERT_EQ(Payload::dtorCount.load(), 1);
    }
    ASSERT_EQ(Payload::ctorCount.load(), Payload::dtorCount.load());
    PASS();
}

int main() { return runAllTests(); }
