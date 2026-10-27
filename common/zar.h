// zar.h -- ImageZAR's on-disk format and decoder, reduced to what a viewer needs.
//
// Ported from an earlier transcription of ImageZAR's decompiled methods, plus
// `ComputeZarRunRowOffsets` (@ 00709??), which that transcription had left as
// a stub.

#ifndef FT_ZAR_H
#define FT_ZAR_H

#include <cstdint>
#include <string>
#include <vector>

#include "stream.h"

namespace ft {

// Run control byte:  type = byte & 3,  count = byte >> 2  (image_zar.h).
enum ZarRunType : uint32_t {
    kZarRunSkip = 0,           // transparent, no payload
    kZarRunOpaque = 1,         // `count` palette indices, alpha implied 0xff
    kZarRunAlpha = 2,          // `count` (index, alpha) pairs
    kZarRunConstantColor = 3,  // `count` alpha bytes, index = m_constantIndex
};

// Packed colours are 0xAARRGGBB, i.e. B,G,R,A in memory (ZAR notes §1).
using Color32 = uint32_t;

class ZarImage {
public:
    // ImageZAR::ReadFromStream @ 006f7d50.  Reads one "<zar>" resource.
    bool Read(Reader& stream, std::string* errorOut);

    // ComputeZarRunRowOffsets @ image_zar.cpp:0x281.  Fills m_rowOffsets with
    // the byte offset of each scanline's first run; height + 1 entries.
    bool EnsureRowOffsets(std::string* errorOut);

    // ImageZAR::DecodeZarImageTo32Bit @ 006f9a10, specialised to the whole image.
    // `fallbackPalette` is used when the resource carries no palette of its own
    // (which, in a .spr, is always -- the palette lives in the collection).
    bool DecodeToRGBA(const std::vector<Color32>& fallbackPalette,
                      std::vector<uint8_t>* rgbaOut, std::string* errorOut);

    int32_t Width() const { return m_width; }
    int32_t Height() const { return m_height; }
    uint8_t ConstantIndex() const { return m_constantIndex; }
    bool HasOwnPalette() const { return !m_palette.empty(); }
    size_t PaletteSize() const { return m_palette.size(); }
    size_t DataSize() const { return m_data.size(); }
    int32_t Version() const { return m_version; }

private:
    int32_t m_version = 0;
    int32_t m_width = 0;               // Image +0x08
    int32_t m_height = 0;              // Image +0x0c
    uint8_t m_constantIndex = 0;       // ImageZAR +0x3b
    std::vector<Color32> m_palette;    // Image::m_palette->entries
    std::vector<uint8_t> m_data;       // ImageZAR +0x4c
    std::vector<int32_t> m_rowOffsets; // ImageZAR +0x3c
};

}  // namespace ft

#endif  // FT_ZAR_H
