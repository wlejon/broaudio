// A minimal PNG writer for the ear's spectrograms: 8-bit RGBA, filter 0,
// zlib stored (uncompressed) deflate blocks. Exact and dependency-free; a
// host with a real encoder (bro.image) is better at size.

#include "broaudio/ear/ear.h"

#include <algorithm>
#include <array>
#include <fstream>

namespace broaudio::ear {

namespace {

const std::array<uint32_t, 256>& crcTable() {
    static const std::array<uint32_t, 256> t = [] {
        std::array<uint32_t, 256> a{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            a[n] = c;
        }
        return a;
    }();
    return t;
}

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t c = 0xFFFFFFFFu;
    const auto& t = crcTable();
    for (size_t i = start; i < out.size(); ++i) c = t[(c ^ out[i]) & 0xFFu] ^ (c >> 8);
    put32(out, c ^ 0xFFFFFFFFu);
}

} // namespace

std::vector<uint8_t> encodePng(const uint8_t* rgba, int width, int height) {
    std::vector<uint8_t> out;
    if (!rgba || width <= 0 || height <= 0) return out;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(width));
    put32(ihdr, static_cast<uint32_t>(height));
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA, deflate, filter set 0, no interlace
    chunk(out, "IHDR", ihdr);

    const size_t rowBytes = static_cast<size_t>(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((rowBytes + 1) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba + rowBytes * static_cast<size_t>(y);
        raw.insert(raw.end(), row, row + rowBytes);
    }
    std::vector<uint8_t> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    size_t pos = 0;
    do {
        const size_t len = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + len >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<uint8_t>(len));
        z.push_back(static_cast<uint8_t>(len >> 8));
        z.push_back(static_cast<uint8_t>(~len));
        z.push_back(static_cast<uint8_t>(~len >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + len));
        pos += len;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z, (b << 16) | a);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});
    return out;
}

bool writePng(const std::string& path, const uint8_t* rgba, int width, int height) {
    const std::vector<uint8_t> bytes = encodePng(rgba, width, height);
    if (bytes.empty()) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return !f.fail();
}

} // namespace broaudio::ear
