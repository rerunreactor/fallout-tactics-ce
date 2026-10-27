#include "zar.h"

namespace ft {

// ---------------------------------------------------------------------------
// ImageZAR::ReadFromStream @ 006f7d50
//
// Original shape (as an earlier transcription of the decompilation had it):
//     ReadFormatToken(tag, version)
//     if (!(tag == "ZAR" && version > 1 && version < 5)) { error; failbit; return; }
//     ... palette reset ...
//     ReadBytes(&m_width, 8)
//     if (ReadBool()) { ResizeBufferAndStreamReadInts();          // @ 00709a00
//                       if (version > 3) m_constantIndex = ReadBool(); }
//     dataSize = ReadUInt32(); m_data.resize(dataSize); ReadBytes(...)
//
// FIXED vs that transcription: the tag literal is "<zar>", not "ZAR".  It had
// to guess -- the decompilation only had the pointer global PTR_DAT_008b0748 --
// and guessed "ZAR".  Every ZAR resource inside the
// real .spr files begins with the bytes `3c 7a 61 72 3e 00` = "<zar>\0", so the
// literal is the bracketed one, matching the "<sprite>" / "<spranim>" family.
// ---------------------------------------------------------------------------
bool ZarImage::Read(Reader& stream, std::string* errorOut) {
    std::string tag;
    int32_t version = 0;
    stream.FormatToken(&tag, &version);

    const bool recognised = (tag == "<zar>") && version > 1 && version < 5;
    if (!recognised) {
        *errorOut = "Unknown zar file type: tag='" + tag + "'";
        stream.Fail(*errorOut);
        return false;
    }
    m_version = version;

    m_palette.clear();
    m_constantIndex = 0;

    // Two int32s read straight over m_width / m_height (+0x08).
    m_width = stream.I32();
    m_height = stream.I32();
    if (m_width < 0 || m_height < 0 || m_width > (1 << 15) || m_height > (1 << 15)) {
        *errorOut = "implausible zar dimensions";
        stream.Fail(*errorOut);
        return false;
    }

    if (stream.U8() != 0) {
        // ImageZAR::ResizeBufferAndStreamReadInts @ 00709a00: the element count
        // is read from the stream inside the callee, not passed in.
        const uint32_t count = stream.U32();
        if (count > 256) {
            *errorOut = "implausible zar palette size";
            stream.Fail(*errorOut);
            return false;
        }
        m_palette.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            m_palette[i] = stream.U32();
        }
        if (version > 3) {
            m_constantIndex = stream.U8();
        }
    }

    const uint32_t dataSize = stream.U32();
    const uint8_t* payload = stream.Take(dataSize);
    if (payload == nullptr) {
        *errorOut = "zar payload truncated";
        return false;
    }
    m_data.assign(payload, payload + dataSize);
    m_rowOffsets.clear();
    return true;
}

