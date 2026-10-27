#include "til.h"

#include <cstdio>

namespace ft {


// ---------------------------------------------------------------------------
// Tile::ReadTileFileHeader @ 006f04c0.
//
// The accepted version range is the literal `5 < v && v < 0xb` of the original,
// i.e. 6..10.  StreamReadBytesWithPutback(this+0x12, 3) / (+0x15, 8) /
// (+0x1d, 8) are the three fixed reads; FUN_006f1a20 is "read one byte",
// EntityStreamReadShort is "read two".  The flag tail is the original's
// cascade, quirks intact:
//
//     if (v < 9)  read and discard one byte
//     if (v < 8) { flags.bit0 = readByte() & 1;
//                  if (v < 7) read and discard one byte;
//                  flags.bit1 = readByte() & 1; }
//     else if (v < 10) flags = readByte();
//     else             flags = readShort();
// ---------------------------------------------------------------------------
bool TileHeader::Read(Reader& stream, std::string* errorOut) {
    std::string tag;
    int32_t v = 0;
    stream.FormatToken(&tag, &v);
    if (tag != "<tile>" || v < 6 || v > 10) {
        *errorOut = "Unknown tile file type: tag='" + tag + "' version=" + std::to_string(v);
        stream.Fail(*errorOut);
        return false;
    }
    version = v;

    // MILESTONE 15.  These three bytes are the world extents in the order
    // (x, y, z), NOT (z, y, x): over mission01's 17,670 placements, every one
    // of the 2,772 whose prototype has bbox.x != bbox.z has
    // `max - min == (byte0, byte1, byte2)`, and none matches the reverse.
    // (Section 1's check could not see it: it counted the symmetric tiles too,
    // and those match either way.)  Nothing depended on the old labelling --
    // section 24.6's roof gate multiplies bbox.x by bbox.z, which is the same
    // either way -- but `ClassifyTileShape` does, because class 1 and class 3
    // are the two wall orientations.
    bboxX = stream.U8();
    bboxY = stream.U8();
    bboxZ = stream.U8();
    offsetX = stream.I32();
    offsetY = stream.I32();
    sizeX = stream.I32();
    sizeY = stream.I32();
    typeId = stream.U8();
    material = stream.U8();

    flags = 0;
    if (v < 9) {
        stream.U8();
    }
    if (v < 8) {
        flags = static_cast<uint16_t>((flags & 0xfffe) | (stream.U8() & 1));
        if (v < 7) {
            stream.U8();
        }
        flags = static_cast<uint16_t>((flags & 0xfffd) | ((stream.U8() & 1) << 1));
    } else if (v < 10) {
        flags = stream.U8();
    } else {
        flags = static_cast<uint16_t>(stream.I16());
    }

    if (stream.Failed()) {
        *errorOut = "truncated <tile> header";
        return false;
    }
    return true;
}

bool TileHeader::ReadV10Payload(Reader& stream) {
    version = 10;
    // MILESTONE 15.  These three bytes are the world extents in the order
    // (x, y, z), NOT (z, y, x): over mission01's 17,670 placements, every one
    // of the 2,772 whose prototype has bbox.x != bbox.z has
    // `max - min == (byte0, byte1, byte2)`, and none matches the reverse.
    // (Section 1's check could not see it: it counted the symmetric tiles too,
    // and those match either way.)  Nothing depended on the old labelling --
    // section 24.6's roof gate multiplies bbox.x by bbox.z, which is the same
    // either way -- but `ClassifyTileShape` does, because class 1 and class 3
    // are the two wall orientations.
    bboxX = stream.U8();
    bboxY = stream.U8();
    bboxZ = stream.U8();
    offsetX = stream.I32();
    offsetY = stream.I32();
    sizeX = stream.I32();
    sizeY = stream.I32();
    typeId = stream.U8();
    material = stream.U8();
    flags = static_cast<uint16_t>(stream.I16());
    return !stream.Failed();
}

// ---------------------------------------------------------------------------
// ParseTileDataAndLoadImages @ 006f0740.
//
//     token "<tiledata>", version must satisfy 0 < v < 2, i.e. exactly 1
//     count = readUInt32()                          (FUN_006f1c00)
//     if (count == 0) return                        (the early-out branch)
//     resize image vector to count, rect vector to count
//     for i in [0, count):
//         images[i] = new ImageZAR; images[i]->vtbl[0x78](stream)   // ZAR read
//         readBytes(&rects[i], 8)                                   // 2 int32
//     m_palette = new Palette; ImageZAR::ResizeBufferAndStreamReadInts(stream)
//
// The last call is the same routine the ZAR reader uses for its own palette:
// an int32 count followed by that many packed 0xAARRGGBB entries.
// ---------------------------------------------------------------------------
bool Tile::Load(const std::vector<uint8_t>& file, std::string* errorOut) {
    Reader stream(file);
    if (!header.Read(stream, errorOut)) {
        return false;
    }

    std::string tag;
    int32_t v = 0;
    stream.FormatToken(&tag, &v);
    if (tag != "<tiledata>" || v != 1) {
        *errorOut = "Unknown tiledata file type: tag='" + tag + "'";
        stream.Fail(*errorOut);
        return false;
    }

    const uint32_t count = stream.U32();
    if (stream.Failed()) {
        *errorOut = "truncated <tiledata>";
        return false;
    }
    if (count == 0) {
        return true;  // the original's `if (local_24 == 0)` early-out
    }
    if (count > 4096) {
        *errorOut = "implausible tiledata frame count " + std::to_string(count);
        return false;
    }

    frames.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        if (!frames[i].zar.Read(stream, errorOut)) {
            *errorOut = "frame " + std::to_string(i) + ": " + *errorOut;
            return false;
        }
        frames[i].offsetX = stream.I32();
        frames[i].offsetY = stream.I32();
    }

    const uint32_t paletteCount = stream.U32();
    if (stream.Failed() || paletteCount > 256) {
        *errorOut = "bad trailing palette";
        return false;
    }
    palette.resize(paletteCount);
    for (uint32_t i = 0; i < paletteCount; ++i) {
        palette[i] = stream.U32();
    }
    if (stream.Failed()) {
        *errorOut = "truncated trailing palette";
        return false;
    }
    return true;
}

bool Tile::DecodeFrame(size_t index, std::vector<uint8_t>* rgbaOut, int32_t* wOut, int32_t* hOut,
                       std::string* errorOut) {
    if (index >= frames.size()) {
        *errorOut = "frame index out of range";
        return false;
    }
    ZarImage& zar = frames[index].zar;
    *wOut = zar.Width();
    *hOut = zar.Height();
    return zar.DecodeToRGBA(palette, rgbaOut, errorOut);
}

}  // namespace ft
