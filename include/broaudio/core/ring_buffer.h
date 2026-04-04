#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <algorithm>
#include <vector>

namespace broaudio {

// Lock-free SPSC ring buffer with cache-line-aligned indices.
// Ported from talkie-qt's LockFreeRingBuffer.
template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t size)
        : size_(nextPowerOfTwo(size))
        , mask_(size_ - 1)
        , buffer_(std::make_unique<T[]>(size_))
    {
        std::memset(buffer_.get(), 0, size_ * sizeof(T));
    }

    bool push(const T& item) {
        const size_t w = writeIndex_.load(std::memory_order_relaxed);
        const size_t next = (w + 1) & mask_;
        if (next == readIndex_.load(std::memory_order_acquire))
            return false;
        buffer_[w] = item;
        writeIndex_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t r = readIndex_.load(std::memory_order_relaxed);
        if (r == writeIndex_.load(std::memory_order_acquire))
            return false;
        item = buffer_[r];
        readIndex_.store((r + 1) & mask_, std::memory_order_release);
        return true;
    }

    size_t push(const T* items, size_t count) {
        const size_t w = writeIndex_.load(std::memory_order_relaxed);
        const size_t r = readIndex_.load(std::memory_order_acquire);
        size_t available = (r - w - 1) & mask_;
        size_t toWrite = std::min(count, available);

        size_t firstPart = std::min(toWrite, size_ - w);
        std::memcpy(&buffer_[w], items, firstPart * sizeof(T));
        if (toWrite > firstPart)
            std::memcpy(&buffer_[0], items + firstPart, (toWrite - firstPart) * sizeof(T));

        writeIndex_.store((w + toWrite) & mask_, std::memory_order_release);
        return toWrite;
    }

    size_t pop(T* items, size_t count) {
        const size_t r = readIndex_.load(std::memory_order_relaxed);
        const size_t w = writeIndex_.load(std::memory_order_acquire);
        size_t available = (w - r) & mask_;
        size_t toRead = std::min(count, available);

        size_t firstPart = std::min(toRead, size_ - r);
        std::memcpy(items, &buffer_[r], firstPart * sizeof(T));
        if (toRead > firstPart)
            std::memcpy(items + firstPart, &buffer_[0], (toRead - firstPart) * sizeof(T));

        readIndex_.store((r + toRead) & mask_, std::memory_order_release);
        return toRead;
    }

    size_t availableRead() const {
        return (writeIndex_.load(std::memory_order_acquire) -
                readIndex_.load(std::memory_order_relaxed)) & mask_;
    }

    size_t availableWrite() const {
        return (readIndex_.load(std::memory_order_acquire) -
                writeIndex_.load(std::memory_order_relaxed) - 1) & mask_;
    }

    bool empty() const {
        return readIndex_.load(std::memory_order_relaxed) ==
               writeIndex_.load(std::memory_order_acquire);
    }

    void reset() {
        readIndex_.store(0, std::memory_order_relaxed);
        writeIndex_.store(0, std::memory_order_release);
    }

private:
    static size_t nextPowerOfTwo(size_t n) {
        n--;
        n |= n >> 1; n |= n >> 2; n |= n >> 4;
        n |= n >> 8; n |= n >> 16;
        if constexpr (sizeof(size_t) == 8) n |= n >> 32;
        return n + 1;
    }

    const size_t size_;
    const size_t mask_;
    std::unique_ptr<T[]> buffer_;
    alignas(64) std::atomic<size_t> writeIndex_{0};
    alignas(64) std::atomic<size_t> readIndex_{0};
};

// Simple analysis ring buffer (single-writer, not lock-free — for waveform display etc.)
class AnalysisBuffer {
public:
    explicit AnalysisBuffer(int capacity = 8192)
        : data_(capacity, 0.0f), capacity_(capacity) {}

    void write(const float* src, int count) {
        for (int i = 0; i < count; i++) {
            data_[writePos_ % capacity_] = src[i];
            writePos_++;
        }
    }

    void readLatest(float* dst, int count) const {
        int start = static_cast<int>(writePos_) - count;
        if (start < 0) start = 0;
        for (int i = 0; i < count; i++)
            dst[i] = data_[(start + i) % capacity_];
    }

    int capacity() const { return capacity_; }

private:
    std::vector<float> data_;
    int capacity_;
    uint64_t writePos_ = 0;
};

} // namespace broaudio
