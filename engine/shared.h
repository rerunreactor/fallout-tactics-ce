// engine/shared.h -- the pieces mapview and fotgame both need.
//
// MILESTONE 7, STEP 0.  `game/NOTES.md` section 8 listed fourteen things that
// had been copied out of `mapview/main.cpp`'s anonymous namespace into
// `game/scene.cpp` and `game/main.cpp`.  They are all here now, once, and both
// tools include this header instead of carrying their own copy.  Nothing about
// any of them changed in the move; the comments that explain where each came
// from are the originals, trimmed to the part that names the source function
// and the decompilation line.
//
// The sources, for the record:
//   Lower / ParseInts / IsEditorMarkerClass          mapview/main.cpp:30, 56, 120
//   ColourFloatToByte, kLuma*, DrawTint,
//     ApplyPaletteColorAdjustments                   ApplyPaletteColorAdjustments
//                                                    @00708e70
//   kEntityHalfExtent / kEntityHeight                mapview/main.cpp:246
//   DecodedTile / LoadedSprite / SpriteCache         mapview/main.cpp:45, 250, 257
//   LightGet / BuildSceneLight                       mapview/main.cpp:624
//   TileInstanceInvisible / TileInstanceRoofHidden   ComputeElementScreenBoundsAndHighlight
//                                                    @006e2c60
//   EntityWorldBox / BuildLayerTints                 mapview/main.cpp:930-1036,
//                                                    SoftwareRender::RenderLitObject
//                                                    @00783340
//   LightQuad / BuildLightQuad / BlitRGBA            DirectX7Render::
//                                                    RenderTrianglesWithVertexLighting
//                                                    @0077a6f0
//   GameDepth                                        mapview/main.cpp:1416, sort.h

#ifndef ENGINE_SHARED_H
#define ENGINE_SHARED_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "ent.h"
#include "esh.h"
#include "iso.h"
#include "light.h"
#include "mis.h"