// ---------------------------------------------------------------------------
// ComputeZarRunRowOffsets, image_zar.cpp line 0x281 in the original.
//
// The decompiled loop: push 0, then walk runs.  A run's payload is skipped by
// type (2 -> 2*count bytes, 1 or 3 -> count bytes, 0 -> nothing); `local_10`
// accumulates pixels.  When it reaches the row width exactly, the current byte
// offset is pushed and the accumulator resets; if it *passes* the width the
// original reports "ZAR run went past edge of image" and returns, leaving the
// offset table short.  Both behaviours are reproduced.
// ---------------------------------------------------------------------------
bool ZarImage::EnsureRowOffsets(std::string* errorOut) {
    if (!m_rowOffsets.empty()) {
        return true;
    }
    if (m_width <= 0 || m_height <= 0) {
        return true;
    }
    m_rowOffsets.reserve(static_cast<size_t>(m_height) + 1);
    m_rowOffsets.push_back(0);

    size_t cursor = 0;
    int32_t rowPixels = 0;
    int32_t rowsDone = 0;
    while (rowsDone < m_height) {
        if (cursor >= m_data.size()) {
            *errorOut = "ZAR run data exhausted before the last row";
            return false;
        }
        const uint8_t control = m_data[cursor++];
        const uint32_t type = control & 3u;
        const int32_t count = static_cast<int32_t>(control >> 2);
        if (type == kZarRunAlpha) {
            cursor += static_cast<size_t>(count) * 2;
        } else if (type == kZarRunOpaque || type == kZarRunConstantColor) {
            cursor += static_cast<size_t>(count);
        }
        if (cursor > m_data.size()) {
            *errorOut = "ZAR run payload past end of data";
            return false;
        }
        rowPixels += count;
        if (rowPixels >= m_width) {
            if (rowPixels != m_width) {
                // FormatAndReportError("ZAR run went past edge of image").
                *errorOut = "ZAR run went past edge of image";
                return false;
            }
            m_rowOffsets.push_back(static_cast<int32_t>(cursor));
            rowPixels = 0;
            ++rowsDone;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// ImageZAR::DecodeZarImageTo32Bit @ 006f9a10, specialised to region == the whole
// image (so the `SkipRleRun` prologue that seeks to region->x0 collapses to
// "read the first run of the row").  The per-run bodies, the
// `if (remaining < runLength) runLength = remaining;` clamp and the
// `(entry & 0x00ffffff) | (alpha << 24)` composition are verbatim.
//
// Output is written as RGBA bytes for the PNG encoder rather than as packed
// 0xAARRGGBB Color32, which is the only deliberate divergence.
// ---------------------------------------------------------------------------
bool ZarImage::DecodeToRGBA(const std::vector<Color32>& fallbackPalette,
                            std::vector<uint8_t>* rgbaOut, std::string* errorOut) {
    if (!EnsureRowOffsets(errorOut)) {
        return false;
    }
    const std::vector<Color32>& table = m_palette.empty() ? fallbackPalette : m_palette;

    rgbaOut->assign(static_cast<size_t>(m_width) * static_cast<size_t>(m_height) * 4, 0);
    if (m_width <= 0 || m_height <= 0) {
        return true;
    }
    if (static_cast<int32_t>(m_rowOffsets.size()) < m_height) {
        *errorOut = "row offset table short";
        return false;
    }

    auto entry = [&](uint32_t index) -> Color32 {
        return index < table.size() ? table[index] : 0u;
    };

    for (int32_t y = 0; y < m_height; ++y) {
        uint8_t* dest = rgbaOut->data() + static_cast<size_t>(y) * m_width * 4;
        size_t cursor = static_cast<size_t>(m_rowOffsets[y]);
        int32_t remaining = m_width;

        uint32_t runType = 0;
        int32_t runLength = 0;
        {
            const uint8_t control = m_data[cursor++];
            runType = control & 3u;
            runLength = static_cast<int32_t>(control >> 2);
        }

        for (;;) {
            if (remaining < runLength) {
                runLength = remaining;
            }
            remaining -= runLength;

            if (runType == kZarRunOpaque) {
                for (int32_t n = runLength; n != 0; --n) {
                    const Color32 c = entry(m_data[cursor++]);
                    dest[0] = static_cast<uint8_t>(c >> 16);
                    dest[1] = static_cast<uint8_t>(c >> 8);
                    dest[2] = static_cast<uint8_t>(c);
                    dest[3] = 0xff;
                    dest += 4;
                }
            } else if (runType == kZarRunAlpha) {
                for (int32_t n = runLength; n != 0; --n) {
                    const Color32 c = entry(m_data[cursor]);
                    const uint8_t a = m_data[cursor + 1];
                    cursor += 2;
                    dest[0] = static_cast<uint8_t>(c >> 16);
                    dest[1] = static_cast<uint8_t>(c >> 8);
                    dest[2] = static_cast<uint8_t>(c);
                    dest[3] = a;
                    dest += 4;
                }
            } else if (runType == kZarRunConstantColor) {
                const Color32 c = entry(m_constantIndex);
                for (int32_t n = runLength; n != 0; --n) {
                    const uint8_t a = m_data[cursor++];
                    dest[0] = static_cast<uint8_t>(c >> 16);
                    dest[1] = static_cast<uint8_t>(c >> 8);
                    dest[2] = static_cast<uint8_t>(c);
                    dest[3] = a;
                    dest += 4;
                }
            } else {
                dest += static_cast<size_t>(runLength) * 4;  // already zeroed
            }

            if (remaining == 0) {
                break;
            }
            if (cursor >= m_data.size()) {
                *errorOut = "ZAR row ran out of data";
                return false;
            }
            const uint8_t control = m_data[cursor++];
            runType = control & 3u;
            runLength = static_cast<int32_t>(control >> 2);
        }
    }
    return true;
}

}  // namespace ft
