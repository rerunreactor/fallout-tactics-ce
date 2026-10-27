// iso.h -- the world-to-screen transform, from the Ghidra export.
//
// ComputeIsoScreenOffset @ 0044de60:
//
//     a  = -(DAT_008be044 * x)          // DAT_008be044 == 3.0f
//     b  = -(DAT_008be044 * z)
//     h  = y * _DAT_008be040            // _DAT_008be040 == 7.0f
//     out[0] = round((b + b) - (a + a))
//     out[1] = round(a + b - h)
//
// which is, with the two constants substituted,
//
//     screenX =  6 * (x - z)
//     screenY = -3 * (x + z) - 7 * y            (y is elevation, up on screen)
//
// The two float globals are not resolved by the decompiler.  They are pinned to
// 3.0 and 7.0 by FreeFT's independently reverse-engineered matrix (src/base.cpp
// worldToScreen: `6 * (x - z), 3 * (x + z) - 7 * y`), whose sign difference on
// the x+z term is only FreeFT's axis flip in TileMap::legacyConvert
// (`pos.x = -pos.x; pos.z = -pos.z`).  Rendering a real map with these values
// produces seamless floors, which is the empirical confirmation.
//
// The third row of the same matrix (FreeFT's `|7 6 7|` column) is the depth
// used for the painter's sort; in this file's axis convention it is
//
//     depth = 6 * y - 7 * (x + z)
//
// ascending == back to front.  MapRegion::LoadRegionFromStream ends with
// std::IntrosortEntityPointersByField55, an ascending sort of the region's tile
// list by the int at entity+0x55, which is the last of the six ints at
// +0x41..+0x58 that RenderMapToImageFiles mins/maxes to size the output image
// (it uses +0x41/+0x4d for x and +0x45/+0x51 for y, leaving +0x49/+0x55 as the
// depth pair).  See NOTES.md.

#ifndef MAPVIEW_ISO_H
#define MAPVIEW_ISO_H

#include <cstdint>

namespace ft {

constexpr int kIsoHalfWidth = 3;   // DAT_008be044
constexpr int kIsoHeight = 7;      // _DAT_008be040

inline void ProjectWorldToScreen(int32_t x, int32_t y, int32_t z, int32_t* sx, int32_t* sy) {
    *sx = 2 * kIsoHalfWidth * (x - z);
    *sy = -kIsoHalfWidth * (x + z) - kIsoHeight * y;
}

inline int64_t IsoDepth(int32_t x, int32_t y, int32_t z) {
    return 2LL * kIsoHalfWidth * y - static_cast<int64_t>(kIsoHeight) * (x + z);
}

}  // namespace ft

#endif  // MAPVIEW_ISO_H
