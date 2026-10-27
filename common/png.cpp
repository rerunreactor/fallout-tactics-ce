// png.cpp -- minimal RGBA PNG encoder (zlib only, no image library).

#include "png.h"

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace ft {
namespace {

void PutBE32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>(v >> 24));
    out->push_back(static_cast<uint8_t>(v >> 16));
    out->push_back(static_cast<uint8_t>(v >> 8));
    out->push_back(static_cast<uint8_t>(v));
}

void Chunk(std::vector<uint8_t>* out, const char type[4], const std::vector<uint8_t>& body) {
    PutBE32(out, static_cast<uint32_t>(body.size()));
    const size_t crcStart = out->size();
    out->insert(out->end(), type, type + 4);
    out->insert(out->end(), body.begin(), body.end());
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), out->data() + crcStart,
                            static_cast<uInt>(out->size() - crcStart));
    PutBE32(out, static_cast<uint32_t>(crc));
}

}  // namespace

bool WritePng(const std::string& path, int32_t width, int32_t height,
              const std::vector<uint8_t>& rgba, std::string* errorOut) {
    if (width <= 0 || height <= 0) {
        *errorOut = "zero-sized image";
        return false;
    }
    if (rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        *errorOut = "pixel buffer size mismatch";
        return false;
    }

    // Filter 0 (None) on every scanline.
    std::vector<uint8_t> raw;
    raw.reserve((static_cast<size_t>(width) * 4 + 1) * static_cast<size_t>(height));
    for (int32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgba.data() + static_cast<size_t>(y) * width * 4;
        raw.insert(raw.end(), row, row + static_cast<size_t>(width) * 4);
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> compressed(compressedSize);
    if (compress2(compressed.data(), &compressedSize, raw.data(),
                  static_cast<uLong>(raw.size()), 6) != Z_OK) {
        *errorOut = "zlib compress2 failed";
        return false;
    }
    compressed.resize(compressedSize);

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

    std::vector<uint8_t> ihdr;
    PutBE32(&ihdr, static_cast<uint32_t>(width));
    PutBE32(&ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type: RGBA
    ihdr.push_back(0);  // deflate
    ihdr.push_back(0);  // adaptive filtering
    ihdr.push_back(0);  // no interlace
    Chunk(&png, "IHDR", ihdr);
    Chunk(&png, "IDAT", compressed);
    Chunk(&png, "IEND", std::vector<uint8_t>());

    FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        *errorOut = "cannot open " + path + " for writing";
        return false;
    }
    const size_t wrote = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    if (wrote != png.size()) {
        *errorOut = "short write on " + path;
        return false;
    }
    return true;
}

}  // namespace ft
