#include "byte_source.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace broaudio {

FILE* openFileUtf8(const char* path, const char* mode)
{
    if (!path || !*path) return nullptr;
#ifdef _WIN32
    auto widen = [](const char* s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
        std::wstring w(n > 0 ? static_cast<size_t>(n) : 0, L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
        if (!w.empty()) w.pop_back();   // the terminator
        return w;
    };
    std::wstring wpath = widen(path), wmode = widen(mode);
    FILE* f = _wfopen(wpath.c_str(), wmode.c_str());
    if (!f) f = fopen(path, mode);       // a path in the ANSI code page
    return f;
#else
    return fopen(path, mode);
#endif
}

static bool seekTo(FILE* f, uint64_t at)
{
#ifdef _WIN32
    return _fseeki64(f, static_cast<__int64>(at), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(at), SEEK_SET) == 0;
#endif
}

ByteSource::~ByteSource() { close(); }

bool ByteSource::openFile(const char* path)
{
    close();
    FILE* f = openFileUtf8(path, "rb");
    if (!f) return false;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    const long long end = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    const long long end = static_cast<long long>(ftello(f));
#endif
    if (end < 0) { fclose(f); return false; }
    file_ = f;
    size_ = static_cast<uint64_t>(end);
    return true;
}

void ByteSource::openMemory(const uint8_t* data, size_t size)
{
    close();
    mem_ = data;
    size_ = data ? size : 0;
}

void ByteSource::close()
{
    if (file_) fclose(file_);
    file_ = nullptr;
    mem_ = nullptr;
    size_ = 0;
    blockLen_ = 0;
    blockAt_ = 0;
}

size_t ByteSource::read(uint64_t offset, uint8_t* dst, size_t n)
{
    if (!dst || n == 0 || offset >= size_) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
    if (mem_) {
        std::memcpy(dst, mem_ + offset, n);
        return n;
    }
    if (!file_) return 0;

    // Small reads go through the block; big ones straight to the file.
    if (n >= kBlock) {
        if (!seekTo(file_, offset)) return 0;
        return fread(dst, 1, n, file_);
    }
    size_t done = 0;
    while (done < n) {
        const uint64_t at = offset + done;
        if (!(blockLen_ && at >= blockAt_ && at < blockAt_ + blockLen_)) {
            if (block_.size() != kBlock) block_.resize(kBlock);
            blockAt_ = at;
            blockLen_ = 0;
            if (!seekTo(file_, at)) break;
            blockLen_ = fread(block_.data(), 1, kBlock, file_);
            if (blockLen_ == 0) break;
        }
        const size_t in = static_cast<size_t>(at - blockAt_);
        const size_t take = std::min(n - done, blockLen_ - in);
        std::memcpy(dst + done, block_.data() + in, take);
        done += take;
    }
    return done;
}

std::vector<uint8_t> ByteSource::read(uint64_t offset, size_t n)
{
    std::vector<uint8_t> out;
    if (offset >= size_) return out;
    n = static_cast<size_t>(std::min<uint64_t>(n, size_ - offset));
    out.resize(n);
    out.resize(read(offset, out.data(), n));
    return out;
}

} // namespace broaudio
