#include "broaudio/rcu.h"

namespace broaudio {

RcuDomain::~RcuDomain()
{
    // Drain any remaining retired nodes. By the time the domain is
    // destroyed, the engine has stopped its audio device and no readers
    // can race; freeing unconditionally is safe.
    Retired* head = retired_;
    retired_ = nullptr;
    while (head) {
        Retired* next = head->next;
        head->deleter(head->node);
        delete head;
        head = next;
    }
}

void RcuDomain::retire(void* node, Deleter deleter) noexcept
{
    if (!node) return;

    // Sample both activeReaders and generation in a single seq_cst load of
    // state_. If no reader is currently in a scope, no one can be holding any
    // prior snapshot — the seq_cst on enterRead and on the writer's exchange
    // that preceded this retire ensures any reader that enters later sees the
    // new head, not the one we're retiring.
    const uint64_t s = state_.load(std::memory_order_seq_cst);
    if ((s & kReaderMask) == 0) {
        deleter(node);
        return;
    }

    // Slow path: at least one reader is in a scope at this instant. Queue with
    // the generation from the same atomic snapshot `s`; reclaim() will free it
    // once generation has advanced (which happens atomically when activeReaders
    // transitions 1 -> 0).
    const uint64_t gen = s >> kGenShift;
    auto* r = new Retired{deleter, node, gen, nullptr};
    std::lock_guard<std::mutex> lock(retireMutex_);
    r->next = retired_;
    retired_ = r;
}

void RcuDomain::reclaim() noexcept
{
    std::lock_guard<std::mutex> lock(retireMutex_);
    Retired** prev = &retired_;
    Retired* cur = retired_;
    uint64_t gen = generation();
    while (cur) {
        if (gen > cur->gen) {
            *prev = cur->next;
            cur->deleter(cur->node);
            Retired* dead = cur;
            cur = cur->next;
            delete dead;
        } else {
            prev = &cur->next;
            cur = cur->next;
        }
    }
}

} // namespace broaudio
