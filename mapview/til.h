// til.h -- the .til tile container, as established from the Ghidra export.
//
//   Tile::ReadTileFileHeader        @ 006f04c0  (phoenix/map/tile.cpp)
//   ParseTileDataAndLoadImages      @ 006f0740  (phoenix/map/tile.cpp)
//
// A .til is one "<tile>" header record followed by one "<tiledata>" block of
// ZAR frames and a trailing shared palette.  The very same header record --
// byte for byte, always at version 10 -- is also embedded in a .mis inside
// <mapManager>, which is why TileHeader is shared by til.cpp and mis.cpp.

#ifndef MAPVIEW_TIL_H
#define MAPVIEW_TIL_H

#include <cstdint>
#include <string>
#include <vector>

#include "stream.h"
#include "file.h"
#include "zar.h"

namespace ft {

// Tile::ReadTileFileHeader, fields named by their object offsets.
struct TileHeader {
    int32_t version = 0;

    // +0x12, +0x13, +0x14, read in that order and holding the world extents
    // (x, y, z) in that same order -- see til.cpp and NOTES.md section 27b.
    // `ClassifyByteTripleRelation @006f02d0` is handed this triple as
    // `Tile + 0x12`, so its param_1[0] is bbox.x.
    uint8_t bboxX = 0, bboxY = 0, bboxZ = 0;

    int32_t offsetX = 0, offsetY = 0;  // +0x15, +0x19
    int32_t sizeX = 0, sizeY = 0;      // +0x1d, +0x21 -- frame 0's pixel size
    uint8_t typeId = 0;                // +0x25
    uint8_t material = 0;              // +0x26
    uint16_t flags = 0;                // +0x08

    // Reads the "<tile>" token and everything after it.  Returns false and
    // sets the stream's failbit exactly where the original does.
    bool Read(Reader& stream, std::string* errorOut);

    // The 23 payload bytes of a version-10 record, with no "<tile>" token.
    // Used by the <mapManager> prototype table, which writes the token itself.
    bool ReadV10Payload(Reader& stream);
};

struct TileFrame {
    ZarImage zar;
    int32_t offsetX = 0, offsetY = 0;  // the 8 bytes that follow each ZAR
};

struct Tile {
    TileHeader header;
    std::vector<TileFrame> frames;
    std::vector<Color32> palette;  // the trailing shared palette

    bool Load(const std::vector<uint8_t>& file, std::string* errorOut);

    // Decodes frame `index` into RGBA.  Frames carry their own palette in a
    // .til, so the trailing one is only a fallback.
    bool DecodeFrame(size_t index, std::vector<uint8_t>* rgbaOut, int32_t* wOut,
                     int32_t* hOut, std::string* errorOut);
};

// ReadWholeFile is declared in ../common/file.h, included above.  It used to
// be declared here and defined once per binary; milestone 3 links spr.o into
// mapview, so the two definitions would now collide.

}  // namespace ft

#endif  // MAPVIEW_TIL_H
