// move.h -- milestone 7: the navigation grid and the path search.
//
// SOURCES, and what is and is not in them.
//
// The search itself is `RunPathfindingSearchStep @005355c0` with
// `InitPathfindingSearchState @005346d0` (l.11390) as its
// constructor.  Between them they give the whole algorithm:
//
//   * a search NODE is 40 bytes: three ints (x, y, z), then g, then h, then the
//     parent pointer (l.11556-11562);
//   * the OPEN set is a red-black tree ordered by `g + h` (l.11577-11579,
//     the key being the node's g plus its h) and the CLOSED/visited set a second
//     red-black tree keyed by the 64-bit `(y << 32) | (x << 16) | z`
//     (l.11566-11573 and l.12163-12167);
//   * the STEP COST is (l.12281-12289)
//         n = (node.x != nb.x) + (node.z != nb.z)
//         cost = n == 0 ? 1 : (n == 1 ? 8 : 10)
//     i.e. 1 for a pure elevation change, 8 orthogonal, 10 diagonal;
//   * the HEURISTIC is (l.11500-11517 for the start node, l.12305-12318 for
//     every other) `10 * (|dx| + |dz| + |dy| - (dy != 0))`;
//   * the GOAL test is an exact match on all three, or a squared distance below
//     the object's field 0 (l.12172-12176), which the constructor sets to
//     `round(radius)^2` (l.11497);
//   * the search is BUDGETED -- `param_1[2]` nodes per call, `+0x79` in total
//     (l.12352-12360) -- and when the budget runs out with a flag set it falls
//     back to the visited node with the SMALLEST h (l.12428-12444,
//     `*(int *)(node[6] + 0x10)` is node[4]), which is a best-effort partial
//     path.
//
// MILESTONE 9 CORRECTION.  `DirSearch::DirSearch @00511370` -- the neighbour
// generator milestone 7 could not find -- IS now decompiled, in
// `DirSearch::DirSearch@00511370`, and it overturns three of milestone 7's four
// movement reconstructions.  It is a triple loop that writes 3 x 4 x 3 = 36
// candidate records of 0x1f bytes (exactly the 0x24 records of 0x1f bytes seen
// being filled at `RunPathfindingSearchStep@005355c0`).  Each record holds the actor's own
// integer bounding box `[min.x min.y min.z max.x max.y max.z]` TRANSLATED by
//
//     dx in {-1, 0, +1}   (`DirSearch::DirSearch@00511370`: three steps,
//                          starting at minX - 1)
//     dy in {-2, -1, 0, +1} (four steps, starting at minY - 2)
//     dz in {-1, 0, +1}   (three steps, starting at minZ - 1)
//
// **in world units**.  The constructor's own search region (l.579-586) is the
// same box grown by (1, 2, 1) on each side, which is the union of the 36.
// So:
//
//   * the step is ONE world unit, not three.  `kNavCell` is 1.
//   * there is no free-step-up constant: the elevation candidates are
//     -2, -1, 0, +1 world units, an asymmetric pair ported as it stands.
//   * there is NO corner-cutting rule.  A diagonal is the whole translated
//     bounding box tested against the world exactly like any other candidate,
//     so squeezing past a corner fails because the box overlaps both walls --
//     not because of a special case.  The box test replaces it.
//
// What DirSearch does NOT settle is what makes a cell "occupied" in the first
// place; `DirSearch::TestVisibilityAndCount @005115b0` delegates that to owner
// virtuals that are in no dump.  So `kBlockHeight` below is still a
// reconstruction, and so is the choice of which tile typeIds carry a surface.
// Those rest on two things the binary does settle:
//
//   * `WGameWorld::FindPrevVisibleTileOfType @0049d500` -- the mouse picker,
//     which is what turns a click into a move order -- accepts a tile only when
//     `Tile+0x25` (the typeId) is **1 or 4**.
//     So Floor and Roof are the two surfaces you can stand on.
//   * `TileType::ParseTileTypeDefinitionFromStream @00646350`
//     names typeIds 1 = Floor, 2 = Object, 3 = Stair.  The shipped
//     data fills in the rest: over mission01's 17,589 placements typeId 0 is
//     every `Village_Wall_*` and `*_Tent*` (a WALL, bbox height 10-12), 4 is
//     every `Village_Roof_*`, and the two stray values 29 and 233 are old
//     Ruined City wall caps and rubble piles.
//
// An entity's bounding box IS recoverable: `Entity::UpdateSprite`
// (`Entity::UpdateSprite@005dfc70`) sets `box.max = box.min + spriteBbox` from
// the three bytes at Sprite+0x0c, and sets `SpriteXYZAdjust` to
// `-0.5 * bbox / 4` on x and z, so the entity's stored position is the CENTRE
// of that footprint.  A RaiderMale is 3 x 8 x 3 world units; a village gate is
// 6 x 5 x 1.
//
// Doors: `Door::SetActivationState @005b2690` is the
// state machine -- the four action strings are "Opened", "Opening", "Closed",
// "Closing" -- and `RotatingDoor::AdjustBoundingBoxForOpenDoor @005b9040`
// swaps in a second, precomputed bounding box, which is how an open door stops
// blocking.  `Door::Door_UpdateProximityAutoOpenTimer @005b28f0` (l.20561) is
// the auto-open: it measures the distance from the door to one entity, subtracts
// half the summed x+z extents of the two boxes, and opens when the result is
// inside the radius stored at Door+0x1b9.

