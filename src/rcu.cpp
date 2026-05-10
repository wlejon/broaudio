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

    // Inline-free fast path: if no reader is currently in a scope, no one
    // can be holding any prior snapshot — the seq_cst on enterRead and on
    // the writer's exchange that preceded this retire ensures any reader
    // that enters now sees the new head, not the one we're retiring.
    if (activeReaders() == 0) {
        deleter(node);
        return;
    }

    // Slow path: at least one reader is in a scope. Queue with the current
    // generation; reclaim() will free it once generation has advanced
    // (i.e., the reader count has transitioned to zero, proving all
    // readers in flight right now have exited).
    uint64_t gen = generation();
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
