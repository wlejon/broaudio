#pragma once
// Minimal std::atomic<std::shared_ptr<T>> replacement that works on any
// C++20 library. libstdc++ and MSVC STL implement the C++20 specialization,
// but libc++ has not landed P0718R2 as of LLVM 22, so direct use of
// std::atomic<std::shared_ptr<T>> fails to compile.
//
// Uses the pre-C++20 free-function atomic_load/atomic_store for shared_ptr,
// which are still supported on all three standard libraries (deprecated in
// C++20 but not removed). Semantics match the specialization we need here:
// thread-safe publish/read of an immutable shared_ptr.

#include <atomic>
#include <memory>

namespace broaudio {

// Returns true if shared_ptr atomic ops on this platform are lock-free.
// On libstdc++ and MSVC STL the free-function atomic_load/atomic_store for
// shared_ptr typically dispatch to a hidden mutex pool, which is not safe
// for the audio thread. Engines should warn at init when this returns false.
inline bool sharedPtrAtomicsAreLockFree()
{
    static const bool lf = []() {
        std::shared_ptr<int> probe;
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        bool result = std::atomic_is_lock_free(&probe);
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
        return result;
    }();
    return lf;
}

template <class T>
class AtomicSharedPtr {
public:
    AtomicSharedPtr() = default;
    AtomicSharedPtr(std::shared_ptr<T> p) : p_(std::move(p)) {}

    std::shared_ptr<T> load(std::memory_order = std::memory_order_seq_cst) const noexcept {
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        return std::atomic_load(&p_);
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    }

    void store(std::shared_ptr<T> x,
               std::memory_order = std::memory_order_seq_cst) noexcept {
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        std::atomic_store(&p_, std::move(x));
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    }

private:
    std::shared_ptr<T> p_;
};

} // namespace broaudio