namespace ft {

// ---------------------------------------------------------------------------
// Small utilities.
// ---------------------------------------------------------------------------
std::string Lower(std::string s);

// "1,2" / "1,2,3" -> n integers.  Returns false when a field is not a number
// or a comma is missing; trailing junk is ignored, as in both originals.
bool ParseInts(const char* s, int n, int64_t* out);

// Entities the editor places but the game does not draw.  `Entity::RenderObjectMaybe`
// skips an entity whose virtual IsHidden() returns true (`Entity::RenderObjectMaybe@005e2080`
// l.14755) and IsHidden is vtable slot 238, but no per-class override is in the
// supplied decompilation, so *which* classes return true is not recoverable
// from it.  These three are chosen from the data; see mapview/NOTES.md 19b.
inline bool IsEditorMarkerClass(const std::string& className) {
    return className == "WayPoint" || className == "Light" || className == "SpawnPoint";
}

// ---------------------------------------------------------------------------
// Colour: the tint pipeline (mapview/NOTES.md 17, 24.4).
// ---------------------------------------------------------------------------

// `SoftwareRender::BlitSpriteWithLighting@00782530`: ROUND(f * 255) with the 0/0xff clamp.
uint8_t ColourFloatToByte(float f);

// _DAT_00831b38 / _DAT_00831b40 / _DAT_00831b48, doubles Ghidra did not
// resolve; the ITU-R BT.601 weights are used and are inert whenever the
// record's second scalar is 0, which it is in every shipped map.
constexpr float kLumaR = 0.299f, kLumaG = 0.587f, kLumaB = 0.114f;

struct DrawTint {
    bool specular = false;              // an entity layer with a colour record
    float S[3] = {1.0f, 1.0f, 1.0f};    // the record's RGB, raw
    float s0 = 0.0f, s1 = 0.0f;         // record[3], record[4]
    float A[3] = {1.0f, 1.0f, 1.0f};    // the ambient
    bool Identity() const {
        return !specular && A[0] == 1.0f && A[1] == 1.0f && A[2] == 1.0f;
    }
};

// One palette entry through `ApplyPaletteColorAdjustments @00708e70`.
//   if (s0 > 0) { L = luma(c); d = lerp(c, L, s1); c = lerp(c, d * S, s0); }
//   c *= A;  out = clamp(round(c * 255))
void ApplyPaletteColorAdjustments(const DrawTint& t, uint8_t* rgb);

// ---------------------------------------------------------------------------
// The synthesised entity world box (mapview/NOTES.md 26).  `ComputeRenderBounds
// Corners` is in no dump and the .mis carries only the position, so a standing
// figure is given a 4x4 footprint and a height of 12.
// ---------------------------------------------------------------------------
constexpr int32_t kEntityHalfExtent = 2;
constexpr int32_t kEntityHeight = 12;

inline void EntityWorldBox(const int32_t world[3], int32_t wmin[3], int32_t wmax[3]) {
    wmin[0] = world[0] - kEntityHalfExtent;
    wmin[1] = world[1];
    wmin[2] = world[2] - kEntityHalfExtent;
    wmax[0] = world[0] + kEntityHalfExtent;
    wmax[1] = world[1] + kEntityHeight;
    wmax[2] = world[2] + kEntityHalfExtent;
}

// element+0x49 / +0x55: the game's depth `7*(x+z) - 6*y` over a world point.
inline int64_t GameDepth(int64_t x, int64_t y, int64_t z) {
    return static_cast<int64_t>(kIsoHeight) * (x + z) - 2LL * kIsoHalfWidth * y;
}

// ---------------------------------------------------------------------------
// Decoded assets.
// ---------------------------------------------------------------------------
struct DecodedTile {
    bool ok = false;
    std::string error;
    int32_t width = 0, height = 0;
    // Blit offset of frame 0's top-left relative to the projected anchor:
    // -header.offset + frame.offset.
    int32_t drawX = 0, drawY = 0;
    std::vector<uint8_t> rgba;
    size_t frameCount = 0;
};

struct LoadedSprite {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> file;
    Sprite sprite;
    uint32_t id = 0;   // dense cache key, so (sprite, collection, frame) fits a word
};

class SpriteCache {
public:
    explicit SpriteCache(const PathIndex* index) : m_index(index) {}
    LoadedSprite* Get(const std::string& name);
    size_t Size() const { return m_byName.size(); }

private:
    const PathIndex* m_index;
    std::map<std::string, LoadedSprite> m_byName;
    uint32_t m_next = 1;
};

// ---------------------------------------------------------------------------
// Lights.
// ---------------------------------------------------------------------------

// Reads one property with the instance/prototype override rule of
// mapview/NOTES.md 14.
template <typename Fn>
bool LightGet(const Esh* inst, const Esh* proto, const char* name, bool instanceWins, Fn get) {
    const Esh* first = instanceWins ? inst : proto;
    const Esh* second = instanceWins ? proto : inst;
    if (first != nullptr && get(*first, name)) {
        return true;
    }
    if (second != nullptr && get(*second, name)) {
        return true;
    }
    return false;
}

SceneLight BuildSceneLight(const EntityInstance& inst, const Esh* proto);

// ---------------------------------------------------------------------------
// The two load-time tile filters, both from
// `ComputeElementScreenBoundsAndHighlight @006e2c60`.
// ---------------------------------------------------------------------------

// The Invisible Tile: `ComputeElementScreenBoundsAndHighlight@006e2c60` tests bit 0x80 of the INSTANCE
// flags word at element+0x02.
inline bool TileInstanceInvisible(const TileInstance& inst) {
    return (inst.flags & 0x0080) != 0;
}

// Roof hiding: `ComputeElementScreenBoundsAndHighlight@006e2c60`.  `k` is _DAT_00830b10, a double Ghidra
// did not resolve; it is the one number in this rule that is not recoverable.
bool TileInstanceRoofHidden(const World& w, const TileInstance& inst, const int64_t inside[3],
                            double k);

// ---------------------------------------------------------------------------
// Per-layer entity colour.  `SoftwareRender::RenderLitObject @00783340`
// (`SoftwareRender::RenderLitObject@00783340`) advances the colour-record pointer 0x14 bytes per
// layer, so layer i takes record i of the four at Entity+0xB3.
// ---------------------------------------------------------------------------
// `layerSlot[i]` is the record index layer i takes (identity, unless a caller
// remaps it for the --layer-slots experiment); `tintOn` false forces every
// layer to the plain `c * A`.
void BuildLayerTints(const EntityInstance& inst, const float ambient[3], const int* layerSlot,
                     bool tintOn, DrawTint out[kLayerCount]);

// ---------------------------------------------------------------------------
// The Gouraud tile quad and the blit.
// ---------------------------------------------------------------------------

// `DirectX7Render::RenderTrianglesWithVertexLighting @0077a6f0` interpolates
// the element's four corner colours across the quad.  (u, v) are the tile's own
// normalised world x and z, recovered from the screen point by inverting the
// affine iso projection on the top-face plane y = max[1].
struct LightQuad {
    bool on = false;
    float c[2][2][3];                // [xIsMax][zIsMax], the top-face corners
    double ax = 0, bx = 0, cx = 0;   // u = ax*SX + bx*SY + cx
    double az = 0, bz = 0, cz = 0;   // v = az*SX + bz*SY + cz
};

// `ClassifyByteTripleRelation @006f02d0`, applied to
// the tile prototype's three bbox bytes at Tile+0x12/+0x13/+0x14 = (x, y, z).
// `Tile::ReadTileFileHeader` caches the result at Tile+0x0a and
// `BuildVisibleTileRenderCache @006e2870` copies it to the render wrapper's
// +0xe4, which is the value `BuildShapeVertices @006e4220`
// switches on.  Returns 0..5; only 1, 3 and 4 take a non-default shape.
int ClassifyTileShape(uint8_t bboxX, uint8_t bboxY, uint8_t bboxZ);

// `BuildShapeVertices @006e4220` chooses the four
// lit vertices from the element's world box by that class:
//
//   class 1  (thin in x)   the vertical quad in the plane x = min.x,
//                          spanning y = min.y..max.y and z = min.z..max.z
//   class 3  (thin in z)   the vertical quad in the plane z = min.z,
//                          spanning x = min.x..max.x and y = min.y..max.y
//   class 4  (tall, thin   a degenerate vertical EDGE at (min.x, *, min.z):
//             in x and z)  v0 == v1 and v2 == v3, so the shading is a pure
//                          vertical gradient -- a billboard/pole
//   otherwise              the TOP face of the box, at y = max.y, and the
//                          function sets the element's +0x3b "flat" flag
//
// `shapeClass` is what ClassifyTileShape returns for the instance's prototype.
void BuildLightQuad(const SceneLighting& scene, const TileInstance& in, int shapeClass,
                    LightQuad* q);

// One RGBA source rectangle composited into an RGBA target, through the tint
// (and, for a lit tile, through the quad).  (dx, dy) is in target pixels;
// (originX, originY) is the world-screen coordinate of the target's (0, 0),
// which is what the quad's affine (u, v) is expressed in.
struct BlitTarget {
    uint8_t* pixels = nullptr;
    int32_t w = 0, h = 0;
    int64_t originX = 0, originY = 0;
};

void BlitRGBA(const BlitTarget& t, const uint8_t* rgba, int32_t width, int32_t height, int64_t dx,
              int64_t dy, const DrawTint& tint, const LightQuad* quad);

}  // namespace ft

#endif  // ENGINE_SHARED_H