#ifndef GAME_MOVE_H
#define GAME_MOVE_H

#include <cstdint>
#include <vector>

namespace game {

// One navigation cell is ONE WORLD UNIT.  `DirSearch::DirSearch @00511370`
// translates the actor's bounding box by dx in {-1, 0, +1} and dz in
// {-1, 0, +1} world units, so the grid the search
// walks is the world-unit lattice itself.  PORTED, not reconstructed.
constexpr int32_t kNavCell = 1;

// The elevation candidates, also from DirSearch: dy in {-2, -1, 0, +1} world
// units.  A walker may drop two units in one step and
// climb one; the asymmetry is the binary's and is ported as it stands.  A
// shipped `Stair` tile is 6 x 1 x 1 -- one unit of rise per one-unit tread --
// so +1 is exactly a stair step.  Milestone 7's single symmetric `kStepUp = 4`
// was a reconstruction and is gone.
constexpr int32_t kStepUp = 1;
constexpr int32_t kStepDown = 2;

// A tile that rises this far above the surface blocks the cell.  Village grass
// is 1 tall and stone pots are 2; rubble piles are 5 and walls 10-12.
// RECONSTRUCTION.
constexpr int32_t kBlockHeight = 3;

constexpr int16_t kNoFloor = -32768;

// The tile typeIds, from TileType::ParseTileTypeDefinitionFromStream and the
// data (see the header comment).
enum TileTypeId { kTileWall = 0, kTileFloor = 1, kTileObject = 2, kTileStair = 3, kTileRoof = 4 };

struct NavCellRef {
    int32_t cx = 0, cz = 0;
    bool operator==(const NavCellRef& o) const { return cx == o.cx && cz == o.cz; }
};

// One path node, in world units.
struct PathPoint {
    int32_t x = 0, y = 0, z = 0;
};

struct NavGrid {
    int32_t ox = 0, oz = 0;   // cell (0,0) covers world x in [ox*kNavCell, +kNavCell)
    int32_t nx = 0, nz = 0;
    std::vector<int16_t> surface;    // top of the walkable tile, or kNoFloor
    // MILESTONE 13: the surface tile's `TileHeader.material` (+0x26), which is
    // what a footstep sound is named after -- 0 Stone, 1 Gravel, 2 Metal,
    // 3 Wood, 4 Water, 5 Snow (game/audio_names.h).  Filled by the same pass
    // that fills `surface`, from the tile that won the height.
    std::vector<uint8_t> material;
    std::vector<uint8_t> stat;       // static blockers: walls, tall objects
    // The top of the tallest static blocker over each cell, and what that
    // blocker does to a line of fire (milestone 8).  `kNoFloor` where nothing
    // blocks.  The line-of-fire code is `kLos*` below.
    std::vector<int16_t> statTop;
    std::vector<uint8_t> los;
    std::vector<uint8_t> dyn;        // entities and closed doors, rebuilt per order

    void Reset(int32_t x0, int32_t z0, int32_t x1, int32_t z1);
    bool In(int32_t cx, int32_t cz) const {
        return cx >= 0 && cz >= 0 && cx < nx && cz < nz;
    }
    size_t Idx(int32_t cx, int32_t cz) const {
        return static_cast<size_t>(cz) * nx + cx;
    }
    int32_t CellX(int32_t worldX) const { return FloorDiv(worldX, kNavCell) - ox; }
    int32_t CellZ(int32_t worldZ) const { return FloorDiv(worldZ, kNavCell) - oz; }
    int32_t CentreX(int32_t cx) const { return (ox + cx) * kNavCell + kNavCell / 2; }
    int32_t CentreZ(int32_t cz) const { return (oz + cz) * kNavCell + kNavCell / 2; }

