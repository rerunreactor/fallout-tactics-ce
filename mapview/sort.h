// sort.h -- `SortEntitiesByDrawOrder` @007426b0, ported.
//
// This is the game's per-frame draw-order sort for pass 2 (objects + entities;
// floors are pass 1 and are not in this list).  It is *not* a comparison sort.
// It is two phases:
//
//   1. an unstable `std::sort` (MSVC6 introsort) on ONE scalar, the cached int
//      at element+0x55, DESCENDING.  The comparator is the 24-byte leaf at
//      @00742a00, which compares that int the other way round:
//      `less(a,b) := b.depthMax < a.depthMax` -> largest +0x55 first.
//
//   2. a topological repair pass that walks the seeded order and, for every
//      position i, asks `std::PartitionEntitiesByDrawOrder` @00742960 whether
//      any LATER element must be painted BEFORE *i; if one is found it is
//      swapped down to i and the scan restarts at i.  Cycles are caught with a
//      generation stamp at element+0x3d against the global counter
//      `DAT_008b0a60`, and one retry with a relaxed predicate before the cycle
//      is broken by force.
//
// mapview implemented phase 1 only.  A scalar key is one number per element and
// it is taken from the element's max corner, which is a fair proxy for a 1x1
// wall or floor but not for a 9x10x5 object such as Bunker Alpha's blue
// generator machine: its near face reaches several cells toward the camera,
// past neighbours whose keys put them later in the list, so they paint over it.
// Phase 2 is what repairs that, and this file is the port of it.
//
// ---------------------------------------------------------------------------
// The element record (0x5D bytes, `operator_new(0x5d)` `MapRegion::LoadRegionFromStream@00740350`),
// as used by the three functions ported here.  Ghidra types both comparator
// arguments as `ushort *`, so `param_1 + N` in the listing is byte offset 2N.
//
//   +0x00  u16   tile-file index                (last tie-break, `*param_1`)
//   +0x04/08/0c  i32 world bbox MIN x / y / z   (`param+2`, `+4`, `+6`)
//   +0x10/14/18  i32 world bbox MAX x / y / z   (`param+8`, `+10`, `+0xc`)
//   +0x34  i32   explicit draw-order priority; 0 = unset.  Copied verbatim from
//                the on-disk record by `Entity::CloneEntityWithNewId`
//                (a field-for-field copy), and it is the FIRST thing
//                the comparator looks at (`param+0x1a`).
//   +0x38  u16   flags; bit0 1 = tile, 0 = entity; bit3 unknown (`param+0x1c`)
//   +0x3d  i32   draw-order generation stamp; `CloneEntityWithNewId` zeroes it
//   +0x41/45/49  i32 screen-space MIN x0 / y0 / depth0
//   +0x4d/51/55  i32 screen-space MAX x1 / y1 / depth1  <- phase-1 sort key
//   +0x59  i32   unique object id from `DAT_008be148` (CloneEntityWithNewId)
//
// `CloneEntityWithNewId` does NOT compute +0x41..+0x58 itself: it copies +0x00
// through +0x37 from the source record, zeroes +0x39/+0x3d, stamps a fresh id
// at +0x59 and then calls `ComputeRenderBoundsCorners()`, which is not in any
// dump.  So the DEPTH FORMULA IS STILL A RECONSTRUCTION -- the documented
// `depth = 7*(x+z) - 6*y` from the projection matrix, evaluated over the box:
//
//     depth1 (+0x55) = 7*(maxX + maxZ) - 6*minY     (the max of depth over the box)
//     depth0 (+0x49) = 7*(minX + minZ) - 6*maxY     (the min)
//
// which is the negation of mapview's `IsoDepth`, so "+0x55 descending" and
// mapview's "IsoDepth ascending" are the same order.  The pair really is a
// min/max INTERVAL and not two unrelated numbers: `PartitionEntitiesByDrawOrder`
// terminates its scan on `a.depth1 <= b.depth0 || b.depth1 <= a.depth0`, which
// is only meaningful as "the two depth intervals are disjoint".
// ---------------------------------------------------------------------------

#ifndef MAPVIEW_SORT_H
#define MAPVIEW_SORT_H

