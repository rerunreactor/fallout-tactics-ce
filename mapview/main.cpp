// main.cpp -- mapview: Fallout Tactics .mis map renderer / .til tile dumper.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <numeric>
#include <string>
#include <unordered_map>
#include <vector>

#include "ent.h"
#include "esh.h"
#include "file.h"
#include "shared.h"   // engine/: the pieces mapview and fotgame share
#include "iso.h"
#include "light.h"
#include "mis.h"
#include "png.h"
#include "sort.h"
#include "spr.h"
#include "til.h"

namespace fs = std::filesystem;
using namespace ft;

namespace {

// Lower, ParseInts, DecodedTile, IsEditorMarkerClass, ColourFloatToByte,
// kLuma*, DrawTint, ApplyPaletteColorAdjustments, kEntityHalfExtent/Height,
// LoadedSprite, SpriteCache, LightGet, BuildSceneLight, the two tile filters,
// BuildLayerTints, LightQuad/BuildLightQuad/BlitRGBA and GameDepth now live in
// engine/shared.h, which fotgame includes too.

// The prototype/sprite/prototype-path names inside a .mis carry the case and
// separators the map's author typed, which do not always match what came out of
// the .bos archives, so every lookup goes through a case-insensitive index of
// the extracted tree.  That used to be a `TileIndex` class here; milestone 3
// needs the same thing for `entities/` and `sprites/`, so it moved to
// ent.h/ent.cpp as `PathIndex` with the stripped prefix as a parameter.
using TileIndex = PathIndex;



// ---------------------------------------------------------------------------
// Entity colour and the tint blit
// ---------------------------------------------------------------------------
//
// SoftwareRender::BlitSpriteWithLighting @00782530
// turns the entity's four 5-float colour records into four packed 0xFFRRGGBB
// corner colours (l.23295-23320: `ROUND(f * 255)` per channel, clamped to
// 0..255, alpha forced to 0xff) and installs them as the corners of the
// bilinear gradient that ImageZAR::BlitRleClippedWithTintBlend stretches over
// the blit rect (ZAR notes, section 3).
//
// Two gates decide whether that happens at all (l.23269-23272):
//
//     if (DAT_008bda4f == '\0')                    -> uniform path
//     else if (<the four triples are not all equal>) -> tinted path
//     ...
//     SetAmbientLightColor(pfVar13);  plain, untinted blit   (l.23748-23752)
//
// `DAT_008bda4f` is the per-vertex-shading switch and the four corner colours
// are the *output of the lighting solver* (ComputeLightColorForFrame @006e4610,
// l.20944, whose own master gate DAT_008bda4c leaves every colour at the global
// ambient when lighting is off).  So with lighting disabled -- which is this
// milestone's brief -- the engine itself takes the uniform, untinted path, and
// that is what `kTintOff` reproduces.  See NOTES.md for why that is also what
// the shipped art wants.
enum TintMode {
    kTintOff = 0,     // no colour multiply at all (milestone-3 diagnostic)
    kTintOn = 1,      // the ported path: per-layer entity colour, flat ambient
};
// Draw order.  See step 3 of RenderWorld for the two rules and where each
// comes from.
enum SortRule {
    kSortDepth = 0,    // milestone 3's reconstruction: 6*y - 7*(x+z), ascending
    kSortScreenY = 1,  // the ported one: screen y0 ascending, elevation descending
};
// Entities the editor places but the game does not draw.  Entity::RenderObjectMaybe
// skips an entity whose virtual IsHidden() returns true (`Entity::RenderObjectMaybe@005e2080`:
// the render item is zeroed and the submit at l.14766 is not reached), and
// IsHidden is vtable slot 238 (l.383 for Entity, l.917 for Actor) -- but no
// per-class override is in the supplied decompilation, so *which* classes return
// true is not recoverable from it.  These three are chosen from the data: their
// sprites are `sprites/misc/waypoint.spr`, the light marker and the spawn marker,
// none of which is scenery, and WayPoint::ClassType::Serialize even derives its
// marker colour by hashing the tag name (l.11477-11493), which is debug-overlay
// behaviour.  `--markers` puts them back.  See NOTES.md.

// l.23297: ROUND(f * 255) with the 0/0xff clamp, `_DAT_007fd780` being 255.0.
// The clamp is written as two pointer selections, high first then low, so a NaN
// would fall through as-is; that cannot arise here.

// MILESTONE 3's reading of the colour records is superseded here.  It read
// float[3] as a premultiplied WEIGHT (from ComputeLightColorForFrame's
// `(c*w + light*lw)/(w+lw)` accumulator, l.21206-21214) and float[4] as
// unexplained.  That accumulator belongs to Light ENTITIES' own colour, not to
// the four per-layer records, and the records are not premultiplied: float[3]
// and float[4] are the two specular scalars, and what they do is now ported in
// full below.
// MILESTONE 4.  The tint is now PORTED, and it is not a per-pixel multiply by a
// precomputed colour: it is a per-PALETTE-ENTRY transform run once per
// (palette, light state) pair by `ApplyPaletteColorAdjustments @00708e70`
// (`ApplyPaletteColorAdjustments@00708e70`), after which the blit is a plain LUT index.
// Applying it per decoded pixel here is exactly equivalent, because it is a
// pure function of the source colour.
//
// SoftwareRender::RenderLitObject sets the light state
// per layer -- InitDefaultLightColors (l.6541), then
// SetSpecularLightColor(record, record[3], record[4]) (l.6544-6546), then
// SetAmbientLightColor(the averaged 4-corner lighting colour) (l.6547), then
// SetDiffuseLightColor only when a scalar is positive (l.6548-6550) -- and
// ApplyPaletteColorAdjustments then does, per palette entry:
//
//   c = pal / 255                                        l.21497-21504
//   if (s0 > 0) {                                        l.21509
//       L = Kr*c.r + Kg*c.g + Kb*c.b                     l.21513-21514
//       d = lerp(c, L, s1)                               l.21516-21517, l.21524
//       c = lerp(c, d * S, s0)                           l.21518-21524
//   }
//   if (dw > 0) c = lerp(c, D, dw)                       l.21528-21537
//   c = c * A                                            l.21544-21547  (always)
//   out = clamp(round(c * 255))                          l.21611-21651
//
// with S = the record's RGB **raw** (SetSpecularLightColor copies it unscaled),
// s0 = record[3], s1 = record[4], A = ambient, D/dw = diffuse.  lerp(a,b,t) =
// (b-a)*t + a.  This corrects milestone 3's inferred
// `lerp(1, record/ambient, record[3])` in three ways: the record is NOT divided
// by the ambient, the ambient is a separate trailing multiply that applies to
// every pixel of every sprite (and to tiles), and record[4] -- previously
// "unexplained" -- is a desaturation weight that collapses the source toward
// its own luminance before the record recolours it.  With a flat white ambient
// and record[4] == 0 the two agree exactly, which is why milestone 3's renders
// were right; they would diverge the moment the ambient stopped being white.
//
// Entities take the specular term; TILES take the diffuse one instead --
// SoftwareRender::BlitSpriteWithLighting sets diffuse from the tile's own colour
// (`SoftwareRender::BlitSpriteWithLighting@00782530`) and never touches specular.  With no scene light loaded
// a tile's diffuse scalar is 0, so a tile gets `c * A` and nothing else, which
// is what DrawTint below applies to tiles.
//
// The one number here that is not recoverable is the luma triple: Kg, Kr and Kb
// are _DAT_00831b38 / _DAT_00831b40 / _DAT_00831b48, doubles Ghidra did not
// resolve.  The ITU-R BT.601 weights are used; they are inert whenever
// record[4] == 0, which is every record in the shipped maps (see --selftest).


struct DecodedLayer {
    bool present = false;
    int32_t x = 0, y = 0, w = 0, h = 0;
    std::vector<uint8_t> rgba;
};

// Everything needed to blit one entity, resolved once per (sprite, animation).
struct EntityDraw {
    size_t instance = 0;
    int32_t x0 = 0, y0 = 0;   // screen position of the collection rect's origin
    int64_t depth = 0;
    int32_t elev = 0;         // world y, the sort's tie-break
    float wpos[3] = {0.0f, 0.0f, 0.0f};   // the entity's own position, in metres
    // The two cached boxes the draw-order sort reads off the element record
    // (sort.h): the screen rect at element+0x41..+0x51 and the world bbox at
    // element+0x04..+0x18.  `rect` is the union of the present layers' rects in
    // absolute screen pixels; the world box is synthesised, see step 1b.
    int32_t rect[4] = {0, 0, 0, 0};   // x0, y0, x1, y1
    int32_t wmin[3] = {0, 0, 0};
    int32_t wmax[3] = {0, 0, 0};
    std::array<DecodedLayer, kLayerCount> layers;
    DrawTint tint[kLayerCount];
};

// The entity world box the engine caches at element+0x04..+0x18.  Its writer,
// `Entity::CloneEntityWithNewId` -> `ComputeRenderBoundsCorners`, is in no
// dump, and the .mis carries only the entity's position, so this is the one
// synthesised input to the ported draw-order sort (section 26).  World units
// are the units the tile instances use: a floor cell is 6x1x6 and a bunker wall
// segment is 12 tall, so a standing figure is given a 4x4 footprint centred on
// its position and a height of 12 -- big enough that it interpenetrates the
// wall and tent tiles it must be ordered against, small enough to stay inside
// its own cell.


// ---------------------------------------------------------------------------
// --info
// ---------------------------------------------------------------------------
int CmdMapInfo(const World& w) {
    std::printf("file                %s (%zu bytes on disk, %zu inflated)\n", w.path.c_str(),
                w.rawSize, w.inflatedSize);
    std::printf("<world> version     %d\n", w.version);
    std::printf("<mapManager> ver    %d\n", w.mapManagerVersion);
    std::printf("world box           min (%d, %d, %d)  max (%d, %d, %d)\n", w.worldBox[0],
                w.worldBox[1], w.worldBox[2], w.worldBox[3], w.worldBox[4], w.worldBox[5]);
    std::printf("world size          %d x %d x %d  (x, elevation, z)\n", w.worldBox[3] - w.worldBox[0],
                w.worldBox[4] - w.worldBox[1], w.worldBox[5] - w.worldBox[2]);
    const int32_t rx = (w.worldBox[3] - w.worldBox[0]) / 256;
    const int32_t ry = (w.worldBox[4] - w.worldBox[1]) / 256;
    const int32_t rz = (w.worldBox[5] - w.worldBox[2]) / 256;
    std::printf("regions             %d  (grid %d x %d x %d of 256-unit cells)\n", w.regionCount, rx,
                ry, rz);
    std::printf("cull box            %d %d %d %d %d %d\n", w.cullBox[0], w.cullBox[1], w.cullBox[2],
                w.cullBox[3], w.cullBox[4], w.cullBox[5]);
    // The three float triples at the head of <mapManager> v50 are the map's
    // light rig.  ReadMapManagerSection reads them into World+0x5D, World+0x69
    // and a stack local it throws away, and
    // World::InitMapLightManagerPart @006e68a0
    // gives those same three fields their defaults -- ambient (0.5,0.5,0.5),
    // diffuse (1,1,1), direction normalize(750,-4000,750) -- which is what
    // identifies them.  Every shipped map stores exactly that default direction,
    // and the engine discards the stored one anyway.
    std::printf("light rig           ambient (%g %g %g)  diffuse (%g %g %g)  dir (%g %g %g)%s\n",
                w.vec1[0], w.vec1[1], w.vec1[2], w.vec2[0], w.vec2[1], w.vec2[2], w.vec3[0],
                w.vec3[1], w.vec3[2], " [editor-only: see NOTES 24]");

    std::map<int32_t, int32_t> elevations;
    for (const TileInstance& i : w.instances) {
        elevations[i.min[1]]++;
    }
    std::printf("elevations          %zu distinct min-y values:", elevations.size());
    for (const auto& kv : elevations) {
        std::printf(" %d(x%d)", kv.first, kv.second);
    }
    std::printf("\n");

    std::printf("tile prototypes     %zu (slot 0 is the empty 'no tile')\n", w.protos.size());
    std::printf("tile instances      %zu (header said %d)\n", w.instances.size(), w.declaredInstances);
    std::printf("entity class names  %d\n", w.entityClassCount);
    std::printf("entity records      %zu <esh> blocks\n", w.entityRecordCount);

    std::printf("\n--- sections in the world stream (by first offset) ---\n");
    for (const SectionInfo& s : w.sections) {
        std::printf("  %-18s version %-3d  count %-6zu  first at 0x%zx\n", s.tag.c_str(),
                    s.firstVersion, s.count, s.firstOffset);
    }

    std::printf("\n--- tile prototypes ---\n");
    std::printf("  %-4s %-6s %-9s %-9s %-9s %-4s %-4s  %s\n", "id", "count", "bbox(x,y,z)",
                "offset", "size", "type", "mat", "name");
    for (size_t i = 0; i < w.protos.size(); ++i) {
        const TileProto& p = w.protos[i];
        char bbox[32], off[32], size[32];
        std::snprintf(bbox, sizeof(bbox), "%d,%d,%d", p.header.bboxX, p.header.bboxY, p.header.bboxZ);
        std::snprintf(off, sizeof(off), "%d,%d", p.header.offsetX, p.header.offsetY);
        std::snprintf(size, sizeof(size), "%d,%d", p.header.sizeX, p.header.sizeY);
        std::printf("  %-4zu %-6zu %-9s %-9s %-9s %-4d %-4d  %s\n", i, p.instanceCount, bbox, off,
                    size, p.header.typeId, p.header.material,
                    p.name.empty() ? "(no tile)" : p.name.c_str());
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --instances -- a raw dump of the <region> tile placements.  Diagnostic only;
// added by fix3 to inspect the mission01 bridge.  Honours --region (x0,z0,x1,z1)
// and --name <substring> to narrow the dump.
// ---------------------------------------------------------------------------
int CmdInstances(const World& w, bool haveRegion, const int64_t* region,
                 const std::string& nameFilter) {
    std::printf("  %-6s %-5s %-6s %-6s %-16s %-16s %s\n", "idx", "reg", "proto", "flags", "min(x,y,z)",
                "max(x,y,z)", "name");
    size_t shown = 0;
    for (size_t i = 0; i < w.instances.size(); ++i) {
        const TileInstance& t = w.instances[i];
        if (haveRegion) {
            if (t.min[0] < region[0] || t.min[0] > region[2] || t.min[2] < region[1] ||
                t.min[2] > region[3]) {
                continue;
            }
        }
        const std::string& nm =
            (t.protoId >= 0 && (size_t)t.protoId < w.protos.size()) ? w.protos[t.protoId].name
                                                                    : std::string();
        if (!nameFilter.empty()) {
            std::string lower = nm, needle = nameFilter;
            for (char& c : lower) c = (char)std::tolower((unsigned char)c);
            for (char& c : needle) c = (char)std::tolower((unsigned char)c);
            if (lower.find(needle) == std::string::npos) continue;
        }
        char mn[40], mx[40];
        std::snprintf(mn, sizeof(mn), "%d,%d,%d", t.min[0], t.min[1], t.min[2]);
        std::snprintf(mx, sizeof(mx), "%d,%d,%d", t.max[0], t.max[1], t.max[2]);
        int type = (t.protoId >= 0 && (size_t)t.protoId < w.protos.size())
                       ? w.protos[t.protoId].header.typeId : -1;
        std::printf("  %-6zu %-5d %-6d 0x%04x %-16s %-16s t%d %s\n", i, t.region, t.protoId, t.flags,
                    mn, mx, type, nm.c_str());
        ++shown;
    }
    std::printf("  (%zu instances shown of %zu)\n", shown, w.instances.size());
    return 0;
}

// ---------------------------------------------------------------------------
// --entities
// ---------------------------------------------------------------------------
int CmdEntities(const World& w, const PathIndex* entIndex, const PathIndex* spriteIndex) {
    const EntityFile& ef = w.entityFile;
    std::printf("<entity_file> ver   %d\n", ef.version);
    std::printf("class names         %zu\n", ef.classNames.size());
    std::printf("slots               %d (slot 0 reserved), %zu occupied, %zu empty\n",
                ef.slotCount, ef.entities.size(), ef.emptySlots);
    std::printf("live count          %d (header)\n", ef.liveCount);
    std::printf("world entity count  %d (header)\n", ef.worldEntityCount);
    if (!ef.error.empty()) {
        std::printf("PARSE ERROR         %s\n", ef.error.c_str());
    }

    std::map<std::string, size_t> byClass;
    for (const EntityInstance& e : ef.entities) {
        byClass[e.className]++;
    }
    std::printf("\n--- entities by class ---\n");
    for (const auto& kv : byClass) {
        std::printf("  %-22s %zu\n", kv.first.c_str(), kv.second);
    }

    EntProtoCache protos(entIndex);
    SpriteCache sprites(spriteIndex);
    size_t resolved = 0, missing = 0;

    std::printf("\n--- entity instances ---\n");
    std::printf("  %-5s %-16s %-22s %-16s %-8s %-6s %-4s %-3s %-20s %s\n", "slot", "class",
                "EntPath", "name", "pos", "dir", "plr", "seq", "sequence", "sprite");
    for (const EntityInstance& raw : ef.entities) {
        EntityInstance e = raw;
        std::string protoErr;
        ApplyEntOverride(protos.Get(e.entPath, &protoErr), &e);

        char pos[48] = "-";
        if (e.hasFrame) {
            std::snprintf(pos, sizeof(pos), "%d,%d,%d", e.world[0], e.world[1], e.world[2]);
        }
        std::string name = e.tagName.empty() ? e.displayName : e.tagName;
        if (name.empty()) {
            name = e.internalName;
        }
        std::string entLeaf = e.entPath;
        const size_t slash = entLeaf.find_last_of('/');
        if (slash != std::string::npos) {
            entLeaf = entLeaf.substr(slash + 1);
        }

        std::string seqName = "-", spriteNote = e.spritePath;
        char dir[16] = "-";
        if (!e.spritePath.empty()) {
            LoadedSprite* ls = sprites.Get(e.spritePath);
            if (!ls->ok) {
                spriteNote += "  [" + ls->error + "]";
                ++missing;
            } else {
                const SpritePick pick = PickSprite(ls->sprite, e);
                seqName = pick.sequenceName;
                std::snprintf(dir, sizeof(dir), "%d/%d", pick.direction, pick.dirCount);
                if (!pick.error.empty()) {
                    seqName += " [" + pick.error + "]";
                }
                ++resolved;
            }
        }
        if (!e.entFound && !e.entPath.empty()) {
            entLeaf += " [" + protoErr + "]";
        }

        std::printf("  %-5d %-16s %-22s %-16s %-8s %-6s %-4d %-3d %-20s %s\n", e.slot,
                    e.className.c_str(), entLeaf.c_str(), name.c_str(), pos, dir, e.playerIndex,
                    e.sequenceIndex, seqName.c_str(), spriteNote.c_str());

        // The four colour records, when the instance carries them.
        bool any = false;
        for (int c = 0; c < 4; ++c) {
            any = any || e.hasColour[c];
        }
        if (any) {
            std::printf("        colours ");
            for (int c = 0; c < 4; ++c) {
                if (!e.hasColour[c]) {
                    continue;
                }
                std::printf(" %s=(%d,%d,%d|%.3f,%.3f)", kEntityColourNames[c],
                            ColourFloatToByte(e.colour[c][0]), ColourFloatToByte(e.colour[c][1]),
                            ColourFloatToByte(e.colour[c][2]), e.colour[c][3], e.colour[c][4]);
            }
            std::printf("\n");
        }
    }
    std::printf("\nentities parsed: %zu\n", ef.entities.size());
    std::printf("sprites resolved/missing: %zu/%zu\n", resolved, missing);
    return 0;
}

// ---------------------------------------------------------------------------
// --til <file> --info / --dump
// ---------------------------------------------------------------------------
int CmdTil(const std::string& path, bool info, const std::string& dumpDir) {
    std::vector<uint8_t> file;
    std::string err;
    if (!ReadWholeFile(path, &file, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Tile tile;
    if (!tile.Load(file, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    if (info) {
        std::printf("file            %s (%zu bytes)\n", path.c_str(), file.size());
        std::printf("<tile> version  %d\n", tile.header.version);
        std::printf("bbox (x,y,z)    %d, %d, %d   (bytes at Tile+0x12..0x14)\n", tile.header.bboxX,
                    tile.header.bboxY, tile.header.bboxZ);
        std::printf("offset          %d, %d\n", tile.header.offsetX, tile.header.offsetY);
        std::printf("size            %d, %d\n", tile.header.sizeX, tile.header.sizeY);
        std::printf("type / material %d / %d\n", tile.header.typeId, tile.header.material);
        std::printf("flags           0x%04x\n", tile.header.flags);
        std::printf("frames          %zu\n", tile.frames.size());
        std::printf("trailing pal    %zu entries\n", tile.palette.size());
        for (size_t i = 0; i < tile.frames.size(); ++i) {
            const TileFrame& f = tile.frames[i];
            std::printf("  frame %-3zu zar v%d %dx%d  offset %d,%d  palette %zu  data %zu\n", i,
                        f.zar.Version(), f.zar.Width(), f.zar.Height(), f.offsetX, f.offsetY,
                        f.zar.PaletteSize(), f.zar.DataSize());
        }
    }
    if (!dumpDir.empty()) {
        std::error_code ec;
        fs::create_directories(dumpDir, ec);
        const std::string stem = fs::path(path).stem().string();
        for (size_t i = 0; i < tile.frames.size(); ++i) {
            std::vector<uint8_t> rgba;
            int32_t width = 0, height = 0;
            if (!tile.DecodeFrame(i, &rgba, &width, &height, &err)) {
                std::fprintf(stderr, "frame %zu: %s\n", i, err.c_str());
                continue;
            }
            char name[512];
            std::snprintf(name, sizeof(name), "%s/%s_%03zu.png", dumpDir.c_str(), stem.c_str(), i);
            if (!WritePng(name, width, height, rgba, &err)) {
                std::fprintf(stderr, "%s: %s\n", name, err.c_str());
                continue;
            }
            std::printf("wrote %s (%dx%d)\n", name, width, height);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
struct RenderOptions {
    int scale = 1;
    bool haveRegion = false;
    int64_t region[4] = {0, 0, 0, 0};  // x0, z0, x1, z1 in world units
    bool haveCrop = false;
    int64_t crop[4] = {0, 0, 0, 0};  // x, y, w, h in output pixels
    // --centre x,z [--size w,h]: crop a window centred on a world point.  Only
    // a convenience for reproducible verification renders.
    bool haveProbe = false;
    int64_t probe[3] = {0, 0, 0};
    bool haveCentre = false;
    int64_t centre[3] = {0, 0, 0};  // world x, y (elevation), z
    int64_t size[2] = {800, 600};
    // --inside x,z or x,y,z: the roof-fade reference point of
    // ComputeElementScreenBoundsAndHighlight @006e2c60.  insideK is its
    // unresolved radius constant _DAT_00830b10.
    bool haveInside = false;
    int64_t inside[3] = {0, 0, 0};
    double insideK = 3.0;
    SortRule sortRule = kSortDepth;
    // --squad: the campaign roster the .mis does not carry.  See step 1c.
    bool haveSquad = false;
    std::vector<std::string> squad;
    uint32_t background = 0x00000000;
    bool quiet = false;
    int32_t maxElev = 0x7fffffff;  // skip tiles whose min elevation exceeds this (roof hiding)
    bool drawEntities = true;
    bool showInvisibleTiles = false;  // --show-invisible defeats the flag-0x80 rule
    bool drawMarkers = false;         // --markers draws the editor-only marker entities
    bool campaignStart = false;       // --campaign-start: the single-player mission-load state
    TintMode tint = kTintOn;
    // The ambient, PORTED.  `light_level_maybe` @0x008bde98 turns out not to be
    // a lighting variable at all: it is the static constant Color::White, one
    // of nine sibling colour constants at 0x8bde18..0x8bde98 written once each
    // by a CRT dynamic initialiser (the one at 0x006cc760 stores 0x3f800000
    // three times and returns; its thunk at 0x006cc750 sits in the
    // _initterm table at 0x894cd4).  That is the sole write in the whole
    // .text -- which is why the Ghidra export's 138 references are 137 reads and
    // one initialiser -- so the runtime ambient is (1, 1, 1) for every map, and
    // --ambient is no longer needed.  See NOTES.md section 24.
    float ambient[3] = {1.0f, 1.0f, 1.0f};
    bool ambientFromMap = false;  // --ambient map: use <mapManager>'s own rig
    // MILESTONE 5.  --lights on|off, --light-base game|map, --time HH:MM,
    // --tick N.  See NOTES.md section 25: with the shipped build's base
    // (constant white) the per-corner sum saturates and the lighting is
    // invisible, which is why `map` exists at all.
    bool lights = true;
    // The vertex base, M+0xA0.  Defaults to the map's own rig, which is what
    // the shipped game looks like (see NOTES.md 25.1 as revised);
    // `--light-base white` is UpdateEntityRenderTransform's DAT_008bda50 == 0
    // branch, kept because that is what the disassembly of that one function
    // literally says and it is the discrepancy 25.1 records.
    // MILESTONE 19 (NOTES.md 25.13): the default is now the CLOCK.
    // World::UpdateWorldTimeAndLighting @004f2c10 overwrites the rig from
    // ComputeTimeOfDayLightingState @004f2770 every frame, so the `.mis`'s
    // stored triple is only the editor's snapshot of the same computation --
    // and on ten maps, Brahmin Wood included, that snapshot is a NIGHT one.
    LightBase lightBase = LightBase::kClock;
    bool neutralRig = false;   // --light-base neutral: multiplier exactly 1, a probe
    bool haveTime = false;
    int timeHour = 0, timeMinute = 0;
    int64_t tick = 0;
    // Which frame-record SLOT each of the .spr's four stored layers is.  The
    // binary pins slot -> colour record (see below); it does NOT pin the file's
    // layer order, because the <spranim_img> reader is not in the
    // decompilation at all (sprview/NOTES.md section 1.6).
    int layerSlot[kLayerCount] = {0, 1, 2, 3};
};



// ---------------------------------------------------------------------------
// MILESTONE 5: building the scene's light list
// ---------------------------------------------------------------------------
//
// `Light::EntityStreamStruct::Serialize` @0059c1c0 is the property table.  It
// is not in any supplied decompilation; it was disassembled out of the shipped
// BOS.exe.  Each entry is
// `EntityStreamWrite<T>(stream, name, &field, DEFAULT, type, FLAGS)`, the same
// six-argument shape the Ghidra export shows for every other class, so it pins
// both the compile-time default and the prototype-override polarity of section
// 14 (FLAGS bit 0 set == placement data, the .mis instance wins; clear == the
// .ent prototype wins):
//
//   name                          field   default   type  flags
//   "On"                          +0x1D5  true       1     2    prototype
//   "Night Only"                  +0x1D6  false      1     1    instance
//   "Radius (meters)"             +0x1D7  2.0f       2     3    instance
//   "Rate of change (ms)"         +0x1DB  0          3     3    instance
//   "Color1"                      +0x1DF  white      5     3    instance
//   "Intensity string (a-z)"      +0x1EB  ""         4     3    instance
//   "Repeating light intenstity"  +0x1EF  true       1     2    prototype
//   (unnamed, 0x8a19d4)           +0x219  0          3     0    prototype
//   "Use Color2"                  +0x1F0  false      1     2    prototype
//   "Color2"                      +0x1F1  white      5     2    prototype
//   "Use Arc"                     +0x1FD  false      1     3    instance
//   "Arc (degrees)"               +0x1FE  360        3     3    instance
//   "High Bound"                  +0x202  0.0f       2     1    instance
//   "Low Bound"                   +0x206  0.0f       2     1    instance
//   "Use Fog"                     +0x20A  false      1     3    instance
//   "Fog Alpha"                   +0x20B  0.0f       2     3    instance
//
// The names the shipped maps actually write are only a subset -- across all 87
// maps and 868 Light entities the census is Color1 and "Rate of change (ms)"
// on every one, "Radius (meters)" on 788, "Night Only" on 498, "Intensity
// string (a-z)" on 207, "High Bound" 158, "Low Bound" 122, "Arc (degrees)" 80,
// "Use Arc" 46.  "On", "Repeating light intenstity", "Use Color2", "Color2",
// "Use Fog" and "Fog Alpha" never appear, so those six are always the
// compile-time defaults above.  Two consequences worth stating up front:
// `Use Fog` is false everywhere, so every shipped light lands in
// World::GatherVisibleLights' FIRST list and none in the fog list; and `On`
// is true everywhere, which makes `Night Only` inert (the gate is
// `on || (nightOnly && dark)`, not `nightOnly ? dark : on`).
struct LightPropSpec {
    const char* name;
    bool instanceWins;
};

// LightGet and BuildSceneLight are in engine/shared.h.

bool RenderWorld(const World& w, const TileIndex& index, const PathIndex* spriteIndex,
                 const PathIndex* entIndex, const std::string& outPath, const RenderOptions& optIn,
                 std::string* errorOut) {
    RenderOptions opt = optIn;
    if (opt.ambientFromMap) {
        for (int k = 0; k < 3; ++k) {
            opt.ambient[k] = w.vec1[k];
        }
    }
    // 1. Load and decode every referenced prototype once.
    std::vector<DecodedTile> decoded(w.protos.size());
    size_t resolved = 0, missing = 0;
    size_t hiddenProtos = 0, hiddenInstances = 0;
    for (size_t i = 1; i < w.protos.size(); ++i) {
        if (w.protos[i].instanceCount == 0) {
            continue;
        }
        // Milestone 4: the Invisible Tile test is now a port, not an inference,
        // and it moved from the PROTOTYPE's flag word to the INSTANCE's.  See
        // the per-instance skip in step 2 below and NOTES.md section 16.
        std::string full;
        if (!index.Find(w.protos[i].name, &full)) {
            decoded[i].error = "not found in tile directory";
            ++missing;
            continue;
        }
        std::vector<uint8_t> file;
        std::string err;
        if (!ReadWholeFile(full, &file, &err)) {
            decoded[i].error = err;
            ++missing;
            continue;
        }
        Tile tile;
        if (!tile.Load(file, &err)) {
            decoded[i].error = err;
            ++missing;
            continue;
        }
        if (tile.frames.empty()) {
            decoded[i].error = "no frames";
            ++missing;
            continue;
        }
        DecodedTile& d = decoded[i];
        d.frameCount = tile.frames.size();
        if (!tile.DecodeFrame(0, &d.rgba, &d.width, &d.height, &err)) {
            d.error = err;
            ++missing;
            continue;
        }
        // FreeFT draws a frame at `frame.rect() + (pos - tile.m_offset)`; the
        // same relation is what makes the .mis's own cached rect come out as
        // (-offsetX, -offsetY, sizeX - offsetX, sizeY - offsetY).
        d.drawX = tile.frames[0].offsetX - tile.header.offsetX;
        d.drawY = tile.frames[0].offsetY - tile.header.offsetY;
        d.ok = true;
        ++resolved;
    }
    if (!opt.quiet) {
        std::printf("tile prototypes resolved/missing: %zu/%zu\n", resolved, missing);
        for (size_t i = 1; i < w.protos.size(); ++i) {
            if (!decoded[i].ok && w.protos[i].instanceCount > 0 &&
                !decoded[i].error.empty()) {
                std::fprintf(stderr, "  MISSING [%zu] %s: %s\n", i, w.protos[i].name.c_str(),
                             decoded[i].error.c_str());
            }
        }
    }

    (void)hiddenProtos;

    // 1b. Resolve every entity: prototype override, sprite, sequence, frame,
    //     direction, and the four layer images of the one frame it shows.
    std::vector<EntityDraw> entities;
    size_t entResolved = 0, entMissing = 0, entMarkers = 0, entPlaceholders = 0;
    // The campaign squad's player slot, taken from the map's SpawnPoint.
    int32_t squadPlayerIndex = -1;
    int32_t squadPos[3] = {0, 0, 0};
    bool haveSquadPos = false;
    for (const EntityInstance& e : w.entityFile.entities) {
        if (e.className == "SpawnPoint" && e.hasFrame) {
            squadPlayerIndex = e.playerIndex;
            squadPos[0] = e.world[0];
            squadPos[1] = e.world[1];
            squadPos[2] = e.world[2];
            haveSquadPos = true;
            break;
        }
    }
    if (opt.campaignStart && !opt.quiet) {
        if (haveSquadPos) {
            std::printf("campaign start      spawn point at world (%d, %d, %d), player slot %d\n",
                        squadPos[0], squadPos[1], squadPos[2], squadPlayerIndex);
        } else {
            std::printf("campaign start      this map has no SpawnPoint\n");
        }
    }
    std::map<std::string, size_t> entMissReasons;
    SpriteCache sprites(spriteIndex);
    EntProtoCache protos(entIndex);

    // 1c. --squad: the campaign roster the .mis does not carry.
    //
    // PORTED, as far as the dump goes.  Spawn-point SELECTION is recoverable:
    // World::FindRandomEntityByPlayerAndType @004f7350
    // filters the world's SpawnPoint list on `Entity+0x163 == playerIndex`
    // (l.3033), `SpawnPoint+0x1A5 == desiredTeam` (l.3039) and
    // `SpawnPoint+0x1AD == the requested spawn type` (l.3040-3044), takes the
    // minimum `spawnCounter` at SpawnPoint+0x1A9 (l.3058-3067) and breaks ties
    // at random (l.3071-3088).  Every spawnCounter is zeroed at mission load
    // (World::ValidateSpawnPointsAndAssignTeamColors l.2783) and bumped once per
    // use (World::ResetAndRespawnPlayerSquads l.3268), so at mission start the
    // minimum is 0 and the choice is uniform over the matching points.  Single
    // player short-circuits the slot/team pick to player 1, team 1
    // (SpawnPlayerAtAvailableSpawnPoint l.2608-2611), which is exactly the slot
    // mission01's SpawnPoint carries.  The roster is walked head-to-tail,
    // index 0..count-1, over the player's member list
    // (ResetAndRespawnPlayerSquads l.3274-3287) with no leader-first special
    // case, so --squad's argument order is the spawn order.
    //
    // The GEOMETRY is NOT recoverable.  `SpawnEntityGroupInFormation` @004f5a10
    // and `InitializeSpawnedEntityInFormation` are call sites only (l.2673,
    // l.3284): both lie in the un-dumped range 004f5971..004f7350, and nothing
    // in any of the nine decompiled files writes an entity position
    // (Entity+0x9A) or `animationRotation` (Entity+0x13B) on the spawn path.
    // The formation below is therefore INVENTED, and labelled as such: members
    // are placed on a ring of radius 6 world units (1.5 tiles) around the spawn
    // point, in roster order, each facing the ring's outward direction -- which
    // is what the shipped screenshots look like, and nothing more than that.
    std::vector<EntityInstance> squadInsts;
    if (opt.haveSquad && haveSquadPos) {
        static const int kRing[8][2] = {{0, -6}, {5, -4}, {6, 0},  {5, 4},
                                        {0, 6},  {-5, 4}, {-6, 0}, {-5, -4}};
        for (size_t i = 0; i < opt.squad.size(); ++i) {
            EntityInstance e;
            e.className = "Actor";
            e.slot = -1 - static_cast<int32_t>(i);
            e.entPath = opt.squad[i];
            if (e.entPath.rfind("entities/", 0) != 0) {
                e.entPath = "entities/recruits/" + e.entPath;
            }
            if (e.entPath.size() < 4 || e.entPath.substr(e.entPath.size() - 4) != ".ent") {
                e.entPath += ".ent";
            }
            e.hasFrame = true;
            const int slot = static_cast<int>(i % 8);
            e.world[0] = squadPos[0] + kRing[slot][0];
            e.world[1] = squadPos[1];
            e.world[2] = squadPos[2] + kRing[slot][1];
            for (int k = 0; k < 3; ++k) {
                e.pos[k] = static_cast<float>(e.world[k]) / kEntityWorldScale;
            }
            e.playerIndex = squadPlayerIndex;
            e.hasRotation = true;
            e.animationRotation = slot;
            // The .ent carries the character's own Base/Hair/Skin/Team records;
            // a .mis instance would carry them too, so read them off the
            // prototype for a member that has no .mis record at all.
            std::string perr;
            const EntProto* proto = protos.Get(e.entPath, &perr);
            if (proto == nullptr) {
                std::fprintf(stderr, "  SQUAD %s: %s\n", e.entPath.c_str(), perr.c_str());
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                e.hasColour[c] = proto->block.esh.GetColour(kEntityColourNames[c], e.colour[c]);
            }
            squadInsts.push_back(std::move(e));
        }
        if (!opt.quiet) {
            std::printf("squad               %zu members at world (%d, %d, %d)\n",
                        squadInsts.size(), squadPos[0], squadPos[1], squadPos[2]);
        }
    } else if (opt.haveSquad && !opt.quiet) {
        std::printf("squad               this map has no SpawnPoint; nothing placed\n");
    }

    if (opt.drawEntities) {
        const size_t mapCount = w.entityFile.entities.size();
        for (size_t n = 0; n < mapCount + squadInsts.size(); ++n) {
            EntityInstance inst =
                n < mapCount ? w.entityFile.entities[n] : squadInsts[n - mapCount];
            if (!inst.hasFrame) {
                continue;  // inventory contents and other unplaced entities
            }
            if (!opt.drawMarkers && IsEditorMarkerClass(inst.className)) {
                ++entMarkers;
                continue;
            }
            // --campaign-start.  World::ValidateSpawnPointsAndAssignTeamColors
            // @004f53d0 destroys, at mission load, every Actor whose owning
            // player slot is unfilled (`World::ValidateSpawnPointsAndAssignTeamColors@004f53d0`:
            // `bVar6 = IsActor(e); if (bVar6) DestructEntity(e);` under
            // `UnknownRelatedToPlayerType(player) != 0 && player[+4] == 0`).
            // The predicate's body is not in the decompilation and neither is
            // any <Player> reader, so the slot cannot be evaluated; what the
            // data shows is that the player index carried by the map's
            // SpawnPoint is the squad's slot, and the Actors sharing it are
            // generic placeholder characters standing on the spawn.  Those are
            // the ones the campaign replaces with the player's own squad, so
            // that is what this hides.  See NOTES.md section 24.
            if ((opt.campaignStart || opt.haveSquad) && n < mapCount &&
                inst.className == "Actor" && squadPlayerIndex >= 0 &&
                inst.playerIndex == squadPlayerIndex) {
                ++entPlaceholders;
                continue;
            }
            std::string protoErr;
            ApplyEntOverride(protos.Get(inst.entPath, &protoErr), &inst);
            if (inst.spritePath.empty()) {
                ++entMissing;
                entMissReasons["(no Sprite property) " + inst.className]++;
                continue;
            }
            LoadedSprite* ls = sprites.Get(inst.spritePath);
            if (!ls->ok) {
                ++entMissing;
                entMissReasons[inst.spritePath + " (" + ls->error + ")"]++;
                continue;
            }
            const SpritePick pick = PickSprite(ls->sprite, inst);
            if (!pick.ok) {
                ++entMissing;
                entMissReasons[inst.spritePath + " (" + pick.error + ")"]++;
                continue;
            }
            if (!ls->sprite.LoadCollectionImages(ls->file, static_cast<size_t>(pick.collection))) {
                ++entMissing;
                entMissReasons[inst.spritePath + " (collection " +
                               std::to_string(pick.collection) + ": " +
                               ls->sprite.collections[static_cast<size_t>(pick.collection)]
                                   .loadError +
                               ")"]++;
                continue;
            }
            const Collection& col = ls->sprite.collections[static_cast<size_t>(pick.collection)];
            const size_t rectIdx =
                static_cast<size_t>(pick.frame) * col.dirCount + pick.direction;
            if (rectIdx >= col.rects.size()) {
                ++entMissing;
                entMissReasons[inst.spritePath + " (rect index out of range)"]++;
                continue;
            }
            const Rect& rect = col.rects[rectIdx];

            EntityDraw d;
            d.instance = n;
            int32_t sx = 0, sy = 0;
            ProjectWorldToScreen(inst.world[0], inst.world[1], inst.world[2], &sx, &sy);
            // Entity::RenderObjectMaybe l.14615-14619 projects
            // `pos + SpriteXYZAdjust`; SpriteXYZAdjust is a runtime field that
            // is zero on a freshly loaded entity, so the projection is of the
            // stored position alone.  The sprite header's (offsetX, offsetY) is
            // the anchor: Entity_UpdateFacingFromVelocityAndBounds l.15519-15522
            // computes the entity's render bounds as `rect - spriteOffset`, and
            // SoftwareRender::RenderLitObject then adds
            // each layer's own (x, y), which the .spr stores relative to the
            // collection rect's origin (sprview/NOTES.md 1.5).
            d.x0 = sx + rect.x0 - ls->sprite.offsetX;
            d.y0 = sy + rect.y0 - ls->sprite.offsetY;
            d.depth = IsoDepth(inst.world[0], inst.world[1], inst.world[2]);
            d.elev = inst.world[1];
            d.wmin[0] = inst.world[0] - kEntityHalfExtent;
            d.wmin[1] = inst.world[1];
            d.wmin[2] = inst.world[2] - kEntityHalfExtent;
            d.wmax[0] = inst.world[0] + kEntityHalfExtent;
            d.wmax[1] = inst.world[1] + kEntityHeight;
            d.wmax[2] = inst.world[2] + kEntityHalfExtent;
            for (int k = 0; k < 3; ++k) {
                d.wpos[k] = inst.pos[k];
            }

            // Per-layer colour.  SoftwareRender::RenderLitObject @00783340
            // walks the frame record's four layer slots and, for each, passes
            // THAT LAYER'S OWN colour record into the blit:
            //
            //   for each of the 4 layer slots (16 bytes apart, from frame+0x18):
            //     if the slot has an image (an absent layer is skipped, but
            //     both cursors still step):
            //       place the layer at its own offset plus the screen point
            //       InitDefaultLightColors()
            //       SetSpecularLightColor(record, record.f[3], record.f[4])
            //       SetAmbientLightColor(the averaged LIGHTING colour)
            //     record += 0x14   (20 bytes: ONE 5-float record)
            //
            // The record cursor advances by **0x14 = 20 bytes = one 5-float
            // colour record** per layer, so layer `i` is blitted with record `i`
            // of the four contiguous records at Entity+0xB3: layer 0 <- Base,
            // layer 1 <- Hair, layer 2 <- Skin, layer 3 <- Team.  The 0xc and
            // 0x10 arguments are that record's float[3] and float[4], handed to
            // SetSpecularLightColor as separate scalars, which is what
            // identifies float[3] as the weight rather than a fourth channel.
            //
            // This also settles the stride puzzle: there are TWO buffers.
            // One is the entity's four 5-float records (stride 0x14, per
            // layer, specular); the other is the 4x3 lighting corner buffer
            // (stride 3), which the function averages into one colour and
            // passes to SetAmbientLightColor once for every layer.  With no
            // scene lighting that ambient is flat, so the entity's own per-layer
            // colour is the whole of the tint.
            DrawTint layerTint[kLayerCount];
            BuildLayerTints(inst, opt.ambient, opt.layerSlot, opt.tint == kTintOn, layerTint);

            for (int l = 0; l < kLayerCount; ++l) {
                const LayerImage& li = col.images[static_cast<size_t>(
                    col.LayerIndex(pick.frame, pick.direction, l))];
                if (!li.present) {
                    continue;
                }
                DecodedLayer& dl = d.layers[static_cast<size_t>(l)];
                std::string derr;
                ZarImage& zar = const_cast<ZarImage&>(li.zar);
                if (!zar.DecodeToRGBA(col.palettes[static_cast<size_t>(l)], &dl.rgba, &derr)) {
                    continue;
                }
                dl.present = true;
                dl.x = li.x;
                dl.y = li.y;
                dl.w = zar.Width();
                dl.h = zar.Height();

                d.tint[l] = layerTint[l];
            }
            entities.push_back(std::move(d));
            ++entResolved;
        }
        if (!opt.quiet) {
            std::printf("entities placed     %zu of %zu (%zu sprite lookups, %zu editor markers "
                        "hidden, %zu squad placeholders removed)\n",
                        entResolved, w.entityFile.entities.size(), sprites.Size(), entMarkers,
                        entPlaceholders);
            std::printf("sprites resolved/missing: %zu/%zu\n", entResolved, entMissing);
            for (const auto& kv : entMissReasons) {
                std::fprintf(stderr, "  MISSING SPRITE %s x%zu\n", kv.first.c_str(), kv.second);
            }
        }
    }

    // 1d. MILESTONE 5: the scene lighting.
    //
    // UpdateEntityRenderTransform @006e1c10 fills
    // the render manager's lighting inputs once per frame:
    //   M+0xA0 = DAT_008bda50 ? World+0x5D : light_level_maybe   (the base)
    //   M+0xAC = World+0x69                                      (the diffuse)
    //   M+0xC5..0xD1 = World+0x81..0x8D                          (the fog base)
    // DAT_008bda50's ONLY writer is FUN_0041c130+0x2fa, one of a block of
    // ~15 `sete al` menu toggles reached from
    // WEditorMap::BuildMapEditorEditMenu+0x15d8, and it lives above
    // 0x8BC000 -- past .data's 0x28000 of raw bytes -- so it is 0 at load and
    // stays 0 in the game.  The shipped base is therefore the constant white
    // and `--light-base map` is the editor's other branch.
    SceneLighting scene;
    scene.enabled = opt.lights;
    scene.time = opt.haveTime ? WorldTime::FromHourMinute(opt.timeHour, opt.timeMinute)
                              : WorldTime::FromMillis(0);
    for (int k = 0; k < 3; ++k) {
        scene.base[k] = opt.lightBase == LightBase::kMap ? w.vec1[k] : 1.0f;
        scene.diffuse[k] = w.vec2[k];
    }
    if (opt.lightBase == LightBase::kClock) {
        float diff[3], amb[3];
        ComputeTimeOfDayLighting(scene.time, diff, amb);
        for (int k = 0; k < 3; ++k) {
            scene.base[k] = amb[k];
            scene.diffuse[k] = diff[k];
        }
    }
    if (opt.neutralRig) {
        scene.addDiffuse = false;
    }
    size_t lightCount = 0, lightSkipped = 0;
    for (const EntityInstance& e : w.entityFile.entities) {
        if (e.className != "Light" || !e.hasFrame) {
            continue;
        }
        ++lightCount;
        const Esh* protoEsh = nullptr;
        if (!e.entPath.empty()) {
            std::string perr;
            const EntProto* pr = protos.Get(e.entPath, &perr);
            if (pr != nullptr) {
                protoEsh = &pr->block.esh;
            }
        }
        SceneLight L = BuildSceneLight(e, protoEsh);
        L.AdvanceTicks(opt.tick);
        if (!L.InRenderList()) {
            ++lightSkipped;
        }
        scene.lights.push_back(std::move(L));
    }
    if (!opt.quiet) {
        std::printf("time of day         %s\n", scene.time.Describe().c_str());
        std::printf("lights              %zu placed, %zu active, %zu in the fog list "
                    "(base %.3f %.3f %.3f, diffuse %.3f %.3f %.3f)%s\n",
                    lightCount, scene.ActiveCount(), lightSkipped, scene.base[0], scene.base[1],
                    scene.base[2], scene.diffuse[0], scene.diffuse[1], scene.diffuse[2],
                    opt.lights ? "" : "  [--lights off]");
    }

    // 2. Screen rect of every drawable instance, and the union.
    struct Placed {
        size_t instance;
        int32_t x0, y0;
        int32_t x1, y1;   // element+0x4d / +0x51
        int64_t depth;
        int32_t elev;
        bool floor;
    };
    std::vector<Placed> placed;
    size_t roofHidden = 0;
    size_t raisedFloors = 0;
    placed.reserve(w.instances.size());

    // MILESTONE 15, the bridge.  Pass 1 (floors) exists because a floor tile
    // nearer the camera must never paint over what stands on the tile behind
    // it, and section 18 justified running it wholesale ahead of pass 2 with
    // "coplanar floors cannot occlude one another".  That premise is false for
    // a floor that is NOT part of the ground sheet: mission01's bridge deck is
    // twenty typeId-1 tiles at elevation 128 spanning the canyon, and every one
    // of them was painted in pass 1 and then overpainted in pass 2 by the
    // canyon's ROCKruff wall tiles (typeId 0) lower down and further away.
    // Verified with `--probe 351,129,339`: the deck tile is drawn, then
    // instances 3693 and 3760 (y 103..109, z 359..371) paint over it.
    //
    // The engine has no such wholesale split to reproduce.  Within a region the
    // tile order is one static depth order --
    // `std::IntrosortEntityPointersByField55` over the whole region, which
    // `BuildVisibleTileRenderCache @006e2870` then preserves 1:1
    // (`BuildVisibleTileRenderCache@006e2870`) -- and it does not partition on typeId at all;
    // `ComputeElementScreenBoundsAndHighlight @006e2c60` gates a tile only on
    // instance-flag bit 0x80 in the game branch, so
    // nothing there excuses the deck either.  What pass 1 buys mapview is the
    // ground sheet, so keep it for the ground sheet only: a floor tile that has
    // another floor tile beneath it in the same footprint cell is a raised deck
    // -- a bridge, a catwalk, a bunker ceiling -- and goes through the pass-2
    // sort, where `CompareEntityDrawOrder @007427c0`'s three-axis separating
    // test orders it against the canyon walls correctly.
    // How far above the floor beneath it a floor tile may sit and still count
    // as a deck laid ON the ground sheet rather than a storey above it.  A
    // floor cell is 6x1x6 world units (section 1), so one unit is the thickness
    // of the deck itself: the bridge deck is at 128 over ground at 127.  Bunker
    // Alpha's black ceiling is at 179 over floors at 127 and its upper storeys
    // are a full wall height up, so neither is promoted and pass 1 keeps them
    // out of the way of the objects below -- which is what section 26's
    // verification renders check.
    static const int32_t kDeckLift = 4;
    std::unordered_map<uint64_t, int32_t> lowestFloorInCell;
    for (const TileInstance& in : w.instances) {
        if (in.protoId <= 0 || static_cast<size_t>(in.protoId) >= w.protos.size()) {
            continue;
        }
        if (w.protos[static_cast<size_t>(in.protoId)].header.typeId != 1) {
            continue;
        }
        const uint64_t cell = (static_cast<uint64_t>(static_cast<uint32_t>(in.min[0])) << 32) |
                              static_cast<uint32_t>(in.min[2]);
        auto it = lowestFloorInCell.find(cell);
        if (it == lowestFloorInCell.end()) {
            lowestFloorInCell.emplace(cell, in.min[1]);
        } else if (in.min[1] < it->second) {
            it->second = in.min[1];
        }
    }
    // --inside x,z with no elevation: take the lowest tile elevation whose
    // footprint covers (x, z).  That is the floor the reference point stands on.
    RenderOptions ropt = opt;
    if (ropt.haveInside && ropt.inside[1] == INT64_MIN) {
        int64_t best = INT64_MAX;
        for (const TileInstance& in : w.instances) {
            if (in.protoId <= 0) continue;
            if (in.min[0] <= ropt.inside[0] && ropt.inside[0] <= in.max[0] &&
                in.min[2] <= ropt.inside[2] && ropt.inside[2] <= in.max[2]) {
                best = std::min<int64_t>(best, in.min[1]);
            }
        }
        ropt.inside[1] = (best == INT64_MAX) ? 0 : best;
        if (!opt.quiet) {
            std::printf("inside reference    (%lld, %lld, %lld)  k=%g\n",
                        (long long)ropt.inside[0], (long long)ropt.inside[1],
                        (long long)ropt.inside[2], ropt.insideK);
        }
    }
    int64_t minX = INT64_MAX, minY = INT64_MAX, maxX = INT64_MIN, maxY = INT64_MIN;
    for (size_t n = 0; n < w.instances.size(); ++n) {
        const TileInstance& inst = w.instances[n];
        const DecodedTile& d = decoded[static_cast<size_t>(inst.protoId)];
        if (!d.ok) {
            continue;
        }
        if (inst.min[1] > opt.maxElev) {
            continue;
        }
        // The Invisible Tile, PORTED.  ComputeElementScreenBoundsAndHighlight
        // @006e2c60 ends, on the game branch (DAT_008bda4c == 0, the
        // lighting/editor master that WGameMain sets to 0 and WEditorMain to
        // 1), by drawing the tile unless bit 0x80 is set in the ushort at
        // +0x02 of the 0x38-byte tile element -- the INSTANCE flags word, not
        // the prototype's.  Milestone 3
        // could only infer this bit from the .til files (NOTES.md section 16);
        // it is now the binary's own test.  The data agrees exactly: over all
        // 87 shipped maps, 879 instances carry bit 0x80 and all 879 of them
        // reference `tiles/Generic tiles/Invisible Tile.til`, and every
        // Invisible Tile placement carries the bit.  (The editor branch instead
        // consults DAT_008bda3b, which is why the tile is visible in the
        // editor.)
        if (!opt.showInvisibleTiles && TileInstanceInvisible(inst)) {
            ++hiddenInstances;
            continue;
        }
        // Roof hiding, PORTED.  Same function
        // (`ComputeElementScreenBoundsAndHighlight@006e2c60`), in the block
        // that runs on the game branch only.  The reference point (refX, refY,
        // refZ) is the three ints at +0x70, +0x74 and +0x78 of the view state:
        //
        //   if (   refY + 8 <= elem.min.y                          l.4489
        //       && elem.min.x < refX - 4                           l.4490
        //       && elem.min.z < refZ - 4                           l.4491
        //       && elem is a tile with a loaded prototype)         l.4492
        //     if (proto.bbox.x * proto.bbox.z > 4                  l.4503
        //         && (cz - refZ)^2 + (cx - refX)^2
        //              < ((elem.max.y - refY) * K)^2)              l.4505-4508
        //       SetElementPropertyRecursive(wrapper, 1, 1, 1, ...) l.4509
        //
        // with cx = (min.x + max.x)/2 and cz = (min.z + max.z)/2 (l.4483,
        // l.4506).  The two byte fields multiplied at l.4503 are Tile+0x14 and
        // Tile+0x12, i.e. the prototype's bbox.x and bbox.z (til.h) -- the test
        // is "this is a big enough tile to be worth fading", which excludes
        // single-cell clutter.  The asymmetric `min < ref - 4` pair is not a
        // bug: screen-up is -x and -z, so a tile whose min corner is beyond the
        // reference point on both axes is exactly one that paints over it.
        //
        // K is _DAT_00830b10, a double constant Ghidra did not resolve, so the
        // radius scale is the one number in this rule that is not recoverable;
        // --inside-k sets it; the default 3.0 is the smallest
        // whole number that clears Bunker Alpha's ceiling from the room the
        // reference point stands in.  Everything else is verbatim.
        if (ropt.haveInside && TileInstanceRoofHidden(w, inst, ropt.inside, ropt.insideK)) {
            ++roofHidden;
            continue;
        }
        if (opt.haveRegion) {
            if (inst.min[0] < opt.region[0] || inst.min[0] > opt.region[2] ||
                inst.min[2] < opt.region[1] || inst.min[2] > opt.region[3]) {
                continue;
            }
        }
        int32_t sx = 0, sy = 0;
        ProjectWorldToScreen(inst.min[0], inst.min[1], inst.min[2], &sx, &sy);
        Placed p;
        p.instance = n;
        p.x0 = sx + d.drawX;
        p.y0 = sy + d.drawY;
        p.x1 = p.x0 + d.width;
        p.y1 = p.y0 + d.height;
        p.depth = IsoDepth(inst.max[0], inst.max[1], inst.max[2]);
        p.elev = inst.min[1];
        // typeId 1 is the floor class: every tile under "Generic floors" has it,
        // and object tiles (pots, poles, rocks, crates) are 2 and up.
        p.floor = w.protos[static_cast<size_t>(inst.protoId)].header.typeId == 1;
        if (p.floor) {
            const uint64_t cell = (static_cast<uint64_t>(static_cast<uint32_t>(inst.min[0])) << 32) |
                                  static_cast<uint32_t>(inst.min[2]);
            const auto it = lowestFloorInCell.find(cell);
            const int32_t lift =
                (it == lowestFloorInCell.end()) ? 0 : (inst.min[1] - it->second);
            if (lift > 0 && lift <= kDeckLift) {
                p.floor = false;  // a deck laid on the ground sheet; see above
                ++raisedFloors;
            }
        }
        placed.push_back(p);
        minX = std::min<int64_t>(minX, p.x0);
        minY = std::min<int64_t>(minY, p.y0);
        maxX = std::max<int64_t>(maxX, p.x0 + d.width);
        maxY = std::max<int64_t>(maxY, p.y0 + d.height);
    }
    for (EntityDraw& d : entities) {
        bool any = false;
        for (const DecodedLayer& l : d.layers) {
            if (!l.present) {
                continue;
            }
            minX = std::min<int64_t>(minX, d.x0 + l.x);
            minY = std::min<int64_t>(minY, d.y0 + l.y);
            maxX = std::max<int64_t>(maxX, d.x0 + l.x + l.w);
            maxY = std::max<int64_t>(maxY, d.y0 + l.y + l.h);
            // element+0x41..+0x51, the union of the layer rects.
            if (!any) {
                d.rect[0] = d.x0 + l.x;
                d.rect[1] = d.y0 + l.y;
                d.rect[2] = d.x0 + l.x + l.w;
                d.rect[3] = d.y0 + l.y + l.h;
                any = true;
            } else {
                d.rect[0] = std::min(d.rect[0], d.x0 + l.x);
                d.rect[1] = std::min(d.rect[1], d.y0 + l.y);
                d.rect[2] = std::max(d.rect[2], d.x0 + l.x + l.w);
                d.rect[3] = std::max(d.rect[3], d.y0 + l.y + l.h);
            }
        }
    }
    if (placed.empty() && entities.empty()) {
        *errorOut = "nothing to draw";
        return false;
    }

    const int64_t fullW = maxX - minX;
    const int64_t fullH = maxY - minY;
    if (fullW <= 0 || fullH <= 0 || fullW > 200000 || fullH > 200000) {
        *errorOut = "implausible image size";
        return false;
    }
    if (!opt.quiet) {
        std::printf("screen extents      x [%lld, %lld)  y [%lld, %lld)  -> %lldx%lld px\n",
                    (long long)minX, (long long)maxX, (long long)minY, (long long)maxY,
                    (long long)fullW, (long long)fullH);
        std::printf("drawing             %zu of %zu tile instances, %zu entities\n",
                    placed.size(), w.instances.size(), entities.size());
        std::printf("hidden              %zu invisible-tile placements, %zu roof placements\n",
                    hiddenInstances, roofHidden);
    }

    // 3. Draw order.  **Floors first, then everything else in one sorted list.**
    //
    //    Both callers issue two draw calls with the entity list rebuilt between
    //    them -- RenderMapToImageFiles @006ea2c0 and
    //    WWorldWindow::RenderWorldView @006bbb00 (l.7552-7560) are the same
    //    sequence:
    //
    //        *(map+0x2c)->0x2c = drawContext;
    //        (**(code **)(**(int **)(param + 0x2c) + 0x44))();   // pass 1
    //        RefreshRegionEntityRenderList(*(int *)(param + 0x2c), status);
    //        (**(code **)(**(int **)(param + 0x2c) + 0x48))();   // pass 2
    //
    //    so there really are two passes and entities belong to the second.
    //    What the two vtable slots draw is NOT in the decompilation (they are
    //    indirect calls and neither body was supplied), so the split between
    //    them is settled by the data instead: pass 1 is the floor tiles and
    //    pass 2 is object tiles and entities together.  That is forced by two
    //    observations that rule out the alternatives:
    //      * one merged list for everything (what milestone 3 first did) lets a
    //        floor tile nearer the camera paint over an entity standing on the
    //        tile behind it -- clipped critters, objects cut by a clean diamond
    //        edge, crates that look as if they float;
    //      * all tiles then all entities makes walls and roofs stop occluding,
    //        so raiders stand on top of the tents they are inside.
    //    Floors first, then objects and entities sorted together, is the only
    //    split of these two calls that produces neither artefact.
    //
    //    Within each pass: ascending depth, std::stable_sort so ties keep file
    //    order, matching the original's per-region lists (each sorted by the int
    //    at element+0x55 by std::IntrosortEntityPointersByField55,
    //    `MapRegion::LoadRegionFromStream@00740350`).
    //
    //    MILESTONE 4.  The comparator is now the binary's, not a
    //    reconstruction.  Two things came out of the new decompilation:
    //
    //    (a) The per-region static order is DESCENDING, not ascending.
    //        std::IntrosortEntityPointersByField55 @007424a0
    //        shifts an element earlier while its predecessor's key
    //        is LESS than its own (both are the int key at +0x55) -- so
    //        the array comes out largest-first.
    //        Milestone 3 read that sort as ascending.
    //
    //    (b) The per-frame order of the pass-2 list is
    //        `SortEntitiesByDrawOrder(iVar1 + 0x60)`, called from
    //        RefreshRegionEntityRenderList @006e4390 over
    //        the render-cache WRAPPERS it has just filled.  That body is not in
    //        the dump, but `std::IntrosortLoopByKeyThenId` @00504479
    // (`std::IntrosortLoopByKeyThenId@005043d0`) is, and it sorts an array of pointers by
    //        `*(int *)(p + 0x20)` ascending with `*(int *)(p + 8)` descending as
    //        the tie-break -- which is exactly the wrapper's layout as
    //        ComputeElementScreenBoundsAndHighlight @006e2c60 writes it:
    //
    //            wrapper + 0x04..0x18   world min/max, floats (l.4416-4421,
    //                                   l.4454-4461)
    //            wrapper + 0x08         world y == elevation
    //            wrapper + 0x1c..0x28   screen AABB x0, y0, x1, y1
    //                                   (l.4438-4441, l.4466-4469)
    //            wrapper + 0x2c         round(elevation * 4)
    //
    //        So the key is the SCREEN Y0 of the element's box, ascending, and
    //        the tie-break is world elevation, descending.  In this projection
    //        (screenY = -3*(x+z) - 7*y) a larger elevation at equal screen y0
    //        means a larger x+z, i.e. further from the camera, so "higher
    //        first" is "further first" -- which reads like a coherent painter's
    //        order.  It is `--order screeny`, and it is NOT the default,
    //        because it was implemented, rendered and REJECTED:
    //
    //        Bunker Alpha's ceiling is 7,225 tiles at elevation 179 over floors
    //        at 127/128.  Sorting by screen y0 puts every one of them before
    //        every floor tile they cover -- a tall element's box starts high on
    //        screen whatever its depth -- so the roof stops occluding entirely
    //        and the bunker renders as if it had no ceiling at all.  That is
    //        exactly the failure NOTES.md section 18 recorded for "all tiles,
    //        then all entities" (renders of bunker01 in both orders show
    //        it).  So the field match is a
    //        coincidence: `std::IntrosortLoopByKeyThenId` sorts some other
    //        pointer array (MSVC6 folds identical template COMDATs, so its
    //        address says nothing about which array), and
    //        `SortEntitiesByDrawOrder`'s own body is still absent.
    //
    //    (c) What (a) DOES settle is that milestone 3's reconstruction points
    //        the right way.  `depth = 6*y - 7*(x+z)` ascending is the exact
    //        negation of `+0x55` descending, so the two orders are the same
    //        order; milestone 3 had the key's sign and the sort's direction
    //        both inverted, and the two errors cancelled.  That is why its
    //        renders were right.  The magnitude of the key is still a
    //        reconstruction from the projection matrix -- `+0x55` is written by
    //        `Entity::CloneEntityWithNewId`, which is still not in any dump.
    //
    //    MILESTONE 5, draw order.  Phase 2 is now ported; see sort.h and
    //    section 26.  What changed here:
    //
    //    * The floor pass keeps the old scalar sort.  It is pass 1, it runs
    //      before the engine's list even exists, and coplanar floors cannot
    //      occlude one another.
    //    * The object+entity pass is now `SortEntitiesByDrawOrder` @007426b0
    //      verbatim: an unstable descending sort on the cached depth, then the
    //      topological repair pass with `CompareEntityDrawOrder` @007427c0 and
    //      the +0x3d generation stamp.  That is what makes a multi-cell object
    //      whole where it abuts a wall: the scalar key speaks for the element's
    //      max corner only, and phase 2 is the engine's own correction for it.
    //    * The depth pair is now an interval, `depth0`/`depth1` = the min and
    //      max of `7*(x+z) - 6*y` over the element's world box, which is
    //      element+0x49/+0x55 up to sign.  `depth1` descending is the identical
    //      order to milestone 3's `IsoDepth` ascending on the max corner except
    //      in the y term (min vs max elevation), which the engine's own
    //      disjoint-interval test in `PartitionEntitiesByDrawOrder` forces.
    struct DrawRef {
        int64_t key;
        int32_t tie;
        size_t index;
        bool entity;
    };
    const bool screenY = opt.sortRule == kSortScreenY;
    std::vector<DrawRef> floors, rest;
    floors.reserve(placed.size());
    rest.reserve(placed.size() + entities.size());
    for (size_t i = 0; i < placed.size(); ++i) {
        const DrawRef r{screenY ? placed[i].y0 : placed[i].depth, placed[i].elev, i, false};
        (placed[i].floor ? floors : rest).push_back(r);
    }
    for (size_t i = 0; i < entities.size(); ++i) {
        rest.push_back({screenY ? entities[i].y0 : entities[i].depth, entities[i].elev, i, true});
    }
    auto byKey = [screenY](const DrawRef& a, const DrawRef& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        return screenY ? a.tie > b.tie : false;
    };
    if (!opt.quiet) {
        std::printf("raised floors       %zu floor tiles moved to the object pass\n", raisedFloors);
    }
    std::stable_sort(floors.begin(), floors.end(), byKey);
    std::stable_sort(rest.begin(), rest.end(), byKey);

    if (!screenY) {
        // element+0x49 / +0x55: the min and max of the game's depth
        // `7*(x+z) - 6*y` over the element's world box.
        auto gameDepth = [](int64_t x, int64_t y, int64_t z) {
            return static_cast<int64_t>(kIsoHeight) * (x + z) - 2LL * kIsoHalfWidth * y;
        };
        std::vector<SortElem> elems(rest.size());
        std::vector<SortElem*> order(rest.size());
        for (size_t i = 0; i < rest.size(); ++i) {
            SortElem& e = elems[i];
            e.index = static_cast<uint32_t>(rest[i].index);
            e.entity = rest[i].entity;
            if (rest[i].entity) {
                const EntityDraw& d = entities[rest[i].index];
                for (int k = 0; k < 3; ++k) {
                    e.wmin[k] = d.wmin[k];
                    e.wmax[k] = d.wmax[k];
                }
                e.sx0 = d.rect[0];
                e.sy0 = d.rect[1];
                e.sx1 = d.rect[2];
                e.sy1 = d.rect[3];
                e.flags = 0;   // +0x38 bit0 clear: entity
                e.id = static_cast<uint32_t>(rest[i].index);
            } else {
                const Placed& p = placed[rest[i].index];
                const TileInstance& in = w.instances[p.instance];
                for (int k = 0; k < 3; ++k) {
                    e.wmin[k] = in.min[k];
                    e.wmax[k] = in.max[k];
                }
                e.sx0 = p.x0;
                e.sy0 = p.y0;
                e.sx1 = p.x1;
                e.sy1 = p.y1;
                e.flags = 1;   // +0x38 bit0 set: tile
                e.id = static_cast<uint32_t>(in.protoId);
            }
            e.depth1 = gameDepth(e.wmax[0], e.wmin[1], e.wmax[2]);
            e.depth0 = gameDepth(e.wmin[0], e.wmax[1], e.wmin[2]);
            order[i] = &e;
        }
        int32_t generation = 0;   // DAT_008b0a60
        SortEntitiesByDrawOrder(order, &generation);
        std::vector<DrawRef> sorted;
        sorted.reserve(order.size());
        for (const SortElem* e : order) {
            sorted.push_back({0, 0, e->index, e->entity});
        }
        rest.swap(sorted);
    }

    // 4. Blit, source-over, full bright.
    std::vector<uint8_t> image(static_cast<size_t>(fullW) * static_cast<size_t>(fullH) * 4);
    for (size_t i = 0; i + 3 < image.size(); i += 4) {
        image[i] = static_cast<uint8_t>(opt.background >> 16);
        image[i + 1] = static_cast<uint8_t>(opt.background >> 8);
        image[i + 2] = static_cast<uint8_t>(opt.background);
        image[i + 3] = static_cast<uint8_t>(opt.background >> 24);
    }
    // MILESTONE 5 (revised).  The shipped game runs the DirectX7 renderer, and
    // its tile path is GOURAUD: DirectX7Render::RenderVertexLitObject @00743450
    // (device vtable slot 6) hands the element's FOUR corner colours to
    // DirectX7Render::RenderTrianglesWithVertexLighting @0077a6f0, whose first
    // block is a four-iteration loop (disassembly 0077a763-0077a8a3, `mov ebx,4`
    // at 0077a71f, `add edi,0xc` per corner) that packs each corner into its own
    // D3DCOLOR and submits a two-triangle quad.  So the four colours are
    // INTERPOLATED across the element, which is why the game's light pools are
    // smooth.  `SoftwareRender::RenderLitObject`'s 0.25*(c0+c1+c2+c3) flat
    // average -- what milestone 5 first ported -- is the software FALLBACK, and
    // it is what made the pools blocky.
    // engine/shared.h has LightQuad, BuildLightQuad and BlitRGBA; `blit` and
    // `buildQuad` here are one-line adapters that supply this render's
    // whole-map target and its (minX, minY) origin.
    const BlitTarget target{image.data(), static_cast<int32_t>(fullW), static_cast<int32_t>(fullH),
                            minX, minY};
    auto blit = [&](const uint8_t* rgba, int32_t width, int32_t height, int64_t dx, int64_t dy,
                    const DrawTint& tint, const LightQuad* quad = nullptr) {
        BlitRGBA(target, rgba, width, height, dx, dy, tint, quad);
    };

    // A tile takes only the trailing `c * A`: BlitSpriteWithLighting sets the
    // diffuse from the tile's own colour and its scalar is 0 with no scene
    // light loaded, and it never touches the specular.
    DrawTint tileTint;
    for (int k = 0; k < 3; ++k) {
        tileTint.A[k] = opt.ambient[k];
    }
    const bool sceneLit = opt.lights || opt.lightBase != LightBase::kWhite;
    auto buildQuad = [&](const TileInstance& in, int shapeClass, LightQuad* q) {
        BuildLightQuad(scene, in, shapeClass, q);
    };
    auto drawTile = [&](size_t i) {
        const Placed& p = placed[i];
        const TileInstance& in = w.instances[p.instance];
        const DecodedTile& d = decoded[static_cast<size_t>(in.protoId)];
        LightQuad q;
        if (sceneLit) {
            const TileHeader& th = w.protos[static_cast<size_t>(in.protoId)].header;
            buildQuad(in, ClassifyTileShape(th.bboxX, th.bboxY, th.bboxZ), &q);
        }
        blit(d.rgba.data(), d.width, d.height, p.x0 - minX, p.y0 - minY, tileTint,
             q.on ? &q : nullptr);
    };
    // Four layers per entity, in slot order, exactly as
    // SoftwareRender::RenderLitObject walks them.
    auto drawEntity = [&](size_t i) {
        const EntityDraw& e = entities[i];
        float A[3] = {1.0f, 1.0f, 1.0f};
        if (sceneLit) {
            scene.AmbientForPoint(e.wpos, A);
        }
        for (int l = 0; l < kLayerCount; ++l) {
            const DecodedLayer& dl = e.layers[static_cast<size_t>(l)];
            if (!dl.present) {
                continue;
            }
            DrawTint t = e.tint[l];
            for (int k = 0; k < 3; ++k) {
                t.A[k] *= A[k];
            }
            blit(dl.rgba.data(), dl.w, dl.h, e.x0 + dl.x - minX, e.y0 + dl.y - minY, t);
        }
    };
    // fix3 diagnostic: --probe wx,wy,wz logs every draw that changes the pixel
    // the world point projects to, in paint order.
    int64_t probePix = -1;
    if (opt.haveProbe) {
        int32_t px = 0, py = 0;
        ProjectWorldToScreen(static_cast<int32_t>(opt.probe[0]), static_cast<int32_t>(opt.probe[1]),
                             static_cast<int32_t>(opt.probe[2]), &px, &py);
        const int64_t ix = px - minX, iy = py - minY;
        if (ix >= 0 && iy >= 0 && ix < fullW && iy < fullH) {
            probePix = (iy * fullW + ix) * 4;
            std::printf("probe               world (%lld,%lld,%lld) -> screen %d,%d -> px %lld,%lld\n",
                        (long long)opt.probe[0], (long long)opt.probe[1], (long long)opt.probe[2], px,
                        py, (long long)ix, (long long)iy);
        }
    }
    uint32_t probePrev = 0;
    auto probeAfter = [&](const char* what, size_t idx, bool ent) {
        if (probePix < 0) return;
        uint32_t now;
        std::memcpy(&now, image.data() + probePix, 4);
        if (now == probePrev) return;
        probePrev = now;
        if (ent) {
            std::printf("  probe %-6s entity #%zu -> %08x\n", what, idx, now);
        } else {
            const Placed& pp = placed[idx];
            const TileInstance& in = w.instances[pp.instance];
            std::printf("  probe %-6s tile inst=%zu proto=%d y=%d..%d x=%d..%d z=%d..%d t%d %s -> %08x\n",
                        what, pp.instance, in.protoId, in.min[1], in.max[1], in.min[0], in.max[0],
                        in.min[2], in.max[2],
                        w.protos[static_cast<size_t>(in.protoId)].header.typeId,
                        w.protos[static_cast<size_t>(in.protoId)].name.c_str(), now);
        }
    };
    for (const DrawRef& r : floors) {
        drawTile(r.index);
        probeAfter("pass1", r.index, false);
    }
    for (const DrawRef& r : rest) {
        if (r.entity) {
            drawEntity(r.index);
        } else {
            drawTile(r.index);
        }
        probeAfter("pass2", r.index, r.entity);
    }

    // 5. Crop (output pixel coordinates), then scale.
    int64_t outX = 0, outY = 0, outW = fullW, outH = fullH;
    if (opt.haveCentre && !opt.haveCrop) {
        int32_t cx = 0, cy = 0;
        ProjectWorldToScreen(static_cast<int32_t>(opt.centre[0]),
                             static_cast<int32_t>(opt.centre[1]),
                             static_cast<int32_t>(opt.centre[2]), &cx, &cy);
        outW = std::min<int64_t>(opt.size[0], fullW);
        outH = std::min<int64_t>(opt.size[1], fullH);
        outX = std::min<int64_t>(std::max<int64_t>(0, cx - minX - outW / 2), fullW - outW);
        outY = std::min<int64_t>(std::max<int64_t>(0, cy - minY - outH / 2), fullH - outH);
        if (!opt.quiet) {
            std::printf("centre              world (%lld, %lld, %lld) -> crop %lld,%lld,%lld,%lld\n",
                        (long long)opt.centre[0], (long long)opt.centre[1],
                        (long long)opt.centre[2], (long long)outX, (long long)outY,
                        (long long)outW, (long long)outH);
        }
    }
    if (opt.haveCrop) {
        outX = std::max<int64_t>(0, opt.crop[0]);
        outY = std::max<int64_t>(0, opt.crop[1]);
        outW = std::min<int64_t>(opt.crop[2], fullW - outX);
        outH = std::min<int64_t>(opt.crop[3], fullH - outY);
        if (outW <= 0 || outH <= 0) {
            *errorOut = "crop is outside the image";
            return false;
        }
    }

    const int scale = std::max(1, opt.scale);
    const int64_t finalW = (outW + scale - 1) / scale;
    const int64_t finalH = (outH + scale - 1) / scale;
    std::vector<uint8_t> out(static_cast<size_t>(finalW) * static_cast<size_t>(finalH) * 4);
    for (int64_t y = 0; y < finalH; ++y) {
        for (int64_t x = 0; x < finalW; ++x) {
            uint32_t acc[4] = {0, 0, 0, 0};
            uint32_t n = 0;
            for (int sy = 0; sy < scale; ++sy) {
                const int64_t py = outY + y * scale + sy;
                if (py >= outY + outH) {
                    break;
                }
                for (int sx = 0; sx < scale; ++sx) {
                    const int64_t px = outX + x * scale + sx;
                    if (px >= outX + outW) {
                        break;
                    }
                    const uint8_t* s = image.data() + (static_cast<size_t>(py) * fullW + px) * 4;
                    acc[0] += s[0];
                    acc[1] += s[1];
                    acc[2] += s[2];
                    acc[3] += s[3];
                    ++n;
                }
            }
            uint8_t* dst = out.data() + (static_cast<size_t>(y) * finalW + x) * 4;
            for (int c = 0; c < 4; ++c) {
                dst[c] = n ? static_cast<uint8_t>(acc[c] / n) : 0;
            }
        }
    }

    if (!WritePng(outPath, static_cast<int32_t>(finalW), static_cast<int32_t>(finalH), out,
                  errorOut)) {
        return false;
    }
    if (!opt.quiet) {
        std::printf("wrote %s (%lldx%lld)\n", outPath.c_str(), (long long)finalW, (long long)finalH);
    }
    return true;
}

// ---------------------------------------------------------------------------
// --selftest
// ---------------------------------------------------------------------------
int CmdSelftest(const std::vector<std::string>& roots, const std::string& tileDir,
                const std::string& spriteDir, const std::string& entDir) {
    std::vector<std::string> files;
    for (const std::string& root : roots) {
        std::error_code ec;
        if (fs::is_regular_file(root, ec)) {
            files.push_back(root);
            continue;
        }
        for (auto it = fs::recursive_directory_iterator(
                 root, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".mis") {
                files.push_back(it->path().string());
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    TileIndex index(tileDir, "tiles/", ".til");
    PathIndex spriteIndex(spriteDir, "sprites/", ".spr");
    PathIndex entIndex(entDir, "entities/", ".ent");
    std::printf("tile index:   %zu files under %s\n", index.Size(), tileDir.c_str());
    std::printf("sprite index: %zu files under %s\n", spriteIndex.Size(), spriteDir.c_str());
    std::printf("ent index:    %zu files under %s\n\n", entIndex.Size(), entDir.c_str());
    EntProtoCache protoCache(&entIndex);
    SpriteCache spriteCache(&spriteIndex);
    size_t entTotal = 0, entPlaced = 0, sprOk = 0, sprMissing = 0, entMissing = 0;
    std::map<std::string, size_t> entClassCounts, sprMissingNames, entMissingNames, eshErrors;

    size_t ok = 0, failed = 0, protoResolved = 0, protoMissing = 0;
    // Per-map sprite resolution and ambient, as milestone 4 asks for.
    struct PerMap {
        std::string name;
        size_t sprOk = 0, sprMiss = 0;
        float ambient[3] = {0, 0, 0};
        size_t invisible = 0;
        // MILESTONE 5
        size_t lights = 0, lightsNightOnly = 0, lightsArc = 0, lightsFlicker = 0, lightsFog = 0;
        float unlit[3] = {0, 0, 0};   // the vertex colour a point no light reaches gets
    };
    std::vector<PerMap> perMap;
    std::map<std::string, size_t> ambientHist;
    std::map<std::string, size_t> sectionCounts;
    std::map<std::string, size_t> missingNames;
    std::map<std::string, size_t> errors;
    for (const std::string& f : files) {
        World w;
        std::string err;
        if (!w.Load(f, &err)) {
            ++failed;
            errors[err]++;
            std::printf("FAIL  %-64s %s\n", fs::path(f).filename().string().c_str(), err.c_str());
            continue;
        }
        ++ok;
        PerMap pm;
        pm.name = fs::path(f).filename().string();
        for (int k = 0; k < 3; ++k) {
            pm.ambient[k] = w.vec1[k];
        }
        for (const TileInstance& in : w.instances) {
            if ((in.flags & 0x0080) != 0) {
                ++pm.invisible;
            }
        }
        char abuf[64];
        std::snprintf(abuf, sizeof abuf, "%.6f %.6f %.6f", w.vec1[0], w.vec1[1], w.vec1[2]);
        ambientHist[abuf]++;
        const size_t sprOkBefore = sprOk, sprMissBefore = sprMissing;
        for (const SectionInfo& s : w.sections) {
            sectionCounts[s.tag] += s.count;
        }
        size_t res = 0, miss = 0;
        for (size_t i = 1; i < w.protos.size(); ++i) {
            if (w.protos[i].instanceCount == 0) {
                continue;
            }
            std::string full;
            if (!index.Find(w.protos[i].name, &full)) {
                ++miss;
                missingNames[w.protos[i].name]++;
                continue;
            }
            std::vector<uint8_t> file;
            Tile tile;
            std::string terr;
            if (!ReadWholeFile(full, &file, &terr) || !tile.Load(file, &terr) ||
                tile.frames.empty()) {
                ++miss;
                missingNames[w.protos[i].name + " (" + terr + ")"]++;
                continue;
            }
            ++res;
        }
        protoResolved += res;
        protoMissing += miss;

        // Entities: prototype resolution, sprite resolution and the animation
        // pick, for every placed entity of every map.
        const EntityFile& ef = w.entityFile;
        if (!ef.error.empty()) {
            eshErrors[ef.error]++;
        }
        size_t mapPlaced = 0, mapSprOk = 0, mapSprMiss = 0, mapEntMiss = 0;
        for (const EntityInstance& rawInst : ef.entities) {
            ++entTotal;
            entClassCounts[rawInst.className]++;
            EntityInstance e = rawInst;
            std::string protoErr;
            const EntProto* proto = protoCache.Get(e.entPath, &protoErr);
            if (proto == nullptr && !e.entPath.empty()) {
                ++mapEntMiss;
                ++entMissing;
                entMissingNames[e.entPath + " (" + protoErr + ")"]++;
            }
            ApplyEntOverride(proto, &e);
            if (!e.hasFrame) {
                continue;  // inventory contents: no position, never drawn
            }
            ++mapPlaced;
            ++entPlaced;
            if (e.spritePath.empty()) {
                ++mapSprMiss;
                ++sprMissing;
                sprMissingNames["(no Sprite property) " + e.className]++;
                continue;
            }
            LoadedSprite* ls = spriteCache.Get(e.spritePath);
            if (!ls->ok) {
                ++mapSprMiss;
                ++sprMissing;
                sprMissingNames[e.spritePath + " (" + ls->error + ")"]++;
                continue;
            }
            const SpritePick pick = PickSprite(ls->sprite, e);
            if (!pick.ok) {
                ++mapSprMiss;
                ++sprMissing;
                sprMissingNames[e.spritePath + " (" + pick.error + ")"]++;
                continue;
            }
            if (!pick.error.empty()) {
                sprMissingNames[e.spritePath + " (warn: " + pick.error + ")"]++;
            }
            ++mapSprOk;
            ++sprOk;
        }

        // MILESTONE 5: light census and the map's own unlit level.
        {
            SceneLighting probe;
            for (int k = 0; k < 3; ++k) {
                probe.base[k] = w.vec1[k];
                probe.diffuse[k] = w.vec2[k];
            }
            const float far[3] = {-1e9f, -1e9f, -1e9f};
            probe.Vertex(far, pm.unlit);
            for (const EntityInstance& e : ef.entities) {
                if (e.className != "Light") {
                    continue;
                }
                const Esh* protoEsh = nullptr;
                std::string perr;
                const EntProto* pr =
                    e.entPath.empty() ? nullptr : protoCache.Get(e.entPath, &perr);
                if (pr != nullptr) {
                    protoEsh = &pr->block.esh;
                }
                const SceneLight L = BuildSceneLight(e, protoEsh);
                ++pm.lights;
                if (L.nightOnly) ++pm.lightsNightOnly;
                if (L.useArc) ++pm.lightsArc;
                if (!L.intensity.empty()) ++pm.lightsFlicker;
                if (L.useFog) ++pm.lightsFog;
            }
        }

        pm.sprOk = sprOk - sprOkBefore;
        pm.sprMiss = sprMissing - sprMissBefore;
        perMap.push_back(pm);
        std::printf("ok    %-52s protos %4zu/%2zu miss; inst %6zu; ent %4zu (%3zu placed, "
                    "spr %3zu/%2zu, proto miss %2zu) invis %3zu ambient %.3f %.3f %.3f "
                    "lights %3zu (night %3zu arc %2zu flicker %3zu fog %zu) unlit %.3f %.3f %.3f\n",
                    fs::path(f).filename().string().c_str(), res, miss, w.instances.size(),
                    ef.entities.size(), mapPlaced, mapSprOk, mapSprMiss, mapEntMiss, pm.invisible,
                    pm.ambient[0], pm.ambient[1], pm.ambient[2], pm.lights, pm.lightsNightOnly,
                    pm.lightsArc, pm.lightsFlicker, pm.lightsFog, pm.unlit[0], pm.unlit[1],
                    pm.unlit[2]);
    }

    std::printf("\n--- sections seen across all maps ---\n");
    for (const auto& kv : sectionCounts) {
        std::printf("  %-18s %zu\n", kv.first.c_str(), kv.second);
    }
    if (!missingNames.empty()) {
        std::printf("\n--- unresolved tile prototypes ---\n");
        for (const auto& kv : missingNames) {
            std::printf("  %-90s x%zu\n", kv.first.c_str(), kv.second);
        }
    }
    if (!errors.empty()) {
        std::printf("\n--- parse errors ---\n");
        for (const auto& kv : errors) {
            std::printf("  %-70s x%zu\n", kv.first.c_str(), kv.second);
        }
    }
    std::printf("\n--- entities by class, all maps ---\n");
    for (const auto& kv : entClassCounts) {
        std::printf("  %-22s %zu\n", kv.first.c_str(), kv.second);
    }
    if (!sprMissingNames.empty()) {
        std::printf("\n--- unresolved / warned sprites ---\n");
        for (const auto& kv : sprMissingNames) {
            std::printf("  %-90s x%zu\n", kv.first.c_str(), kv.second);
        }
    }
    if (!entMissingNames.empty()) {
        std::printf("\n--- unresolved .ent prototypes ---\n");
        for (const auto& kv : entMissingNames) {
            std::printf("  %-90s x%zu\n", kv.first.c_str(), kv.second);
        }
    }
    if (!eshErrors.empty()) {
        std::printf("\n--- tagged-property parse errors ---\n");
        for (const auto& kv : eshErrors) {
            std::printf("  %-90s x%zu\n", kv.first.c_str(), kv.second);
        }
    }

    // Per-map ambient.  The runtime ambient is Color::White for every map (see
    // RenderOptions::ambient); what varies is the <mapManager> light rig, which
    // only the editor's per-vertex path consumes.  It is reported so the claim
    // can be checked.
    std::printf("\n--- <mapManager> light rig ambient, by value ---\n");
    for (const auto& kv : ambientHist) {
        std::printf("  (%s)  %zu maps\n", kv.first.c_str(), kv.second);
    }
    // MILESTONE 5.  The light census, and the level a point no light reaches
    // gets in the EDITOR's preview (--light-base map).  In the game the base is
    // the constant white, so that level is 1.000 everywhere and no light of any
    // map changes a pixel; see NOTES.md section 25.
    {
        size_t tot = 0, night = 0, arc = 0, flick = 0, fog = 0, dark = 0, mapsWithLights = 0;
        for (const PerMap& m : perMap) {
            tot += m.lights;
            night += m.lightsNightOnly;
            arc += m.lightsArc;
            flick += m.lightsFlicker;
            fog += m.lightsFog;
            if (m.lights > 0) {
                ++mapsWithLights;
            }
            if (m.unlit[0] < 1.0f || m.unlit[1] < 1.0f || m.unlit[2] < 1.0f) {
                ++dark;
            }
        }
        std::printf("\n--- Light entities ---\n");
        std::printf("  lights placed                      %zu across %zu of %zu maps\n", tot,
                    mapsWithLights, perMap.size());
        std::printf("  with \"Night Only\"                  %zu   (inert: \"On\" defaults true "
                    "and no shipped light writes it)\n", night);
        std::printf("  with \"Use Arc\"                     %zu\n", arc);
        std::printf("  with an intensity string           %zu\n", flick);
        std::printf("  with \"Use Fog\" (the second list)   %zu\n", fog);
        std::printf("  maps whose editor-preview unlit level is below white   %zu of %zu\n", dark,
                    perMap.size());
        const WorldTime t0 = WorldTime::FromMillis(0);
        std::printf("  world time 0 (every map's start)   %s\n", t0.Describe().c_str());
    }

    size_t mapsWithMisses = 0, invisTotal = 0;
    for (const PerMap& m : perMap) {
        if (m.sprMiss > 0) {
            ++mapsWithMisses;
        }
        invisTotal += m.invisible;
    }
    std::printf("\nmaps parsed: %zu\n", ok);
    std::printf("maps with unresolved sprites: %zu\n", mapsWithMisses);
    std::printf("invisible-tile placements (instance flags & 0x0080): %zu\n", invisTotal);
    std::printf("runtime ambient (light_level_maybe @0x008bde98): 1.000 1.000 1.000 for every map\n");
    std::printf("parse errors: %zu\n", failed);
    std::printf("tile prototypes resolved/missing: %zu/%zu\n", protoResolved, protoMissing);
    std::printf("entities parsed: %zu (%zu placed)\n", entTotal, entPlaced);
    std::printf("tagged-property parse errors: %zu\n", eshErrors.size());
    std::printf(".ent prototypes missing: %zu\n", entMissing);
    std::printf("sprites resolved/missing: %zu/%zu\n", sprOk, sprMissing);
    return failed == 0 && eshErrors.empty() ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  mapview <file.mis> --info\n"
                 "  mapview <file.mis> --entities [--ents <entdir>] [--sprites <sprdir>]\n"
                 "  mapview <file.mis> --tiles <tiledir> [--sprites <sprdir>] [--ents <entdir>]\n"
                 "          --render <out.png>\n"
                 "          [--scale N] [--region x0,z0,x1,z1] [--crop x,y,w,h] [--bg AARRGGBB]\n"
                 "          [--max-elev N] [--no-entities] [--markers] [--show-invisible]\n"
                 "          [--campaign-start] [--squad <ent1,ent2,...>]\n"
                 "          [--tint on|off] [--ambient L|r,g,b|map]\n"
                 "          [--lights on|off] [--light-base clock|map|white] [--time HH:MM] [--tick N]\n"
                 "          [--order depth|screeny] [--layer-order s0,s1,s2,s3]\n"
                 "          [--inside x,z | x,y,z] [--inside-k K]\n"
                 "          [--centre x,z | x,y,z] [--size w,h]\n"
                 "  mapview <file.mis|file.sav> --resave <out.sav>\n"
                 "  mapview --til <file.til> [--info] [--dump <outdir>]\n"
                 "  mapview --selftest [--tiles <d>] [--sprites <d>] [--ents <d>] <dir-or-file>...\n"
                 "\n"
                 "  --region is in world units on the (x, z) ground plane, the same units the\n"
                 "           .mis stores; instances whose min corner falls inside are drawn.\n"
                 "  --crop   is in output pixels of the un-scaled render.\n"
                 "  --centre crops a --size window around a world point; --size defaults to\n"
                 "           800,600.  Give an elevation (x,y,z) or the crop lands 7*y px off.\n"
                 "  --inside is the roof-fade reference point of\n"
                 "           ComputeElementScreenBoundsAndHighlight @006e2c60; with x,z only,\n"
                 "           the elevation is the lowest tile under that point.\n"
                 "  --squad  names .ent files under entities/recruits/ (bare names allowed);\n"
                 "           they are placed at the map's SpawnPoint and the map's own\n"
                 "           placeholder Actors in that player slot are removed.\n"
                 "  --lights     on (the default) sums the map's Light entities into every\n"
                 "               element's four shape vertices, exactly as\n"
                 "               RefreshEntityVisionAndLighting @006e3260 does.\n"
                 "  --light-base game (the default) is DAT_008bda50 == 0, the shipped build:\n"
                 "               the vertex base is the constant white, so the sum saturates and\n"
                 "               the lighting is INVISIBLE.  map is DAT_008bda50 == 1, the editor\n"
                 "               menu toggle: the base is the <mapManager> ambient and the lights\n"
                 "               are what you see.  NOTES.md section 25.\n"
                 "  --time       HH:MM sets the world clock; it decides night/sunrise/sunset,\n"
                 "               which gates Night Only lights.  Default is world time 0, which\n"
                 "               ConvertMsecToDaysHMS + NormalizeTimeForYearsAndMonths make\n"
                 "               06:29 on day 1 of 2197.\n"
                 "  --tick       advances every light's flicker index N steps (one step per\n"
                 "               `Rate of change (ms)`, Light::DoEngineThink @0059cb40).\n"
                 "  --ambient map uses the <mapManager> light rig, which the shipped game does\n"
                 "           NOT: the runtime ambient is the constant white.  See NOTES 24.\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> positional;
    std::string tileDir, spriteDir, entDir, renderOut, tilPath, dumpDir;
    bool info = false, selftest = false, listEntities = false, listInstances = false;
    std::string resaveOut;
    std::string nameFilter;
    RenderOptions opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs an argument\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--info") {
            info = true;
        } else if (a == "--instances") {
            listInstances = true;
        } else if (a == "--name") {
            nameFilter = next("--name");
        } else if (a == "--selftest") {
            selftest = true;
        } else if (a == "--tiles") {
            tileDir = next("--tiles");
        } else if (a == "--sprites") {
            spriteDir = next("--sprites");
        } else if (a == "--ents") {
            entDir = next("--ents");
        } else if (a == "--entities") {
            listEntities = true;
        } else if (a == "--no-entities") {
            opt.drawEntities = false;
        } else if (a == "--show-invisible") {
            opt.showInvisibleTiles = true;
        } else if (a == "--campaign-start") {
            opt.campaignStart = true;
        } else if (a == "--markers") {
            opt.drawMarkers = true;
        } else if (a == "--ambient") {
            int64_t m[3] = {0, 0, 0};
            const std::string v = next("--ambient");
            if (v == "map") {
                opt.ambientFromMap = true;
            } else if (ParseInts(v.c_str(), 3, m)) {
                for (int k = 0; k < 3; ++k) {
                    opt.ambient[k] = static_cast<float>(m[k]) / 255.0f;
                }
            } else {
                const float f = static_cast<float>(std::atof(v.c_str()));
                opt.ambient[0] = opt.ambient[1] = opt.ambient[2] = f;
            }
        } else if (a == "--lights") {
            const std::string v = next("--lights");
            if (v == "off") {
                opt.lights = false;
            } else if (v == "on") {
                opt.lights = true;
            } else {
                std::fprintf(stderr, "--lights wants on or off\n");
                return 2;
            }
        } else if (a == "--light-base") {
            const std::string v = next("--light-base");
            if (v == "map") {
                opt.lightBase = LightBase::kMap;
            } else if (v == "white" || v == "game") {
                opt.lightBase = LightBase::kWhite;
            } else if (v == "clock") {
                opt.lightBase = LightBase::kClock;
            } else if (v == "neutral") {
                opt.lightBase = LightBase::kWhite;
                opt.neutralRig = true;
            } else {
                std::fprintf(stderr, "--light-base wants clock, map, white or neutral\n");
                return 2;
            }
        } else if (a == "--time") {
            const std::string v = next("--time");
            int hh = 0, mm = 0;
            if (std::sscanf(v.c_str(), "%d:%d", &hh, &mm) != 2 || hh < 0 || hh > 23 || mm < 0 ||
                mm > 59) {
                std::fprintf(stderr, "--time wants HH:MM\n");
                return 2;
            }
            opt.haveTime = true;
            opt.timeHour = hh;
            opt.timeMinute = mm;
        } else if (a == "--tick") {
            opt.tick = std::atoll(next("--tick").c_str());
        } else if (a == "--layer-order") {
            int64_t m[4] = {0, 0, 0, 0};
            if (!ParseInts(next("--layer-order").c_str(), 4, m)) {
                std::fprintf(stderr, "--layer-order wants four slot indices, e.g. 3,2,1,0\n");
                return 2;
            }
            for (int k = 0; k < kLayerCount; ++k) {
                opt.layerSlot[k] = static_cast<int>(m[k]);
            }
        } else if (a == "--tint") {
            const std::string mode = next("--tint");
            if (mode == "off") {
                opt.tint = kTintOff;
            } else if (mode == "on") {
                opt.tint = kTintOn;
            } else {
                std::fprintf(stderr, "--tint wants on|off\n");
                return 2;
            }
        } else if (a == "--resave") {
            resaveOut = next("--resave");
        } else if (a == "--render") {
            renderOut = next("--render");
        } else if (a == "--til") {
            tilPath = next("--til");
        } else if (a == "--dump") {
            dumpDir = next("--dump");
        } else if (a == "--scale") {
            opt.scale = std::atoi(next("--scale").c_str());
        } else if (a == "--order") {
            const std::string v = next("--order");
            if (v == "depth") {
                opt.sortRule = kSortDepth;
            } else if (v == "screeny") {
                opt.sortRule = kSortScreenY;
            } else {
                std::fprintf(stderr, "--order wants depth|screeny\n");
                return 2;
            }
        } else if (a == "--inside") {
            const std::string v = next("--inside");
            int64_t m[3] = {0, 0, 0};
            if (ParseInts(v.c_str(), 3, m)) {
                opt.inside[0] = m[0];
                opt.inside[1] = m[1];
                opt.inside[2] = m[2];
            } else if (ParseInts(v.c_str(), 2, m)) {
                opt.inside[0] = m[0];
                opt.inside[2] = m[1];
                opt.inside[1] = INT64_MIN;  // "use the lowest tile elevation under (x, z)"
            } else {
                std::fprintf(stderr, "--inside wants x,z or x,y,z in world units\n");
                return 2;
            }
            opt.haveInside = true;
        } else if (a == "--probe") {
            if (!ParseInts(next("--probe").c_str(), 3, opt.probe)) {
                std::fprintf(stderr, "--probe wants x,y,z in world units\n");
                return 2;
            }
            opt.haveProbe = true;
        } else if (a == "--inside-k") {
            opt.insideK = std::atof(next("--inside-k").c_str());
        } else if (a == "--centre" || a == "--center") {
            const std::string v = next("--centre");
            int64_t m[3] = {0, 0, 0};
            if (ParseInts(v.c_str(), 3, m)) {
                opt.centre[0] = m[0];
                opt.centre[1] = m[1];
                opt.centre[2] = m[2];
            } else if (ParseInts(v.c_str(), 2, m)) {
                opt.centre[0] = m[0];
                opt.centre[1] = 0;
                opt.centre[2] = m[1];
            } else {
                std::fprintf(stderr, "--centre wants x,z or x,y,z in world units\n");
                return 2;
            }
            opt.haveCentre = true;
        } else if (a == "--size") {
            if (!ParseInts(next("--size").c_str(), 2, opt.size)) {
                std::fprintf(stderr, "--size wants w,h in pixels\n");
                return 2;
            }
        } else if (a == "--squad") {
            const std::string v = next("--squad");
            opt.squad.clear();
            size_t start = 0;
            while (start <= v.size()) {
                const size_t comma = v.find(',', start);
                const std::string one =
                    v.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!one.empty()) {
                    opt.squad.push_back(one);
                }
                if (comma == std::string::npos) {
                    break;
                }
                start = comma + 1;
            }
            opt.haveSquad = true;
        } else if (a == "--max-elev") {
            opt.maxElev = std::atoi(next("--max-elev").c_str());
        } else if (a == "--quiet") {
            opt.quiet = true;
        } else if (a == "--region") {
            if (!ParseInts(next("--region").c_str(), 4, opt.region)) {
                std::fprintf(stderr, "--region wants x0,z0,x1,z1\n");
                return 2;
            }
            opt.haveRegion = true;
        } else if (a == "--crop") {
            if (!ParseInts(next("--crop").c_str(), 4, opt.crop)) {
                std::fprintf(stderr, "--crop wants x,y,w,h\n");
                return 2;
            }
            opt.haveCrop = true;
        } else if (a == "--bg") {
            opt.background = static_cast<uint32_t>(std::strtoul(next("--bg").c_str(), nullptr, 16));
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            return 2;
        } else {
            positional.push_back(a);
        }
    }

    if (!tilPath.empty()) {
        return CmdTil(tilPath, info || dumpDir.empty(), dumpDir);
    }
    if (selftest) {
        if (positional.empty()) {
            Usage();
            return 2;
        }
        return CmdSelftest(positional, tileDir, spriteDir, entDir);
    }
    if (positional.size() != 1) {
        Usage();
        return 2;
    }

    World w;
    std::string err;
    if (!w.Load(positional[0], &err)) {
        std::fprintf(stderr, "%s: %s\n", positional[0].c_str(), err.c_str());
        return 1;
    }
    PathIndex spriteIndex(spriteDir, "sprites/", ".spr");
    PathIndex entIndex(entDir, "entities/", ".ent");
    if (!opt.quiet && (!spriteDir.empty() || !entDir.empty())) {
        std::printf("sprite index        %zu files under %s\n", spriteIndex.Size(),
                    spriteDir.c_str());
        std::printf("ent index           %zu files under %s\n", entIndex.Size(), entDir.c_str());
    }
    if (!resaveOut.empty()) {
        // Re-emit the world through `World::Save`'s save-game branch.  Reading
        // a save produced by the shipped game and writing it back must give the
        // same bytes; that is the whole test of the format.
        std::string werr;
        if (!w.WriteSaveWorld(resaveOut, {}, &werr)) {
            std::fprintf(stderr, "--resave: %s\n", werr.c_str());
            return 1;
        }
        std::printf("resaved             %s\n", resaveOut.c_str());
    }
    if (info) {
        CmdMapInfo(w);
    }
    if (listEntities) {
        CmdEntities(w, &entIndex, &spriteIndex);
    }
    if (listInstances) {
        CmdInstances(w, opt.haveRegion, opt.region, nameFilter);
    }
    if (!renderOut.empty()) {
        if (tileDir.empty()) {
            std::fprintf(stderr, "--render needs --tiles <tiledir>\n");
            return 2;
        }
        TileIndex index(tileDir, "tiles/", ".til");
        if (!opt.quiet) {
            std::printf("tile index          %zu files under %s\n", index.Size(), tileDir.c_str());
        }
        if (!RenderWorld(w, index, &spriteIndex, &entIndex, renderOut, opt, &err)) {
            std::fprintf(stderr, "render failed: %s\n", err.c_str());
            return 1;
        }
    }
    if (!info && renderOut.empty() && !listEntities && resaveOut.empty()) {
        Usage();
        return 2;
    }
    return 0;
}
