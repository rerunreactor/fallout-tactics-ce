// spr.h -- the .spr container, as established from the Ghidra export.
//
//   SpriteAnimation::LoadSpriteAnimationsFromStream @ 0070af30  -- file header,
//     sequence table, collection table.
//   ParseSpriteAnimationBlock @ 00716?? ("phoenix/sprite/sprite_data.cpp")
//     -- one "<spranim>" record: offset, name, frame/dir counts, rects.
//   SpriteAnimation::GenerateSpriteAnimationZars @ 00712ae0 -- the writer side,
//     which is what fixes the in-memory frame record layout (4 layers, each an
//     (x, y, Image*) triple, plus the union rect).
//
// The "<spranim_img>" payload block itself has no reader in the decompilation;
// see NOTES.md §"Gaps".

#ifndef SPRVIEW_SPR_H
#define SPRVIEW_SPR_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "file.h"
#include "zar.h"

namespace ft {

constexpr int kLayerCount = 4;

struct Rect {
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int32_t Width() const { return x1 - x0; }
    int32_t Height() const { return y1 - y0; }
};

// One entry of the sequence's frame table.  A frame id < 0 is an event marker
// and swallows `paramCount(id)` following table entries as its parameters.
struct Frame {
    int16_t id = 0;
    std::array<int16_t, 4> params{{0, 0, 0, 0}};
};

struct Sequence {
    std::string name;
    int16_t collectionId = 0;
    int32_t rawFrameCount = 0;  // entries in the on-disk table, params included
    std::vector<Frame> frames;  // params folded in
    std::vector<int32_t> frameData2;
};

// One decoded image: a ZAR resource plus the (x, y) it sits at inside the
// frame's union rect.  `present == false` is the type-0 "empty image" entry.
struct LayerImage {
    bool present = false;
    int32_t x = 0, y = 0;
    ZarImage zar;
};

struct Collection {
    std::string name;
    int32_t offset = 0;      // absolute file offset of the "<spranim_img>" block
    int32_t frameCount = 0;
    int32_t dirCount = 0;
    int32_t imgType = 0;     // '1' plain, '2' gzip-compressed
    std::vector<Rect> rects; // frameCount * dirCount, indexed frame*dirCount+dir
    std::array<std::vector<Color32>, kLayerCount> palettes;
    std::vector<LayerImage> images;  // frameCount * dirCount * kLayerCount
    bool imagesLoaded = false;
    std::string loadError;

    // The two read orders of LoadSpranimImageSectionFromStream @00716390
    // (the Ghidra export), which is the real "<spranim_img>" reader.  Both
    // branches store into the same frame record -- layer `l`'s slot is at
    // frame + (l + 1) * 0x10, with (x, y) at slot+0 and the Image* at slot+8,
    // and its palette is `this + 0x18 + l * 0x7d` -- so **stored layer index ==
    // frame slot index == palette index**, identity, in both.  Only the order
    // the entries arrive in differs:
    //
    //   version 1 (plain), l.5859-5896:  frame { dir { layer } }
    //       outer vector at +0xc steps 0x10 per frame (l.5897),
    //       inner steps 0x50 per dir (l.5891), layer slot steps 0x10 (l.5894)
    //       and the palette pointer steps 0x7d alongside it (l.5895).
    //   version 2 (gzip), l.5941-5999:   layer { dir { frame } }
    //       the layer is the OUTERMOST loop (l.5941, l.5999),
    //       dir steps 0x50 (l.5995), frame steps 0x10 (l.5991), and the slot is
    //       `(layer + 1) * 0x10 + base` (l.5964) with palette
    //       `layer * 0x7d + 0x18` (l.5982).
    //
    // Both formulas below were originally reconstructed from the data plus
    // FreeFT (NOTES.md 1.6) and are confirmed unchanged by the reader.
    int32_t LayerIndex(int32_t frame, int32_t dir, int32_t layer) const {
        const int32_t imageCount = frameCount * dirCount;
        return imgType == '1' ? (frame * dirCount + dir) * kLayerCount + layer
                              : dir * frameCount + layer * imageCount + frame;
    }
};

struct Sprite {
    int32_t version = 0;
    // The three bytes at +0x0c.  NOTES.md 4.1 read them as (z, y, x) following
    // FreeFT, with nothing in the Ghidra export to settle it.  Entity::UpdateSprite
    // (`Entity::UpdateSprite@005dfc70`) does settle it: it assigns
    // +0x0c -> boundingbox.x2, +0x0d -> y2, +0x0e -> z2, i.e. (x, y, z).
    uint8_t bboxX = 0, bboxY = 0, bboxZ = 0;
    int32_t offsetX = 0, offsetY = 0;
    std::array<uint8_t, 3> header{{0, 0, 0}};
    std::vector<Sequence> sequences;
    std::vector<Collection> collections;

    // MILESTONE 20.  Byte offset of the first `<spranim>` header, i.e. the
    // position just past the collection count.  Everything before it -- the
    // file header, the sequence table and the count itself -- is copied
    // verbatim by `sprtrim`, which is what makes a trim a byte-exact edit of
    // the parts it does not touch rather than a re-serialisation.
    size_t collectionTableStart = 0;

    // Reads the header, sequence table and collection table; the per-collection
    // image blocks are only read when `loadImages` is set.
    bool Load(const std::vector<uint8_t>& file, bool loadImages, std::string* errorOut);

    // Decodes one collection's "<spranim_img>" payload on demand.  `Load` with
    // loadImages calls this for every collection; a caller that needs a single
    // animation can call `Load(file, false, ...)` and then this.  Returns false
    // and sets Collection::loadError when the block is unusable.
    bool LoadCollectionImages(const std::vector<uint8_t>& file, size_t collection);
};

// --- collection usage logging -----------------------------------------------
//
// MILESTONE 20.  `sprtrim` needs to know which collections a run actually drew,
// and the only honest source for that is a run.  `LoadCollectionImages` is the
// single place a collection's images are ever decoded, so the log lives there:
// with it on, every decode records the names of the *sequences* that point at
// that collection.
//
// Sequence names, not collection names, because a collection name is
// per-sprite ("RaHM Upright Walk Club" in RaiderMaleHuge.spr) while a sequence
// name is the shared vocabulary every character sprite uses ("Walk",
// "CrouchAttackClubSwing").  A keep-set gathered from one sprite therefore
// applies to all of them, which is what makes a single manifest usable across
// the twelve character sprites a mission loads.
void SprSetUsageLog(bool on);
std::vector<std::string> SprTakeUsageLog();

// ReadWholeFile is declared in ../common/file.h, included above.  It used to
// be declared here and defined once per binary; milestone 3 links spr.o into
// mapview, so the two definitions would now collide.

}  // namespace ft

#endif  // SPRVIEW_SPR_H
