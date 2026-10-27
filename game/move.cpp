// move.cpp -- see move.h.

#include "move.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <unordered_map>

namespace game {

void NavGrid::Reset(int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
    ox = FloorDiv(x0, kNavCell);
    oz = FloorDiv(z0, kNavCell);
    nx = FloorDiv(x1 + kNavCell - 1, kNavCell) - ox + 1;
    nz = FloorDiv(z1 + kNavCell - 1, kNavCell) - oz + 1;
    if (nx < 1) {
        nx = 1;
    }
    if (nz < 1) {
        nz = 1;
    }
    surface.assign(static_cast<size_t>(nx) * nz, kNoFloor);
    material.assign(static_cast<size_t>(nx) * nz, 0);
    stat.assign(static_cast<size_t>(nx) * nz, 0);
    statTop.assign(static_cast<size_t>(nx) * nz, kNoFloor);
    los.assign(static_cast<size_t>(nx) * nz, kLosClear);
    dyn.assign(static_cast<size_t>(nx) * nz, 0);
}

void NavGrid::MarkBox(std::vector<uint8_t>& target, int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                      int32_t topY, std::vector<int16_t>* topOut) {
    const int32_t cx0 = CellX(x0), cx1 = CellX(x1 - 1);
    const int32_t cz0 = CellZ(z0), cz1 = CellZ(z1 - 1);
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            if (!In(cx, cz)) {
                continue;
            }
            const size_t i = Idx(cx, cz);
            const int32_t s = surface[i];
            // A blocker above a cell with no floor is irrelevant -- nothing can
            // stand there anyway -- but marking it keeps the two tests
            // independent, which is what the fallback in FindPath wants.
            if (s == kNoFloor || topY - s > kBlockHeight) {
                target[i] = 1;
                if (topOut != nullptr && (*topOut)[i] < static_cast<int16_t>(topY)) {
                    (*topOut)[i] = static_cast<int16_t>(topY);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The search.
// ---------------------------------------------------------------------------
namespace {

// `RunPathfindingSearchStep@005355c0`.
inline int32_t StepCost(int32_t dx, int32_t dz) {
    const int n = (dx != 0 ? 1 : 0) + (dz != 0 ? 1 : 0);
    return n == 0 ? 1 : (n == 1 ? 8 : 10);
}

// `RunPathfindingSearchStep@005355c0`.  `dy` is in elevation STEPS, not world units:
// the binary's y axis is the search's own third dimension, and this grid has
// one surface per column, so the term is carried but is always 0 here.  It is
// left in place because it is what the binary computes.
inline int32_t Heuristic(int32_t x, int32_t z, int32_t gx, int32_t gz, int32_t dy) {
    const int32_t ax = std::abs(x - gx), az = std::abs(z - gz), ay = std::abs(dy);
    return (ax + az + (ay - (dy != 0 ? 1 : 0))) * 10;
}

struct Node {
    int32_t cx = 0, cz = 0;
    int32_t g = 0, h = 0;
    int32_t parent = -1;
};

}  // namespace

PathResult FindPath(const NavGrid& g, const PathRequest& req) {
    PathResult out;
    if (!g.In(req.startX, req.startZ)) {
        return out;
    }

    std::vector<Node> nodes;
    nodes.reserve(1024);
    // The closed/visited set: the binary keys it on (y << 32) | (x << 16) | z.
    std::unordered_map<uint64_t, int32_t> visited;
    // The open set: the binary's red-black tree ordered by f == g + h.  A
    // std::multimap is the same container with the same ordering and the same
    // tie-break (insertion order within a key).
    std::multimap<int32_t, int32_t> open;

    auto key = [](int32_t cx, int32_t cz) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 16) ^
               static_cast<uint32_t>(cz);
    };

    const int32_t r2 = req.goalRadiusCells * req.goalRadiusCells;
    Node start;
    start.cx = req.startX;
    start.cz = req.startZ;
    start.h = Heuristic(start.cx, start.cz, req.goalX, req.goalZ, 0);
    nodes.push_back(start);
    visited[key(start.cx, start.cz)] = 0;
    open.emplace(start.g + start.h, 0);

    int32_t best = 0;                 // the smallest-h node seen, for the fallback
    int32_t bestH = start.h;
    int32_t found = -1;
    size_t expanded = 0;

    while (!open.empty() && expanded < static_cast<size_t>(req.maxNodes)) {
        const int32_t ni = open.begin()->second;
        open.erase(open.begin());
        ++expanded;
        const int32_t cx = nodes[static_cast<size_t>(ni)].cx;
        const int32_t cz = nodes[static_cast<size_t>(ni)].cz;
        // `RunPathfindingSearchStep@005355c0`: exact match, or inside the radius.
        const int32_t ddx = cx - req.goalX, ddz = cz - req.goalZ;
        if ((cx == req.goalX && cz == req.goalZ) || (r2 > 0 && ddx * ddx + ddz * ddz < r2)) {
            found = ni;
            break;
        }
        if (nodes[static_cast<size_t>(ni)].h < bestH) {
            bestH = nodes[static_cast<size_t>(ni)].h;
            best = ni;
        }
        const int32_t here = g.Surface(cx, cz);
        for (int32_t dz = -1; dz <= 1; ++dz) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dz == 0) {
                    continue;
                }
                const int32_t tx = cx + dx, tz = cz + dz;
                // `DirSearch::DirSearch @00511370`: the candidate is the
                // actor's whole bounding box translated by (dx, dy, dz), and it
                // is that box which is tested against the world.  There is no
                // corner rule -- a diagonal past a corner fails here because
                // the translated box overlaps both walls.
                if (!g.BoxOpen(tx, tz, req.footW, req.footD)) {
                    continue;
                }
                const int32_t there = g.Surface(tx, tz);
                // dy in {-2, -1, 0, +1} world units, and with kNavCell == 1 a
                // world unit is a cell, so this is the candidate list itself.
                if (there - here > kStepUp || here - there > kStepDown) {
                    continue;
                }
                const int32_t cost = nodes[static_cast<size_t>(ni)].g + StepCost(dx, dz);
                const uint64_t k = key(tx, tz);
                auto it = visited.find(k);
                if (it != visited.end()) {
                    Node& n = nodes[static_cast<size_t>(it->second)];
                    if (cost < n.g) {   // l.12327-12345: relax and re-propagate
                        n.g = cost;
                        n.parent = ni;
                        open.emplace(n.g + n.h, it->second);
                    }
                    continue;
                }
                Node n;
                n.cx = tx;
                n.cz = tz;
                n.g = cost;
                n.h = Heuristic(tx, tz, req.goalX, req.goalZ, 0);
                n.parent = ni;
                const int32_t idx = static_cast<int32_t>(nodes.size());
                nodes.push_back(n);
                visited[k] = idx;
                open.emplace(n.g + n.h, idx);
            }
        }
    }

    int32_t tail = found;
    if (tail < 0) {
        // l.12428-12444: no exact hit, so take the visited node with the
        // smallest h and walk back from there.
        if (!req.allowPartial || bestH >= start.h) {
            return out;
        }
        tail = best;
    }
    out.exact = found >= 0;
    out.ok = true;
    out.expanded = expanded;
    for (int32_t i = tail; i >= 0; i = nodes[static_cast<size_t>(i)].parent) {
        out.cells.push_back({nodes[static_cast<size_t>(i)].cx, nodes[static_cast<size_t>(i)].cz});
    }
    std::reverse(out.cells.begin(), out.cells.end());
    out.points.reserve(out.cells.size());
    for (const NavCellRef& c : out.cells) {
        PathPoint p;
        p.x = g.CentreX(c.cx);
        p.z = g.CentreZ(c.cz);
        p.y = g.Surface(c.cx, c.cz);
        out.points.push_back(p);
    }
    return out;
}

void CollapseCollinear(PathResult* p) {
    if (p->cells.size() < 3) {
        return;
    }
    std::vector<NavCellRef> cells;
    std::vector<PathPoint> pts;
    cells.push_back(p->cells.front());
    pts.push_back(p->points.front());
    int32_t lastDx = 0, lastDz = 0;
    for (size_t i = 1; i < p->cells.size(); ++i) {
        const int32_t dx = p->cells[i].cx - p->cells[i - 1].cx;
        const int32_t dz = p->cells[i].cz - p->cells[i - 1].cz;
        const bool turn = (dx != lastDx || dz != lastDz);
        const bool elev = p->points[i].y != p->points[i - 1].y;
        if ((turn || elev) && i > 1) {
            cells.push_back(p->cells[i - 1]);
            pts.push_back(p->points[i - 1]);
        }
        lastDx = dx;
        lastDz = dz;
    }
    cells.push_back(p->cells.back());
    pts.push_back(p->points.back());
    p->cells.swap(cells);
    p->points.swap(pts);
}

}  // namespace game
