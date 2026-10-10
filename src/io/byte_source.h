#pragma once

// A random-access byte source over a file or a memory span: the tag reader
// and the MP4 demuxer read through it, a few bytes at an offset at a time, so
// neither ever holds a whole file. Not thread-safe (one owner).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace broaudio {

class ByteSource {
public:
    ByteSource() = default;
    ~ByteSource();
    ByteSource(const ByteSource&) = delete;
    ByteSource& operator=(const ByteSource&) = delete;

    // Open a file (UTF-8 path) for reading. Returns false when it cannot be
    // opened or its size cannot be found.
    bool openFile(const char* path);
    // Read from memory; the span must outlive the source.
    void openMemory(const uint8_t* data, size_t size);
    void close();

    bool isOpen() const { return file_ != nullptr || mem_ != nullptr; }
    uint64_t size() const { return size_; }

    // Copy up to n bytes at `offset` into dst; returns how many were read
    // (fewer at the end of the source, 0 past it).
    size_t read(uint64_t offset, uint8_t* dst, size_t n);

    // The same into a vector, shorter at the end of the source.
    std::vector<uint8_t> read(uint64_t offset, size_t n);

private:
    FILE* file_ = nullptr;
    const uint8_t* mem_ = nullptr;
    uint64_t size_ = 0;
    // The last block read from the file: the MP4 demuxer reads its boxes a
    // byte at a time, and this keeps that from being a syscall per byte.
    static constexpr size_t kBlock = 64 * 1024;
    std::vector<uint8_t> block_;
    uint64_t blockAt_ = 0;
    size_t blockLen_ = 0;
};

// fopen for a UTF-8 path (wide on Windows).
FILE* openFileUtf8(const char* path, const char* mode);

} // namespace broaudio