#include <algorithm>
#include <cstdint>
#include <vector>

namespace ft {

// One entry of the pass-2 draw list: the fields of the 0x5D-byte element that
// the three ported functions actually read, plus mapview's own back-pointer.
struct SortElem {
    int32_t wmin[3] = {0, 0, 0};   // +0x04 / +0x08 / +0x0c
    int32_t wmax[3] = {0, 0, 0};   // +0x10 / +0x14 / +0x18
    int32_t sx0 = 0, sy0 = 0;      // +0x41 / +0x45
    int32_t sx1 = 0, sy1 = 0;      // +0x4d / +0x51
    int64_t depth0 = 0;            // +0x49
    int64_t depth1 = 0;            // +0x55  -- the phase-1 key
    uint32_t order = 0;            // +0x34  (always 0 here; see below)
    uint16_t flags = 0;            // +0x38  bit0 = tile
    uint32_t id = 0;               // +0x00 (widened from u16: mapview's entity
                                   //  ordinals can exceed 65535 on a big map)
    int32_t stamp = 0;             // +0x3d
    // mapview payload, not part of the game's record.
    uint32_t index = 0;
    bool entity = false;
};

// `CompareEntityDrawOrder` @007427c0, ported line for line.
//
// Returns TRUE when `a` may stand before `b` (no constraint), FALSE when `b`
// must be painted before `a`.  That polarity is fixed by the call site:
// `PartitionEntitiesByDrawOrder` returns the offending position exactly when
// this returns 0.
//
// `relaxed` is the low byte of `SortEntitiesByDrawOrder`'s 4th
// argument -- the retry flag.  It can only loosen: it skips the elevation
// early-outs and turns the final interpenetration verdict into "no constraint",
// which is how a genuine A-before-B-before-C-before-A cycle is broken.
inline bool CompareEntityDrawOrder(const SortElem& a, const SortElem& b, bool relaxed) {
    // The explicit per-record draw priority at +0x34.  When both records carry
    // one it overrides every geometric test.  mapview does not read this field
    // out of the .mis, so it is 0 everywhere and this branch never fires; it is
    // ported for completeness.
    if (a.order != 0 && b.order != 0) {
        return a.order < b.order;
    }

    if (!relaxed) {
        // A's top (max y) is at or below B's bottom (min y), and B is an
        // entity (flag bit 0 clear): A first.
        if (a.wmax[1] <= b.wmin[1] && (b.flags & 1) == 0) {
            return true;
        }
        // The mirror case: B first.
        if (b.wmax[1] <= a.wmin[1] && (a.flags & 1) == 0) {
            return false;
        }
    }

    // Screen-rect overlap.  Disjoint on either axis -> the two can never paint
    // over one another, so there is no constraint at all.
    if (a.sx1 <= b.sx0 || b.sx1 <= a.sx0) {
        return true;
    }
    if (a.sy1 <= b.sy0 || b.sy1 <= a.sy0) {
        return true;
    }

    // The three-axis test, twice, with the arguments swapped.  Ghidra spells it
    // out as a nest of ifs plus one redundant re-test of the negation (the
    // inlined tail of the same helper), which is dropped here.
    //
    //   A(a,b) := a.minX < b.maxX  &&  b.minY < a.maxY  &&  a.minZ < b.maxZ
    //
    // !A(a,b) means a is separated from b on one of the three iso axes in the
    // direction that puts a further back: greater x, greater z, or lower
    // elevation.  Then a is painted first and there is nothing to fix.
    // B(a,b) is A(b,a); !B means b must go first.
    const bool A = a.wmin[0] < b.wmax[0] && b.wmin[1] < a.wmax[1] && a.wmin[2] < b.wmax[2];
    if (!A) {
        return true;
    }
    const bool B = b.wmin[0] < a.wmax[0] && a.wmin[1] < b.wmax[1] && b.wmin[2] < a.wmax[2];
    if (!B) {
        return false;
    }

    // Genuinely interpenetrating on all three axes: no separating plane exists.
    // On the retry pass the engine simply declares the pair fine, which is what
    // stops phase 2 spinning.
    if (relaxed) {
        return true;
    }

    // if (a.maxY < b.maxY) return 1;  if (b.maxY < a.maxY) return 0;
    if (a.wmax[1] != b.wmax[1]) {
        return a.wmax[1] < b.wmax[1];
    }
    // if (b.d49 < a.d49) return 1;  if (a.d49 < b.d49) return 0;
    if (a.depth0 != b.depth0) {
        return b.depth0 < a.depth0;
    }
    // tiles (+0x38 bit0) before entities
    const bool at = (a.flags & 1) != 0, bt = (b.flags & 1) != 0;
    if (at != bt) {
        return at;
    }
    // +0x38 bit3: unknown, and mapview never sets it.
    const bool a8 = (a.flags & 8) != 0, b8 = (b.flags & 8) != 0;
    if (a8 != b8) {
        return !a8;
    }
    // Last tie-break: the u16 at +0x00.
    return a.id < b.id;
}

// `std::PartitionEntitiesByDrawOrder` @00742960, ported.
//
//   int * f(int *first,int *last,int *hint,uint flags) {
//     if (first != last) {
//       bool past = hint <= first;
//       int *j = first;
//       do {
//         while (true) {
//           j = j + 1;
//           if (j == last) return last;
//           if (j == hint) past = true;
//           if (a.d55 <= b.d49 || b.d55 <= a.d49) break;      // depths disjoint
//           if (CompareEntityDrawOrder(*first,*j,flags) == 0) return j;
//         }
//       } while (!past);
//     }
//     return last;
//   }
//
// The disjoint-depth test does NOT end the scan on its own -- it only ends it
// once the scan has reached the caller's high-water mark `hint`.  Before that
// point the scan skips the pair and keeps going.  That is what makes the whole
// pass near-linear on a depth-seeded list: the depth interval is monotone in
// the seed order, so beyond the last element whose interval still overlaps
// there is nothing left to find.
//
// Returns `last` for "nothing must precede list[first]", else the index of an
// element that must be painted before it.
inline size_t PartitionEntitiesByDrawOrder(std::vector<SortElem*>& v, size_t first, size_t last,
                                           size_t hint, bool relaxed) {
    if (first == last) {
        return last;
    }
    bool past = hint <= first;
    size_t j = first;
    for (;;) {
        for (;;) {
            ++j;
            if (j == last) {
                return last;
            }
            if (j == hint) {
                past = true;
            }
            const SortElem& a = *v[first];
            const SortElem& b = *v[j];
            if (a.depth1 <= b.depth0 || b.depth1 <= a.depth0) {
                break;
            }
            if (!CompareEntityDrawOrder(a, b, relaxed)) {
                return j;
            }
        }
        if (past) {
            return last;
        }
    }
}

// `SortEntitiesByDrawOrder` @007426b0, ported.  `gen` is the file-static stand-in
// for the global counter `DAT_008b0a60`.
inline void SortEntitiesByDrawOrder(std::vector<SortElem*>& v, int32_t* gen) {
    // Phase 1.  MSVC6 std::sort with less(a,b) := b.depth1 < a.depth1, i.e.
    // descending on +0x55, unstable.  std::sort here is the same contract.
    std::sort(v.begin(), v.end(),
              [](const SortElem* a, const SortElem* b) { return b->depth1 < a->depth1; });

    // Phase 2.
    const size_t end = v.size();
    size_t i = 0;
    size_t hint = 0;
    bool retry = false;
    *gen += 1;
    while (i != end) {
        const size_t j = PartitionEntitiesByDrawOrder(v, i, end, hint, retry);
        if (j == end) {
            ++i;                // LAB_00742794
            retry = false;
            *gen += 1;          // LAB_007427a4
            continue;
        }
        hint = std::max(hint, j);          // piVar9 = max(hint, local_4)
        v[i]->stamp = *gen;                // *(int *)(*piVar8 + 0x3d) = DAT_008b0a60
        std::swap(v[i], v[j]);
        if (v[i]->stamp == *gen) {         // the element swapped in has been here already
            if (retry) {                   // second strike: force past it
                ++i;
                retry = false;
            } else {                       // first strike: rescan with the relaxed test
                retry = true;
            }
            *gen += 1;
        }
    }
}

}  // namespace ft

#endif  // MAPVIEW_SORT_H