    bool HasFloor(int32_t cx, int32_t cz) const {
        return In(cx, cz) && surface[Idx(cx, cz)] != kNoFloor;
    }
    // `ignoreDyn` lets an actor leave the cell it is itself standing on.
    bool Open(int32_t cx, int32_t cz, bool ignoreDyn = false) const {
        if (!HasFloor(cx, cz)) {
            return false;
        }
        const size_t i = Idx(cx, cz);
        return stat[i] == 0 && (ignoreDyn || dyn[i] == 0);
    }
    int32_t Surface(int32_t cx, int32_t cz) const {
        return In(cx, cz) ? surface[Idx(cx, cz)] : kNoFloor;
    }
    int32_t Material(int32_t cx, int32_t cz) const {
        return In(cx, cz) ? material[Idx(cx, cz)] : 0;
    }

    // DirSearch's candidate test, done directly: the actor's footprint box,
    // `w` x `d` world units, placed so that (cx, cz) is its CENTRE cell --
    // `Entity::UpdateSprite` sets
    // SpriteXYZAdjust to -0.5 * bbox on x and z, so an entity's stored
    // position is the centre of its footprint.  Every cell the box covers must
    // have a floor and no blocker.  This is what replaces milestone 7's
    // corner-cutting rule.
    bool BoxOpen(int32_t cx, int32_t cz, int32_t w, int32_t d, bool ignoreDyn = false) const {
        if (w < 1) w = 1;
        if (d < 1) d = 1;
        const int32_t x0 = cx - w / 2, z0 = cz - d / 2;
        for (int32_t z = z0; z < z0 + d; ++z) {
            for (int32_t x = x0; x < x0 + w; ++x) {
                if (!Open(x, z, ignoreDyn)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Marks every cell the box [x0,x1) x [z0,z1) touches, when the box rises
    // more than `kBlockHeight` above that cell's own surface.
    void MarkBox(std::vector<uint8_t>& target, int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                 int32_t topY, std::vector<int16_t>* topOut = nullptr);

    static int32_t FloorDiv(int32_t a, int32_t b) {
        return a >= 0 ? a / b : -(((-a) + b - 1) / b);
    }
};

// What a tile does to a shot passing through it, from the four flag bits in
// `TileHeader.flags` and assets/tiles/Flag Meanings.txt:
//   Ethereal  0x0001  no stand on, walk through, weapons fire through
//   Window    0x0004  no walk through, weapons fire through, a fraction of cover
//   Cover     0x0008  bonus cover; only meaningful with Window or Ethereal
//   Climbable 0x0010  climbed like a ladder
// The bit numbers are not in any dump; they were read off the shipped tiles,
// where every ladder carries 0x0010, grass and low rubble 0x0001, window and
// fence panels 0x0004, and two-unit rubble 0x0009 (Ethereal | Cover).  See
// game/NOTES.md section 23.
constexpr uint16_t kTileEthereal  = 0x0001;
constexpr uint16_t kTileWindow    = 0x0004;
constexpr uint16_t kTileCover     = 0x0008;
constexpr uint16_t kTileClimbable = 0x0010;

enum LosClass : uint8_t {
    kLosClear = 0,       // nothing here
    kLosCover = 1,       // Window or Ethereal: the shot passes, with a penalty
    kLosBonusCover = 2,  // ... and the Cover flag as well
    kLosSolid = 3,       // neither: the shot stops
};

// Search parameters.  `maxNodes` is `InitPathfindingSearchState`'s total budget
// (`+0x79`); `goalRadius` is its `param_8`, the radius whose square goes into
// field 0 and ends the search early.
struct PathRequest {
    int32_t startX = 0, startZ = 0;
    int32_t goalX = 0, goalZ = 0;      // in cells
    int32_t goalRadiusCells = 0;
    int32_t maxNodes = 200000;
    bool allowPartial = true;          // the `+0x18` fallback
    // The moving actor's own footprint in world units -- `DirSearch`'s
    // `param_1` box.  Every candidate is this box translated (see BoxOpen).
    int32_t footW = 1, footD = 1;
};

struct PathResult {
    bool ok = false;                   // a path was produced at all
    bool exact = false;                // it reaches the requested cell
    std::vector<NavCellRef> cells;     // start .. goal, inclusive
    std::vector<PathPoint> points;     // the same, as world-unit positions
    size_t expanded = 0;
};

// A* exactly as RunPathfindingSearchStep runs it: costs 1/8/10, heuristic
// 10 per Manhattan cell, an f-ordered open set, a coordinate-keyed closed set,
// a node budget, and the smallest-h fallback when the budget runs out.
PathResult FindPath(const NavGrid& g, const PathRequest& req);

// `ComputeWaypointListDirections @00536750` collapses runs of collinear nodes:
// RunPathfindingSearchStep only emits a waypoint where the unit step direction
// CHANGES (it compares each step's direction with the previous one).
void CollapseCollinear(PathResult* p);

}  // namespace game

#endif  // GAME_MOVE_H
