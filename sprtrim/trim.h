// trim.h -- rewrite a `.spr` keeping only the collections a run needs.
//
// MILESTONE 20.  A Tactics character `.spr` carries every animation collection
// for every weapon and every stance the character can hold: `RaiderMaleHuge.spr`
// is 25.6 MB across 100 collections, and one mission draws a handful of them.
// 194 of the 214 MB e2e fixture, and most of the ~500 MB "playable subset" the
// browser lane asks a player to import, is animation nobody in that mission
// ever sees.
//
// The trim is deliberately the *smallest* edit that removes those bytes:
//
//   - the collection COUNT never changes, and no collection is reordered.
//     `Sequence::collectionId` is an index into that table, so dropping an
//     entry would silently repoint every sequence after it.  A dropped
//     collection instead keeps its `<spranim>` header and is written with
//     `frameCount == 0`.  That is a shape the shipped data already contains and
//     that the reader already handles -- `ParseSpriteAnimationBlock` returns
//     the moment it reads a zero frame count, without even reading a direction
//     count (see sprview/NOTES.md 1.4) -- so nothing downstream needs a change.
//
//   - a KEPT collection's `<spranim_img>` block is copied BYTE FOR BYTE.  It is
//     never decoded and never re-encoded, so the gzip stays the shipper's, the
//     palettes stay bit-identical and the rects cannot drift.  "Rect-exact" is
//     therefore true by construction, and `--selftest` proves it anyway by
//     reloading the output and comparing every kept rect and image.
//
//   - everything before the collection table -- the file header, the whole
//     sequence table, the collection count -- is copied verbatim from the
//     input.  The sequence table is what maps a sequence name to a collection
//     id, and it is the reason a keep-set can be expressed in sequence names
//     that are the same across all twelve character sprites.
//
// The keep-set is a set of sequence names (case-insensitive).  A collection is
// kept when any sequence naming it is in the set.  Collections no sequence
// points at are kept or dropped by `keepUnreferenced`; they are drawn directly
// by index in a few places (scene.cpp picks `pick.collection`), so the default
// is to keep them.

#ifndef FT_SPRTRIM_H
#define FT_SPRTRIM_H

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace ft {

struct TrimOptions {
    // Sequence names to keep, lowercased. Empty means "keep everything", which
    // makes the trim a pure repack and is what `--selftest` uses as its control.
    std::set<std::string> keepSequences;
    // Collections no sequence references. Kept by default: a handful of call
    // sites choose a collection by index rather than through a sequence.
    bool keepUnreferenced = true;
};

struct TrimStats {
    size_t collections = 0;
    size_t collectionsKept = 0;
    uint64_t bytesIn = 0;
    uint64_t bytesOut = 0;
};

// Trim `in` into `out`. Returns false with `*errorOut` set if `in` is not a
// readable `<sprite>` v4 stream.
bool TrimSprite(const std::vector<uint8_t>& in, const TrimOptions& opts,
                std::vector<uint8_t>* out, TrimStats* stats, std::string* errorOut);

// Reload `trimmed` and check it against `original`: same sequence table, same
// collection count, and for every collection kept in `trimmed`, identical
// rects, identical layer palettes and identical decoded image bytes. Returns
// false with a description of the first difference.
bool VerifyTrim(const std::vector<uint8_t>& original, const std::vector<uint8_t>& trimmed,
                std::string* errorOut);

}  // namespace ft

#endif  // FT_SPRTRIM_H
