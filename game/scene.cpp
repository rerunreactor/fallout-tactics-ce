// scene.cpp -- see scene.h.

#include "scene.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "file.h"

namespace game {
namespace {

// Lower, IsEditorMarkerClass, ColourFloatToByte, DrawTint,
// ApplyPaletteColorAdjustments, LightGet, BuildSceneLight, GameDepth,
// DecodedTile, LoadedSprite, SpriteCache, LightQuad and BuildLightQuad are in
// engine/shared.h now; milestone 6 had its own copies of all of them.






// MILESTONE 17.  `Entity::IsCollectable` is vtable slot 43 and returns true for
// the whole `Collectable` hierarchy -- `Collectable::NewCollectable @005c1230`
// and `GenericItem::NewGenericItem @005c7290` are its two constructors, and the
// class registry names are what the `<entity_file>` class table stores.  These
// are every item class that appears across the 103 shipped maps.
bool IsCollectableClass(const std::string& cls) {
    static const char* const kItemClasses[] = {
        "Weapon", "Ammo", "Armour", "Armor", "Consumable", "SkillConsumable",
        "GenericItem", "Lockpick", "Keys", "Collectable", "Money", "Trap",
    };
    for (const char* c : kItemClasses) {
        if (cls == c) return true;
    }
    return false;
}

// A 256-entry per-channel table for the uniform case `out = round(c * A)`.
struct ChannelLut {
    uint8_t t[3][256];
};

uint32_t QuantiseAmbient(const float A[3]) {
    uint32_t k = 0;
    for (int i = 0; i < 3; ++i) {
        int v = static_cast<int>(A[i] * 255.0f + 0.5f);
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        k = (k << 8) | static_cast<uint32_t>(v);
    }
    return k;
}

}  // namespace

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
bool Scene::Load(const std::string& misPath, const SceneOptions& opt, std::string* errorOut) {
    m_opt = opt;
    m_misPath = misPath;
    if (!m_world.Load(misPath, errorOut)) {
        return false;
    }
    if (opt.saveWorld != nullptr && !opt.saveWorld->empty()) {
        World saved;
        if (!saved.LoadFromMemory(*opt.saveWorld, misPath + " (save)", errorOut)) {
            return false;
        }
        if (!saved.isSaveForm) {
            *errorOut = "the supplied world is not a save (no <sgd> section)";
            return false;
        }
        if (!saved.entityFile.error.empty()) {
            *errorOut = "save world entities: " + saved.entityFile.error;
            return false;
        }
        m_world.entityFile = std::move(saved.entityFile);
        m_world.worldPrefix = std::move(saved.worldPrefix);
        m_world.worldTail = std::move(saved.worldTail);
        m_world.entityFileOffset = saved.entityFileOffset;
        m_world.entityFileEnd = saved.entityFileEnd;
        m_world.entityClassCount = saved.entityClassCount;
        m_world.entityRecordCount = saved.entityRecordCount;
    }
    m_tileIndex = std::make_unique<PathIndex>(opt.tileDir, "tiles/", ".til");
    m_spriteIndex = std::make_unique<PathIndex>(opt.spriteDir, "sprites/", ".spr");
    m_entIndex = std::make_unique<PathIndex>(opt.entDir, "entities/", ".ent");
    m_sprites = std::make_unique<SpriteCache>(m_spriteIndex.get());
    m_protos = std::make_unique<EntProtoCache>(m_entIndex.get());

    // 1. Decode every referenced tile prototype once (mapview/main.cpp step 1).
    m_tiles.assign(m_world.protos.size(), DecodedTile());
    m_tileFrames.assign(m_world.protos.size(), std::vector<DecodedTile>());
    size_t resolved = 0, missing = 0, animatedProtos = 0;
    for (size_t i = 1; i < m_world.protos.size(); ++i) {
        if (m_world.protos[i].instanceCount == 0) {
            continue;
        }
        std::string full, err;
        std::vector<uint8_t> file;
        if (!m_tileIndex->Find(m_world.protos[i].name, &full)) {
            m_tiles[i].error = "not found";
            ++missing;
            continue;
        }
        if (!ReadWholeFile(full, &file, &err)) {
            m_tiles[i].error = err;
            ++missing;
            continue;
        }
        m_manifest.push_back(full);
        Tile tile;
        if (!tile.Load(file, &err) || tile.frames.empty()) {
            m_tiles[i].error = err.empty() ? "no frames" : err;
            ++missing;
            continue;
        }
        DecodedTile& d = m_tiles[i];
        if (!tile.DecodeFrame(0, &d.rgba, &d.width, &d.height, &err)) {
            d.error = err;
            ++missing;
            continue;
        }
        d.drawX = tile.frames[0].offsetX - tile.header.offsetX;
        d.drawY = tile.frames[0].offsetY - tile.header.offsetY;
        d.frameCount = tile.frames.size();
        d.ok = true;
        ++resolved;
        // MILESTONE 13 -- ANIMATED TILES.  `<tiledata>` is `frameCount` ZAR
        // images each with its own offset pair (mapview/NOTES.md section 1),
        // and the shipped data uses more than one for exactly the things that
        // burn: `Flame Normal.til` and `Burning Barrel.til` carry fifteen
        // frames apiece.  Decode the rest so Render can cycle them.
        if (tile.frames.size() > 1) {
            std::vector<DecodedTile>& fr = m_tileFrames[i];
            fr.assign(tile.frames.size(), DecodedTile());
            fr[0] = d;                       // copy, so index == frame number
            for (size_t f = 1; f < tile.frames.size(); ++f) {
                DecodedTile& df = fr[f];
                if (!tile.DecodeFrame(f, &df.rgba, &df.width, &df.height, &err)) {
                    df = d;                  // a bad frame holds frame 0
                    continue;
                }
                df.drawX = tile.frames[f].offsetX - tile.header.offsetX;
                df.drawY = tile.frames[f].offsetY - tile.header.offsetY;
                df.frameCount = tile.frames.size();
                df.ok = true;
            }
            ++animatedProtos;
        }
    }
    m_animatedTileProtos = animatedProtos;
    if (std::getenv("FOTGAME_ANIM_TILES") != nullptr) {
        for (size_t i = 1; i < m_tileFrames.size(); ++i) {
            if (m_tileFrames[i].size() <= 1) continue;
            size_t n = 0;
            int64_t fx = 0, fy = 0, fz = 0;
            for (const TileInstance& in : m_world.instances) {
                if (in.protoId != static_cast<int32_t>(i)) continue;
                if (n == 0) { fx = in.min[0]; fy = in.min[1]; fz = in.min[2]; }
                ++n;
            }
            std::fprintf(stderr, "animated tile proto %zu  %s  frames %zu  placements %zu  first %lld,%lld,%lld\n",
                         i, m_world.protos[i].name.c_str(), m_tileFrames[i].size(), n,
                         (long long)fx, (long long)fy, (long long)fz);
        }
    }

    // 2. Lighting (mapview/main.cpp step 1d).
    m_scene.enabled = opt.lights;
    m_scene.time = opt.haveTime ? WorldTime::FromHourMinute(opt.timeHour, opt.timeMinute)
                                : WorldTime::FromMillis(0);
    // The rig.  See scene.h on SceneOptions::lightBase and NOTES.md 25.13:
    // the clock is the shipped game's source; the map's own triple is the
    // editor's snapshot of it.
    if (opt.lightBase == LightBase::kClock) {
        float diff[3], amb[3];
        ComputeTimeOfDayLighting(m_scene.time, diff, amb);
        for (int k = 0; k < 3; ++k) {
            m_scene.base[k] = amb[k];
            m_scene.diffuse[k] = diff[k];
        }
    } else {
        for (int k = 0; k < 3; ++k) {
            m_scene.base[k] = opt.lightBase == LightBase::kMap ? m_world.vec1[k] : 1.0f;
            m_scene.diffuse[k] = m_world.vec2[k];
        }
    }
    for (const EntityInstance& e : m_world.entityFile.entities) {
        if (e.className != "Light" || !e.hasFrame) {
            continue;
        }
        const Esh* protoEsh = nullptr;
        if (!e.entPath.empty()) {
            std::string perr;
            const EntProto* pr = m_protos->Get(e.entPath, &perr);
            if (pr != nullptr) {
                protoEsh = &pr->block.esh;
            }
        }
        SceneLight L = BuildSceneLight(e, protoEsh);
        m_lightRate.push_back(L.rateChange);
        m_lightAccum.push_back(0);
        m_scene.lights.push_back(std::move(L));
    }

    // 3. Placed tiles: the load-time filters (invisible tile, roof hiding).
    //    These do not change frame to frame, so they run once here.
    SceneOptions ropt = m_opt;
    if (ropt.haveInside && ropt.inside[1] == INT64_MIN) {
        int64_t best = INT64_MAX;
        for (const TileInstance& in : m_world.instances) {
            if (in.protoId <= 0) continue;
            if (in.min[0] <= ropt.inside[0] && ropt.inside[0] <= in.max[0] &&
                in.min[2] <= ropt.inside[2] && ropt.inside[2] <= in.max[2]) {
                best = std::min<int64_t>(best, in.min[1]);
            }
        }
        ropt.inside[1] = (best == INT64_MAX) ? 0 : best;
    }
    // MILESTONE 15: the lowest floor elevation in each footprint cell, so the
    // loop below can tell a deck laid on the ground sheet from the sheet itself.
    // See the note at the `p.floor` assignment and mapview/main.cpp.
    static const int32_t kDeckLift = 4;
    std::unordered_map<uint64_t, int32_t> lowestFloorInCell;
    for (const TileInstance& in : m_world.instances) {
        if (in.protoId <= 0 || static_cast<size_t>(in.protoId) >= m_world.protos.size()) {
            continue;
        }
        if (m_world.protos[static_cast<size_t>(in.protoId)].header.typeId != 1) {
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
    int64_t minX = INT64_MAX, minY = INT64_MAX, maxX = INT64_MIN, maxY = INT64_MIN;
    size_t hiddenInv = 0, roofHidden = 0;
    for (size_t n = 0; n < m_world.instances.size(); ++n) {
        const TileInstance& inst = m_world.instances[n];
        const DecodedTile& d = m_tiles[static_cast<size_t>(inst.protoId)];
        if (!d.ok) {
            continue;
        }
        if (!m_opt.showInvisibleTiles && (inst.flags & 0x0080) != 0) {  // mapview/main.cpp:1163
            ++hiddenInv;
            continue;
        }
        if (ropt.haveInside) {  // mapview/main.cpp:1195
            const int64_t refX = ropt.inside[0], refY = ropt.inside[1], refZ = ropt.inside[2];
            const TileHeader& ph = m_world.protos[static_cast<size_t>(inst.protoId)].header;
            if (refY + 8 <= inst.min[1] && inst.min[0] < refX - 4 && inst.min[2] < refZ - 4 &&
                static_cast<int>(ph.bboxX) * static_cast<int>(ph.bboxZ) > 4) {
                const double cx = (inst.min[0] + inst.max[0]) / 2;
                const double cz = (inst.min[2] + inst.max[2]) / 2;
                const double r = (inst.max[1] - refY) * ropt.insideK;
                if ((cz - refZ) * (cz - refZ) + (cx - refX) * (cx - refX) < r * r) {
                    ++roofHidden;
                    continue;
                }
            }
        }
        int32_t sx = 0, sy = 0;
        ProjectWorldToScreen(inst.min[0], inst.min[1], inst.min[2], &sx, &sy);
        PlacedTile p;
        p.instance = n;
        p.protoId = inst.protoId;
        p.x0 = sx + d.drawX;
        p.y0 = sy + d.drawY;
        p.x1 = p.x0 + d.width;
        p.y1 = p.y0 + d.height;
        p.depth1 = GameDepth(inst.max[0], inst.min[1], inst.max[2]);
        p.depth0 = GameDepth(inst.min[0], inst.max[1], inst.min[2]);
        p.elev = inst.min[1];
        p.floor = m_world.protos[static_cast<size_t>(inst.protoId)].header.typeId == 1;
        if (p.floor) {
            // MILESTONE 15, the bridge.  Same rule as mapview/main.cpp: a floor
            // tile laid on top of the ground sheet -- mission01's canyon bridge
            // deck at elevation 128 over ground at 127 -- is a deck, not part of
            // the sheet, and must go through the pass-2 sort or the canyon's
            // ROCKruff wall tiles paint over it.  A ceiling or an upper storey
            // is far higher than kDeckLift and stays in pass 1.
            const uint64_t cell = (static_cast<uint64_t>(static_cast<uint32_t>(inst.min[0])) << 32) |
                                  static_cast<uint32_t>(inst.min[2]);
            const auto it = lowestFloorInCell.find(cell);
            const int32_t lift =
                (it == lowestFloorInCell.end()) ? 0 : (inst.min[1] - it->second);
            if (lift > 0 && lift <= kDeckLift) {
                p.floor = false;
            }
        }
        m_placed.push_back(p);
        minX = std::min<int64_t>(minX, p.x0);
        minY = std::min<int64_t>(minY, p.y0);
        maxX = std::max<int64_t>(maxX, p.x1);
        maxY = std::max<int64_t>(maxY, p.y1);
    }

    ResolveEntities();
    for (const EntityInstance& e : m_world.entityFile.entities) {
        std::string full;
        if (!e.entPath.empty() && m_entIndex->Find(e.entPath, &full)) {
            m_manifest.push_back(full);
        }
    }
    std::sort(m_manifest.begin(), m_manifest.end());
    m_manifest.erase(std::unique(m_manifest.begin(), m_manifest.end()), m_manifest.end());
    for (LiveEntity& e : m_entities) {
        RefreshEntityFrame(e);
        minX = std::min<int64_t>(minX, e.rect[0]);
        minY = std::min<int64_t>(minY, e.rect[1]);
        maxX = std::max<int64_t>(maxX, e.rect[2]);
        maxY = std::max<int64_t>(maxY, e.rect[3]);
    }
    m_bounds[0] = minX;
    m_bounds[1] = minY;
    m_bounds[2] = maxX;
    m_bounds[3] = maxY;

    BuildNavGrid();
    AttachTurrets();
    RefreshDynamicBlockers();
    size_t walkable = 0;
    for (int32_t i = 0; i < m_nav.nx * m_nav.nz; ++i) {
        if (m_nav.surface[static_cast<size_t>(i)] != kNoFloor &&
            m_nav.stat[static_cast<size_t>(i)] == 0) {
            ++walkable;
        }
    }

    size_t animating = 0, flickering = 0;
    for (const LiveEntity& e : m_entities) {
        if (e.animates) ++animating;
    }
    for (const SceneLight& L : m_scene.lights) {
        if (!L.intensity.empty() && L.rateChange > 0) ++flickering;
    }
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "tiles %zu/%zu protos (%zu animated), %zu placed (%zu invisible, %zu roof); "
                  "entities %zu "
                  "(%zu animated); lights %zu (%zu flickering); nav %dx%d cells (%zu walkable); "
                  "screen %lldx%lld",
                  resolved, resolved + missing, m_animatedTileProtos, m_placed.size(), hiddenInv,
                  roofHidden,
                  m_entities.size(), animating, m_scene.lights.size(), flickering, m_nav.nx,
                  m_nav.nz, walkable, (long long)(maxX - minX), (long long)(maxY - minY));
    m_info = buf;

    // MILESTONE 10: the scripting sections.  A map with none of them (an
    // editor test map, a multiplayer map) loads exactly as before.
    {
        std::string serr;
        if (m_script.Load(misPath, &serr)) {
            m_info += "; " + m_script.Info();
        } else {
            m_info += "; script: " + serr;
        }
    }

    // MILESTONE 11: the AI layer, which is more entities in the same slot
    // table -- BaseAI, Controller and the WayPoint chain -- plus
    // `tables/aitable.txt`.  It must come after the script load, because the
    // controllers steer by the `<world_zone>` boxes the script section carries.
    LoadAI();
    {
        char aib[192];
        std::snprintf(aib, sizeof(aib), "; ai %zu baseAI, %zu controllers", m_ai.size(),
                      m_controllers.size());
        m_info += aib;
    }
    return true;
}

// ---------------------------------------------------------------------------
void Scene::ResolveEntities() {
    // MILESTONE 17: the slot table, indexed, so `kEshEntityRef` properties can
    // be followed.  See the note beside `m_slotIndex` in scene.h.
    m_slotIndex.clear();
    for (size_t i = 0; i < m_world.entityFile.entities.size(); ++i) {
        m_slotIndex[m_world.entityFile.entities[i].slot] = i;
    }
    LoadRaceTables();

    // The map's SpawnPoint, and the player slot it carries -- which is the
    // squad's slot (mapview/NOTES.md 19).
    for (const EntityInstance& e : m_world.entityFile.entities) {
        if (e.className == "SpawnPoint" && e.hasFrame) {
            m_spawnPlayer = e.playerIndex;
            for (int k = 0; k < 3; ++k) {
                m_spawn[k] = e.world[k];
            }
            m_haveSpawn = true;
            break;
        }
    }

    // --squad: the campaign roster the .mis does not carry.  PORTED from
    // mapview/main.cpp step 1c; the formation itself is INVENTED there and is
    // still invented here (SpawnEntityGroupInFormation @004f5a10 is a call site
    // only).  Members go on a ring of radius 6 world units around the spawn.
    std::vector<EntityInstance> squadInsts;
    if (!m_opt.squad.empty() && m_haveSpawn) {
        static const int kRing[8][2] = {{0, -6}, {5, -4}, {6, 0},  {5, 4},
                                        {0, 6},  {-5, 4}, {-6, 0}, {-5, -4}};
        for (size_t i = 0; i < m_opt.squad.size(); ++i) {
            EntityInstance e;
            e.className = "Actor";
            e.slot = -1 - static_cast<int32_t>(i);
            e.entPath = m_opt.squad[i];
            if (e.entPath.rfind("entities/", 0) != 0) {
                e.entPath = "entities/recruits/" + e.entPath;
            }
            if (e.entPath.size() < 4 || e.entPath.substr(e.entPath.size() - 4) != ".ent") {
                e.entPath += ".ent";
            }
            e.hasFrame = true;
            const int slot = static_cast<int>(i % 8);
            e.world[0] = m_spawn[0] + kRing[slot][0];
            e.world[1] = m_spawn[1];
            e.world[2] = m_spawn[2] + kRing[slot][1];
            for (int k = 0; k < 3; ++k) {
                e.pos[k] = static_cast<float>(e.world[k]) / kEntityWorldScale;
            }
            e.playerIndex = m_spawnPlayer;
            e.hasRotation = true;
            e.animationRotation = slot;
            std::string perr;
            const EntProto* proto = m_protos->Get(e.entPath, &perr);
            if (proto == nullptr) {
                std::fprintf(stderr, "  SQUAD %s: %s\n", e.entPath.c_str(), perr.c_str());
                continue;
            }
            for (int c = 0; c < 4; ++c) {
                e.hasColour[c] = proto->block.esh.GetColour(kEntityColourNames[c], e.colour[c]);
            }
            squadInsts.push_back(std::move(e));
        }
    }

    // MILESTONE 22.  The squad has to become part of the WORLD, not a list
    // beside it, or `World::Save` has nothing to write for it: in a save
    // written by the shipped game the squad is ordinary entity records in the
    // player's slot, and the map's own placeholder Actors in that slot are
    // gone.  So the two edits `World::ValidateSpawnPointsAndAssignTeamColors
    // @004f53d0` makes at load are made to the record table itself here --
    // destroy the placeholders, add the squad -- and everything downstream,
    // the save included, sees one table.
    if (m_opt.campaignStart || !m_opt.squad.empty()) {
        if (m_spawnPlayer >= 0) {
            std::vector<EntityInstance> kept;
            kept.reserve(m_world.entityFile.entities.size());
            for (EntityInstance& e : m_world.entityFile.entities) {
                if (e.className == "Actor" && e.playerIndex == m_spawnPlayer) {
                    const size_t row = static_cast<size_t>(e.slot) - 1;
                    if (row < m_world.entityFile.slots.size()) {
                        m_world.entityFile.slots[row].classIndex = 0xffff;
                    }
                    continue;
                }
                kept.push_back(std::move(e));
            }
            m_world.entityFile.entities = std::move(kept);
        }
        // Each new member takes the next slot after the table's last, which is
        // what the original's slot allocator does with a free list it does not
        // have here.
        const size_t beforeAdd = m_world.entityFile.entities.size();
        int32_t classIdx = -1;
        for (size_t c = 0; c < m_world.entityFile.classNames.size(); ++c) {
            if (m_world.entityFile.classNames[c] == "Actor") {
                classIdx = static_cast<int32_t>(c);
                break;
            }
        }
        for (EntityInstance& e : squadInsts) {
            std::string perr;
            const EntProto* proto = m_protos->Get(e.entPath, &perr);
            if (proto == nullptr || classIdx < 0) continue;
            e.esh = proto->block.esh;
            float frame[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            for (int k = 0; k < 9; ++k) frame[k] = e.rot[k];
            for (int k = 0; k < 3; ++k) frame[9 + k] = e.pos[k];
            e.esh.PutFrame("frame", frame);
            e.esh.PutString("EntPath", e.entPath, 4);
            e.esh.PutInt("Player Index", e.playerIndex);
            if (e.hasRotation) e.esh.PutInt("animationRotation", e.animationRotation);
            EntitySlot row;
            row.classIndex = static_cast<uint16_t>(classIdx);
            m_world.entityFile.slots.push_back(row);
            e.slot = static_cast<int32_t>(m_world.entityFile.slots.size());
            e.classIndex = classIdx;
            e.className = "Actor";
            m_world.entityFile.entities.push_back(e);
        }
        m_world.entityFile.slotCount =
            static_cast<uint16_t>(m_world.entityFile.slots.size() + 1);
        m_world.entityFile.liveCount =
            static_cast<uint16_t>(m_world.entityFile.entities.size());
        m_squadAdded = m_world.entityFile.entities.size() - beforeAdd;
        squadInsts.clear();
        m_slotIndex.clear();
        for (size_t i = 0; i < m_world.entityFile.entities.size(); ++i) {
            m_slotIndex[m_world.entityFile.entities[i].slot] = i;
        }
    }

    const size_t mapCount = m_world.entityFile.entities.size();
    const size_t firstSquad = mapCount - m_squadAdded;
    for (size_t n = 0; n < mapCount; ++n) {
        EntityInstance inst = m_world.entityFile.entities[n];
        const bool isSquad = n >= firstSquad;
        if (!inst.hasFrame) {
            continue;
        }
        if (!m_opt.drawMarkers && IsEditorMarkerClass(inst.className)) {
            continue;
        }
        std::string protoErr;
        const EntProto* proto = m_protos->Get(inst.entPath, &protoErr);
        ApplyEntOverride(proto, &inst);
        if (inst.spritePath.empty()) {
            continue;
        }
        LoadedSprite* ls = m_sprites->Get(inst.spritePath);
        if (!ls->ok) {
            continue;
        }
        {
            std::string full;
            if (m_spriteIndex->Find(NormaliseSpriteName(inst.spritePath), &full)) {
                m_manifest.push_back(full);
            }
            if (m_entIndex->Find(inst.entPath, &full)) {
                m_manifest.push_back(full);
            }
        }
        const SpritePick pick = PickSprite(ls->sprite, inst);
        if (!pick.ok) {
            continue;
        }
        if (!ls->sprite.LoadCollectionImages(ls->file, static_cast<size_t>(pick.collection))) {
            continue;
        }
        LiveEntity e;
        e.misIndex = n;
        e.className = inst.className;
        e.entPath = inst.entPath;
        e.spritePath = inst.spritePath;
        e.tag = inst.tagName;
        // MILESTONE 18: the two speech-node names an entity carries in the
        // `.mis` itself.  Brahmin Wood writes them on every villager, captive,
        // the Elder and Hawkeye; `IsEntityValidTalkTarget @004a3070` reads the
        // first through `Entity::GetClickSpeechIDString` (vtable slot 507).
        inst.esh.GetString("Click Speech", &e.clickSpeech);
        inst.esh.GetString("Random Speech", &e.randomSpeech);
        if (proto != nullptr) {
            if (e.clickSpeech.empty()) {
                proto->block.esh.GetString("Click Speech", &e.clickSpeech);
            }
            if (e.randomSpeech.empty()) {
                proto->block.esh.GetString("Random Speech", &e.randomSpeech);
            }
        }
        e.label = inst.tagName.empty() ? inst.displayName : inst.tagName;
        if (e.label.empty()) {
            e.label = inst.internalName.empty() ? inst.className : inst.internalName;
        }
        e.ls = ls;
        e.sequence = pick.sequence;
        e.sequenceName = pick.sequenceName;
        e.collection = pick.collection;
        e.direction = pick.direction;
        e.dirCount = pick.dirCount;
        e.frame = pick.frame;
        e.spriteOffX = ls->sprite.offsetX;
        e.spriteOffY = ls->sprite.offsetY;
        // Entity::UpdateSprite: the entity's
        // bounding box is the three bytes at Sprite+0x0c, and SpriteXYZAdjust
        // puts the stored position at the centre of that footprint.
        e.bbox[0] = ls->sprite.bboxX > 0 ? ls->sprite.bboxX : 2 * kEntityHalfExtent;
        e.bbox[1] = ls->sprite.bboxY > 0 ? ls->sprite.bboxY : kEntityHeight;
        e.bbox[2] = ls->sprite.bboxZ > 0 ? ls->sprite.bboxZ : 2 * kEntityHalfExtent;
        e.isActor = inst.className == "Actor";
        e.isDoor = inst.className == "Door" || inst.className == "RotatingDoor";
        if (e.isDoor) {
            e.action = "Closed";
            e.doorOpen = false;
        }
        for (int k = 0; k < 3; ++k) {
            e.world[k] = inst.world[k];
            e.wpos[k] = inst.pos[k];
        }
        ProjectWorldToScreen(e.world[0], e.world[1], e.world[2], &e.sx, &e.sy);
        // MILESTONE 13: the actor's voice set.  `Actor::GetSoundType` is a
        // vtable slot with no body in the dumps; audio_names.cpp derives the
        // set from the sprite (which IS the armour and the race in this game)
        // and the variant from a hash of the display name, so the voice is
        // stable for the life of the actor.  How many variants a set has is
        // read off the shipped files, hence the probe through the mixer.
        if (e.isActor) {
            const std::string set = snd::ActorVoiceSet(e.spritePath, std::string());
            int variants = 1;
            if (m_audio != nullptr && snd::VoiceSetIsNumbered(set)) {
                // The set name is never a file on its own -- only
                // `<set><n><event><variant>` is -- so the probe asks how many
                // `<set><n>Hit1` exist.  Every humanoid set has a Hit file.
                variants = 0;
                while (variants < 16 &&
                       !m_audio
                            ->Resolve("sound/game/" + set +
                                      std::to_string(variants + 1) + "Hit1")
                            .empty()) {
                    ++variants;
                }
                if (variants < 1) variants = 1;
            }
            e.voice = snd::ActorVoice(e.spritePath, e.label, std::string(), variants);
        }
        e.wmin[0] = e.world[0] - kEntityHalfExtent;
        e.wmin[1] = e.world[1];
        e.wmin[2] = e.world[2] - kEntityHalfExtent;
        e.wmax[0] = e.world[0] + kEntityHalfExtent;
        e.wmax[1] = e.world[1] + kEntityHeight;
        e.wmax[2] = e.world[2] + kEntityHalfExtent;

        // Per-layer colour, mapview/main.cpp:993-1015.
        for (int l = 0; l < kLayerCount; ++l) {
            DrawTint& t = e.tint[l];
            for (int k = 0; k < 3; ++k) {
                t.A[k] = m_opt.ambient[k];
            }
            if (inst.hasColour[l]) {
                t.specular = true;
                for (int k = 0; k < 3; ++k) {
                    t.S[k] = inst.colour[l][k];
                }
                t.s0 = inst.colour[l][3];
                t.s1 = inst.colour[l][4];
            }
        }

        // The animation cursor.  `Entity::SetAnimationRelated` leaves
        // sequenceframe at the 0xffff sentinel and ProcessAnimation's leading
        // `++` turns it into 0, so the first tick lands on raw table entry 0 --
        // which closes the open question at mapview/NOTES.md section 15
        // ("Frame").  PickSprite has already resolved the same first frame for
        // the static case, so the two agree before the first tick.
        e.posture = inst.posture.empty() ? "Stand" : inst.posture;
        e.table = RawTable(e, pick.sequence);
        e.cursor = -1;
        e.delayMs = kDefaultSequenceDelayMs;
        e.nextThinkMs = 0;
        int realFrames = 0;
        for (const Frame& f :
             ls->sprite.sequences[static_cast<size_t>(pick.sequence)].frames) {
            if (f.id >= 0) {
                ++realFrames;
            }
        }
        e.animates = realFrames > 1;
        // milestone 8: the character sheet, the equipped weapon and the armour.
        e.playerIndex = inst.playerIndex;
        if (e.isActor) {
            LoadCombatant(e, inst.esh, proto);
        }
        // MILESTONE 17: containers and ground items.
        //
        // `Container::Container @005ab1e0` -- the class the `.ent` files under
        // `entities/containers/` declare.  Every field it needs is a plain
        // property, taken from the map instance when it states one and from
        // the prototype otherwise (the `.ent` override rule).  A crate says
        // `Is Always Open = True`, a chest says `Action = Closed` and animates
        // through Opening / Opened; both carry `Maximum Weight = 100`, the one
        // weight limit the game enforces.
        {
            const Esh* pe = proto != nullptr ? &proto->block.esh : nullptr;
            auto pick = [&](const char* name) -> const Esh* {
                if (inst.esh.Find(name) != nullptr) return &inst.esh;
                if (pe != nullptr && pe->Find(name) != nullptr) return pe;
                return nullptr;
            };
            if (e.className == "Container") {
                e.isContainer = true;
                if (const Esh* s = pick("Maximum Weight")) s->GetInt("Maximum Weight",
                                                                    &e.maxWeight);
                if (const Esh* s = pick("Is Always Open")) s->GetBool("Is Always Open",
                                                                     &e.alwaysOpen);
                if (const Esh* s = pick("Is Open")) s->GetBool("Is Open", &e.isOpen);
                if (const Esh* s = pick("Is Locked")) s->GetBool("Is Locked", &e.isLocked);
                // MILESTONE 18: the two `Container::ApplySkillToTargetEntity
                // @005ad3f0` reads -- `islockable` gates the pick at all
                // (`use_code |= 0x20` when false) and `unlockchance` is what
                // the actor's Lockpick is measured against.
                if (const Esh* s = pick("Is Lockable")) s->GetBool("Is Lockable", &e.isLockable);
                if (const Esh* s = pick("Chance to Unlock")) {
                    s->GetInt("Chance to Unlock", &e.unlockChance);
                }
                if (const Esh* s = pick("Action")) s->GetString("Action", &e.action);
                if (e.action.empty()) e.action = e.isOpen ? "Opened" : "Closed";
                if (e.alwaysOpen) e.isOpen = true;
                // The contents.  `Container.inventory` is an entity reference to
                // an `Inventory`, whose `ILink<n>` are references to the item
                // entities -- separate slots in the same table.
                if (const EntityInstance* inv = ResolveRef(inst.esh, "Inventory")) {
                    ReadInventoryList(inv->esh, &e.inventory);
                }
            } else if (e.className == "Trap" || e.className == "Mine") {
                // MILESTONE 18: `Trap::ApplySkillToTargetEntity @005cfcf0`.
                e.isTrap = true;
            } else if (!e.isActor && !e.isDoor && e.className != "Vehicle" &&
                       IsCollectableClass(e.className)) {
                // `Entity::IsCollectable` (vtable slot 43) is true for the
                // `Collectable` hierarchy: Weapon, Ammo, Armour, Consumable,
                // GenericItem and their siblings.  One of those with a `frame`
                // property is an item lying in the world.
                e.isCollectable = true;
                ReadItemInstance(inst, &e.item);
                // `Lootable` is a shipped `.ent` boolean; the punch and kick
                // entities set it false and nothing may take them.
                bool lootable = true;
                if (const Esh* s = pick("Lootable")) s->GetBool("Lootable", &lootable);
                e.lootable = lootable;
                e.label = e.item.displayName.empty() ? e.label : e.item.displayName;
            }
        }
        // MILESTONE 12: a `Vehicle`.  Its stats are plain `.ent` properties;
        // the nested "Main Weapon" is a `VehicleWeapon` entity block, which
        // `ReadWeapon` reads like any other weapon.
        if (e.className == "Vehicle") {
            e.isVehicle = true;
            const Esh* pe = proto != nullptr ? &proto->block.esh : nullptr;
            auto vpick = [&](const char* name) -> const Esh* {
                if (inst.esh.Find(name) != nullptr) return &inst.esh;
                if (pe != nullptr && pe->Find(name) != nullptr) return pe;
                return nullptr;
            };
            auto vf = [&](const char* name, float* out) {
                const Esh* src = vpick(name);
                if (src == nullptr) return;
                if (src->GetFloat(name, out)) return;
                // "Turning Rate" is an int in every shipped vehicle `.ent`.
                int32_t iv = 0;
                if (src->GetInt(name, &iv)) *out = static_cast<float>(iv);
            };
            auto vi = [&](const char* name, int32_t* out) {
                if (const Esh* src = vpick(name)) src->GetInt(name, out);
            };
            vf("Acceleration", &e.accel);
            vf("Max Speed", &e.maxSpeed);
            vf("Max Run Speed", &e.maxRunSpeed);
            vf("Turning Rate", &e.turnRate);
            vi("Num Passangers", &e.seats);
            if (const Esh* src = vpick("Gunner Enabled")) src->GetBool("Gunner Enabled",
                                                                      &e.gunnerEnabled);
            if (const Esh* src = vpick("Vehicle Type")) src->GetString("Vehicle Type",
                                                                      &e.vehicleType);
            if (const Esh* src = vpick("Main Weapon")) {
                Esh inner;
                std::string werr;
                const EshProp* pr = src->Find("Main Weapon");
                if (pr != nullptr && pr->payload.size() > 4) {
                    ft::Reader rr(pr->payload.data() + 4, pr->payload.size() - 4);
                    EntityBlock blk;
                    if (!blk.Read(rr, &werr)) {
                        std::fprintf(stderr, "  vehicle main weapon: %s\n", werr.c_str());
                    } else if (!ReadWeapon(blk.esh, &e.vehicleWeapon)) {
                        std::fprintf(stderr, "  vehicle main weapon '%s': %zu props, no modes\n",
                                     blk.className.c_str(), blk.esh.props.size());
                    }
                }
                (void)inner;
            }
            e.destReached = true;
        }
        // A turret can also be a map entity of its own -- mission19 places
        // `vehicletankturret.ent` beside its damaged tank rather than nesting
        // it -- which is what `VehicleWeapon::TurretMaybeRelated @005d5b60`
        // steers.  Read it as a weapon and let the post-load pass attach it.
        if (e.className == "VehicleWeapon") {
            const Esh* pe = proto != nullptr ? &proto->block.esh : nullptr;
            if (pe != nullptr) ReadWeapon(*pe, &e.vehicleWeapon);
            ReadWeapon(inst.esh, &e.vehicleWeapon);
            e.isTurret = true;
        }
        if (isSquad) {
            m_squad.push_back(static_cast<int32_t>(m_entities.size()));
        }
        m_entities.push_back(std::move(e));
    }

    // A corpse restored from a save has to LOOK dead: `Actor::EntityWasKilled
    // @0054cc40` leaves the actor on the last frame of a Death sequence, and
    // that is the state a save records at zero hit points.
    for (LiveEntity& e : m_entities) {
        if (!e.dead || !e.isActor) continue;
        if (SetAnimationType(e, "Death")) {
            e.action = "Death";
            e.animates = false;
            if (e.table != nullptr) e.cursor = static_cast<int32_t>(e.table->size()) - 1;
        }
    }

    // MILESTONE 22.  A world restored from a save brings its own squad: the
    // members are ordinary entity records in the player's slot, exactly as the
    // shipped game writes them, and nothing was spawned at the SpawnPoint.  The
    // player's team is the rule the original uses everywhere else too --
    // `World::ValidateSpawnPointsAndAssignTeamColors @004f53d0` is what makes
    // the SpawnPoint's player index the player's -- so it is the rule here.
    if (m_squad.empty() && m_opt.saveWorld != nullptr && m_spawnPlayer >= 0) {
        for (size_t i = 0; i < m_entities.size(); ++i) {
            const LiveEntity& e = m_entities[i];
            if (e.isActor && e.playerIndex == m_spawnPlayer && !e.isTurret) {
                m_squad.push_back(static_cast<int32_t>(i));
            }
        }
    }
}

// ---------------------------------------------------------------------------
const Bitmap* Scene::DecodeImage(const LiveEntity& e, const Collection& col, int32_t layer) {
    const int32_t idx = col.LayerIndex(e.frame, e.direction, layer);
    if (idx < 0 || static_cast<size_t>(idx) >= col.images.size()) {
        return nullptr;
    }
    const LayerImage& li = col.images[static_cast<size_t>(idx)];
    if (!li.present) {
        return nullptr;
    }
    const uint64_t key = (static_cast<uint64_t>(e.ls->id) << 40) |
                         (static_cast<uint64_t>(e.collection) << 24) |
                         static_cast<uint64_t>(idx);
    auto it = m_imageCache.find(key);
    if (it != m_imageCache.end()) {
        return it->second.w > 0 ? &it->second : nullptr;
    }
    Bitmap b;
    std::string derr;
    ZarImage& zar = const_cast<ZarImage&>(li.zar);
    if (zar.DecodeToRGBA(col.palettes[static_cast<size_t>(layer)], &b.rgba, &derr)) {
        b.w = zar.Width();
        b.h = zar.Height();
    }
    auto res = m_imageCache.emplace(key, std::move(b));
    return res.first->second.w > 0 ? &res.first->second : nullptr;
}

void Scene::RefreshEntityFrame(LiveEntity& e) {
    const Collection& col = e.ls->sprite.collections[static_cast<size_t>(e.collection)];
    const size_t rectIdx = static_cast<size_t>(e.frame) * col.dirCount + e.direction;
    if (rectIdx >= col.rects.size()) {
        return;
    }
    const Rect& rect = col.rects[rectIdx];
    e.x0 = e.sx + rect.x0 - e.spriteOffX;   // mapview/main.cpp:943
    e.y0 = e.sy + rect.y0 - e.spriteOffY;
    bool any = false;
    for (int l = 0; l < kLayerCount; ++l) {
        LiveEntity::Layer& L = e.layers[static_cast<size_t>(l)];
        L = LiveEntity::Layer();
        const int32_t idx = col.LayerIndex(e.frame, e.direction, l);
        if (idx < 0 || static_cast<size_t>(idx) >= col.images.size()) {
            continue;
        }
        const LayerImage& li = col.images[static_cast<size_t>(idx)];
        if (!li.present) {
            continue;
        }
        const Bitmap* raw = DecodeImage(e, col, l);
        if (raw == nullptr) {
            continue;
        }
        L.present = true;
        L.x = li.x;
        L.y = li.y;
        L.raw = raw;
        const int32_t rx0 = e.x0 + L.x, ry0 = e.y0 + L.y;
        if (!any) {
            e.rect[0] = rx0;
            e.rect[1] = ry0;
            e.rect[2] = rx0 + raw->w;
            e.rect[3] = ry0 + raw->h;
            any = true;
        } else {
            e.rect[0] = std::min(e.rect[0], rx0);
            e.rect[1] = std::min(e.rect[1], ry0);
            e.rect[2] = std::max(e.rect[2], rx0 + raw->w);
            e.rect[3] = std::max(e.rect[3], ry0 + raw->h);
        }
    }
    if (!any) {
        e.rect[0] = e.rect[2] = e.x0;
        e.rect[1] = e.rect[3] = e.y0;
    }
    e.dirty = true;
}

// Bake the four layers' pixels through ApplyPaletteColorAdjustments once per
// (image, tint) pair.  The engine does the equivalent once per (palette, light
// state) in ApplyPaletteColorAdjustments @00708e70 and then blits a plain LUT
// index; the transform is a pure function of the source colour, so doing it on
// the decoded RGBA is exact.
void Scene::BakeEntity(LiveEntity& e) {
    float A[3] = {1.0f, 1.0f, 1.0f};
    m_scene.AmbientForPoint(e.wpos, A);
    const uint32_t key = QuantiseAmbient(A);
    if (!e.dirty && key == e.tintKey) {
        return;
    }
    e.tintKey = key;
    e.dirty = false;
    for (int l = 0; l < kLayerCount; ++l) {
        LiveEntity::Layer& L = e.layers[static_cast<size_t>(l)];
        if (!L.present || L.raw == nullptr) {
            continue;
        }
        DrawTint t = e.tint[l];
        for (int k = 0; k < 3; ++k) {
            t.A[k] *= A[k];
        }
        const uint64_t bkey = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(L.raw)) << 8) ^
                              (static_cast<uint64_t>(key) << 3) ^ static_cast<uint64_t>(l);
        auto it = m_bakedCache.find(bkey);
        if (it == m_bakedCache.end()) {
            Bitmap b;
            b.w = L.raw->w;
            b.h = L.raw->h;
            b.rgba = L.raw->rgba;
            if (!t.Identity()) {
                for (size_t i = 0; i + 3 < b.rgba.size(); i += 4) {
                    if (b.rgba[i + 3] == 0) {
                        continue;
                    }
                    ApplyPaletteColorAdjustments(t, &b.rgba[i]);
                }
            }
            it = m_bakedCache.emplace(bkey, std::move(b)).first;
        }
        L.bmp = &it->second;
    }
}

// ---------------------------------------------------------------------------
// The animation.  Entity::ProcessAnimation @005e2b00, ported.
// ---------------------------------------------------------------------------

// `Sequence` folds each event marker's parameters into the marker's own record,
// so the raw int16 table the engine indexes has to be put back together.
// sprview/spr.cpp:14 `ParamCount` is the same table read the other way round,
// and it agrees with ProcessAnimation's switch: one parameter for -2, -3 and
// -5, three for -43 (ev_fire), none otherwise.
const std::vector<int16_t>* Scene::RawTable(const LiveEntity& e, int32_t sequence) {
    const uint64_t key = (static_cast<uint64_t>(e.ls->id) << 32) |
                         static_cast<uint32_t>(sequence);
    auto it = m_tableCache.find(key);
    if (it != m_tableCache.end()) {
        return &it->second;
    }
    const Sequence& seq = e.ls->sprite.sequences[static_cast<size_t>(sequence)];
    std::vector<int16_t> t;
    t.reserve(static_cast<size_t>(seq.rawFrameCount));
    for (const Frame& f : seq.frames) {
        t.push_back(f.id);
        const int8_t ev = static_cast<int8_t>(f.id & 0xff);
        int params = 0;
        if (f.id < 0) {
            if (ev == -2 || ev == -3 || ev == -5) {
                params = 1;
            } else if (ev == -43) {
                params = 3;
            }
        }
        for (int p = 0; p < params; ++p) {
            t.push_back(f.params[static_cast<size_t>(p)]);
        }
    }
    return &m_tableCache.emplace(key, std::move(t)).first->second;
}

// `Entity::SetAnimationType` -> `Entity::SetAnimationRelated @005df6d0`
// (`Entity::SetAnimationRelated@005df6d0`): look the name up in the sprite's sequence table
// and, on a miss, retry with "default"; sequencedelay goes back to 66 and
// sequenceframe to the 0xffff sentinel.
bool Scene::SetAnimationType(LiveEntity& e, const std::string& name) {
    const Sprite& spr = e.ls->sprite;
    int32_t found = -1;
    const std::string want = Lower(name);
    for (size_t i = 0; i < spr.sequences.size(); ++i) {
        if (Lower(spr.sequences[i].name) == want) {
            found = static_cast<int32_t>(i);
            break;
        }
    }
    if (found < 0) {
        return false;
    }
    const Sequence& seq = spr.sequences[static_cast<size_t>(found)];
    if (seq.collectionId < 0 ||
        seq.collectionId >= static_cast<int32_t>(spr.collections.size())) {
        return false;
    }
    if (!e.ls->sprite.LoadCollectionImages(e.ls->file,
                                           static_cast<size_t>(seq.collectionId))) {
        return false;
    }
    const Collection& col = spr.collections[static_cast<size_t>(seq.collectionId)];
    if (col.frameCount <= 0 || col.dirCount <= 0) {
        return false;
    }
    const int32_t prevCollection = e.collection;
    e.sequence = found;
    e.sequenceName = seq.name;
    e.collection = seq.collectionId;
    e.dirCount = col.dirCount;
    if (e.direction >= col.dirCount) {
        e.direction = 0;
    }
    e.table = RawTable(e, found);
    e.cursor = -1;                                       // the 0xffff sentinel
    if (e.collection != prevCollection) {
        e.frame = -1;   // force RefreshEntityFrame: the collection has changed
    }
    e.delayMs = kDefaultSequenceDelayMs;
    int realFrames = 0;
    for (const Frame& f : seq.frames) {
        if (f.id >= 0) {
            ++realFrames;
        }
    }
    e.animates = realFrames > 1;
    return true;
}

// `Actor::FinishedAnimationSequenceMaybe @00542ac0` -> `Vtable225ThinkRelated
// @00541f90`, whose `currentaction == ""` branch calls
// `SetCurrentActionStringMaybe(GBL_StringBreathe, "")`.
// That in turn goes through `TryApplySequenceNameVariants @005e28a0`, which
// concatenates base1+base2+suffix1+suffix2 and retries with the suffixes
// dropped -- but its caller, Actor vtable slot 218, is in no dump, so WHICH
// parts it passes is not recoverable.  What is recoverable is that the action
// component is "Breathe" and the posture component is Actor.ct_.posture, whose
// property default is "Stand" (`Actor::ClassType::Serialize@00570cb0`).  So this tries
// "<Posture>Breathe" and falls back to "default", which is the load-time name.
void Scene::FinishedAnimationSequence(LiveEntity& e) {
    // `Door::FinishedAnimationSequenceMaybe @005b0270`:
    // Opening -> AdjustBoundingBoxForOpenDoor + "Opened"; Closing ->
    // AdjustBoundingBoxForClosedDoor + "Closed".
    if (e.isDoor) {
        if (e.action == "Opening") {
            SetDoorState(e, "Opened");
            return;
        }
        if (e.action == "Closing") {
            SetDoorState(e, "Closed");
            return;
        }
    }
    // MILESTONE 17.  `Container::FinishedAnimationSequenceMaybe @005ac780`:
    // when the "Opening" sequence ends the container becomes open, switches to
    // "Opened", and -- if the actor it recorded is still valid -- opens the
    // loot window for it (`WorldGlobal->gamemain_ptr_maybe` vtable +0x20).
    // "Closing" ends the same way into "Closed".
    if (e.isContainer) {
        if (e.action == "Opening") {
            e.isOpen = true;
            e.action = "Opened";
            SetAnimationType(e, "Opened");
            if (e.openWhenAnimDone && e.openedBy >= 0) {
                m_lootTarget = static_cast<int32_t>(&e - m_entities.data());
                m_lootActor = e.openedBy;
            }
            e.openWhenAnimDone = false;
            return;
        }
        if (e.action == "Closing") {
            e.isOpen = false;
            e.action = "Closed";
            SetAnimationType(e, "Closed");
            return;
        }
        return;
    }
    // A death animation holds its last frame: `Actor::EntityWasKilled
    // @0054cc40` clears the entity's "animate" flag and generates a corpse, and
    // nothing re-selects a sequence afterwards.
    if (e.dead) {
        e.cursor = static_cast<int32_t>(e.table != nullptr ? e.table->size() : 0) - 1;
        e.nextThinkMs = m_ticks * kTickMs + 1000;
        return;
    }
    // An attack animation that has played out: back to the weapon idle.  This
    // is `Actor::Vtable225ThinkRelated`'s final else, which puts currentaction
    // back to "".
    if (e.action == "Attack" || e.action == "Recoil") {
        e.action.clear();
        e.attackTarget = -1;
    }
    // MILESTONE 13.  The waypoint idle actions end the same way:
    // `Actor::Vtable225ThinkRelated @00541f90`'s `currentaction == "Magic"`
    // branch is the one that catches them, and its
    // final else puts currentaction back to "" so the next end-of-sequence
    // re-selects Breathe.
    if (e.action == "Magiclow" || e.action == "Magichigh" || e.action == "Magic") {
        e.action.clear();
    }
    // A stance transition that has just played out.
    if (!e.pendingPosture.empty()) {
        e.posture = e.pendingPosture;
        e.pendingPosture.clear();
        ApplyAction(e);
        return;
    }
    if (e.moving) {
        e.action.clear();                 // force a re-select
        SelectDestinationAnimationState(e);
        return;
    }
    // `TryApplySequenceNameVariants @005e28a0` appends
    // the weapon suffix and falls back without it -- StandBreatheSMG, then
    // StandBreathe.  Milestone 7 could not name the suffix because the caller
    // is missing; the sprite table settles it (NOTES.md section 21).
    if (e.isCombatant && e.weapon.valid) {
        std::string weaponPart, modePart;
        AttackAnimSuffixes(e.weapon, &weaponPart, &modePart);
        if (!weaponPart.empty() && SetAnimationType(e, e.posture + "Breathe" + weaponPart)) {
            return;
        }
    }
    if (SetAnimationType(e, e.posture + "Breathe")) {
        return;
    }
    if (SetAnimationType(e, "default")) {
        return;
    }
    e.cursor = -1;
    e.delayMs = kDefaultSequenceDelayMs;
}

void Scene::ProcessAnimation(LiveEntity& e) {
    // `Entity::ProcessAnimation@005e2b00`: `if (parked != -1) SoundRelated1(this, parked)`
    // before anything else, then the marker is cleared.
    if (e.pendingSoundEvent != -1) {
        const int parked = e.pendingSoundEvent;
        e.pendingSoundEvent = -1;
        FrameEventSound(e, parked);
    }
    e.frameShown = false;
    // The whole of this function is `Entity::ProcessAnimation@005e2b00`, with the sound and
    // combat-message vtable calls (opcodes -45, -44, -42, -41, -6) left as
    // no-ops: milestone 6 has neither sound nor combat.
    int guard = 0;
    for (;;) {
        if (++guard > 64) {
            e.nextThinkMs = m_ticks * kTickMs + e.delayMs;
            return;
        }
        const std::vector<int16_t>& t = *e.table;
        const int32_t n = static_cast<int32_t>(t.size());
        ++e.cursor;                                     // `Entity::ProcessAnimation@005e2b00`
        if (e.cursor < 0 || e.cursor >= n) {
            FinishedAnimationSequence(e);
            continue;
        }
        int16_t w = t[static_cast<size_t>(e.cursor)];
        for (;;) {
            const int id = static_cast<int16_t>(w);
            if (id >= 0) {                              // `Entity::ProcessAnimation@005e2b00`
                if (e.frame != id) {
                    e.frame = id;
                    RefreshEntityFrame(e);
                }
                e.frameShown = true;
                e.nextThinkMs = m_ticks * kTickMs + e.delayMs;
                return;
            }
            ++e.cursor;                                 // `Entity::ProcessAnimation@005e2b00`
            const int32_t u = e.cursor;
            switch (id) {
                case -0x2b:                             // ev_fire, three params
                    // `Entity::ProcessAnimation@005e2b00`: the three parameters are the
                    // muzzle offset, and the case FALLS THROUGH to -0x2a, which
                    // calls ResolveCombatMessageForAttacker.  fotgame does not
                    // draw a projectile, so the offset is read and discarded.
                    e.cursor = u + 3;
                    ResolveAttack(e);
                    break;
                case -0x2a:                             // ev_hit (melee)
                    ResolveAttack(e);
                    break;
                case -5:                                // ev_jump_to_frame
                    e.cursor = (u < n) ? t[static_cast<size_t>(u)] : n;
                    break;
                case -4:                                // restart at entry 0
                    e.cursor = 0;
                    break;
                case -3:                                // ev_time_of_display
                    if (u < n) {
                        e.delayMs = t[static_cast<size_t>(u)];
                    }
                    e.cursor = u + 1;
                    break;
                case -2:                                // ev_stop_anim: hold
                    e.nextThinkMs =
                        m_ticks * kTickMs + (u < n ? t[static_cast<size_t>(u)] : 0);
                    e.cursor = u + 1;
                    return;
                case -0x2c:                             // ev_sound
                case -0x29:                             // footstep A
                case -0x28:                             // footstep B
                    // `Entity::ProcessAnimation@005e2b00`: when the frame this marker
                    // belongs to has not been displayed yet the opcode is
                    // PARKED and fired at the top of the next think, so the
                    // sound lands with the picture rather than ahead of it.
                    if (!e.frameShown) {
                        e.pendingSoundEvent = id;
                    } else {
                        FrameEventSound(e, id);
                    }
                    break;
                default:                                // pickup / effect / no-op
                    break;
            }
            if (e.cursor < 0 || e.cursor >= n) {
                FinishedAnimationSequence(e);
                break;                                  // restart the outer loop
            }
            w = t[static_cast<size_t>(e.cursor)];
        }
    }
}

// ---------------------------------------------------------------------------
// The tick.  See NOTES.md.
// ---------------------------------------------------------------------------
void Scene::Update(int64_t deltaMs) {
    if (deltaMs <= 0) {
        return;
    }
    m_nowMs += deltaMs;

    // MILESTONE 22 (VIDEO_GAPS M4): the floating status words expire.
    m_statusFloats.erase(std::remove_if(m_statusFloats.begin(), m_statusFloats.end(),
                                        [&](const StatusFloat& f) {
                                            return f.untilMs <= m_nowMs;
                                        }),
                         m_statusFloats.end());

    // MILESTONE 13: `UpdateWorldZonesTimeAndAmbientSound @004fd4e0` runs once
    // per world step, before the entities think.
    UpdateAmbient(deltaMs);

    // Lights: one intensity character per `Rate of change (ms)`
    // (Light::DoEngineThink @0059cb40, mapview/NOTES.md 25.3).
    for (size_t i = 0; i < m_scene.lights.size(); ++i) {
        const int32_t rate = m_lightRate[i];
        if (rate <= 0) {
            continue;
        }
        m_lightAccum[i] += deltaMs;
        const int64_t steps = m_lightAccum[i] / rate;
        if (steps > 0) {
            m_lightAccum[i] -= steps * rate;
            m_scene.lights[i].AdvanceTicks(steps);
        }
    }

    // Entities.  `World::AdvanceWorldSimulationTimeStep @004fbf50` clamps the
    // real delta to World+0x2bd (0.1 s) and then runs whole ticks; every
    // entity whose `NextEngineThink` deadline has passed thinks once per tick
    // (`Entity::CheckThinkTimers @005e7500`).
    if (deltaMs > kMaxFrameMs) {
        deltaMs = kMaxFrameMs;
    }
    m_tickAccumMs += deltaMs;
    const int64_t ticks = m_tickAccumMs / kTickMs;
    if (ticks <= 0) {
        return;
    }
    m_tickAccumMs -= ticks * kTickMs;
    for (int64_t s = 0; s < ticks; ++s) {
        ++m_ticks;
        const int64_t realtimeMs = m_ticks * kTickMs;
        for (LiveEntity& e : m_entities) {
            if (e.table == nullptr) {
                continue;
            }
            // MILESTONE 14.  There is NO "does this sequence animate" gate in
            // the original: `Entity::CheckThinkTimers @005e7500`
            // (`Entity::CheckThinkTimers@005e7500`) fires DoEngineThink for every entity
            // whose NextEngineThink deadline has passed, whatever its frame
            // table looks like.  Milestone 6's `!e.animates` skip deadlocked
            // every actor whose load-time sequence is the one-frame `default`
            // (which is every `.spr` in the game, characters and critters
            // alike): ProcessAnimation never ran, so the cursor never ran off
            // the end, so `Actor::FinishedAnimationSequenceMaybe @00542ac0`
            // never fired and `<Posture>Breathe` was never selected.  A
            // character that was given a move order escaped through
            // SelectDestinationAnimationState; an actor that never moves --
            // every brahmin in Brahmin Wood -- stood on `default` frame 0 for
            // the whole mission.  Section 3's idle rule only works if this
            // runs.
            if (e.nextThinkMs > realtimeMs) {
                continue;
            }
            ProcessAnimation(e);
        }
        // MILESTONE 7.  `Actor::DoEngineThink @0056c090` runs ProcessAnimation
        // and then Actor::Move, in that order, so the distance a tick covers is
        // the frame the tick has just switched to.
        bool anyMoving = false;
        for (LiveEntity& e : m_entities) {
            // MILESTONE 14 (PORT ADDITION).  Remember where this entity stood
            // before the tick moves it, so Render() can interpolate.
            const bool willStep = (!e.isDoor && ((e.isVehicle) || (e.isActor && e.moving)));
            if (willStep) {
                for (int k = 0; k < 3; ++k) e.prevWpos[k] = e.wpos[k];
            } else {
                e.interp = false;
            }
            if (e.isDoor) {
                DoorThink(e);
            } else if (e.isVehicle) {
                if (VehicleStep(e)) anyMoving = true;
            } else if (e.isActor && e.moving) {
                StepMovement(e);
                anyMoving = true;
            }
            if (willStep) {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float dk = e.wpos[k] - e.prevWpos[k];
                    d2 += dk * dk;
                }
                // A step longer than a run stride is a teleport (a spawn, a
                // PlaceEntity, a reposition off a blocked tile); snap it.
                e.interp = d2 > 0.0f &&
                           d2 < kInterpSnapWorldUnits * kInterpSnapWorldUnits /
                                    (kEntityWorldScale * kEntityWorldScale);
            }
        }
        if (anyMoving) {
            RefreshDynamicBlockers();
        }
        // MILESTONE 8.  The AI think, and the continuous AP regeneration.
        // `Actor::AdjustAP2 @0056ff80` refills AP at `clamp(maxAP, 1, 10)` per
        // second while the mode is 0, and does nothing at all in turn-based.
        if (m_combatMode == 0) {
            const float dt = static_cast<float>(kTickMs) / 1000.0f;
            for (LiveEntity& e : m_entities) {
                if (!e.isCombatant || e.dead) continue;
                float rate = static_cast<float>(e.maxAp);
                if (rate > 9.0f) rate = 10.0f;
                if (rate < 1.0f) rate = 1.0f;
                if (e.ap < static_cast<float>(e.maxAp)) {
                    e.ap = std::min(e.ap + rate * dt, static_cast<float>(e.maxAp));
                }
            }
        }
        // MILESTONE 11.  The AI think: one BaseAI::DoEngineThink per AI
        // actor whose deadline has passed, then the player-side sentries.
        AiTick();
        // MILESTONE 17.  The queued activate: an actor that clicked something
        // out of reach is walking to it and uses it on arrival.
        StepPendingUse();
        // MILESTONE 18: the same retry for the talk order (0x17) and the skill
        // order (0x16), both of which walk to an out-of-reach target rather
        // than refusing (`Actor::PerformSkillUseCheck @00547e90`'s 0x368 arm).
        StepPendingSkill();
        // MILESTONE 10.  The trigger engine runs once per engine tick, after
        // the world has moved: `World::AdvanceWorldStateOneTick` walks the
        // TriggerManager's list at the end of the step.
        if (m_scriptEnabled) {
            m_script.Tick(*this);
        }
    }
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MILESTONE 14.  Render-side interpolation.  A PORT ADDITION, NOT a
// reconstruction: see kInterpSnapWorldUnits in scene.h and NOTES.md section 72
// for what the binary actually does (one 70 ms position step per animation
// frame, and a render transform that reads the current position verbatim).
//
// The interpolation is the ordinary "render between the last two simulation
// states" one: it draws at `prev + (cur - prev) * accum/70ms`, so the picture
// trails the simulation by up to one tick and never extrapolates past it.
// Everything downstream of `sx`/`sy` -- the sprite anchor, the screen AABB the
// visibility cull and Pick() use -- is rebuilt by RefreshEntityFrame, so the
// pick agrees with what was drawn.  The world triple, the bounding box, the
// navigation grid and the audio listener are untouched: the simulation is
// exactly as it was.
// ---------------------------------------------------------------------------
void Scene::ApplyRenderInterpolation() {
    float alpha = 1.0f;
    if (m_interpolate) {
        alpha = static_cast<float>(m_tickAccumMs) / static_cast<float>(kTickMs);
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
    }
    for (LiveEntity& e : m_entities) {
        if (!e.interp && !e.interpApplied) {
            continue;
        }
        int32_t sx = 0, sy = 0;
        if (e.interp && m_interpolate) {
            // `ProjectWorldToScreen` (mapview/iso.h) is LINEAR --
            // sx = 2*3*(x - z), sy = -3*(x + z) - 7*y -- so interpolating the
            // world triple and projecting is the same thing as interpolating
            // the projection, and doing it in floating point is what buys the
            // sub-pixel resolution.  Rounding the interpolated world triple to
            // integer world units first would not: one world unit is six
            // screen pixels across, and a walking actor covers only 0.56 of
            // one per tick, so the integer grid IS the stutter.
            float f[3];
            for (int k = 0; k < 3; ++k) {
                f[k] = (e.prevWpos[k] + (e.wpos[k] - e.prevWpos[k]) * alpha) * kEntityWorldScale;
            }
            sx = static_cast<int32_t>(std::lround(2.0f * kIsoHalfWidth * (f[0] - f[2])));
            sy = static_cast<int32_t>(
                std::lround(-static_cast<float>(kIsoHalfWidth) * (f[0] + f[2]) -
                            static_cast<float>(kIsoHeight) * f[1]));
        } else {
            ProjectWorldToScreen(e.world[0], e.world[1], e.world[2], &sx, &sy);
        }
        if (sx != e.sx || sy != e.sy) {
            e.sx = sx;
            e.sy = sy;
            RefreshEntityFrame(e);
        }
        e.interpApplied = e.interp && m_interpolate;
    }
}

void Scene::Render(int64_t camX, int64_t camY, int32_t W, int32_t H, uint8_t* out,
                   RenderStats* stats) {
    ApplyRenderInterpolation();
    const int64_t vx0 = camX, vy0 = camY, vx1 = camX + W, vy1 = camY + H;
    std::memset(out, 0, static_cast<size_t>(W) * H * 4);

    // --- the visible sets -------------------------------------------------
    std::vector<uint32_t> floors;
    std::vector<SortElem> elems;
    std::vector<SortElem*> order;
    floors.reserve(1024);
    elems.reserve(2048);
    for (uint32_t i = 0; i < m_placed.size(); ++i) {
        const PlacedTile& p = m_placed[i];
        if (p.x1 <= vx0 || p.x0 >= vx1 || p.y1 <= vy0 || p.y0 >= vy1) {
            continue;
        }
        if (p.floor) {
            floors.push_back(i);
            continue;
        }
        SortElem e;
        const TileInstance& in = m_world.instances[p.instance];
        for (int k = 0; k < 3; ++k) {
            e.wmin[k] = in.min[k];
            e.wmax[k] = in.max[k];
        }
        e.sx0 = p.x0;
        e.sy0 = p.y0;
        e.sx1 = p.x1;
        e.sy1 = p.y1;
        e.depth0 = p.depth0;
        e.depth1 = p.depth1;
        e.flags = 1;
        e.id = static_cast<uint32_t>(in.protoId);
        e.index = i;
        e.entity = false;
        elems.push_back(e);
    }
    for (uint32_t i = 0; i < m_entities.size(); ++i) {
        LiveEntity& en = m_entities[i];
        if (en.rect[2] <= vx0 || en.rect[0] >= vx1 || en.rect[3] <= vy0 || en.rect[1] >= vy1) {
            continue;
        }
        BakeEntity(en);
        SortElem e;
        for (int k = 0; k < 3; ++k) {
            e.wmin[k] = en.wmin[k];
            e.wmax[k] = en.wmax[k];
        }
        e.sx0 = en.rect[0];
        e.sy0 = en.rect[1];
        e.sx1 = en.rect[2];
        e.sy1 = en.rect[3];
        e.depth0 = GameDepth(en.wmin[0], en.wmax[1], en.wmin[2]);
        e.depth1 = GameDepth(en.wmax[0], en.wmin[1], en.wmax[2]);
        e.flags = 0;
        e.id = 0x40000000u + i;
        e.index = i;
        e.entity = true;
        elems.push_back(e);
    }
    // Pass 1: the floors, ascending depth (mapview/main.cpp step 3).
    std::stable_sort(floors.begin(), floors.end(), [&](uint32_t a, uint32_t b) {
        return -m_placed[a].depth1 < -m_placed[b].depth1;
    });
    // Pass 2: SortEntitiesByDrawOrder @007426b0, verbatim (mapview/sort.h).
    order.resize(elems.size());
    for (size_t i = 0; i < elems.size(); ++i) {
        order[i] = &elems[i];
    }
    int32_t generation = 0;
    SortEntitiesByDrawOrder(order, &generation);

    // --- the blit ---------------------------------------------------------
    // The uniform fast path: `out = round(c * A)` per channel, tabulated.
    static thread_local std::unordered_map<uint32_t, ChannelLut> lutCache;
    auto lutFor = [&](const float A[3]) -> const ChannelLut* {
        const uint32_t k = QuantiseAmbient(A);
        auto it = lutCache.find(k);
        if (it != lutCache.end()) {
            return &it->second;
        }
        ChannelLut lut;
        for (int ch = 0; ch < 3; ++ch) {
            for (int v = 0; v < 256; ++v) {
                lut.t[ch][v] = ColourFloatToByte((v / 255.0f) * A[ch]);
            }
        }
        return &lutCache.emplace(k, lut).first->second;
    };

    auto blitPlain = [&](const uint8_t* rgba, int32_t w, int32_t h, int64_t dx, int64_t dy) {
        const int32_t cx0 = static_cast<int32_t>(std::max<int64_t>(0, -dx));
        const int32_t cy0 = static_cast<int32_t>(std::max<int64_t>(0, -dy));
        const int32_t cx1 = static_cast<int32_t>(std::min<int64_t>(w, W - dx));
        const int32_t cy1 = static_cast<int32_t>(std::min<int64_t>(h, H - dy));
        for (int32_t row = cy0; row < cy1; ++row) {
            const uint8_t* src = rgba + (static_cast<size_t>(row) * w + cx0) * 4;
            uint8_t* dst = out + ((static_cast<size_t>(dy + row) * W) + dx + cx0) * 4;
            for (int32_t col = cx0; col < cx1; ++col, src += 4, dst += 4) {
                const uint32_t a = src[3];
                if (a == 0) {
                    continue;
                }
                if (a == 255) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                    continue;
                }
                const uint32_t ia = 255 - a;
                dst[0] = static_cast<uint8_t>((src[0] * a + dst[0] * ia) / 255);
                dst[1] = static_cast<uint8_t>((src[1] * a + dst[1] * ia) / 255);
                dst[2] = static_cast<uint8_t>((src[2] * a + dst[2] * ia) / 255);
                dst[3] = static_cast<uint8_t>(a + dst[3] * ia / 255);
            }
        }
    };
    auto blitLut = [&](const uint8_t* rgba, int32_t w, int32_t h, int64_t dx, int64_t dy,
                       const ChannelLut& lut) {
        const int32_t cx0 = static_cast<int32_t>(std::max<int64_t>(0, -dx));
        const int32_t cy0 = static_cast<int32_t>(std::max<int64_t>(0, -dy));
        const int32_t cx1 = static_cast<int32_t>(std::min<int64_t>(w, W - dx));
        const int32_t cy1 = static_cast<int32_t>(std::min<int64_t>(h, H - dy));
        for (int32_t row = cy0; row < cy1; ++row) {
            const uint8_t* src = rgba + (static_cast<size_t>(row) * w + cx0) * 4;
            uint8_t* dst = out + ((static_cast<size_t>(dy + row) * W) + dx + cx0) * 4;
            for (int32_t col = cx0; col < cx1; ++col, src += 4, dst += 4) {
                const uint32_t a = src[3];
                if (a == 0) {
                    continue;
                }
                const uint8_t s0 = lut.t[0][src[0]], s1 = lut.t[1][src[1]], s2 = lut.t[2][src[2]];
                if (a == 255) {
                    dst[0] = s0;
                    dst[1] = s1;
                    dst[2] = s2;
                    dst[3] = 255;
                    continue;
                }
                const uint32_t ia = 255 - a;
                dst[0] = static_cast<uint8_t>((s0 * a + dst[0] * ia) / 255);
                dst[1] = static_cast<uint8_t>((s1 * a + dst[1] * ia) / 255);
                dst[2] = static_cast<uint8_t>((s2 * a + dst[2] * ia) / 255);
                dst[3] = static_cast<uint8_t>(a + dst[3] * ia / 255);
            }
        }
    };
    // Gouraud, mapview/main.cpp:1507-1526.  Only reached for a tile whose four
    // corner colours are not all equal, i.e. one a light actually reaches.
    auto blitGouraud = [&](const uint8_t* rgba, int32_t w, int32_t h, int64_t dx, int64_t dy,
                           const LightQuad& q) {
        const int32_t cx0 = static_cast<int32_t>(std::max<int64_t>(0, -dx));
        const int32_t cy0 = static_cast<int32_t>(std::max<int64_t>(0, -dy));
        const int32_t cx1 = static_cast<int32_t>(std::min<int64_t>(w, W - dx));
        const int32_t cy1 = static_cast<int32_t>(std::min<int64_t>(h, H - dy));
        for (int32_t row = cy0; row < cy1; ++row) {
            const uint8_t* src = rgba + (static_cast<size_t>(row) * w + cx0) * 4;
            uint8_t* dst = out + ((static_cast<size_t>(dy + row) * W) + dx + cx0) * 4;
            const double SY = static_cast<double>(camY + dy + row);
            for (int32_t col = cx0; col < cx1; ++col, src += 4, dst += 4) {
                const uint32_t a = src[3];
                if (a == 0) {
                    continue;
                }
                const double SX = static_cast<double>(camX + dx + col);
                double u = q.ax * SX + q.bx * SY + q.cx;
                double v = q.az * SX + q.bz * SY + q.cz;
                u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
                v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
                uint8_t px[3] = {src[0], src[1], src[2]};
                DrawTint g;
                for (int k = 0; k < 3; ++k) {
                    const double a0 = q.c[0][0][k] + (q.c[0][1][k] - q.c[0][0][k]) * v;
                    const double a1 = q.c[1][0][k] + (q.c[1][1][k] - q.c[1][0][k]) * v;
                    g.A[k] = static_cast<float>(m_opt.ambient[k] * (a0 + (a1 - a0) * u));
                }
                ApplyPaletteColorAdjustments(g, px);
                if (a == 255) {
                    dst[0] = px[0];
                    dst[1] = px[1];
                    dst[2] = px[2];
                    dst[3] = 255;
                    continue;
                }
                const uint32_t ia = 255 - a;
                dst[0] = static_cast<uint8_t>((px[0] * a + dst[0] * ia) / 255);
                dst[1] = static_cast<uint8_t>((px[1] * a + dst[1] * ia) / 255);
                dst[2] = static_cast<uint8_t>((px[2] * a + dst[2] * ia) / 255);
                dst[3] = static_cast<uint8_t>(a + dst[3] * ia / 255);
            }
        }
    };

    auto buildQuad = [&](const TileInstance& in, int shapeClass, LightQuad* q) {
        BuildLightQuad(m_scene, in, shapeClass, q);
    };

    auto drawTile = [&](uint32_t i) {
        const PlacedTile& p = m_placed[i];
        const TileInstance& in = m_world.instances[p.instance];
        // MILESTONE 13.  An animated tile shows `tick % frameCount`.  There is
        // ONE counter for the whole map -- the game does not store a cursor per
        // placement -- and it runs at the animation clock, one frame per 70 ms
        // tick.  Two independent sources fix that rate: `Barrel Fire.spr`,
        // which is the same fifteen images as `Burning Barrel.til` wrapped in a
        // sequence, plays them at the default `sequencedelay` of 66 ms against
        // the 70 ms tick (NOTES.md section 2), i.e. one frame per tick; and
        // FreeFT's independently reverse-engineered renderer drives every tile
        // off one global `s_frame_counter` at 15 Hz (`Tile::accessFrame`,
        // `frame_counter % frameCount`).
        const std::vector<DecodedTile>& fr = m_tileFrames[static_cast<size_t>(p.protoId)];
        const DecodedTile& d =
            fr.size() > 1 ? fr[static_cast<size_t>(m_ticks % static_cast<int64_t>(fr.size()))]
                          : m_tiles[static_cast<size_t>(p.protoId)];
        // p.x0/p.y0 were built with frame 0's offset pair; a later frame may
        // carry its own (mapview's survey found 8 of 396 non-zero).
        const DecodedTile& d0 = m_tiles[static_cast<size_t>(p.protoId)];
        const int64_t dx = p.x0 - camX + (d.drawX - d0.drawX);
        const int64_t dy = p.y0 - camY + (d.drawY - d0.drawY);
        LightQuad q;
        const TileHeader& th = m_world.protos[static_cast<size_t>(p.protoId)].header;
        buildQuad(in, ClassifyTileShape(th.bboxX, th.bboxY, th.bboxZ), &q);
        bool uniform = true;
        for (int k = 0; k < 3 && uniform; ++k) {
            const float v = q.c[0][0][k];
            if (q.c[0][1][k] != v || q.c[1][0][k] != v || q.c[1][1][k] != v) {
                uniform = false;
            }
        }
        if (uniform) {
            float A[3];
            for (int k = 0; k < 3; ++k) {
                A[k] = m_opt.ambient[k] * q.c[0][0][k];
            }
            if (A[0] == 1.0f && A[1] == 1.0f && A[2] == 1.0f) {
                blitPlain(d.rgba.data(), d.width, d.height, dx, dy);
            } else {
                blitLut(d.rgba.data(), d.width, d.height, dx, dy, *lutFor(A));
            }
        } else {
            blitGouraud(d.rgba.data(), d.width, d.height, dx, dy, q);
        }
    };
    auto drawEntity = [&](uint32_t i) {
        const LiveEntity& e = m_entities[i];
        // MILESTONE 17: an item that has been picked up is gone from the world.
        if (e.hidden) return;
        for (int l = 0; l < kLayerCount; ++l) {
            const LiveEntity::Layer& L = e.layers[static_cast<size_t>(l)];
            if (!L.present || L.bmp == nullptr) {
                continue;
            }
            blitPlain(L.bmp->rgba.data(), L.bmp->w, L.bmp->h, e.x0 + L.x - camX,
                      e.y0 + L.y - camY);
        }
    };

    for (uint32_t i : floors) {
        drawTile(i);
    }
    // MILESTONE 8: the blood decals go on the ground, between the floors and
    // everything that stands on them.  `CombatFX` spawns them as real entities
    // with these sprites (`CombatFX::CombatFXBloodSprayRelated @005953b0`,
    // `...BloodTrailRelated @00595af0`, `...BloodPoolRelated @00596820`); they
    // are drawn directly here because they never animate or move.
    DrawDecals(camX, camY, W, H, out);
    m_lastEntityOrder.clear();
    for (const SortElem* e : order) {
        if (e->entity) {
            drawEntity(e->index);
            m_lastEntityOrder.push_back(static_cast<int32_t>(e->index));
        } else {
            drawTile(e->index);
        }
    }
    if (stats != nullptr) {
        stats->tilesDrawn = floors.size() + (elems.size() - m_lastEntityOrder.size());
        stats->entitiesDrawn = m_lastEntityOrder.size();
        stats->sortElems = elems.size();
    }
}

// The four sprites CombatFX names, in the order Scene::Decal::kind uses.
// `CombatFXBloodSprayRelated` picks between the first three by the damage
// (under 15, under 31, above) and `CombatFXBloodPoolRelated` uses the fourth.
void Scene::DrawDecals(int64_t camX, int64_t camY, int32_t W, int32_t H, uint8_t* out) {
    static const char* const kDecalSprites[4] = {
        "sprites/impactfx/BloodSpurting.spr", "sprites/impactfx/BloodSpraying.spr",
        "sprites/impactfx/BloodGushing.spr", "sprites/impactfx/BloodPools.spr"};
    for (const Decal& d : m_decals) {
        const int kind = (d.kind >= 0 && d.kind < 4) ? d.kind : 0;
        LoadedSprite* ls = m_sprites->Get(kDecalSprites[kind]);
        if (ls == nullptr || !ls->ok || ls->sprite.collections.empty()) continue;
        const size_t col = 0;
        if (!ls->sprite.LoadCollectionImages(ls->file, col)) continue;
        const Collection& c = ls->sprite.collections[col];
        if (c.frameCount <= 0 || c.dirCount <= 0) continue;
        // One frame per decal, chosen once and kept: the original picks it with
        // the world RNG when the effect entity is created.
        const int32_t frame = static_cast<int32_t>(
            (static_cast<uint32_t>(d.tick * 2654435761u) >> 8) % static_cast<uint32_t>(c.frameCount));
        LiveEntity tmp;
        tmp.ls = ls;
        tmp.collection = static_cast<int32_t>(col);
        tmp.direction = 0;
        tmp.dirCount = c.dirCount;
        tmp.frame = frame;
        tmp.sx = d.sx;
        tmp.sy = d.sy;
        tmp.spriteOffX = ls->sprite.offsetX;
        tmp.spriteOffY = ls->sprite.offsetY;
        for (int l = 0; l < kLayerCount; ++l) {
            for (int k = 0; k < 3; ++k) tmp.tint[l].A[k] = m_opt.ambient[k];
        }
        RefreshEntityFrame(tmp);
        BakeEntity(tmp);
        for (int l = 0; l < kLayerCount; ++l) {
            const LiveEntity::Layer& L = tmp.layers[static_cast<size_t>(l)];
            if (!L.present || L.bmp == nullptr) continue;
            const int64_t x0 = tmp.x0 + L.x - camX, y0 = tmp.y0 + L.y - camY;
            for (int32_t yy = 0; yy < L.bmp->h; ++yy) {
                const int64_t dy = y0 + yy;
                if (dy < 0 || dy >= H) continue;
                for (int32_t xx = 0; xx < L.bmp->w; ++xx) {
                    const int64_t dx = x0 + xx;
                    if (dx < 0 || dx >= W) continue;
                    const uint8_t* src = &L.bmp->rgba[(static_cast<size_t>(yy) * L.bmp->w + xx) * 4];
                    if (src[3] == 0) continue;
                    uint8_t* dst = out + (static_cast<size_t>(dy) * W + dx) * 4;
                    const int a = src[3];
                    for (int ch = 0; ch < 3; ++ch) {
                        dst[ch] = static_cast<uint8_t>((src[ch] * a + dst[ch] * (255 - a)) / 255);
                    }
                    dst[3] = 255;
                }
            }
        }
    }
}

std::string Scene::TraceAnimation() const {
    std::string out;
    char buf[512];
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        // MILESTONE 14: actors are listed even when their current sequence
        // does not animate -- that is how the brahmin stuck on the one-frame
        // `default` were found, and after the fix it should print nothing new.
        if (!e.animates && !e.isActor) {
            continue;
        }
        std::snprintf(buf, sizeof(buf),
                      "  [%zu] %-22s %-34s seq %-22s col %-3d cur %-4d frame %-3d delay %-4d "
                      "screen %d,%d\n",
                      i, e.label.c_str(), e.spritePath.c_str(), e.sequenceName.c_str(),
                      e.collection, e.cursor, e.frame, (int)e.delayMs, e.rect[0], e.rect[1]);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------
int32_t Scene::Pick(int64_t camX, int64_t camY, int32_t px, int32_t py) const {
    const int64_t sx = camX + px, sy = camY + py;
    // Front to back: the draw list is back to front, so walk it in reverse.
    for (size_t k = m_lastEntityOrder.size(); k-- > 0;) {
        const int32_t i = m_lastEntityOrder[k];
        const LiveEntity& e = m_entities[static_cast<size_t>(i)];
        if (e.hidden) continue;   // MILESTONE 17: picked up, so gone
        if (sx < e.rect[0] || sx >= e.rect[2] || sy < e.rect[1] || sy >= e.rect[3]) {
            continue;
        }
        for (int l = 0; l < kLayerCount; ++l) {
            const LiveEntity::Layer& L = e.layers[static_cast<size_t>(l)];
            if (!L.present || L.raw == nullptr) {
                continue;
            }
            const int64_t lx = sx - (e.x0 + L.x), ly = sy - (e.y0 + L.y);
            if (lx < 0 || ly < 0 || lx >= L.raw->w || ly >= L.raw->h) {
                continue;
            }
            if (L.raw->rgba[(static_cast<size_t>(ly) * L.raw->w + lx) * 4 + 3] != 0) {
                return i;
            }
        }
    }
    return -1;
}


// ===========================================================================
// MILESTONE 7 -- movement.  See move.h for where each rule comes from.
// ===========================================================================

namespace {

// `_DAT_008bc668` in Actor::Move: the int-world-unit -> metres scale, 1/4
// (mapview/NOTES.md 14 pins _DAT_0082431c, its inverse, at 4).
constexpr float kWorldToMetres = 1.0f / kEntityWorldScale;

// `Entity::MaxMovementSpeedMaybe`, entity vtable slot 231, multiplies the
// per-frame distance in Actor::Move.  Its body is
// in NO dump and there is no speed column in any of the shipped tables, so it
// is 1 here.  PLACEHOLDER.
constexpr float kMaxMovementSpeed = 1.0f;

// `Door+0x1b9`, the proximity radius Door_UpdateProximityAutoOpenTimer compares
// against.  The property that fills it is not in any
// Door serializer in the dumps and no shipped door `.ent` writes it, so this is
// a PLACEHOLDER: one and a half tiles, which is about an arm's length past the
// door leaf once the two half-extents are subtracted.
constexpr float kDoorOpenRadius = 9.0f * kWorldToMetres;

// How long an auto-opened door stays open once nothing is near it.  `Door::
// FinishedAnimationSequenceMaybe` reads `Door.timeopen` (`Door::FinishedAnimationSequenceMaybe@005b01c0`
// l.18786) and that property is not in the dumps either.  PLACEHOLDER.
constexpr int64_t kDoorHoldMs = 2000;

}  // namespace

float Scene::FrameMoveDistance(const LiveEntity& e) const {
    // Actor::Move @0055ba20, `Actor::Move@0055ba20`:
    //
    //   n       = (unk7 - unk6) >> 2                    // the float array's length
    //   idx     = clamp(sequenceframe, 0, n - 1)
    //   speed   = unk6[idx]
    //   speed  *= MaxMovementSpeedMaybe()
    //
    // `Entity.unk6..unk7` is a float vector as long as the sequence's RAW frame
    // table, indexed by the raw cursor.  That is `Sequence::frameData2` in the
    // `.spr` -- sprview/NOTES.md "Gaps" 3 read it as int32 and could not say
    // what it was for.  Read as float32 it is the per-frame movement distance
    // in metres, and the shipped values say so plainly: RaiderMale's StandWalk
    // is 0.14 on every frame, StandRun 0.5, CrouchWalk 0.07, ProneWalk
    // 0.03-0.09, and every non-moving sequence is 0 throughout.
    if (e.ls == nullptr || e.sequence < 0) {
        return 0.0f;
    }
    const Sequence& seq = e.ls->sprite.sequences[static_cast<size_t>(e.sequence)];
    const int32_t n = static_cast<int32_t>(seq.frameData2.size());
    if (n <= 0) {
        return 0.0f;
    }
    int32_t idx = e.cursor;
    if (idx >= n) {
        idx = n - 1;
    }
    if (idx < 0) {
        idx = 0;
    }
    float v = 0.0f;
    const int32_t raw = seq.frameData2[static_cast<size_t>(idx)];
    std::memcpy(&v, &raw, sizeof(v));
    return v * kMaxMovementSpeed;
}

void Scene::BuildNavGrid() {
    m_nav.Reset(m_world.worldBox[0], m_world.worldBox[2], m_world.worldBox[3],
                m_world.worldBox[5]);

    // Pass 1: the surfaces.  `WGameWorld::FindPrevVisibleTileOfType @0049d500`
    // -- the picker a move order goes through -- accepts typeId 1 (Floor) and
    // typeId 4 (Roof) and nothing else.  A Stair
    // (typeId 3) also carries a surface: it is how the elevation changes.
    // `BuildShapeVertices @006e4220`'s default case shows the walkable face is
    // the box's TOP, i.e. max[1] (mapview/NOTES.md 25).
    for (const TileInstance& in : m_world.instances) {
        if (in.protoId <= 0 ||
            static_cast<size_t>(in.protoId) >= m_world.protos.size()) {
            continue;
        }
        if (TileInstanceInvisible(in)) {
            continue;
        }
        const int type = m_world.protos[static_cast<size_t>(in.protoId)].header.typeId;
        if (type != kTileFloor && type != kTileRoof && type != kTileStair) {
            continue;
        }
        const int32_t cx0 = m_nav.CellX(in.min[0]), cx1 = m_nav.CellX(in.max[0] - 1);
        const int32_t cz0 = m_nav.CellZ(in.min[2]), cz1 = m_nav.CellZ(in.max[2] - 1);
        for (int32_t cz = cz0; cz <= cz1; ++cz) {
            for (int32_t cx = cx0; cx <= cx1; ++cx) {
                if (!m_nav.In(cx, cz)) {
                    continue;
                }
                int16_t& s = m_nav.surface[m_nav.Idx(cx, cz)];
                const int32_t top = in.max[1];
                if (s == kNoFloor || top > s) {
                    s = static_cast<int16_t>(top);
                    // MILESTONE 13: the tile that owns the surface owns the
                    // footstep material too.
                    m_nav.material[m_nav.Idx(cx, cz)] =
                        m_world.protos[static_cast<size_t>(in.protoId)].header.material;
                }
            }
        }
    }

    // Pass 2: the static blockers.  Everything that is not a surface tile and
    // rises more than kBlockHeight above the surface it sits on.  Roofs are
    // excluded because they ARE a surface; the tile a roof would otherwise
    // block is the one under it.
    for (const TileInstance& in : m_world.instances) {
        if (in.protoId <= 0 ||
            static_cast<size_t>(in.protoId) >= m_world.protos.size()) {
            continue;
        }
        if (TileInstanceInvisible(in)) {
            continue;
        }
        const int type = m_world.protos[static_cast<size_t>(in.protoId)].header.typeId;
        if (type == kTileFloor || type == kTileRoof || type == kTileStair) {
            continue;
        }
        m_nav.MarkBox(m_nav.stat, in.min[0], in.min[2], in.max[0], in.max[2], in.max[1],
                      &m_nav.statTop);
        // MILESTONE 8: what this tile does to a line of fire.  The four flag
        // bits are named in move.h; a tile that is neither Ethereal nor Window
        // is opaque, and the Cover bit doubles the penalty of one that is not.
        const uint16_t flags = m_world.protos[static_cast<size_t>(in.protoId)].header.flags;
        uint8_t cls = kLosSolid;
        if ((flags & (kTileEthereal | kTileWindow)) != 0) {
            cls = (flags & kTileCover) != 0 ? kLosBonusCover : kLosCover;
        }
        const int32_t lx0 = m_nav.CellX(in.min[0]), lx1 = m_nav.CellX(in.max[0] - 1);
        const int32_t lz0 = m_nav.CellZ(in.min[2]), lz1 = m_nav.CellZ(in.max[2] - 1);
        for (int32_t cz = lz0; cz <= lz1; ++cz) {
            for (int32_t cx = lx0; cx <= lx1; ++cx) {
                if (!m_nav.In(cx, cz)) continue;
                const size_t i = m_nav.Idx(cx, cz);
                if (m_nav.statTop[i] == kNoFloor ||
                    static_cast<int32_t>(m_nav.statTop[i]) > in.max[1]) {
                    continue;   // a taller tile already speaks for this cell
                }
                m_nav.statTop[i] = static_cast<int16_t>(in.max[1]);
                m_nav.los[i] = cls;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// MILESTONE 13 -- LOCAL AVOIDANCE AND ARRIVAL SPREAD
//
// `DirSearch::DirSearch @00511370` builds its 36
// candidates as the MOVING ACTOR'S OWN integer bounding box translated by
// (dx, dy, dz), and `DirSearch::TestVisibilityAndCount @005115b0` tests that
// translated box against the world.  So "can I stand here" is a box test, not
// a point test, and another actor's box is part of the world:
// `ComputeDirectionToFocusWithObstacleCheck @00536410`
// builds exactly such a box around the step it is about to take,
// grows it by the search object's half-extents at +0x6d/+0x71/+0x75, runs
// `ScanRegionForClippedLines` with a `ListCollision` visitor over it, and
// records the first hit whose flag byte (`entity+2` bit 4) is set at
// search+0x80 -- the blocking entity.
//
// `Entity::Entity_FindNearestFreeGroundTileAndReposition @00587690`
// (`Entity::Entity_FindNearestFreeGroundTileAndReposition@00587690`) is the resolution.  It is a ring search in
// CELL units around the entity's own cell:
//
//     minX--, maxX++, minZ--, maxZ++          one cell per iteration
//     walk the four sides, stepping TWO cells at a time
//     test each with the placement predicate (vtable slot 0x7a4)
//     first hit: write pos, MaybeFixBoundingBox, return 1
//
// bounded by `param_1_00[5] - param_1_00[2]` iterations.  What the predicate
// is, is the same box test.
//
// RECONSTRUCTED: the iteration bound (the two ints it is built from are the
// search object's, and DirSearch's own field layout is not in any dump), taken
// here as kFreeTileRings.
// ---------------------------------------------------------------------------

// The translated-bounding-box test.  `self` is excluded; every other live
// actor's box blocks, which is what makes six squad members refuse to stand on
// each other.
bool Scene::ActorBoxFree(const LiveEntity& e, int32_t wx, int32_t wz, bool ignoreActors) const {
    const int32_t bw = std::max(1, e.bbox[0]), bd = std::max(1, e.bbox[2]);
    const int32_t x0 = wx - bw / 2, z0 = wz - bd / 2;
    const int32_t cx0 = m_nav.CellX(x0), cx1 = m_nav.CellX(x0 + bw - 1);
    const int32_t cz0 = m_nav.CellZ(z0), cz1 = m_nav.CellZ(z0 + bd - 1);
    for (int32_t cz = cz0; cz <= cz1; ++cz) {
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            if (!m_nav.In(cx, cz)) return false;
            if (!m_nav.HasFloor(cx, cz)) return false;
            if (m_nav.stat[m_nav.Idx(cx, cz)] != 0) return false;
        }
    }
    if (ignoreActors) return true;
    const int32_t x1 = x0 + bw, z1 = z0 + bd;
    for (const LiveEntity& o : m_entities) {
        if (&o == &e || !o.isActor || o.dead) continue;
        const int32_t ow = std::max(1, o.bbox[0]), od = std::max(1, o.bbox[2]);
        const int32_t ox0 = o.world[0] - ow / 2, oz0 = o.world[2] - od / 2;
        if (x0 < ox0 + ow && ox0 < x1 && z0 < oz0 + od && oz0 < z1) return false;
    }
    return true;
}

// `Entity::Entity_FindNearestFreeGroundTileAndReposition @00587690`, ported.
bool Scene::FindNearestFreeGroundTile(const LiveEntity& e, int32_t wx, int32_t wz, int32_t* outX,
                                      int32_t* outZ) const {
    if (ActorBoxFree(e, wx, wz, false)) {
        *outX = wx;
        *outZ = wz;
        return true;
    }
    // The ring walks CELLS; kNavCell is one world unit (NOTES.md section 27),
    // so a cell step and a world step are the same thing here.
    const int32_t cx = m_nav.CellX(wx), cz = m_nav.CellZ(wz);
    int32_t minX = cx, maxX = cx, minZ = cz, maxZ = cz;
    for (int32_t ring = 0; ring < kFreeTileRings; ++ring) {
        ++maxX;
        ++maxZ;
        --minX;
        --minZ;
        // The four sides, stepping two cells at a time -- the original
        // advances its cursor by 2 on each of its four loops.
        for (int32_t side = 0; side < 4; ++side) {
            for (int32_t i = (side < 2 ? minX : minZ); i <= (side < 2 ? maxX : maxZ); i += 2) {
                const int32_t tx = side == 0 ? i : (side == 1 ? i : (side == 2 ? minX : maxX));
                const int32_t tz = side == 0 ? minZ : (side == 1 ? maxZ : i);
                const int32_t w0 = m_nav.CentreX(tx), w2 = m_nav.CentreZ(tz);
                if (ActorBoxFree(e, w0, w2, false)) {
                    *outX = w0;
                    *outZ = w2;
                    return true;
                }
            }
        }
    }
    return false;
}

void Scene::RefreshDynamicBlockers() {
    std::fill(m_nav.dyn.begin(), m_nav.dyn.end(), 0);
    for (const LiveEntity& e : m_entities) {
        if (e.isDoor) {
            // A door does not block a path.  `RotatingDoor::CanDoorOpen
            // @005b8c40` is the gate Door::SetActivationState consults before
            // opening, and the auto-open in
            // Door_UpdateProximityAutoOpenTimer runs on approach, so an
            // unlocked door is a passable cell that happens to play an
            // animation as you reach it.  A LOCKED one would block -- but the
            // lock state lives in `Door::IsValidTargetForSkill`'s lockpick
            // fields and no shipped door `.ent` in
            // mission01 writes them, so every door here is unlocked.
            // RECONSTRUCTION: doors never enter the blocker set.
            continue;
        }
        if (!e.isActor) {
            continue;       // loose items, waypoints, lights: no collision here
        }
        const int32_t x0 = e.world[0] - e.bbox[0] / 2;
        const int32_t z0 = e.world[2] - e.bbox[2] / 2;
        m_nav.MarkBox(m_nav.dyn, x0, z0, x0 + std::max(1, e.bbox[0]),
                      z0 + std::max(1, e.bbox[2]), e.world[1] + e.bbox[1]);
    }
}

void Scene::SetEntityWorldPos(LiveEntity& e, const float pos[3]) {
    for (int k = 0; k < 3; ++k) {
        e.wpos[k] = pos[k];
        // Entity::ClassType::Serialize l.15133: round((pos + spriteAdjust) * 4),
        // round-to-nearest.
        e.world[k] = static_cast<int32_t>(std::lround(pos[k] * kEntityWorldScale));
    }
    ProjectWorldToScreen(e.world[0], e.world[1], e.world[2], &e.sx, &e.sy);
    EntityWorldBox(e.world, e.wmin, e.wmax);
    RefreshEntityFrame(e);
}

void Scene::SetFacingFromVector(LiveEntity& e, float dx, float dz) {
    if (dx == 0.0f && dz == 0.0f) {
        return;
    }
    // `Entity::Entity_UpdateFacingFromVelocityAndBounds @005e7080` after
    // `BuildOrientationBasisFromForward(velocity)`.  The quantisation is
    // `Entity::Entity_UpdateFacingFromVelocityAndBounds@005dfe90`:
    //
    //     if (m02 == 0) theta = (m00 == 0) ? 0 : (m00 >= 0 ? pi/2 : 3pi/2);
    //     else { theta = fpatan(m00, m02); if (theta < 0) theta += 2pi; }
    //     dir = round(theta / 2pi * dirCount);        // then ONE subtraction
    //     if (dir >= dirCount) dir -= dirCount;       // and a hard clamp to 0
    //     if (dir >= dirCount) dir = 0;
    //
    // The degenerate branch is what pins the argument order: it fires on
    // `m02 == 0` (the divisor) and takes its +-pi/2 from the *sign of m00*, so
    // the angle is `atan2(m00, m02)` -- x over z, not z over x.  Row 0 of the
    // basis `BuildOrientationBasisFromForward` writes at Entity+0x8e is the
    // forward vector (m00 = fx at +0x8e, m02 = fz at +0x96), so
    //
    //     theta = atan2(dx, dz)
    //
    // i.e. the facing angle is measured from +z towards +x.  MILESTONE 13: this
    // was `atan2(dz, dx)` through milestones 7-12, which is the reflection
    // `pi/2 - theta` -- it agrees on the two directions that lie on the screen's
    // horizontal (dir 3 and dir 7) and mirrors every other one, which is why
    // actors walked sideways.  Section 63 has the whole table and how the
    // deathclaw contact sheet confirms it.
    static const double kTwoPi = 6.283185307179586476925286766559;
    double theta = std::atan2(static_cast<double>(dx), static_cast<double>(dz));
    if (theta < 0.0) {
        theta += kTwoPi;
    }
    int32_t dir = static_cast<int32_t>(
        std::floor(theta / kTwoPi * static_cast<double>(e.dirCount) + 0.5));
    if (dir >= e.dirCount) {
        dir -= e.dirCount;
    }
    if (dir >= e.dirCount) {
        dir = 0;
    }
    if (dir != e.direction) {
        e.direction = dir;
        RefreshEntityFrame(e);
    }
}

// `Actor::SelectDestinationAnimationState @0055b5e0`.
// The three animation strings it can choose between are unresolved globals
// (DAT_008bc824 / DAT_008bc7fc for the two moving ones, GBL_StringStand and
// GBL_StringClimb for the rest), so their TEXT is not recoverable from the
// decompilation.  The sprite data supplies it and leaves no room for doubt:
// every character `.spr` carries exactly `<Posture>Walk` for all three postures
// and `StandRun` alone -- there is no CrouchRun or ProneRun -- which is also
// why a crouching actor ordered to run walks.
void Scene::SelectDestinationAnimationState(LiveEntity& e) {
    std::string want;
    if (e.moving) {
        want = e.running ? "Run" : "Walk";
        if (want == "Run" && e.posture != "Stand") {
            want = "Walk";
        }
    }
    if (want == e.action) {
        return;
    }
    e.action = want;
    ApplyAction(e);
}

// TryApplySequenceNameVariants @005e28a0 concatenates base1+base2+suffix1+
// suffix2 and retries with the suffixes dropped; its caller (Actor vtable slot
// 218) is in no dump, so which parts it passes is NOT recoverable.  What the
// data shows is <Posture><Action>[<Weapon>] for actors and <Action><In|Out>
// for doors, and this tries exactly those, longest first.
bool Scene::ApplyAction(LiveEntity& e) {
    if (e.isDoor) {
        const char* suffix = e.doorOutwards ? "Out" : "In";
        if (SetAnimationType(e, e.action + suffix)) {
            return true;
        }
        if (SetAnimationType(e, e.action + (e.doorOutwards ? "In" : "Out"))) {
            return true;
        }
        return SetAnimationType(e, e.action) || SetAnimationType(e, "default");
    }
    const std::string base = e.action.empty() ? "Breathe" : e.action;
    if (SetAnimationType(e, e.posture + base)) {
        return true;
    }
    if (SetAnimationType(e, e.posture)) {
        return true;
    }
    return SetAnimationType(e, "default");
}

bool Scene::SetPosture(int32_t index, const std::string& posture) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) {
        return false;
    }
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    if (!e.isActor || e.posture == posture) {
        return false;
    }
    e.hasOrder = true;
    // The transition sequence is `<from><to>`: StandCrouch, CrouchStand,
    // CrouchProne, ProneCrouch are the four that exist in every character
    // sprite.  Stand <-> Prone has no sequence, so it goes through Crouch, one
    // step per order, exactly as the sprite set allows.
    std::string to = posture;
    if (e.posture == "Stand" && posture == "Prone") {
        to = "Crouch";
    } else if (e.posture == "Prone" && posture == "Stand") {
        to = "Crouch";
    }
    if (!SetAnimationType(e, e.posture + to)) {
        // No transition art: change posture outright.
        e.posture = to;
        e.pendingPosture.clear();
        ApplyAction(e);
        return true;
    }
    e.pendingPosture = to;
    e.animates = true;
    return true;
}

bool Scene::ScreenToGround(int64_t camX, int64_t camY, int32_t px, int32_t py, int32_t* wx,
                           int32_t* wz) const {
    // Invert `ComputeIsoScreenOffset @0044de60` on a plane of constant
    // elevation:  SX = 6*(x - z), SY = -3*(x + z) - 7*y.
    const double SX = static_cast<double>(camX + px);
    const double SY = static_cast<double>(camY + py);
    double y = 0.0;
    int32_t bx = 0, bz = 0;
    bool got = false;
    for (int it = 0; it < 6; ++it) {
        const double sum = (-SY - static_cast<double>(kIsoHeight) * y) /
                           static_cast<double>(kIsoHalfWidth);
        const double diff = SX / (2.0 * kIsoHalfWidth);
        const double x = (diff + sum) * 0.5;
        const double z = (sum - diff) * 0.5;
        bx = static_cast<int32_t>(std::lround(x));
        bz = static_cast<int32_t>(std::lround(z));
        const int32_t cx = m_nav.CellX(bx), cz = m_nav.CellZ(bz);
        const int32_t s = m_nav.Surface(cx, cz);
        got = s != kNoFloor;
        const double ny = got ? static_cast<double>(s) : 0.0;
        if (ny == y) {
            break;
        }
        y = ny;
    }
    *wx = bx;
    *wz = bz;
    return got;
}

bool Scene::OrderMove(int32_t index, int32_t worldX, int32_t worldZ, bool run) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) {
        return false;
    }
    // `World::IsEntityUsableThisTurn @00507270` -- see OrderAttack.
    if (m_combatMode != 0 && ActiveTurnEntity() != index) {
        return false;
    }
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    if (!e.isActor) {
        return false;
    }
    RefreshDynamicBlockers();
    // An actor never blocks its own start cell: clear the cells its own box
    // covers before searching.
    {
        const int32_t x0 = e.world[0] - e.bbox[0] / 2, z0 = e.world[2] - e.bbox[2] / 2;
        for (int32_t z = m_nav.CellZ(z0); z <= m_nav.CellZ(z0 + std::max(1, e.bbox[2]) - 1); ++z) {
            for (int32_t x = m_nav.CellX(x0); x <= m_nav.CellX(x0 + std::max(1, e.bbox[0]) - 1);
                 ++x) {
                if (m_nav.In(x, z)) {
                    m_nav.dyn[m_nav.Idx(x, z)] = 0;
                }
            }
        }
    }
    PathRequest req;
    req.startX = m_nav.CellX(e.world[0]);
    req.startZ = m_nav.CellZ(e.world[2]);
    req.goalX = m_nav.CellX(worldX);
    req.goalZ = m_nav.CellZ(worldZ);
    // `DirSearch::DirSearch`'s `param_1` is the moving actor's own bounding
    // box; every candidate is that box translated.  RaiderMale is 3 x 8 x 3
    // world units, so the footprint here is 3 x 3.
    req.footW = std::max(1, e.bbox[0]);
    req.footD = std::max(1, e.bbox[2]);
    PathResult p = FindPath(m_nav, req);
    m_lastPathExact = p.ok && p.exact;
    m_lastPathExpanded = p.expanded;
    if (!p.ok || p.points.size() < 2) {
        return false;
    }
    // RunPathfindingSearchStep only emits a waypoint where the step direction
    // changes, so the walk is over the corners,
    // not over every cell.
    CollapseCollinear(&p);
    e.path = p.points;
    e.pathAt = 1;               // [0] is the cell the actor already stands in
    e.moving = true;
    e.running = run;
    e.hasOrder = true;
    e.arrivedTick = -1;
    e.goal[0] = worldX;
    e.goal[1] = m_nav.Surface(req.goalX, req.goalZ);
    e.goal[2] = worldZ;
    SelectDestinationAnimationState(e);
    return true;
}

void Scene::StopMove(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) {
        return;
    }
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    e.moving = false;
    e.path.clear();
    e.pathAt = 0;
    SelectDestinationAnimationState(e);
}

const std::vector<PathPoint>* Scene::PathOf(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) {
        return nullptr;
    }
    return &m_entities[static_cast<size_t>(index)].path;
}

// `Actor::AdvanceAlongPathAndUpdateFacing @0055b4a0`
// plus the integration Actor::Move does around it.  The two constants in the
// original's arrival test -- `|node.y - goal.y| < 9` and `dist^2 < 0x191` --
// are a test of the NODE against the search object's own goal triple at +0x98,
// i.e. "the node I am walking to is the last one", and they are in the units of
// a search node, whose spacing DirSearch::DirSearch decides and which is not in
// any dump.  This uses the equivalent test on this grid: the actor arrives when
// it reaches the final path point.
void Scene::StepMovement(LiveEntity& e) {
    if (!e.moving) {
        return;
    }
    if (e.pathAt >= e.path.size()) {
        e.moving = false;
        e.path.clear();
        SelectDestinationAnimationState(e);
        return;
    }
    float budget = FrameMoveDistance(e);
    if (budget <= 0.0f) {
        return;   // a zero-distance frame: the animation is between strides
    }
    float pos[3] = {e.wpos[0], e.wpos[1], e.wpos[2]};
    int guard = 0;
    while (budget > 0.0f && e.pathAt < e.path.size() && ++guard < 64) {
        const PathPoint& t = e.path[e.pathAt];
        const float tx = t.x * kWorldToMetres, tz = t.z * kWorldToMetres;
        const float dx = tx - pos[0], dz = tz - pos[2];
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d <= budget || d <= 1e-4f) {
            pos[0] = tx;
            pos[2] = tz;
            pos[1] = t.y * kWorldToMetres;
            budget -= d;
            ++e.pathAt;
            if (e.pathAt < e.path.size()) {
                const PathPoint& n = e.path[e.pathAt];
                SetFacingFromVector(e, n.x * kWorldToMetres - pos[0],
                                    n.z * kWorldToMetres - pos[2]);
            }
        } else {
            pos[0] += dx / d * budget;
            pos[2] += dz / d * budget;
            // Elevation follows the grid under the actor's feet.
            const int32_t s = m_nav.Surface(m_nav.CellX(static_cast<int32_t>(
                                                std::lround(pos[0] * kEntityWorldScale))),
                                            m_nav.CellZ(static_cast<int32_t>(
                                                std::lround(pos[2] * kEntityWorldScale))));
            if (s != kNoFloor) {
                pos[1] = s * kWorldToMetres;
            }
            SetFacingFromVector(e, dx, dz);
            budget = 0.0f;
        }
    }
    // MILESTONE 8.  `Actor::Move @0055ba20` charges
    // the distance covered against the actor's AP, but only in turn-based
    // mode: `hexesPerAP = (1 + 0.2 * BonusMove) * stanceDivisor` with the
    // divisor 1.5 standing or running, 0.85 crouching, 0.65 prone and 1.0 on a
    // ladder, and Tunnel Rat doubling it while prone.  In mode 0 the divisor is
    // left at zero and movement is free.  The five divisors were unresolved
    // `_DAT_00824cd8..ce8` in milestone 7; they are read out of .rdata now.
    // MILESTONE 13 -- LOCAL AVOIDANCE.  Before the step is committed, the
    // actor's own bounding box translated to the new position is tested
    // against the world and against every other actor's box, which is
    // `DirSearch`'s candidate test and the `ListCollision` sweep
    // `ComputeDirectionToFocusWithObstacleCheck @00536410` runs over the step.
    // A blocked step is not taken.  If the goal is what is blocked, the actor
    // settles on the nearest free ground tile around it
    // (`Entity_FindNearestFreeGroundTileAndReposition @00587690`); otherwise it
    // replans, which is the binary's own answer to a recorded blocker -- the
    // search object's +0x80 is read by Actor::Move and the path is rebuilt.
    {
        const int32_t nx = static_cast<int32_t>(std::lround(pos[0] * kEntityWorldScale));
        const int32_t nz = static_cast<int32_t>(std::lround(pos[2] * kEntityWorldScale));
        if ((nx != e.world[0] || nz != e.world[2]) && !ActorBoxFree(e, nx, nz, false)) {
            ++e.blockedTicks;
            const int32_t gdx = e.goal[0] - e.world[0], gdz = e.goal[2] - e.world[2];
            const int32_t near2 = (kFreeTileRings + std::max(e.bbox[0], e.bbox[2])) *
                                  (kFreeTileRings + std::max(e.bbox[0], e.bbox[2]));
            if (gdx * gdx + gdz * gdz <= near2) {
                // Close enough that this IS the arrival: take the nearest free
                // tile around the ordered destination and stop there.
                int32_t fx = 0, fz = 0;
                if (FindNearestFreeGroundTile(e, e.goal[0], e.goal[2], &fx, &fz)) {
                    float np[3] = {fx * kWorldToMetres, e.wpos[1], fz * kWorldToMetres};
                    const int32_t s2 = m_nav.Surface(m_nav.CellX(fx), m_nav.CellZ(fz));
                    if (s2 != kNoFloor) np[1] = s2 * kWorldToMetres;
                    SetEntityWorldPos(e, np);
                }
                e.moving = false;
                e.arrivedTick = m_ticks;
                e.path.clear();
                e.blockedTicks = 0;
                SelectDestinationAnimationState(e);
                RefreshDynamicBlockers();
                return;
            }
            // Held up mid-route: replan around the obstacle, at most once a
            // second so two actors cannot livelock replanning at each other.
            if (e.blockedTicks >= kBlockedTicksBeforeRepath &&
                (e.repathTick < 0 || m_ticks - e.repathTick > 14)) {
                e.repathTick = m_ticks;
                e.blockedTicks = 0;
                const int32_t self = static_cast<int32_t>(&e - m_entities.data());
                const int32_t gx = e.goal[0], gz = e.goal[2];
                const bool run = e.running;
                if (OrderMove(self, gx, gz, run)) return;
            }
            return;   // stand still this tick
        }
        e.blockedTicks = 0;
    }
    if (m_combatMode != 0 && e.isCombatant) {
        const float moved = std::sqrt((pos[0] - e.wpos[0]) * (pos[0] - e.wpos[0]) +
                                      (pos[1] - e.wpos[1]) * (pos[1] - e.wpos[1]) +
                                      (pos[2] - e.wpos[2]) * (pos[2] - e.wpos[2]));
        float divisor = k::kHexPerApStand;
        if (e.posture == "Crouch") divisor = k::kHexPerApCrouch;
        else if (e.posture == "Prone") divisor = k::kHexPerApProne;
        float hexesPerAp =
            (static_cast<float>(e.attr.bonusMove) * k::kBonusMovePerRank + 1.0f) * divisor;
        if (hexesPerAp > 0.0f) {
            e.ap -= moved / hexesPerAp;
            if (e.ap < 0.05f) e.ap = 0.0f;
        }
        if (e.ap <= 0.0f) {
            SetEntityWorldPos(e, pos);
            e.moving = false;
            e.path.clear();
            SelectDestinationAnimationState(e);
            return;
        }
    }
    SetEntityWorldPos(e, pos);
    if (e.pathAt >= e.path.size()) {
        // MILESTONE 13.  Arrived.  If the box overlaps another actor's -- two
        // actors ordered to the same point, or one that walked onto this spot
        // while this one was en route -- take the nearest free ground tile
        // around it: `Entity::Entity_FindNearestFreeGroundTileAndReposition
        // @00587690`, which is what stops a squad settling in a heap.
        if (!ActorBoxFree(e, e.world[0], e.world[2], false)) {
            int32_t fx = 0, fz = 0;
            if (FindNearestFreeGroundTile(e, e.world[0], e.world[2], &fx, &fz)) {
                float np[3] = {fx * kWorldToMetres, e.wpos[1], fz * kWorldToMetres};
                const int32_t s3 = m_nav.Surface(m_nav.CellX(fx), m_nav.CellZ(fz));
                if (s3 != kNoFloor) np[1] = s3 * kWorldToMetres;
                SetEntityWorldPos(e, np);
            }
        }
        e.moving = false;
        e.arrivedTick = m_ticks;
        e.path.clear();
        e.blockedTicks = 0;
        SelectDestinationAnimationState(e);
    }
}

// `Door::SetActivationState @005b2690`: the four action
// strings, the CanDoorOpen gate, and the bounding-box swap.
void Scene::SetDoorState(LiveEntity& e, const std::string& state) {
    if (e.action == state) {
        return;
    }
    e.action = state;
    if (state == "Opened") {
        e.doorOpen = true;
    } else if (state == "Closed") {
        e.doorOpen = false;
    }
    // MILESTONE 13: the door families of `sound/Game` are <name>Open /
    // <name>Close, and the sound belongs to the transition, not the rest
    // state -- the -44 marker sits at frame 0 of Opening / Closing.
    if (state == "Opening" || state == "Closing") {
        PlaySoundStem(e, snd::DoorStem(e.spritePath, state == "Opening"),
                      kChanDefault, state == "Opening" ? "door-open" : "door-close");
    }
    // AdjustBoundingBoxForOpenDoor runs at the START of Opening and at the end
    // of the animation; the box that matters for pathing is the one at the end,
    // so the blocker is cleared only when the door reaches "Opened".
    ApplyAction(e);
}

// `Door::Door_UpdateProximityAutoOpenTimer @005b28f0`:
// distance from the door to the nearest entity, minus half the summed x+z
// extents of the two boxes, against the radius at Door+0x1b9.
void Scene::DoorThink(LiveEntity& e) {
    const float doorHalf =
        static_cast<float>(e.bbox[0] + e.bbox[2]) * 0.5f * kWorldToMetres * 0.5f;
    bool near = false;
    for (const LiveEntity& a : m_entities) {
        if (!a.isActor) {
            continue;
        }
        const float dx = e.wpos[0] - a.wpos[0];
        const float dy = e.wpos[1] - a.wpos[1];
        const float dz = e.wpos[2] - a.wpos[2];
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        d -= doorHalf +
             static_cast<float>(a.bbox[0] + a.bbox[2]) * 0.5f * kWorldToMetres * 0.5f;
        if (d < 0.0f) {
            d = 0.0f;
        }
        if (d <= kDoorOpenRadius) {
            near = true;
            break;
        }
    }
    const int64_t now = m_ticks * kTickMs;
    if (near) {
        e.doorHoldUntilMs = now + kDoorHoldMs;
        if (e.action == "Closed" || e.action == "Closing" || e.action.empty()) {
            SetDoorState(e, "Opening");
        }
    } else if ((e.action == "Opened" || e.action == "Opening") && now >= e.doorHoldUntilMs) {
        SetDoorState(e, "Closing");
    }
}

std::string Scene::TraceMovement() const {
    std::string out;
    char buf[512];
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        if (!e.moving && e.action.empty() && !e.isDoor && !e.hasOrder) {
            continue;
        }
        if (e.isDoor && e.action == "Closed") {
            continue;
        }
        const int32_t cx = m_nav.CellX(e.world[0]), cz = m_nav.CellZ(e.world[2]);
        std::snprintf(buf, sizeof(buf),
                      "  [%zu] %-20s %-9s world %4d,%3d,%4d cell %3d,%3d %-6s dir %d/%d "
                      "seq %-14s frame %-3d step %.3f node %2zu/%zu blocked=%d floor=%d "
                      "goaldist=%.1f arrived=%lld\n",
                      i, e.label.c_str(), e.action.empty() ? "(idle)" : e.action.c_str(),
                      e.world[0], e.world[1], e.world[2], cx, cz, e.posture.c_str(), e.direction,
                      e.dirCount, e.sequenceName.c_str(), e.frame, FrameMoveDistance(e), e.pathAt,
                      e.path.size(),
                      m_nav.In(cx, cz) ? (m_nav.stat[m_nav.Idx(cx, cz)] ? 1 : 0) : -1,
                      m_nav.HasFloor(cx, cz) ? 1 : 0,
                      std::sqrt(static_cast<double>((e.world[0] - e.goal[0]) *
                                                        (e.world[0] - e.goal[0]) +
                                                    (e.world[2] - e.goal[2]) *
                                                        (e.world[2] - e.goal[2]))),
                      (long long)e.arrivedTick);
        out += buf;
    }
    return out;
}


// ===========================================================================
// MILESTONE 8 -- combat.  See combat.h for where every table and constant
// comes from, and NOTES.md sections 19-24 for the derivations.
// ===========================================================================

namespace {

// `ComputeWeaponAttackHitChance @00615e80` measures the gap between the two
// bounding boxes, not the centres:
//     gap = |dp| - (sumExtents(a) + sumExtents(b)) * 0.5 * 0.25
// with `sumExtents = (x2-x1) + (z2-z1)` and the product of _DAT_008bd194 (0.5)
// and _DAT_007fd598 (0.25).  The result is floored at 0.001 (_DAT_0080a6e8).
// The same expression appears verbatim in Actor::Actor_HandleFireWeaponCommand
// (`Actor::Actor_HandleFireWeaponCommand@00546760`) as the range test.
constexpr float kGapFloor = 0.001f;

// The range-class factor: `local_40` in ComputeWeaponAttackHitChance, set from
// the mode's `Range Class` string at `ComputeWeaponAttackHitChance@00615e80`.
int32_t RangeClassFactor(const std::string& rangeType, bool* isLongRange) {
    if (rangeType == "Scoped" || rangeType == "OnlyLong") {
        *isLongRange = true;
        return 5;
    }
    if (rangeType == "Long") {
        *isLongRange = true;
        return 4;
    }
    *isLongRange = false;
    return 2;   // Melee, Short, Thrown and anything unnamed
}

}  // namespace

// ---------------------------------------------------------------------------
// Loading one actor's character sheet and equipment.
// ---------------------------------------------------------------------------
void Scene::LoadCombatant(LiveEntity& e, const Esh& instance, const EntProto* proto) {
    // Every property is taken from the instance when the instance carries it
    // and from the prototype otherwise -- the same override rule `.mis` uses
    // for the rest of the entity (mapview/NOTES.md 14).
    const Esh* protoEsh = proto != nullptr ? &proto->block.esh : nullptr;
    auto pick = [&](const char* name) -> const Esh* {
        if (instance.Find(name) != nullptr) return &instance;
        if (protoEsh != nullptr && protoEsh->Find(name) != nullptr) return protoEsh;
        return nullptr;
    };

    std::string err;
    Esh block;
    if (const Esh* src = pick("Attributes")) {
        if (src->GetNested("Attributes", &block, &err)) {
            ReadAttributes(block, &e.attr, false);
        }
    }
    // MILESTONE 22.  `Modifiers` is a SECOND `<attribs>` blob, a sibling of
    // `Attributes` and the same 197-int layout, and it is ADDITIVE: it is the
    // permanent modifier block `Attributes::SumFinalAttributes` folds in before
    // the derived rows are computed.  Only one shipped actor in Brahmin Wood
    // carries a non-zero one -- `dog_02_nerf.ent`, `armorClass -3` -- and it is
    // exactly the difference between the port's dog (AC 6, agility alone) and
    // the original's (AC 3): FTSE's `ENT` records log 3 for every dog in the
    // session.  Reading it costs nothing anywhere else, because every other
    // shipped `Modifiers` blob is all zeroes.
    if (const Esh* src = pick("Modifiers")) {
        Esh mods;
        if (src->GetNested("Modifiers", &mods, &err)) {
            ReadAttributes(mods, &e.attr, true);
        }
    }
    // MILESTONE 21.  The `.ent` sheet is the BASE; every derived row and every
    // skill percentage is summed on top of it here, by the two functions
    // `Attributes::Derive` transcribes.  Milestone 8 had the max-HP line as a
    // fallback with a hard-coded 15 -- which turns out to be the Human race
    // bonus -- and the eighteen skills as a reconstruction that only applied
    // where the `.ent` stored a zero; both are now the binary's own.
    // MILESTONE 25.  The levelled actor.  The `.mis` instance carries an int
    // `Level` -- `Actor.ct_.npclevel`, written by the actor serializer
    // (`Actor::ClassType::Serialize@00570cb0`) with a default of 0 -- and every named and BOS
    // actor in Brahmin Wood has one above 1 while every raider and tribal has
    // 1.  `Actor::HandleXPForNewPlayerCharacterMaybe @00577830` is what turns
    // it into stats at spawn, and this is that call: it runs on the sheet AS
    // READ, before the derivation, because the grant it applies is a permanent
    // `maxHitPoints` delta of exactly the kind the `.ent` would have stored.
    // The `IsAttachedBaseAIMaybe` arm of the original also adds a campaign-wide
    // level offset (`WorldGlobal->unknown4 + 0x28`) before the grant; nothing
    // in this port keeps that counter, and a mission started fresh has it at
    // zero.
    int32_t npcLevel = 0;
    if (instance.Find("Level") != nullptr) instance.GetInt("Level", &npcLevel);
    const int grantedLevels = ApplyStoredLevel(&e.attr, npcLevel);

    e.attr.Derive();
    if (grantedLevels > 0) {
        // `Actor::Levelup @00577280` -> `AutoAllocateLevelUpPoints @005775c0`,
        // whose deterministic arm spends `(skillPoints * 5) / 18` on each
        // tagged skill.  Its two random arms are not ported; see combat.h.
        AutoAllocateLevelUpPoints(&e.attr);
    }

    // `Current Attributes` is the only part of the sheet READ BACK rather than
    // recomputed: it is the live `ActorStatus`, so its `hitPoints` /
    // `actionPoints` are the CURRENT values, not the maxima.  Milestone 8 took
    // maxHp and maxAp from it as well.
    e.maxHp = e.attr.maxHitPoints;
    e.maxAp = e.attr.maxActionPoints;
    e.hp = e.maxHp;
    e.ap = static_cast<float>(e.maxAp);
    if (const Esh* src = pick("Current Attributes")) {
        Esh cur;
        if (src->GetNested("Current Attributes", &cur, &err)) {
            int32_t hp = 0;
            float ap = 0.0f;
            // MILESTONE 22: a saved actor at zero hit points is a CORPSE, and
            // has to come back as one.  Milestone 8 read this field with a
            // `> 0` guard, so a casualty in a save stood back up at full
            // health when the mission was resumed.
            if (cur.GetInt("hitPoints", &hp)) {
                if (hp > 0) {
                    e.hp = std::min(hp, e.maxHp);
                } else {
                    e.hp = 0;
                    e.dead = true;
                }
            }
            if (cur.GetFloat("actionPoints", &ap) && ap > 0.0f) {
                e.ap = std::min(ap, static_cast<float>(e.maxAp));
            }
        }
    }
    // `Actor::HandleXPForNewPlayerCharacterMaybe` finishes with
    // `(this->ct_).actor_status.hp = current_stats.derived.maxhitpoints`, so a
    // levelled actor starts at its NEW maximum and not at the hit points the
    // instance was authored with -- the elder is logged at 45/45 while its
    // `Current Attributes/hitPoints` says 30.
    if (grantedLevels > 0 && !e.dead) e.hp = e.maxHp;

    // MILESTONE 9.  The portrait key and the rank badge.  `WPortrait::LoadPortrait
    // @004bc020` takes an id, not a path, and its body is in no dump -- but the
    // shipped art settles the naming: every recruit's `Display Name` is exactly
    // the stem of a file in `gui/char/` (hm01.ent says "CORE_rec_hm01" and
    // `gui/char/CORE_rec_hm01.zar` ships), and its `Rank` is exactly the stem of
    // a file in `gui/rank/` ("Initiate" -> `gui/rank/Initiate.zar`).  The
    // fallback the art itself provides is `gui/char/no_char.zar`.
    if (const Esh* src = pick("Display Name")) src->GetString("Display Name", &e.displayName);
    if (const Esh* src = pick("Rank")) src->GetString("Rank", &e.rank);

    // Hand1 is the active slot: `Actor::GetIDOfEquippedItemInActiveSlot`
    // returns ct_.slot_equip[handselected], and every shipped actor starts on
    // hand 1.  Hand1_ is the .mis instance's override of it.
    ft::EntityBlock inner;
    const char* const kHandSlots[] = {"Hand1_", "Hand1"};
    for (const char* slot : kHandSlots) {
        if (instance.Find(slot) != nullptr && ReadSlotEntity(instance, slot, &inner) &&
            inner.className == "Weapon") {
            ReadWeapon(inner.esh, &e.weapon);
            break;
        }
        if (protoEsh != nullptr && ReadSlotEntity(*protoEsh, slot, &inner) &&
            inner.className == "Weapon") {
            ReadWeapon(inner.esh, &e.weapon);
            break;
        }
    }
    // Hand2, the second `WIIHand` the game bar draws.  `slot_equip[0]` and
    // `slot_equip[1]` are the two hands and `slot_equip[2]` is the armour --
    // those three indices are the only ones used anywhere (`Actor::Actor_HandleGoToCommandOrQueueAttack@00546100`
    // and `Actor::UpdateSprite@00540e50`).
    for (const char* slot : {"Hand2_", "Hand2"}) {
        if (instance.Find(slot) != nullptr && ReadSlotEntity(instance, slot, &inner) &&
            inner.className == "Weapon") {
            ReadWeapon(inner.esh, &e.weapon2);
            break;
        }
        if (protoEsh != nullptr && ReadSlotEntity(*protoEsh, slot, &inner) &&
            inner.className == "Weapon") {
            ReadWeapon(inner.esh, &e.weapon2);
            break;
        }
    }

    // MILESTONE 17 correction.  `unarmedSlot1` / `unarmedSlot2` are NOT hand
    // slots: they are `Actor.ct_.unarmed_left` and `unarmed_right`, the two
    // invisible weapons `Actor::LevelUpUnarmedAttacks @0054f4e0` keeps outside
    // `slot_equip` and refills on every level-up from `tables/races.txt`.
    // Milestone 8 read `unarmedSlot1` into HAND ONE and only when hand one was
    // empty, which is why an actor whose `.ent` omits the pair -- every one of
    // the 36 human recruits -- could not attack at all.  Both are read here,
    // into their own fields; `EquipUnarmed` below fills whichever the data
    // leaves out from the race tables and then puts them in the empty hands.
    for (int u = 0; u < 2; ++u) {
        const char* slot = u == 0 ? "unarmedSlot1" : "unarmedSlot2";
        WeaponDef& out = u == 0 ? e.unarmedPunch : e.unarmedKick;
        if (instance.Find(slot) != nullptr && ReadSlotEntity(instance, slot, &inner) &&
            inner.className == "Weapon") {
            ReadWeapon(inner.esh, &out);
        } else if (protoEsh != nullptr && ReadSlotEntity(*protoEsh, slot, &inner) &&
                   inner.className == "Weapon") {
            ReadWeapon(inner.esh, &out);
        }
        if (out.valid) inner.esh.GetString("EntPath", &out.path);
    }
    for (const char* slot : {"Armour", "Armour_"}) {
        if (instance.Find(slot) != nullptr && ReadSlotEntity(instance, slot, &inner) &&
            inner.className == "Armour") {
            ReadArmour(inner.esh, &e.armour);
            inner.esh.GetString("EntPath", &e.armour.path);
            break;
        }
        if (protoEsh != nullptr && ReadSlotEntity(*protoEsh, slot, &inner) &&
            inner.className == "Armour") {
            ReadArmour(inner.esh, &e.armour);
            inner.esh.GetString("EntPath", &e.armour.path);
            break;
        }
    }
    // The armour's Effect block is folded into the derived stats below, after
    // the instance has had its say: `Armour::ApplyEquip @005bf690` only
    // rebuilds the sprite, and the AC / DT / DR reach the formula through the
    // wearer's derived stats.
    // A loaded magazine, plus whatever the actor is carrying.
    // `Weapon::GetAmmoOrUsesLeft @005db4e0` reads the weapon's own counter and
    // every actor spawns with a full magazine; the spare rounds live in the
    // `Inventory` block as `ILink<n>` entities of class Ammo, each with its own
    // `Count`, which `Weapon::FindSuitableAmmoInInventory @005dae40` matches on
    // `Ammo Type`.
    e.ammo = e.weapon.magazine;
    e.ammoReserve = 0;
    {
        // The instance's own Inventory when it embeds one, the prototype's
        // otherwise: a `.mis` actor references its inventory items by entity
        // slot id rather than embedding them, and mapview's reader does not
        // resolve those references, so a map instance yields nothing here and
        // falls back to what its prototype carries.
        ft::EntityBlock inv;
        bool haveInv = ReadSlotEntity(instance, "Inventory", &inv);
        if (!haveInv && protoEsh != nullptr) {
            haveInv = ReadSlotEntity(*protoEsh, "Inventory", &inv);
        }
        if (haveInv) {
            int32_t n = 0;
            inv.esh.GetInt("IListSz", &n);
            for (int32_t i = 0; i < n && i < 64; ++i) {
                char key[24];
                std::snprintf(key, sizeof(key), "ILink%d", i);
                ft::EntityBlock item;
                if (!ReadSlotEntity(inv.esh, key, &item)) continue;
                // The stack size is the item's own `Count`, and the owning
                // list mirrors it in `ILinkCnt<n>` -- both are the number of
                // rounds, not a multiplier, so only one of them counts.
                int32_t count = 0;
                if (!item.esh.GetInt("Count", &count) || count <= 0) {
                    std::snprintf(key, sizeof(key), "ILinkCnt%d", i);
                    inv.esh.GetInt(key, &count);
                }
                if (count <= 0) count = 1;

                // MILESTONE 9: keep every item, in the list's own order, for
                // the inventory screen.  `Inventory::Inventory_BuildSerializedItemPathCountList
                // @005b4b20` walks the list forward from head->next and emits
                // (path, count, nonLootableCount) in that order, so the
                // ILink<n> numbering IS the display order.
                LiveEntity::InvItem it;
                it.cls = item.className;
                item.esh.GetString("Display Name", &it.displayName);
                item.esh.GetString("Item Type", &it.itemType);
                item.esh.GetString("EntPath", &it.entPath);
                item.esh.GetString("Sprite", &it.spritePath);
                it.count = count;
                item.esh.GetFloat("Weight", &it.weight);
                if (it.displayName.empty()) it.displayName = it.cls;
                e.inventory.push_back(it);

                if (item.className != "Ammo" || e.weapon.ammoType.empty()) continue;
                std::string type;
                item.esh.GetString("Ammo Type", &type);
                if (type != e.weapon.ammoType) continue;
                e.ammoReserve += count;
                // The variant the spare rounds carry overrides the weapon's
                // default: it is what LookupDamage*ByName is keyed on.
                std::string variant;
                if (item.esh.GetString("Ammo Variant", &variant) && !variant.empty()) {
                    e.weapon.ammoVariant = variant;
                }
            }
        }
    }
    // MILESTONE 17.  A `.mis` actor does not embed its loadout: `Hand1`,
    // `Hand2`, `Armour` and `Inventory` are `kEshEntityRef` properties pointing
    // at other slots in the same table (`Hand1_override` / `Hand1_` are the
    // instance's own overrides of the prototype's).  Milestone 8 read only the
    // embedded type-4 form, which a map instance never uses, so every raider in
    // Brahmin Wood was armed out of its PROTOTYPE and carried nothing -- which
    // is why a corpse had nothing to loot.  Following the references gives each
    // one the loadout the map actually placed.
    {
        const EntityInstance* hand1Rec = nullptr;
        auto refWeapon = [&](const char* a, const char* b, WeaponDef* out) {
            const EntityInstance* r = ResolveRef(instance, a);
            if (r == nullptr) r = ResolveRef(instance, b);
            if (r == nullptr || r->className != "Weapon") return false;
            if (out == &e.weapon) hand1Rec = r;
            LiveEntity::InvItem it;
            ReadItemInstance(*r, &it);
            if (!it.weapon.valid) return false;
            *out = it.weapon;
            return true;
        };
        // The `_`-suffixed pair is the instance's override and wins when its
        // `<slot>_override` boolean is set, which is `ApplyEntOverride`'s rule.
        bool ov = false;
        instance.GetBool("Hand1_override", &ov);
        if (ov) refWeapon("Hand1_", "Hand1", &e.weapon);
        else refWeapon("Hand1", "Hand1_", &e.weapon);
        ov = false;
        instance.GetBool("Hand2_override", &ov);
        if (ov) refWeapon("Hand2_", "Hand2", &e.weapon2);
        else refWeapon("Hand2", "Hand2_", &e.weapon2);
        if (e.weapon.valid) {
            // MILESTONE 22.  The rounds actually in the magazine are NOT a
            // number on the weapon: `Weapon::FindSuitableAmmoInInventory
            // @005dae40` and the shipped save agree that the loaded ammunition
            // is its own entity, referenced by the weapon's `Ammo` slot, and
            // its `Count` is the round count.  Milestone 8 filled the magazine
            // to capacity on every load because nothing followed that
            // reference; a saved half-empty magazine came back full.
            e.ammo = e.weapon.magazine;
            if (hand1Rec != nullptr) {
                if (const EntityInstance* am = ResolveRef(hand1Rec->esh, "Ammo")) {
                    int32_t count = -1;
                    if (am->esh.GetInt("Count", &count) && count >= 0) e.ammo = count;
                }
            }
        }

        // MILESTONE 22.  THE ARMOUR SLOT IS THE INSTANCE'S, NOT THE
        // PROTOTYPE'S, AND THE COPY EMBEDDED IN THE PROTOTYPE IS STALE.
        //
        // `raider_06_infbas.ent` embeds a whole `Armour<esh>` copy of
        // `raiderArmor` whose `Effect` says `armorClass 8`; the item's own
        // `entities/items/Armor/raiderArmor.ent` says **5**, and the original
        // agrees with the item: FTSE logs those raiders at AC 11 = agility 6 +
        // 5.  So the embedded block names the slot and the instance's
        // `Armour` reference -- which resolves to the real item entity --
        // carries the numbers.
        //
        // And the reference is only worn when the instance does NOT set
        // `Armour_override`.  That flag is the same one `Hand1_override` is
        // for the weapons, and every actor in Brahmin Wood that carries it
        // reads its bare agility in the original: the six `raider_06_infbas`,
        // `raider_02_hth`, `raider_03_melee`, `raider_13_medic` (AC 7, 7, 7)
        // and the raider leader, who has an `Armour_` of his own resolving to
        // `raiderArmor2` and still logs AC 5.  The four `raider_06_infbas`
        // instances that leave the flag clear are exactly the four the
        // original logs at AC 11.
        bool armourOverride = false;
        instance.GetBool("Armour_override", &armourOverride);
        const EntityInstance* ar = armourOverride ? nullptr : ResolveRef(instance, "Armour");
        if (ar != nullptr && (ar->className == "Armour" || ar->className == "Armor")) {
            LiveEntity::InvItem it;
            ReadItemInstance(*ar, &it);
            if (it.armour.valid) {
                const std::string keep = e.armour.path;
                e.armour = it.armour;
                if (e.armour.path.empty()) e.armour.path = keep;
            }
        } else if (armourOverride) {
            e.armour = ArmourDef();
        }
        // MILESTONE 23 (§136.3's leftover).  Four `raider_07_inflig` instances
        // leave `Armour_override` clear, so the raiderArmor reference is worn --
        // and the original still logs them at AC 6, their bare agility, while
        // the two `raider_06_infbas` wearing the SAME item log 11.  The two
        // prototypes differ in exactly one thing: `Race Type` / `Item Type`
        // "RaiderLarge" against "Raider".  An armour's `Item Type` names the
        // character sprite its wearer switches to -- `Leather` ->
        // `sprites/characters/LeatherMale.spr`, `Raider` -> `RaiderMale.spr`,
        // and `Armour::ApplyEquip @005bf690` "only rebuilds the wearer's
        // sprite" (§22) -- and the shipped sprite set has `RaiderMale`,
        // `RaiderFemale`, `LeatherMale/Female`, `MetalMale/Female`, `Power`,
        // `GhoulArmour`, `MutantArmour`, but NOTHING for `RaiderMaleLarge` or
        // `RaiderMaleHuge`.  A size-variant body has no armoured sprite, the
        // rebuild cannot happen, and the item sits in the slot conferring
        // nothing.  The session's own ENT records are the check: every
        // RaiderLarge reads 6 with or without the armour, every armoured
        // Raider reads 11.
        // `Race Type`, or `Item Type` when the `.ent` states none -- the BOS
        // actors and the recruits carry only the latter, and the original's own
        // ENT records report "BOS" for them.
        if (const Esh* rsrc = pick("Race Type")) rsrc->GetString("Race Type", &e.raceType);
        if (e.raceType.empty()) {
            if (const Esh* isrc = pick("Item Type")) isrc->GetString("Item Type", &e.raceType);
        }
        auto endsWith = [](const std::string& v, const char* suffix) {
            const size_t n = std::strlen(suffix);
            return v.size() >= n && v.compare(v.size() - n, n, suffix) == 0;
        };
        const bool sizeVariantBody =
            endsWith(e.raceType, "Large") || endsWith(e.raceType, "Huge");
        if (e.armour.valid && !sizeVariantBody) {
            e.attr.armorClass += e.armour.armorClass;
            for (int i = 0; i < kDamageTypeCount; ++i) {
                e.attr.thresh[i] += e.armour.thresh[i];
                e.attr.resist[i] += e.armour.resist[i];
            }
            // MILESTONE 25: and the Effect block's SKILL deltas -- bosMetal's
            // `sneak -25`, which is why the original logs the Lieutenant who
            // wears it at sneak -7 where the port read 18.
            for (int i = 0; i < kSkillCount; ++i) e.attr.skills[i] += e.armour.skills[i];
        }
        if (const EntityInstance* inv = ResolveRef(instance, "Inventory")) {
            std::vector<LiveEntity::InvItem> items;
            ReadInventoryList(inv->esh, &items);
            if (!items.empty()) {
                // The referenced list is the real one; anything the prototype
                // pass put here is the prototype's placeholder.
                e.inventory = items;
                e.ammoReserve = 0;
                for (const LiveEntity::InvItem& it : e.inventory) {
                    if (it.cls != "Ammo" || e.weapon.ammoType.empty()) continue;
                    std::string perr;
                    const EntProto* ip = m_protos->Get(it.entPath, &perr);
                    std::string type;
                    if (ip != nullptr) ip->block.esh.GetString("Ammo Type", &type);
                    if (type == e.weapon.ammoType) e.ammoReserve += it.count;
                }
            }
        }
    }

    // MILESTONE 17: the unarmed attack.  `Actor::LevelUpUnarmedAttacks
    // @0054f4e0` keeps a punch and a kick entity outside `slot_equip`, chosen
    // by `GameData::GameData__GetRaceUnarmedTableEntry @0061f0f0` from
    // `tables/races.txt`'s `punch_table` / `kick_table` against the actor's
    // race, level, unarmed skill, strength and agility.  The race is the
    // `.ent`'s "Race Type" (`Entity::GetEntityCharacterTypeString`).
    {
        if (e.raceType.empty()) {
            if (const Esh* src = pick("Race Type")) src->GetString("Race Type", &e.raceType);
            if (e.raceType.empty()) {
                if (const Esh* i = pick("Item Type")) i->GetString("Item Type", &e.raceType);
            }
        }
        EquipUnarmed(e, e.raceType);
    }
    e.isCombatant = true;
    e.aiNextTick = 0;
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
float Scene::BoxGapDistance(const LiveEntity& a, const LiveEntity& b) const {
    // The original works in METRES: Entity.pos_x/y/z are the stream's own
    // floats, and the bounding box, which is in world units, is converted by
    // the `* 0.5 * 0.25` pair.  A world unit is a quarter of a metre
    // (mapview/NOTES.md 14), so the positions here are `wpos`, not `world`.
    const float dx = b.wpos[0] - a.wpos[0];
    const float dy = b.wpos[1] - a.wpos[1];
    const float dz = b.wpos[2] - a.wpos[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float extentsA = static_cast<float>(a.bbox[0] + a.bbox[2]);
    const float extentsB = static_cast<float>(b.bbox[0] + b.bbox[2]);
    const float gap = len - (extentsA + extentsB) * k::kHalf * k::kMetresPerUnit;
    return gap < kGapFloor ? kGapFloor : gap;
}

float Scene::CentreDistance(const LiveEntity& a, const LiveEntity& b) const {
    const float dx = b.wpos[0] - a.wpos[0];
    const float dy = b.wpos[1] - a.wpos[1];
    const float dz = b.wpos[2] - a.wpos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// `CheckLineOfSightBetweenEntities @004ff9b0` walks a
// swept box between the two eye points and asks the tile grid what it crosses;
// `ComputeWeaponAttackHitChance`'s cover term is `min(nBlockers * 5, 20)`
// (`ComputeWeaponAttackHitChance@00615e80`).  The tile flags that decide it are documented
// in assets/tiles/Flag Meanings.txt: an `Ethereal` or `Window` tile can be
// fired through, and a `Cover` tile gives the bonus.  fotgame does not carry
// those per-tile flags yet, so this walks milestone 7's navigation grid
// instead and counts blocked cells: a wall between the two blocks the shot, a
// low object gives cover.  RECONSTRUCTION -- labelled, see NOTES.md 23.
int32_t Scene::CountLineBlockers(const LiveEntity& a, const LiveEntity& b, bool* blocked) const {
    *blocked = false;
    const int32_t ax = m_nav.CellX(a.world[0]), az = m_nav.CellZ(a.world[2]);
    const int32_t bx = m_nav.CellX(b.world[0]), bz = m_nav.CellZ(b.world[2]);
    const int32_t steps = std::max(std::abs(bx - ax), std::abs(bz - az));
    if (steps <= 1) return 0;
    // The shooter's eye is at 90% of half the sprite height, the same point
    // CalculateHitChance uses.
    const float eyeA = static_cast<float>(a.world[1]) +
                       static_cast<float>(a.bbox[1]) * k::kHalf * k::kEyeHeightFraction;
    const float eyeB = static_cast<float>(b.world[1]) +
                       static_cast<float>(b.bbox[1]) * k::kHalf * k::kEyeHeightFraction;
    int32_t cover = 0;
    for (int32_t s = 1; s < steps; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(steps);
        const int32_t cx = ax + static_cast<int32_t>(std::lround(t * (bx - ax)));
        const int32_t cz = az + static_cast<int32_t>(std::lround(t * (bz - az)));
        if (!m_nav.In(cx, cz)) continue;
        const size_t idx = m_nav.Idx(cx, cz);
        if (m_nav.stat[idx] == 0) continue;
        // The grid records the top of the tallest static tile over the cell
        // and what that tile's flags do to a shot.  A tile whose top is under
        // the line is fired over; one that is neither Ethereal nor Window
        // stops the shot; the rest are cover.  That is exactly the rule
        // assets/tiles/Flag Meanings.txt states.
        const int32_t top = m_nav.statTop[idx];
        if (top == kNoFloor) continue;
        const float lineY = eyeA + t * (eyeB - eyeA);
        if (static_cast<float>(top) <= lineY) continue;   // the shot goes over
        const uint8_t cls = m_nav.los[idx];
        if (cls == kLosSolid) {
            *blocked = true;
            return cover;
        }
        cover += (cls == kLosBonusCover) ? 2 : 1;
    }
    return cover;
}

bool Scene::Hostile(const LiveEntity& a, const LiveEntity& b) const {
    // MILESTONE 10.  `GetFactionRelationValue @00505880` is the whole rule and
    // it is a table lookup: 0 (neutral) when either side is player 0, which is
    // always Scenery; 10 (friend) when both are the same player; the 9x9 signed
    // alignment matrix at World+0x520 indexed [teamA][teamB] when both players
    // have a non-zero team; and -10 (hostile) otherwise, which is how a unit
    // with no team fights everything.  `MissionScript::Relation` is that
    // function; hostility is a negative relation.
    //
    // The matrix comes off the wire straight after the nine `<Team>` records,
    // and `World::InitWorldTeamsAndDefaults @00505200` shows its shape (the
    // identity, 10 on the diagonal).  In Brahmin Wood row 1 (BOS) reads
    // 0 10 3 -10 0 0 2 0 0: friendly to itself, mildly friendly to the
    // Tribals and the Hostages, HOSTILE to the Raiders, neutral to everyone
    // else -- which is exactly the mission as played, and it replaces the
    // milestone 8 placeholder that could only say "squad versus everyone".
    if (a.playerIndex < 0 || b.playerIndex < 0) return false;
    if (m_script.Loaded()) return m_script.Relation(a.playerIndex, b.playerIndex) < 0;
    // No script section (a map with no <Player> list): fall back to milestone
    // 8's rule so nothing regresses.
    if (m_spawnPlayer < 0) return a.playerIndex != b.playerIndex;
    const bool aSquad = a.playerIndex == m_spawnPlayer;
    const bool bSquad = b.playerIndex == m_spawnPlayer;
    return aSquad != bSquad;
}

int32_t Scene::FindByTag(const std::string& tag) const {
    if (tag.empty()) return -1;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].tag == tag) return static_cast<int32_t>(i);
    }
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].label == tag) return static_cast<int32_t>(i);
    }
    return -1;
}

bool Scene::LineOfSight(int32_t ai, int32_t bi) const {
    if (ai < 0 || bi < 0 || ai >= static_cast<int32_t>(m_entities.size()) ||
        bi >= static_cast<int32_t>(m_entities.size()))
        return false;
    const LiveEntity& a = m_entities[static_cast<size_t>(ai)];
    const LiveEntity& b = m_entities[static_cast<size_t>(bi)];
    if (BoxGapDistance(a, b) > 40.0f) return false;
    bool blocked = false;
    CountLineBlockers(a, b, &blocked);
    return !blocked;
}

bool Scene::ScriptKill(int32_t index) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return false;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    if (e.dead || !e.isCombatant) return false;
    KillEntity(e, "Normal", e.maxHp, 1);
    return true;
}

bool Scene::ScriptSetPlayer(int32_t index, int32_t player) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return false;
    m_entities[static_cast<size_t>(index)].playerIndex = player;
    return true;
}

bool Scene::ScriptGiveItem(const std::string& unitTag, const std::string& itemTag) {
    const int32_t from = FindByTag(unitTag);
    if (from < 0) return false;
    int32_t to = -1;
    for (int32_t i : m_squad) {
        if (i >= 0 && !m_entities[static_cast<size_t>(i)].dead) {
            to = i;
            break;
        }
    }
    if (to < 0) {
        for (size_t i = 0; i < m_entities.size(); ++i) {
            if (m_entities[i].isCombatant && !m_entities[i].dead &&
                m_entities[i].playerIndex == m_spawnPlayer) {
                to = static_cast<int32_t>(i);
                break;
            }
        }
    }
    if (to < 0) return false;
    LiveEntity& src = m_entities[static_cast<size_t>(from)];
    for (size_t k = 0; k < src.inventory.size(); ++k) {
        if (src.inventory[k].displayName == itemTag || src.inventory[k].itemType == itemTag) {
            m_entities[static_cast<size_t>(to)].inventory.push_back(src.inventory[k]);
            src.inventory.erase(src.inventory.begin() + static_cast<long>(k));
            return true;
        }
    }
    // The item is a world entity carrying that Tag Name rather than an
    // inventory row (mission01 stores HPowder and Detonator both ways).
    const int32_t world = FindByTag(itemTag);
    if (world >= 0) {
        LiveEntity::InvItem it;
        it.cls = m_entities[static_cast<size_t>(world)].className;
        it.displayName = itemTag;
        it.entPath = m_entities[static_cast<size_t>(world)].entPath;
        m_entities[static_cast<size_t>(to)].inventory.push_back(it);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The to-hit chance.
// ---------------------------------------------------------------------------
bool Scene::ComputeHitChance(int32_t attackerIdx, int32_t targetIdx, HitTerms* out) const {
    *out = HitTerms();
    if (attackerIdx < 0 || targetIdx < 0 ||
        attackerIdx >= static_cast<int32_t>(m_entities.size()) ||
        targetIdx >= static_cast<int32_t>(m_entities.size())) {
        out->reason = "no such entity";
        return false;
    }
    const LiveEntity& a = m_entities[static_cast<size_t>(attackerIdx)];
    const LiveEntity& t = m_entities[static_cast<size_t>(targetIdx)];
    if (!a.isCombatant || a.dead) {
        out->reason = "attacker cannot act";
        return false;
    }
    if (!t.isCombatant || t.dead) {
        out->reason = "target is not a legal target";
        return false;
    }
    if (!a.weapon.valid || a.weapon.Mode() == nullptr) {
        out->reason = "no weapon";
        return false;
    }
    const WeaponMode& mode = *a.weapon.Mode();

    const float dist = BoxGapDistance(a, t);
    out->distance = dist;
    // The function's own distance variable, `local_5c` -- the metre gap times
    // `_DAT_00809580 == 1.5` (combat.h).  Every distance comparison below is
    // against this, not against `dist`.
    const float rdist = dist * k::kRangeUnitsPerMetre;

    // --- the eligibility flags, CalculateHitChance @00610d20 ----------------
    out->outOfRange = dist > mode.maxRange;
    out->noAmmo = a.weapon.magazine > 0 && a.ammo < mode.ammoUsage;
    out->notEnoughAp = a.ap < static_cast<float>(ActionTypeCost(mode.actionCostType));
    out->coverPenalty = 0;
    bool blocked = false;
    const int32_t nCover = CountLineBlockers(a, t, &blocked);
    out->blocked = blocked;

    // --- the chance itself, ComputeWeaponAttackHitChance @00615e80 ----------
    // skill: Skills::GetSkillValueByName(attacker.stats, mode.weaponskill).
    int skillIdx = SkillIndexFromName(mode.weaponSkill);
    if (skillIdx < 0) skillIdx = kSkillSmallGuns;
    out->skill = a.attr.skills[skillIdx];
    out->accuracyBonus = mode.accuracyBonus;

    bool isLongRange = false;
    out->rangeClass = RangeClassFactor(mode.rangeType, &isLongRange);

    // The light term.  `TraceLightVisibilityToEntity` returns how well lit the
    // target is, 0..1, and a Night Scope or the Glowing One trait forces 1.
    // fotgame's scene lighting is milestone 5's; the world is fully lit at the
    // default 06:29 clock, so this is 1 unless the lights are on and the
    // target is unlit.  Night Vision adds `perks.nightvision * 0.2`.
    out->light = 1.0f;

    // The range penalty: free range is rangeClass * (perception + 2*Sharpshooter)
    // scaled by the light, and every unit beyond it costs 4.
    const float freeRange = static_cast<float>(out->rangeClass) *
                            (static_cast<float>(a.attr.perception) +
                             static_cast<float>(a.attr.sharpshooter * 2)) *
                            out->light;
    const int32_t rangePenalty =
        static_cast<int32_t>((rdist - freeRange) * k::kRangePenaltyPerUnit);
    out->rangePenalty = rangePenalty > 0 ? rangePenalty : 0;

    // The close-range bonus, short-range weapons only.
    if (rdist < k::kCloseBonusRange && !isLongRange) {
        out->closeBonus =
            static_cast<int32_t>((1.0f - rdist * k::kCloseBonusSlope) * k::kCloseBonus);
    }

    // The target's armour class, capped at 80 when it comes from the fallback
    // vtable path.  A live actor uses derived.armorclass directly.
    out->armourClass = std::min(t.attr.armorClass, k::kArmourClassCap);

    // Inside three range units the target's own unarmed/melee skill is a dodge,
    // and MILESTONE 22 corrects whose weapon decides it.  `ComputeWeaponAttackHitChance@00615e80`:
    // 2072-2101 fetches the equipped item of `EVar31` -- the TARGET, the same
    // entity the armour class and the bounding box come from -- and asks THAT
    // weapon `IsUnarmedAttack` / `IsMeleeAttack`.  The port asked the
    // attacker's, so a hand-to-hand defender never dodged a gun.  It is worth
    // 19 points in the session log: Stitch's shotgun on a dog at 1.17 m reads
    // 64 in the original where every other term sums to 83, and the dog's
    // unarmed 70 through `70 * (1 - 0.447/3) * 0.33` is exactly 19.
    if (rdist < k::kMeleeDodgeRange && (t.weapon.IsUnarmed() || t.weapon.IsMelee())) {
        const int32_t defSkill = t.weapon.IsUnarmed() ? t.attr.skills[kSkillUnarmed]
                                                      : t.attr.skills[kSkillMeleeWeapons];
        out->meleeDodge = static_cast<int32_t>(static_cast<float>(defSkill) *
                                               (1.0f - rdist * k::kMeleeDodgeSlope) *
                                               k::kMeleeDodgeScale);
    }

    out->coverPenalty = std::min(nCover * 5, 20);
    out->calledShot = CalledShotToHitPenalty(std::string());   // uncalled: 0

    // A target that has fallen is far easier to hit.
    if (t.knockedDown) out->targetDown = 40;

    // A big target: the bounding box volume against 81.
    const float volume = static_cast<float>(t.bbox[0]) * static_cast<float>(t.bbox[1]) *
                         static_cast<float>(t.bbox[2]);
    if (volume > k::kBigTargetVolume) out->bigTarget = k::kBigTargetBonus;

    // `GetDifficultyModifierForAttackerTarget @0061b990`, the term
    // `ComputeWeaponAttackHitChance@00615e80` folds in as `__ftol((modifier - 1.0) * 40)`
    // (`_DAT_0080afd4` is 40, and `__ftol` TRUNCATES -- it is not a round).
    // MILESTONE 25 recovered the function: it switches on the `bos.cfg`
    // `{game.difficulty}` index (0 -> 0.7, 1 -> 1.0, 2 -> 1.3, 3 -> 2.0,
    // 4 -> 5.0) and returns the RECIPROCAL when the attacker is a player
    // character, the value itself when the target is one, and 1.0 otherwise --
    // which is exactly the attacker/target shape §136.1 measured.  The index is
    // a run-time choice (`--difficulty`), not a constant to bake in.
    out->difficulty = 0;
    if (a.playerIndex == m_spawnPlayer && t.playerIndex != m_spawnPlayer) {
        out->difficulty = static_cast<int32_t>((m_difficultyMultiplier - 1.0f) *
                                               k::kDifficultyScale);
    }

    int32_t chance = out->skill + out->accuracyBonus + out->rangeClass + out->closeBonus +
                     out->targetDown + out->bigTarget + out->difficulty - out->rangePenalty -
                     out->armourClass - out->meleeDodge - out->coverPenalty - out->calledShot;
    // The clamp is `> 0x5e ? 0x5f : (x < 0 ? 0 : x)` -- 95 at the top, 0 at the
    // bottom (`ComputeWeaponAttackHitChance@00615e80`).
    if (chance > k::kChanceCap - 1) chance = k::kChanceCap;
    if (chance < 0) chance = 0;
    out->total = chance;

    if (out->blocked) out->reason = "no line of fire";
    else if (out->outOfRange) out->reason = "out of range";
    else if (out->noAmmo) out->reason = "out of ammo";
    else if (out->notEnoughAp) out->reason = "not enough AP";
    return out->reason.empty();
}

// ---------------------------------------------------------------------------
// Ordering an attack.
// ---------------------------------------------------------------------------
bool Scene::OrderAttack(int32_t attackerIdx, int32_t targetIdx, std::string* whyNot) {
    // Out of ammo but carrying more: reload first, as the UI's fire button and
    // the AI both do before giving up on the shot.
    if (attackerIdx >= 0 && attackerIdx < static_cast<int32_t>(m_entities.size())) {
        LiveEntity& a0 = m_entities[static_cast<size_t>(attackerIdx)];
        const WeaponMode* m0 = a0.weapon.Mode();
        if (m0 != nullptr && a0.weapon.magazine > 0 && a0.ammo < m0->ammoUsage) {
            ReloadWeapon(a0);
        }
    }
    HitTerms terms;
    const bool ok = ComputeHitChance(attackerIdx, targetIdx, &terms);
    if (!ok) {
        if (whyNot != nullptr) *whyNot = terms.reason;
        return false;
    }
    // `World::IsEntityUsableThisTurn @00507270`: in mode 0 anyone may act; in
    // turn-based only the entity the queue is pointing at may.
    if (m_combatMode != 0 && ActiveTurnEntity() != attackerIdx) {
        if (whyNot != nullptr) *whyNot = "not this entity's turn";
        return false;
    }
    LiveEntity& a = m_entities[static_cast<size_t>(attackerIdx)];
    const LiveEntity& t = m_entities[static_cast<size_t>(targetIdx)];
    const WeaponMode& mode = *a.weapon.Mode();

    // `Actor::BeginAttackAnEntity @0054a980`: stop moving, face the target,
    // fill the combat message, set currentaction to "Attack".
    a.moving = false;
    a.path.clear();
    SetFacingFromVector(a, static_cast<float>(t.world[0] - a.world[0]),
                        static_cast<float>(t.world[2] - a.world[2]));
    a.attackTarget = targetIdx;
    a.attackResolved = false;
    a.attackChance = terms.total;
    // numshots = min(ammoLeft, mode.ammousage) (`Actor::BeginAttackAnEntity@0054a980`).
    a.attackShots = mode.ammoUsage;
    if (a.weapon.magazine > 0 && a.ammo < a.attackShots) a.attackShots = a.ammo;
    a.action = "Attack";

    // `TryApplySequenceNameVariants`: <Posture>Attack<Weapon><Mode>, then the
    // three shorter fallbacks.
    std::string weaponPart, modePart;
    AttackAnimSuffixes(a.weapon, &weaponPart, &modePart);
    const std::string base = a.posture + "Attack";
    if (!SetAnimationType(a, base + weaponPart + modePart) &&
        !SetAnimationType(a, base + weaponPart) && !SetAnimationType(a, base + modePart) &&
        !SetAnimationType(a, base)) {
        // No art for this weapon: resolve immediately rather than stall, and
        // say so.  This is the "could not be set" arm of the original.
        ResolveAttack(a);
        a.action.clear();
        if (whyNot != nullptr) *whyNot = "no attack animation";
        return true;
    }
    a.nextThinkMs = m_ticks * kTickMs;
    return true;
}

bool Scene::SetWeaponMode(int32_t index, const std::string& modeName) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return false;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    if (!e.weapon.valid) return false;
    const int32_t m = e.weapon.ModeIndexByName(modeName);
    if (m < 0) return false;
    e.weapon.selectedMode = m;
    return true;
}

// `Actor::ReloadActiveWeaponWithAnyAmmo @0054ec40`:
// costs `Tables::ActionTypes("reload")` AP and tops the magazine up from the
// inventory.  It refuses when the actor cannot afford the AP
// (`Actor::ReloadWeaponWithSelectedAmmo@0054f110`, `GetCurrentAP() >= ActionTypes["reload"]`).
bool Scene::ReloadWeapon(LiveEntity& e) {
    if (!e.weapon.valid || e.weapon.magazine <= 0) return false;
    if (e.ammoReserve <= 0 || e.ammo >= e.weapon.magazine) return false;
    const int32_t cost = ActionTypeCost("reload");
    if (e.ap < static_cast<float>(cost)) return false;
    const int32_t take = std::min(e.weapon.magazine - e.ammo, e.ammoReserve);
    e.ammo += take;
    e.ammoReserve -= take;
    e.ap -= static_cast<float>(cost);
    if (e.ap < 0.05f) e.ap = 0.0f;
    Logf("%s reloads %s: %d rounds (%d left in pack), %d AP", e.label.c_str(),
         e.weapon.displayName.c_str(), take, e.ammoReserve, cost);
    PlaySoundStem(e, snd::ReloadStem(e.weapon.soundType), kChanDefault, "reload");
    return true;
}

// ---------------------------------------------------------------------------
// The hit roll, shared by the live attack path and by --hit-test so the
// verification measures exactly what the game does.
//
// A single shot is `RollCriticalHitChance @0060fdf0`:
// one percentile in 1..100, a hit when it is at or under the chance, and a
// critical when it is also at or under `chance / 10 + critBonus`.
//
// A burst is `ResolveBurstAttackTargets @00614ac0`:
// the per-target chance is first capped at 70, then every shot allotted to the
// target rolls `Random__GetRandomValueInFloatRange(0, 100) < chance`
// independently.  The allotment itself divides the shots between the entities
// the sphere search found, in proportion to their chances against a
// denominator seeded with `max(0, 100 - skill)`; with one legal target in the
// cone it is all of them, which is the case the verification runs.
// ---------------------------------------------------------------------------
int32_t Scene::RollHits(int32_t chance, int32_t shots, bool burst, int32_t critBonus,
                        bool betterCriticals, int32_t* critFlagsOut) {
    *critFlagsOut = 0;
    if (burst) {
        const int32_t capped = std::min(chance, k::kBurstChanceCap);
        int32_t hits = 0;
        for (int32_t s = 0; s < shots; ++s) {
            if (m_rng.RangeI(0.0f, 100.0f) < capped) ++hits;
        }
        return hits;
    }
    const int32_t roll = m_rng.Percentile();
    if (roll > chance) return 0;
    if (roll <= chance / 10 + critBonus) {
        // `Trap::ComputeMineDamageToLeg @005d1df0` is
        // the one complete copy of the critical roll: the margin by which the
        // shot beat its chance seeds a second roll, which indexes the
        // critical-effect table.
        const int32_t margin = (chance - roll) + 1;
        int32_t v = m_rng.RangeI(1.0f, static_cast<float>(margin));
        v = static_cast<int32_t>(static_cast<float>(v) + m_rng.Range(0.0f, 100.0f));
        if (betterCriticals) {
            v = static_cast<int32_t>(static_cast<float>(v) * k::kBetterCriticals);
        }
        *critFlagsOut = RollCriticalEffect(&m_rng, 0 /* Torso, an uncalled shot */, v);
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Resolving one attack, at the animation's fire marker.
// ---------------------------------------------------------------------------
void Scene::ResolveAttack(LiveEntity& e) {
    // `Actor::ResolveCombatMessageForAttacker @0054dc40` starts with the
    // once-per-attack latch at Actor.ct_.unk26a.
    if (!e.isCombatant || e.dead || e.attackResolved) return;
    if (e.attackTarget < 0 || e.attackTarget >= static_cast<int32_t>(m_entities.size())) return;
    e.attackResolved = true;
    const WeaponMode* modePtr = e.weapon.Mode();
    if (modePtr == nullptr) return;
    const WeaponMode& mode = *modePtr;

    const int32_t apCost = ActionTypeCost(mode.actionCostType);
    const int32_t attackerIdx = static_cast<int32_t>(&e - m_entities.data());
    HitTerms terms;
    ComputeHitChance(attackerIdx, e.attackTarget, &terms);

    const float apBefore = e.ap;
    const int32_t ammoBefore = e.ammo;

    // `Weapon::UseAmmoForCurrentMode @005da520` then `AdjustAP(-GetAPCost)`.
    int32_t shots = mode.ammoUsage;
    if (e.weapon.magazine > 0) {
        if (e.ammo < shots) shots = e.ammo;
        e.ammo -= shots;
    }
    // Actor::AdjustAP @0056fcf0: add, clamp to [0, maxAP], snap under 0.05.
    e.ap -= static_cast<float>(apCost);
    if (e.ap < 0.05f) e.ap = 0.0f;

    // MILESTONE 13.  `Actor::PlayActorAttackSound @00562910` is called from the
    // same place: the fire marker, after the ammo goes.  It builds
    // "sound/game/" + GetSoundType() + the mode's attack type + ".wav" and
    // picks among the numbered variants.  A magazine weapon with nothing left
    // plays the `<type>outofammo` file instead -- the shipped set has one for
    // every sound type.
    if (e.weapon.magazine > 0 && shots == 0) {
        PlaySoundStem(e, snd::OutOfAmmoStem(e.weapon.soundType), kChanAttack,
                      "out-of-ammo");
    } else if (!PlaySoundStem(e, snd::AttackStem(e.weapon.soundType, mode.modeName),
                              kChanAttack, "weapon-fire")) {
        // A melee or thrown weapon has no `<type>single` / `<type>burst`
        // file -- its sound is the bare sound type (`club`, `knifestab`,
        // `SpearThrow`, `kick`, `PunchLight`), which is what
        // `GetWeaponAttackTypeDisplayString` returning an empty attack type
        // produces.
        PlaySoundStem(e, e.weapon.soundType.empty()
                             ? std::string()
                             : "sound/game/" + e.weapon.soundType,
                      kChanAttack, "weapon-fire");
    }

    LiveEntity& t = m_entities[static_cast<size_t>(e.attackTarget)];
    const char* modeName = mode.modeName.empty() ? "attack" : mode.modeName.c_str();

    if (shots <= 0) {
        Logf("%s: %s has no ammunition", e.label.c_str(), e.weapon.displayName.c_str());
        return;
    }

    int32_t hits = 0;
    int32_t critFlags = 0;
    if (shots > 1) {
        ResolveBurst(e, terms, shots, apBefore, ammoBefore);
        if (!t.dead) t.aiEngaged = true;
        return;
    }
    // `RollCriticalHitChance @0060fdf0` tests `roll <= chance / 10 + critBonus`.
    // The bonus is the weapon's own `Critical Chance` plus the mode's plus the
    // actor's `derived.criticalChance`.  There is NO Luck term in the dumped
    // code: Luck reaches criticals only through that derived stat, which the
    // shipped actors leave at zero.
    const int32_t critBonus = e.weapon.criticalChance + mode.critBonus +
                              e.attr.criticalChance;
    hits = RollHits(terms.total, shots, false, critBonus, e.attr.betterCriticals != 0,
                    &critFlags);

    if (hits > 0) {
        ApplyHit(e, t, hits, critFlags);
    } else {
        // `Entity::ShotAtMissed`.
        Logf("%s fires %s (%s) at %s: chance %d%%, MISS  [AP %.0f->%.0f, ammo %d->%d]",
             e.label.c_str(), e.weapon.displayName.c_str(), modeName, t.label.c_str(),
             terms.total, apBefore, e.ap, ammoBefore, e.ammo);
    }
    // `BroadcastHearCombatEventToNearbyEntities` -- being shot at wakes the AI.
    if (!t.dead) t.aiEngaged = true;
}

// ---------------------------------------------------------------------------
// `ResolveBurstAttackTargets @00614ac0`, ported.
//
// A burst does not simply fire N shots at one target.  `SphereSearch_4` collects
// every entity near the line to the aim point; each one's own hit chance is
// computed and capped at 70 (`_DAT_...` literal 0x46), and the chances are summed
// into a denominator SEEDED with `max(0, 100 - burstSkill)` -- the share of the
// burst that hits nothing -- and then floored at 100.  Each candidate is allotted
// `round(chance * numshots / total)` shots, at least one and never more than are
// left, and rolls each of them independently against its own chance:
//     `Random__GetRandomValueInFloatRange(rng, 0, 100) < chance`
// Everything that lands goes through the normal damage path; a candidate that
// takes none gets `Entity::ShotAtMissed`.
//
// The cone that `SphereSearch_4`'s radius stands in for is the arc test at the
// top of `ComputeWeaponAttackHitChance`: the angle between the aim direction and
// the direction to the candidate, expressed as a fraction of pi (the acos is
// scaled by `_DAT_00810d60 == 1/pi`), against `_DAT_00824c80 == 0.12` for a
// `Burst` spread and `_DAT_00824c84 == 0.17` for a `Spray`.
// ---------------------------------------------------------------------------
void Scene::ResolveBurst(LiveEntity& e, const HitTerms& primaryTerms, int32_t shots,
                         float apBefore, int32_t ammoBefore) {
    const WeaponMode& mode = *e.weapon.Mode();
    const int32_t self = static_cast<int32_t>(&e - m_entities.data());
    const LiveEntity& aim = m_entities[static_cast<size_t>(e.attackTarget)];

    const float ax = e.wpos[0], az = e.wpos[2];
    float dirX = aim.wpos[0] - ax, dirZ = aim.wpos[2] - az;
    const float dirLen = std::sqrt(dirX * dirX + dirZ * dirZ);
    if (dirLen > 1e-5f) {
        dirX /= dirLen;
        dirZ /= dirLen;
    }
    float arc = 0.0f;
    if (mode.spreadType == "Burst") arc = 0.12f;
    else if (mode.spreadType == "Spray") arc = 0.17f;
    const float cosArc = std::cos(arc * 3.14159265f);

    struct Cand {
        int32_t idx;
        int32_t chance;
    };
    std::vector<Cand> cands;
    cands.push_back({e.attackTarget, std::min(primaryTerms.total, k::kBurstChanceCap)});
    if (arc > 0.0f) {
        for (size_t i = 0; i < m_entities.size(); ++i) {
            const int32_t idx = static_cast<int32_t>(i);
            if (idx == self || idx == e.attackTarget) continue;
            const LiveEntity& c = m_entities[i];
            if (!c.isCombatant || c.dead) continue;
            float cx = c.wpos[0] - ax, cz = c.wpos[2] - az;
            const float cl = std::sqrt(cx * cx + cz * cz);
            if (cl < 1e-5f || cl > mode.maxRange) continue;
            if ((cx / cl) * dirX + (cz / cl) * dirZ < cosArc) continue;   // outside the cone
            // Only the targeting flags matter here.  `ComputeHitChance` also
            // reports the attacker-side gates -- AP and ammunition -- and by
            // this point the burst has already paid for itself, so those would
            // refuse every bystander.  The original keeps the two apart: the
            // flags live in `CalculateHitChance`'s bitfield and the percentage
            // is computed whatever they say.
            HitTerms t2;
            ComputeHitChance(self, idx, &t2);
            if (t2.blocked || t2.outOfRange || t2.total <= 0) continue;
            cands.push_back({idx, std::min(t2.total, k::kBurstChanceCap)});
        }
    }

    int skillIdx = SkillIndexFromName(mode.weaponSkill);
    if (skillIdx < 0) skillIdx = kSkillSmallGuns;
    int32_t total = std::max(0, 100 - e.attr.skills[skillIdx]);
    for (const Cand& c : cands) total += c.chance;
    if (total < 101) total = 100;

    int32_t remaining = shots;
    int32_t landed = 0;
    for (const Cand& c : cands) {
        if (remaining < 1) break;
        int32_t n = static_cast<int32_t>(static_cast<float>(c.chance) *
                                             static_cast<float>(shots) /
                                             static_cast<float>(total) +
                                         0.5f);
        if (n > remaining) n = remaining;
        if (n < 1) n = 1;
        remaining -= n;
        int32_t hits = 0;
        for (int32_t s = 0; s < n; ++s) {
            if (m_rng.RangeI(0.0f, 100.0f) < c.chance) ++hits;
        }
        LiveEntity& target = m_entities[static_cast<size_t>(c.idx)];
        Logf("  burst -> %s: %d of %d shots at %d%%, %d hit", target.label.c_str(), n, shots,
             c.chance, hits);
        if (hits > 0) {
            ApplyHit(e, target, hits, 0);
            ++landed;
        }
    }
    if (landed == 0) {
        Logf("%s fires %s (%s) at %s: chance %d%%, burst MISSES  [AP %.0f->%.0f, ammo %d->%d]",
             e.label.c_str(), e.weapon.displayName.c_str(),
             mode.modeName.empty() ? "burst" : mode.modeName.c_str(), aim.label.c_str(),
             std::min(primaryTerms.total, k::kBurstChanceCap), apBefore, e.ap, ammoBefore,
             e.ammo);
    }
}

// ---------------------------------------------------------------------------
// Damage.
// ---------------------------------------------------------------------------
// `Weapon::GetMinDmg @005d9930` / `GetMaxDmg @005d9c90`, and the ammunition
// multiplier that `Trap::ComputeMineDamageToLeg @005d1df0` applies to the roll.
// Split out of `ApplyHit` for MILESTONE 23: `--hitcalc` has to report the band
// the rolls are actually drawn from, because that is what the original's
// `DMG` records carry.  The session that pinned it: a Pump-action Shotgun
// (`ithacaShotgun`, 14-24) rolled 25, 28, 28 and 38 -- impossible from 14-24,
// exact for 12 ga. Flechette, whose `Ammo Variant` is "Jacketed Hollow Point"
// and whose damage multiplier is 1.6 (trunc(24 * 1.6) = 38, trunc(14 * 1.6) =
// 22).  The band was right; the AMMO the shooters were holding was not.
void Scene::DamageBandOf(const LiveEntity& attacker, int32_t* minOut, int32_t* maxOut,
                         float* ammoMultOut, float* drModOut, float centreMetres) const {
    const WeaponMode* modePtr = attacker.weapon.Mode();
    const float modeMult = modePtr != nullptr ? modePtr->damageMult : 1.0f;
    auto scaled = [&](int32_t base) {
        int32_t d = static_cast<int32_t>(static_cast<float>(base) * modeMult);
        if (attacker.weapon.IsMelee() || attacker.weapon.IsUnarmed()) {
            d += attacker.attr.meleeDamage;    // where Strength enters
        } else {
            d = static_cast<int32_t>(
                (1.0f + static_cast<float>(attacker.attr.bonusRangedDamage) *
                            k::kBonusRangedDamagePerRank) *
                static_cast<float>(d));
        }
        return d;
    };
    const AmmoVariant variant = AmmoVariantByName(attacker.weapon.ammoVariant);
    // MILESTONE 25.  A `Spread (Arc) = Cone` weapon -- the shotguns, and only
    // the shotguns in the shipped data -- does not roll inside its printed
    // band, and the reason is in the binary.
    // `FinalizeCombatMessageFields @00611730` sends a `Cone` mode to
    // `ApplyAreaDamageAttackWithFactionCheck @00615100`, which computes a
    // damage multiplier from the range and hands it to
    // `ApplyRadiusDamageToTarget @00613230`; every other spread hands that
    // argument the literal 1.0.  The multiplier is
    //     mult = clamp(2.5 - 2 * centre_dist / weapon.GetRange(), 0.2, 2.5)
    // and `CombatAI_EvaluateAttackApproachAndDamage @00612170` applies it as
    // `trunc(GetMinDmg * mult)` / `trunc(GetMaxDmg * mult)`.  See combat.h for
    // the instruction addresses.  Milestone 23 fitted `2.5 - gap_m / 7` to 28
    // pooled rolls; that is this rule for a weapon whose Maximum Range is 14,
    // which both shipped shotguns are, and the measurement is now retired.
    float coneMult = 1.0f;
    if (modePtr != nullptr && modePtr->spreadType == "Cone" && centreMetres >= 0.0f &&
        modePtr->maxRange > 0.0f) {
        coneMult = k::kConeDamageMax - k::kConeRangeTerms * (centreMetres / modePtr->maxRange);
        if (coneMult > k::kConeDamageMax) coneMult = k::kConeDamageMax;
        if (coneMult < k::kConeDamageMin) coneMult = k::kConeDamageMin;
    }
    if (minOut != nullptr)
        *minOut = static_cast<int32_t>(static_cast<float>(scaled(attacker.weapon.minDamage)) *
                                       coneMult);
    if (maxOut != nullptr)
        *maxOut = static_cast<int32_t>(static_cast<float>(scaled(attacker.weapon.maxDamage)) *
                                       coneMult);
    if (ammoMultOut != nullptr) *ammoMultOut = variant.damageMult;
    if (drModOut != nullptr) *drModOut = variant.drMod;
}

void Scene::ApplyHit(LiveEntity& attacker, LiveEntity& target, int32_t hits, int32_t critFlags) {
    const WeaponMode* modePtr = attacker.weapon.Mode();
    if (modePtr == nullptr) return;
    const WeaponMode& mode = *modePtr;

    // MILESTONE 11.  Being hit is aitable.txt's `tookDamage` event, which
    // `BroadcastHearCombatEventToNearbyEntities @00552530` raises on the
    // target along with `hearCombat` on everyone nearby; and it is what the
    // `sentryScout` mode's return-fire test reads.
    {
        const int32_t ai = static_cast<int32_t>(&attacker - m_entities.data());
        const int32_t ti = static_cast<int32_t>(&target - m_entities.data());
        target.lastAttacker = ai;
        for (int k = 0; k < 3; ++k) target.investigate[k] = attacker.world[k];
        AiRaiseEvent(ti, kEvTookDamage, ai);
    }

    int32_t minDmg = 0, maxDmg = 0;
    float ammoMult = 1.0f, drMod = 1.0f;
    // The cone term measures the CENTRE-to-centre distance, not the
    // bounding-box gap the to-hit sum uses.
    DamageBandOf(attacker, &minDmg, &maxDmg, &ammoMult, &drMod,
                 CentreDistance(attacker, target));
    const DamageType dt = DamageTypeFromName(attacker.weapon.primaryDamage);
    float dmgMult = ammoMult;

    int32_t armourDT = target.attr.thresh[dt];
    int32_t armourDR = target.attr.resist[dt];
    if (critFlags != 0) {
        if ((critFlags & kCritBypassArmour) != 0) {
            armourDT = 0;
            armourDR = 0;
        }
        dmgMult *= static_cast<float>(
            CriticalDamageMultiplier(&m_rng, critFlags, target.playerIndex == m_spawnPlayer));
    }

    // The roll, once per hit, exactly as Trap::ComputeMineDamageToLeg does it:
    //   d = trunc(uniform(min,max) * mult)
    //   d = trunc(d - DT * drMod)
    //   d = trunc(d - DR * d * drMod * 0.01)
    //   d = max(0, d)
    int32_t total = 0;
    for (int32_t h = 0; h < hits; ++h) {
        int32_t d = m_rng.RangeI(static_cast<float>(minDmg), static_cast<float>(maxDmg));
        d = static_cast<int32_t>(static_cast<float>(d) * dmgMult);
        d = static_cast<int32_t>(static_cast<float>(d) -
                                 static_cast<float>(armourDT) * drMod);
        d = static_cast<int32_t>(static_cast<float>(d) -
                                 static_cast<float>(armourDR) * static_cast<float>(d) * drMod *
                                     k::kPercentToFraction);
        if (d < 0) d = 0;
        total += d;
    }

    const int32_t hpBefore = target.hp;
    // `Actor::ApplyDamage @0054c910`: a plain subtraction; the armour was
    // already taken off above.
    target.hp -= total;

    // MILESTONE 13: the impact, named for what it hit and what hit it --
    // `sound/game/<Flesh|Leath|Metal|Pow>_<kind>[_Burst]`.  The mode's
    // `Impact Sound FX` property is the kind; the target's armour is the tier.
    PlaySoundStem(target,
                  snd::ImpactStem(target.armour.valid ? target.armour.itemType
                                                      : std::string(),
                                  mode.soundFx, attacker.weapon.primaryDamage,
                                  attacker.weapon.IsMelee(),
                                  attacker.weapon.IsUnarmed(), hits > 1),
                  kChanDefault, "impact");

    const char* modeName = mode.modeName.empty() ? "attack" : mode.modeName.c_str();
    Logf("%s hits %s with %s (%s) x%d: %d damage (DT %d, DR %d%%, %s) HP %d->%d%s",
         attacker.label.c_str(), target.label.c_str(), attacker.weapon.displayName.c_str(),
         modeName, hits, total, armourDT, armourDR, DamageTypeName(dt), hpBefore,
         target.hp < 0 ? 0 : target.hp, critFlags != 0 ? "  CRITICAL" : "");

    // `CombatFX::CombatFXBloodSprayRelated @005953b0` is gated on damage > 2.
    if (total > 2) SpawnBlood(target, total);

    if (target.hp <= 0) {
        KillEntity(target, attacker.weapon.primaryDamage, total, hits);
        // `Actor::IncrementKillCount` and the XP award; the XP table is in no
        // dump and no shipped file (NOTES.md 24), so the kill is only logged.
        ++attacker.kills;
        Logf("%s is killed by %s", target.label.c_str(), attacker.label.c_str());
        return;
    }

    // The hit reaction.  `Actor::ApplyAttackResultToEntity @0054b720`
    // (`Actor::ApplyAttackResultToEntity@0054b720`): a knockdown critical goes to the fall
    // animation; otherwise the actor flinches when the damage exceeds a tenth
    // of its maximum hit points or it was not idle, and only if it is not
    // already moving.
    if ((critFlags & (kCritKnockOver | kCritKnockout)) != 0) {
        target.knockedDown = true;
        if (SetAnimationType(target, target.posture + "Fallback")) {
            target.action = "Fall";
            target.nextThinkMs = m_ticks * kTickMs;
            return;
        }
    }
    if (total > target.maxHp / 10 || !target.action.empty()) {
        if (!target.moving && target.action != "Attack") {
            if (SetAnimationType(target, target.posture + "Recoil")) {
                target.action = "Recoil";
                target.nextThinkMs = m_ticks * kTickMs;
            }
        }
    }
    target.aiEngaged = true;
}

void Scene::KillEntity(LiveEntity& e, const std::string& damageType, int32_t damage,
                       int32_t hits) {
    (void)hits;
    e.dead = true;
    e.hp = 0;
    // MILESTONE 17.  A corpse IS its inventory: nothing in the binary moves it
    // anywhere, and `Actor::IsLegalTarget` simply goes false, which is the arm
    // of `HandleActivateOrLootCommand @00549120` that ends in the loot window.
    // What death does do is `RandomizeDeadEntityLoot @006ae030`: one percentile
    // K is rolled once for the corpse, then for every stack a second roll r --
    // `K < r` destroys the stack outright (`SetCount(0)`), otherwise a stack of
    // more than one is scaled to `count * r / 100`, and anything left at zero
    // is destroyed.  So a corpse keeps a random part of what it carried, which
    // is why looting one is worth doing and never a guaranteed refill.
    {
        const int32_t k = m_rng.Percentile() - 1;   // `rand() % 100`
        std::vector<LiveEntity::InvItem> kept;
        for (LiveEntity::InvItem it : e.inventory) {
            const int32_t r = m_rng.Percentile() - 1;
            if (k < r) continue;                    // SetCount(0)
            if (it.count > 1) it.count = it.count * r / 100;
            if (it.count < 1) continue;
            kept.push_back(it);
        }
        e.inventory.swap(kept);
        // The equipped weapons and armour stay on the body and are what the
        // loot window's left list shows beside the pack; the unarmed pair is
        // not in `slot_equip` and is not lootable.
        if (e.weapon.valid && !e.unarmedHand[0]) {
            LiveEntity::InvItem it;
            it.cls = "Weapon";
            it.displayName = e.weapon.displayName;
            it.itemType = e.weapon.itemType;
            it.entPath = e.weapon.path;
            it.weight = e.weapon.weight;
            it.weapon = e.weapon;
            e.inventory.insert(e.inventory.begin(), it);
        }
        if (e.weapon2.valid && !e.unarmedHand[1]) {
            LiveEntity::InvItem it;
            it.cls = "Weapon";
            it.displayName = e.weapon2.displayName;
            it.itemType = e.weapon2.itemType;
            it.entPath = e.weapon2.path;
            it.weight = e.weapon2.weight;
            it.weapon = e.weapon2;
            e.inventory.insert(e.inventory.begin(), it);
        }
        if (e.armour.valid) {
            LiveEntity::InvItem it;
            it.cls = "Armour";
            it.displayName = e.armour.displayName;
            it.itemType = e.armour.itemType;
            it.entPath = e.armour.path;
            it.armour = e.armour;
            e.inventory.push_back(it);
        }
        if (e.ammoReserve > 0) {
            // The magazine's rounds are not a stack of their own; the spare
            // ammunition already is one and stayed in the list.
        }
    }
    // The body fall.  The death CRY rides the Death sequence's own -44 marker
    // (Scene::FrameEventSound); this is the thud that goes with it.
    PlaySoundStem(e, snd::BodyFallStem(e.spritePath,
                                       e.armour.valid ? e.armour.itemType : std::string()),
                  kChanDefault, "body-fall");
    e.moving = false;
    e.path.clear();
    e.attackTarget = -1;
    // `Actor::EntityWasKilled @0054cc40` sets the action to
    // ("Death", ResolveDamageEffectStringName(damageType, hits, damage)).
    // `ResolveDamageEffectStringName @00543d30` is in the dump this milestone
    // exported; the sequence names it can produce are the ones every character
    // sprite carries: Death, DeathBighole, DeathCutinhalf, DeathElectrify,
    // DeathExplode, DeathFire, DeathMelt, DeathRiddled.
    std::string suffix;
    const DamageType dt = DamageTypeFromName(damageType);
    if (dt == kDmgFire) suffix = "Fire";
    else if (dt == kDmgElectrical) suffix = "Electrify";
    else if (dt == kDmgExplode) suffix = "Explode";
    else if (dt == kDmgEnergy) suffix = "Melt";
    else if (damage > e.maxHp) suffix = "Riddled";   // heavy overkill
    e.deathType = suffix;
    if (!SetAnimationType(e, "Death" + suffix)) {
        SetAnimationType(e, "Death");
        e.deathType.clear();
    }
    e.action = "Death";
    e.nextThinkMs = m_ticks * kTickMs;
    // A corpse stops blocking the navigation grid.
    RefreshDynamicBlockers();
    SpawnBlood(e, damage);
    // `CombatFX::CombatFXBloodPoolRelated @00596820` -- a pool under the body.
    Decal d;
    for (int i = 0; i < 3; ++i) d.world[i] = e.world[i];
    ProjectWorldToScreen(d.world[0], d.world[1], d.world[2], &d.sx, &d.sy);
    d.kind = 3;
    d.tick = m_ticks;
    m_decals.push_back(d);
    // `World::UpdateCombatEntityCountAndMaybeEndCombat @00507af0`.
    if (m_combatMode != 0) RebuildTurnQueue();
}

void Scene::SpawnBlood(const LiveEntity& target, int32_t damage) {
    // `CombatFX::CombatFXBloodSprayRelated @005953b0` picks the sprite by the
    // damage: bloodspurting under 15, bloodspraying to 30, bloodgushing above.
    Decal d;
    for (int i = 0; i < 3; ++i) d.world[i] = target.world[i];
    ProjectWorldToScreen(d.world[0], d.world[1], d.world[2], &d.sx, &d.sy);
    d.kind = damage < 15 ? 0 : (damage < 31 ? 1 : 2);
    d.tick = m_ticks;
    m_decals.push_back(d);
    // `Actor::ApplyDamage @0054c910` then spawns `clamp(ln(damage/2), 1, 10)`
    // blood trails scattered around the impact.
    int32_t n = static_cast<int32_t>(std::log(std::max(1.0f, static_cast<float>(damage) * 0.5f)));
    n = std::min(std::max(n, 1), 10);
    for (int32_t i = 0; i < n; ++i) {
        Decal tr = d;
        tr.kind = 0;
        tr.world[0] += m_rng.RangeI(-3.0f, 3.0f);
        tr.world[2] += m_rng.RangeI(-3.0f, 3.0f);
        ProjectWorldToScreen(tr.world[0], tr.world[1], tr.world[2], &tr.sx, &tr.sy);
        m_decals.push_back(tr);
    }
}

// ---------------------------------------------------------------------------
// Verification harness.  Neither of these is a thing the game itself does.
// ---------------------------------------------------------------------------
bool Scene::PlaceEntity(int32_t index, int32_t worldX, int32_t worldZ) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return false;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    const int32_t cx = m_nav.CellX(worldX), cz = m_nav.CellZ(worldZ);
    int32_t y = e.world[1];
    if (m_nav.In(cx, cz) && m_nav.surface[m_nav.Idx(cx, cz)] != kNoFloor) {
        y = m_nav.surface[m_nav.Idx(cx, cz)];
    }
    const float pos[3] = {static_cast<float>(worldX) / kEntityWorldScale,
                          static_cast<float>(y) / kEntityWorldScale,
                          static_cast<float>(worldZ) / kEntityWorldScale};
    SetEntityWorldPos(e, pos);
    e.moving = false;
    e.path.clear();
    RefreshDynamicBlockers();
    return true;
}

bool Scene::HitRateTest(int32_t attacker, int32_t target, int32_t n, HitTerms* terms,
                        int64_t* hits, int64_t* shots) {
    *hits = 0;
    *shots = 0;
    if (!ComputeHitChance(attacker, target, terms)) return false;
    LiveEntity& a = m_entities[static_cast<size_t>(attacker)];
    const WeaponMode* mode = a.weapon.Mode();
    if (mode == nullptr) return false;
    int32_t perAttack = mode->ammoUsage;
    if (a.weapon.magazine > 0 && perAttack > a.weapon.magazine) perAttack = a.weapon.magazine;
    const bool burst = perAttack > 1;
    const int32_t critBonus = a.weapon.criticalChance + mode->critBonus +
                              a.attr.criticalChance;
    for (int32_t i = 0; i < n; ++i) {
        int32_t crit = 0;
        *hits += RollHits(terms->total, perAttack, burst, critBonus,
                          a.attr.betterCriticals != 0, &crit);
        *shots += perAttack;
    }
    if (burst) terms->total = std::min(terms->total, k::kBurstChanceCap);
    return true;
}

// ---------------------------------------------------------------------------
// Turn-based mode.
// ---------------------------------------------------------------------------
void Scene::SetTurnBased(bool on) {
    if (on == (m_combatMode != 0)) return;
    if (on) {
        // `World::StartCombat @00506b40` picks 1 (individual) or 2 (squad) from
        // the campaign setting; fotgame has no campaign settings file, so it
        // uses 1, individual turn-based, which is the one the queue key below
        // was written for.
        m_combatMode = 1;
        m_round = 1;
        RebuildTurnQueue();
        m_turnAt = 0;
        if (!m_turnQueue.empty()) BeginTurnFor(m_turnQueue[0]);
        Logf("turn-based ON: round 1, %zu combatants", m_turnQueue.size());
        for (size_t i = 0; i < m_turnQueue.size() && i < 6; ++i) {
            const LiveEntity& q = m_entities[static_cast<size_t>(m_turnQueue[i])];
            Logf("  queue[%zu] = %d %s  key %.2f (AP %.0f/%d x PE %d)", i, m_turnQueue[i],
                 q.label.c_str(), m_turnKey[i], q.ap, q.maxAp, q.attr.perception);
        }
    } else {
        // `World::EndCombatAndGrantSquadXP @00506fe0` clears the queue and puts
        // the mode back to 0.
        m_combatMode = 0;
        m_turnQueue.clear();
        m_turnKey.clear();
        m_turnAt = 0;
        // Everyone comes back to full AP: outside combat AdjustAP2 refills
        // continuously, so this is where that lands.
        for (LiveEntity& e : m_entities) {
            if (e.isCombatant && !e.dead) e.ap = static_cast<float>(e.maxAp);
        }
        Logf("turn-based OFF");
    }
}

void Scene::RebuildTurnQueue() {
    // `TurnQueueEntry::RecomputeOrderKey @0050b160`:
    //     key = mean over the entry's members of (AP / maxAP) * Perception
    // and the entity that started the fight gets +100.  The queue is sorted
    // descending, so the highest Perception acts first.
    std::vector<int32_t> live;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        if (e.isCombatant && !e.dead) live.push_back(static_cast<int32_t>(i));
    }
    std::vector<float> keys(live.size(), 0.0f);
    for (size_t i = 0; i < live.size(); ++i) {
        const LiveEntity& e = m_entities[static_cast<size_t>(live[i])];
        const float frac = e.maxAp > 0 ? e.ap / static_cast<float>(e.maxAp) : 0.0f;
        keys[i] = frac * static_cast<float>(e.attr.perception);
    }
    std::vector<size_t> order(live.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t x, size_t y) { return keys[x] > keys[y]; });
    const int32_t wasActive = ActiveTurnEntity();
    m_turnQueue.clear();
    m_turnKey.clear();
    for (size_t i : order) {
        m_turnQueue.push_back(live[i]);
        m_turnKey.push_back(keys[i]);
    }
    // Keep pointing at the same actor when the queue is rebuilt mid-round.
    m_turnAt = 0;
    if (wasActive >= 0) {
        for (size_t i = 0; i < m_turnQueue.size(); ++i) {
            if (m_turnQueue[i] == wasActive) {
                m_turnAt = i;
                break;
            }
        }
    }
}

int32_t Scene::ActiveTurnEntity() const {
    if (m_combatMode == 0 || m_turnAt >= m_turnQueue.size()) return -1;
    return m_turnQueue[m_turnAt];
}

void Scene::BeginTurnFor(int32_t index) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    e.aiNextTick = m_ticks;
}

void Scene::EndTurn() {
    // `World::StartNextCombatTurn @00507610`: advance the index, skip anyone
    // who cannot act, and when it runs past the end refresh every combatant's
    // AP to full, re-sort, and start a new round.  The world clock advances by
    // `kRoundSeconds / queueSize` per turn, so a whole round is 7 seconds.
    if (m_combatMode == 0 || m_turnQueue.empty()) return;
    const size_t n = m_turnQueue.size();
    for (size_t step = 0; step < n; ++step) {
        ++m_turnAt;
        if (m_turnAt >= n) break;
        const LiveEntity& e = m_entities[static_cast<size_t>(m_turnQueue[m_turnAt])];
        if (!e.dead) {
            BeginTurnFor(m_turnQueue[m_turnAt]);
            return;
        }
    }
    // A new round: `TurnQueueEntry::RefreshAllMembersAP @0050ae90` calls
    // `ActorUnconsciousAPHandling(true)` on every member, which sets ap = maxAP.
    ++m_round;
    for (int32_t i : m_turnQueue) {
        LiveEntity& e = m_entities[static_cast<size_t>(i)];
        if (!e.dead) e.ap = static_cast<float>(e.maxAp);
    }
    RebuildTurnQueue();
    m_turnAt = 0;
    Logf("--- round %lld ---", static_cast<long long>(m_round));
    if (!m_turnQueue.empty()) BeginTurnFor(m_turnQueue[0]);
}

// (milestone 8's ActorAI is gone: milestone 11 replaces it with the real
// BaseAI/Controller/SentryAI system, in the AI section at the end of this
// file.)

// ===========================================================================
// MILESTONE 11 -- the AI.
//
// Three entity classes carry it and all three are in the `.mis` slot table:
// `BaseAI` (one per AI actor), `Controller` (one per PLAYER slot) and the
// `WayPoint` chain.  The think is `BaseAI::DoEngineThink @005128f0`, the
// desire model is `tables/aitable.txt`, and the move types are
// `Controller::SomethingMoveTypeRelated @0052e6b0`.  Every constant below was
// read out of the executable's `.rdata` at the address named beside it.
// ===========================================================================

namespace {

// `Actor::Actor_TrySneakPastDetection @00544770`, the detection radius.
// Nothing here is invented: each is the float at the address in the comment.
constexpr float kDetectPerceptionAdd = 4.0f;    // _DAT_0080c0c0
constexpr float kDetectScale = 3.0f;            // _DAT_0080c0b8
[[maybe_unused]] constexpr float kDetectDeathSense = 1.25f;   // _DAT_0080c0c8
constexpr float kFacingFullDeg = 60.0f;         // _DAT_00803368 and _DAT_0080c0e0
constexpr float kFacingFalloffDeg = 120.0f;     // 1 / _DAT_0080c0d8 (0.008333)
constexpr float kSneakNumerator = 100.0f;       // _DAT_0080c0f8
constexpr float kSneakDenomAdd = 100.0f;        // _DAT_0080c100
constexpr float kSneakObserverWeight = 0.5f;    // _PTR_007fd778
constexpr float kLightAdd = 0.5f;               // _PTR_007fd778, the same 0.5
constexpr float kLightScale = 0.6666667f;       // _DAT_0080c108
constexpr float kDarkSneakBonus = 1.2f;         // _DAT_0080c110
constexpr float kDarkThreshold = 0.6f;          // _DAT_0080a71c
constexpr float kPostureCrouch = 0.8f;          // the literal in the switch
constexpr float kPostureProne = 0.6f;
[[maybe_unused]] constexpr float kDetectLevelPartial = 0.75f; // 0x3f400000, CanDetectEntity==2

// `BaseAI::DoEngineThink @005128f0` and `BaseAI::DoGameThink @005133f0`.
constexpr float kThinkRateActive = 0.5f;        // the literal
constexpr float kThinkRateIdleBase = 1.5f;      // _DAT_007ff268, + rand[0,1)
constexpr float kPerceiveInterval = 2.0f;       // FLOAT_007fded8, + rand[0,1.5)
constexpr float kPerceiveJitter = 1.5f;         // the literal
constexpr float kThinkRateTurnBased = 0.33f;    // _DAT_0080a6dc

// `BaseAI::BuildDemeanourNatureFunctionTable @0051e8a0`: every aitable weight
// is scaled by this before it becomes a desire.
constexpr float kAiWeightScale = 0.1f;          // _DAT_0080a6fc

// `ComputeTargetScoreForEntity @005206f0`.
constexpr float kTargetDistWeight = 0.5f;       // _DAT_007fd93c
constexpr float kTargetRelationWeight = 0.05f;  // _DAT_0080a73c
constexpr float kTargetTagBonus = 10.0f;        // _DAT_007fd5a8

// `SentryAI::SentryAIMaybeCheckingTargets @0053ef80`: the sentry's reach is
// `max(1600, weaponRange^2)`, i.e. never under 40 world units.
constexpr float kSentryMinRange = 40.0f;        // sqrt(_DAT_0080a738 == 1600)

float Deg(float rad) { return rad * 57.29578f; }   // _DAT_0080a74c

}  // namespace

// ---------------------------------------------------------------------------
// The light level at an entity.  `Entity__GetLightLevelMaybe` is a call site
// only in this material, so this is milestone 5's own lighting sampled at the
// entity's feet and folded to luma -- a RECONSTRUCTION of the scalar, not of
// the formula that consumes it.
// ---------------------------------------------------------------------------
float Scene::EntityLightLevel(const LiveEntity& e) const {
    float A[3] = {1.0f, 1.0f, 1.0f};
    m_scene.AmbientForPoint(e.wpos, A);
    float l = kLumaR * A[0] + kLumaG * A[1] + kLumaB * A[2];
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    return l;
}

// ---------------------------------------------------------------------------
// Detection.  `Actor::Actor_TrySneakPastDetection @00544770` computes, for
// each candidate observer, a radius
//
//   radius = (PE * deathSense + 4) * 3
//            * facing * detectLevel * sneak * light * action * posture
//
// and the sneaker is spotted when the box-gap distance is under it.  The
// factors, all recovered:
//
//   facing       1 while the target is inside 60 degrees of the observer's
//                heading, then 1 - (deg - 60) / 120, reaching 0 at 180
//   sneak        100 / ((sneakerSneakSkill - observerSneakSkill * 0.5) + 100),
//                with the sneaker's skill multiplied by 1.2 when the local
//                light level is under 0.6
//   light        (lightLevel + 0.5) * 0.6666667, so 1.0 in full light and
//                0.333 in the pitch dark
//   posture      1.0 standing, 0.8 crouching, 0.6 prone
//   action       2.0 for a quiet attack, 1.33 for a non-Sneak skill use,
//                1.2 / 1.1 for the other action classes, 1.0 for standing
//                still; a LOUD attack skips the roll and is always seen
//   detectLevel  0.75 when CanDetectEntity returns 2 rather than 1
//
// What is NOT ported: the World+0x7c9 flag that doubles the radius (it is
// only ever set alongside a "see everything" branch in
// `CanPlayerDetectTargetEntity @00508ff0`, which reads as the multiplayer /
// no-fog case and never fires here), and `CanDetectEntity @006e59f0`'s inner
// `perception * 1.2` radius, whose companion predicate `FUN_00553790` has no
// body in any dump -- so `detectLevel` is left at 1.0 and labelled.
//
// The line test is milestone 8's `CountLineBlockers`: a blocked line is not a
// detection.  The original runs `MapSearch` over a swept box instead.
// ---------------------------------------------------------------------------
int32_t Scene::PlayerSide() const {
    if (m_spawnPlayer >= 0) return m_spawnPlayer;
    if (!m_squad.empty() && m_squad.front() >= 0 &&
        static_cast<size_t>(m_squad.front()) < m_entities.size()) {
        return m_entities[static_cast<size_t>(m_squad.front())].playerIndex;
    }
    if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_entities.size()) {
        return m_entities[static_cast<size_t>(m_selected)].playerIndex;
    }
    return -1;
}

// The `<Player>` record's own `side` is the answer wherever the map carries a
// player list: `PlayerDef::side` is 0 scenery, 1 AI and 2 THE HUMAN SQUAD
// (NOTES.md 36), and `--triggers` over `tut1.mis` prints its six slots with
// only "Human" on side 2.  That is the test used here, because the four
// tutorial maps place their `player` entity directly and carry no SpawnPoint,
// so `m_spawnPlayer` is -1 on them and nothing is selected on the first tick.
// `PlayerSide()` is the fallback for a map with no `<Player>` list at all.
bool Scene::IsPlayerOwned(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return false;
    const int32_t pi = m_entities[static_cast<size_t>(index)].playerIndex;
    if (pi < 0) return false;
    const std::vector<PlayerDef>& ps = m_script.Players();
    if (m_script.Loaded() && static_cast<size_t>(pi) < ps.size()) {
        return ps[static_cast<size_t>(pi)].side == 2;
    }
    const int32_t side = PlayerSide();
    return side >= 0 && pi == side;
}

// MILESTONE 22 (VIDEO_GAPS M4).  One floating status word over an entity.  The
// list is deduplicated per entity: the capture never shows the same word twice.
void Scene::PushStatusFloat(int32_t entity, const std::string& text, int64_t ms) {
    for (StatusFloat& f : m_statusFloats) {
        if (f.entity == entity && f.text == text) {
            f.untilMs = m_nowMs + ms;
            return;
        }
    }
    StatusFloat f;
    f.entity = entity;
    f.text = text;
    f.untilMs = m_nowMs + ms;
    m_statusFloats.push_back(f);
}

bool Scene::Detect(int32_t observer, int32_t target, DetectTerms* out,
                   float actionFactor) const {
    DetectTerms t;
    t.action = actionFactor;
    if (out != nullptr) *out = t;
    if (observer < 0 || target < 0 || observer >= static_cast<int32_t>(m_entities.size()) ||
        target >= static_cast<int32_t>(m_entities.size()) || observer == target) {
        return false;
    }
    const LiveEntity& o = m_entities[static_cast<size_t>(observer)];
    const LiveEntity& s = m_entities[static_cast<size_t>(target)];
    if (o.dead || s.dead) return false;

    t.perception = static_cast<float>(o.attr.perception);
    // The Death Sense perk multiplies the observer's perception by
    // `kDetectDeathSense`.  fotgame's Attributes carries no deathSense field
    // (no shipped actor in this campaign has the perk), so the factor is
    // recovered, wired, and always 1.0 here.
    const float deathSense = 1.0f;   // else kDetectDeathSense
    t.base = (t.perception * deathSense + kDetectPerceptionAdd) * kDetectScale;

    // Facing.  The observer's heading is its animation direction; the original
    // takes the atan2 of the observer's stored velocity/heading pair and
    // compares it with the atan2 of the vector to the sneaker.
    const float dx = static_cast<float>(s.world[0] - o.world[0]);
    const float dz = static_cast<float>(s.world[2] - o.world[2]);
    const float toTarget = std::atan2(dx, dz);
    // Direction index -> radians.  `Entity_UpdateFacingFromVelocityAndBounds`
    // quantises a heading to `dirCount` sectors; this inverts that.
    const int32_t dc = o.dirCount > 0 ? o.dirCount : 8;
    const float heading = 6.2831853f * static_cast<float>(o.direction) / static_cast<float>(dc);
    float diff = std::fabs(toTarget - heading);
    while (diff > 6.2831853f) diff -= 6.2831853f;
    if (diff > 3.14159265f) diff = 6.2831853f - diff;
    t.angleDeg = Deg(diff);
    t.facing = 1.0f;
    if (t.angleDeg > kFacingFullDeg) {
        t.facing = 1.0f - (t.angleDeg - kFacingFullDeg) / kFacingFalloffDeg;
        if (t.facing < 0.0f) t.facing = 0.0f;
    }

    // The sneak term.
    t.lightLevel = EntityLightLevel(s);
    float sneakSkill = static_cast<float>(s.attr.skills[kSkillSneak]);
    if (t.lightLevel < kDarkThreshold) sneakSkill *= kDarkSneakBonus;
    t.sneakSkill = sneakSkill;
    const float denom =
        (sneakSkill - static_cast<float>(o.attr.skills[kSkillSneak]) * kSneakObserverWeight) +
        kSneakDenomAdd;
    t.sneak = denom > 1.0f ? kSneakNumerator / denom : kSneakNumerator;

    t.light = (t.lightLevel + kLightAdd) * kLightScale;

    t.posture = 1.0f;
    if (s.posture == "Crouch") t.posture = kPostureCrouch;
    else if (s.posture == "Prone") t.posture = kPostureProne;

    // `CanDetectEntity @006e59f0` returns 2 -- and the roll then uses
    // `kDetectLevelPartial` instead of 1.0 -- when the target is inside the
    // observer's inner `perception * 1.2` radius AND the companion predicate
    // `FUN_00553790` is false.  That predicate has no body in any dump, so the
    // branch cannot be taken faithfully and the factor stays at 1.0.
    t.level = 1.0f;   // else kDetectLevelPartial
    t.radius = t.base * t.facing * t.level * t.sneak * t.light * t.action * t.posture;

    t.distance = BoxGapDistance(o, s);
    bool blocked = false;
    CountLineBlockers(o, s, &blocked);
    t.blocked = blocked;
    t.detected = !blocked && t.distance < t.radius;
    if (out != nullptr) *out = t;
    return t.detected;
}

// ---------------------------------------------------------------------------
// `ComputeTargetScoreForEntity @005206f0`, transcribed:
//
//   if (relation(self, target) >= 0)  return 0;      // only hostiles score
//   if (target is hidden)             return 0;
//   score = controllerScore * 0.333 + invDistance * 0.5 + (0.5 - relation * 0.05)
//   if (target's tag == the controller's move-target tag)  score += 10
//
// The controller term is `Controller::ControllerGetFromUnknownList2`, whose
// body IS in the dump but whose list is filled from a slot with no writer in
// this material; it is left at 0 and labelled.  Everything else is exact:
// `relation` is milestone 10's alignment matrix, so a -10 enemy contributes
// 1.0 and a -1 contributes 0.55, and the tag bonus dominates -- which is how
// a mission tells a controller's units to go for one named unit.
// ---------------------------------------------------------------------------
float Scene::AiTargetScore(int32_t self, int32_t target) const {
    if (self < 0 || target < 0) return 0.0f;
    const LiveEntity& a = m_entities[static_cast<size_t>(self)];
    const LiveEntity& b = m_entities[static_cast<size_t>(target)];
    if (b.dead || !b.isCombatant) return 0.0f;
    int32_t rel = 0;
    if (a.playerIndex >= 0 && b.playerIndex >= 0 && m_script.Loaded()) {
        rel = m_script.Relation(a.playerIndex, b.playerIndex);
    } else {
        rel = Hostile(a, b) ? -10 : 10;
    }
    if (rel >= 0) return 0.0f;
    const float dist = BoxGapDistance(a, b);
    const float invDist = dist > 1.0f ? 1.0f / dist : 1.0f;
    float score = invDist * kTargetDistWeight +
                  (kTargetDistWeight - static_cast<float>(rel) * kTargetRelationWeight);
    if (a.ai >= 0) {
        const BaseAiState& s = m_ai[static_cast<size_t>(a.ai)];
        if (s.controller >= 0) {
            const ControllerState& c = m_controllers[static_cast<size_t>(s.controller)];
            if (!c.moveTargetTag.empty() && b.tag == c.moveTargetTag) score += kTargetTagBonus;
        }
    }
    return score;
}

// ---------------------------------------------------------------------------
// Load.  The `.mis` carries the whole AI layer as ordinary entities, so this
// is a second pass over the same slot table `ResolveEntities` walked:
//
//   BaseAI      "Attached" (an EntityID: u16 slot, u16 sequence number),
//               "Nature", "Tactic", "Use Cover", "Use Sneak", "Sleeping",
//               "Current Target", "Controller", "Target Rating".
//   Controller  "Player Index", "Controller Type", "Move Target Tag",
//               "Move Type Priority", "Working Zone", "Retreat Zone",
//               "Aggression", "Communicate", "Mobility", "Group Move",
//               "Rand Move", "Never Leave Zone", "Follow Target".
//   WayPoint    "Tag Name" (the route), "Waypoint Index", "Next Waypoint
//               List", "Pause Time", "Action".
//
// The Controller is per PLAYER, not per actor: `Controller::Vtable112` and
// `SomethingMoveTypeRelated` both walk `WorldGlobal->ptrPlayerList[playerIndex]`'s
// own entity list and call `SetMoveTargetToEntity` on every one.  So a
// BaseAI's controller is "the Controller whose Player Index equals my actor's".
// ---------------------------------------------------------------------------
void Scene::LoadAI() {
    m_ai.clear();
    m_controllers.clear();

    // aitable.txt lives beside the other tables in the extracted tree.
    std::string aiErr;
    const std::string tablePath =
        (m_opt.entDir.empty() ? std::string(".") : m_opt.entDir) + "/tables/aitable.txt";
    if (!m_aiTable.Load(tablePath, &aiErr)) {
        std::fprintf(stderr, "  AI: %s (natures and demeanours unavailable)\n", aiErr.c_str());
    }

    // slot -> live entity index.
    std::unordered_map<int32_t, int32_t> byMis;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        byMis[static_cast<int32_t>(m_entities[i].misIndex)] = static_cast<int32_t>(i);
    }
    std::unordered_map<int32_t, int32_t> liveBySlot;
    for (size_t n = 0; n < m_world.entityFile.entities.size(); ++n) {
        auto it = byMis.find(static_cast<int32_t>(n));
        if (it != byMis.end()) {
            liveBySlot[m_world.entityFile.entities[n].slot] = it->second;
        }
    }
    auto entityRef = [](const Esh& esh, const char* name) -> int32_t {
        for (const ft::EshProp& p : esh.props) {
            if (p.name == name && p.payload.size() >= 4) {
                const int32_t id = p.payload[0] | (p.payload[1] << 8);
                return id == 0 ? -1 : id;   // slot 0 is the reserved null
            }
        }
        return -1;
    };

    // --- the controllers, and the waypoint routes -------------------------
    std::map<std::string, std::vector<WaypointNode>> routes;
    for (const EntityInstance& inst : m_world.entityFile.entities) {
        if (inst.className != "WayPoint") continue;
        WaypointNode w;
        auto lit = liveBySlot.find(inst.slot);
        w.entity = lit == liveBySlot.end() ? -1 : lit->second;
        inst.esh.GetInt("Waypoint Index", &w.index);
        inst.esh.GetFloat("Pause Time", &w.pause);
        inst.esh.GetString("Action", &w.action);
        inst.esh.GetString("Next Waypoint List", &w.nextList);
        for (int k = 0; k < 3; ++k) w.world[k] = inst.world[k];
        routes[inst.tagName].push_back(w);
    }
    // `Controller::ValidateAndBuildWaypointList @0052ebf0` walks the chain in
    // `Waypoint Index` order and logs "Waypoint index %d should be %d" when a
    // node is out of place, so index order is the route order.
    for (auto& kv : routes) {
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const WaypointNode& a, const WaypointNode& b) { return a.index < b.index; });
    }

    for (const EntityInstance& inst : m_world.entityFile.entities) {
        if (inst.className != "Controller") continue;
        ControllerState c;
        auto lit = liveBySlot.find(inst.slot);
        c.entity = lit == liveBySlot.end() ? -1 : lit->second;
        c.playerIndex = inst.playerIndex;
        inst.esh.GetInt("Player Index", &c.playerIndex);
        inst.esh.GetString("Controller Type", &c.type);
        if (c.type.empty()) c.type = "Stationary";
        inst.esh.GetString("Move Target Tag", &c.moveTargetTag);
        inst.esh.GetInt("Move Type Priority", &c.movePriority);
        inst.esh.GetInt("Aggression", &c.aggression);
        inst.esh.GetInt("Communicate", &c.communicate);
        inst.esh.GetInt("Mobility", &c.mobility);
        inst.esh.GetString("Working Zone", &c.workingZone);
        inst.esh.GetString("Retreat Zone", &c.retreatZone);
        inst.esh.GetBool("Group Move", &c.groupMove);
        inst.esh.GetBool("Rand Move", &c.randMove);
        inst.esh.GetBool("Never Leave Zone", &c.neverLeaveZone);
        const int32_t follow = entityRef(inst.esh, "Follow Target");
        if (follow > 0) {
            auto f = liveBySlot.find(follow);
            if (f != liveBySlot.end()) c.followTarget = f->second;
        }
        // The route: `Move Target Tag` names it for the Waypoints move types.
        auto r = routes.find(c.moveTargetTag);
        if (r != routes.end()) c.route = r->second;
        // `SomethingMoveTypeRelated` defaults an empty working zone to
        // "Anywhere" and steers by `(min + max) * 0.5` of the zone box.  The
        // zones are in metres; the world is in the .mis's integer units, which
        // are metres * kEntityWorldScale.
        const WorldZone* wz = m_script.Zone(c.workingZone);
        if (wz != nullptr) {
            for (int k = 0; k < 3; ++k) {
                c.centre[k] = (wz->box[k] + wz->box[k + 3]) * 0.5f * kEntityWorldScale;
            }
            c.haveCentre = true;
        }
        const WorldZone* rz = m_script.Zone(c.retreatZone);
        if (rz != nullptr) {
            for (int k = 0; k < 3; ++k) {
                c.retreatCentre[k] = (rz->box[k] + rz->box[k + 3]) * 0.5f * kEntityWorldScale;
            }
            c.haveRetreatCentre = true;
        }
        m_controllers.push_back(std::move(c));
    }

    // --- the BaseAIs -------------------------------------------------------
    for (const EntityInstance& inst : m_world.entityFile.entities) {
        if (inst.className != "BaseAI") continue;
        const int32_t attachedSlot = entityRef(inst.esh, "Attached");
        auto a = liveBySlot.find(attachedSlot);
        if (a == liveBySlot.end()) continue;
        BaseAiState s;
        s.valid = true;
        s.entity = a->second;
        inst.esh.GetString("Nature", &s.nature);
        if (s.nature.empty()) s.nature = "Default";
        inst.esh.GetString("Tactic", &s.tactic);
        if (s.tactic.empty()) s.tactic = "Advance";
        inst.esh.GetBool("Use Cover", &s.useCover);
        inst.esh.GetBool("Use Sneak", &s.useSneak);
        inst.esh.GetBool("Sleeping", &s.sleeping);
        inst.esh.GetFloat("Target Rating", &s.targetRating);
        s.natureIndex = m_aiTable.Loaded() ? m_aiTable.NatureIndex(s.nature) : -1;
        if (s.natureIndex < 0 && m_aiTable.Loaded()) {
            // `BuildDemeanourNatureFunctionTable`'s own failure message.
            Logf("Failed to find nature '%s' in AI", s.nature.c_str());
            s.natureIndex = m_aiTable.NatureIndex("Default");
        }
        // The ten records BuildDemeanourNatureFunctionTable constructs: the
        // starting desire and the nature's preference, both scaled by 0.1.
        for (int d = 0; d < kDemeanourCount; ++d) {
            if (!m_aiTable.Loaded()) break;
            s.desire[d] = static_cast<float>(m_aiTable.starting[static_cast<size_t>(d)]) *
                          kAiWeightScale;
            if (s.natureIndex >= 0 &&
                s.natureIndex < static_cast<int32_t>(m_aiTable.vsNature.size())) {
                s.natureWeight[d] =
                    static_cast<float>(
                        m_aiTable.vsNature[static_cast<size_t>(s.natureIndex)]
                                          [static_cast<size_t>(d)]) *
                    kAiWeightScale;
            }
        }
        // The controller is the one for this actor's player slot.
        const LiveEntity& owner = m_entities[static_cast<size_t>(s.entity)];
        for (size_t ci = 0; ci < m_controllers.size(); ++ci) {
            if (m_controllers[ci].playerIndex == owner.playerIndex) {
                s.controller = static_cast<int32_t>(ci);
                break;
            }
        }
        // `BaseAI+0x1f1` is seeded from the record's `nextEngineThink`
        // (1.5 in every shipped instance) and the perception timer starts
        // immediately.
        s.thinkRate = kThinkRateIdleBase;
        s.nextEngineThink = 0.0;
        s.nextPerceive = 0.0;
        m_ai.push_back(std::move(s));
    }
    for (size_t i = 0; i < m_ai.size(); ++i) {
        m_entities[static_cast<size_t>(m_ai[i].entity)].ai = static_cast<int32_t>(i);
    }
    for (BaseAiState& s : m_ai) {
        if (s.controller >= 0) {
            m_controllers[static_cast<size_t>(s.controller)].units.push_back(s.entity);
        }
    }
}

// ---------------------------------------------------------------------------
// `BaseAI::BaseAIEngineThinkRelated @0051e1d0` -- one of aitable.txt's nine
// events raises the desires:
//
//   for each demeanour d:
//       w = demeanours_versus_events[event][d]
//       if (w > 0) { desire = clamp(w * strength * 0.1);
//                    highestdesire = max(highestdesire, desire);
//                    with 80% probability, push the event's source onto the
//                    vislist (_DAT_0080954c == 0.8, _DAT_00808bb0 == 1/32768) }
//   NextEngineThink = now + (1 - highestdesire) * (NextEngineThink - now)
//
// so a strong event both raises the desire and pulls the next think forward.
// `strength` is the event record's own scale (`param_1[7]`); no shipped
// producer of that field is in the dumps, so it is 1 here and LABELLED.
// ---------------------------------------------------------------------------
void Scene::AiRaiseEvent(int32_t entity, int event, int32_t source) {
    if (entity < 0 || entity >= static_cast<int32_t>(m_entities.size())) return;
    LiveEntity& e = m_entities[static_cast<size_t>(entity)];
    if (e.ai < 0 || !m_aiTable.Loaded()) return;
    if (event < 0 || event >= static_cast<int32_t>(m_aiTable.vsEvent.size())) return;
    BaseAiState& s = m_ai[static_cast<size_t>(e.ai)];
    const double now = static_cast<double>(m_ticks) * 0.07;
    for (int d = 0; d < kDemeanourCount; ++d) {
        const int32_t w = m_aiTable.vsEvent[static_cast<size_t>(event)][static_cast<size_t>(d)];
        if (w <= 0) continue;
        const float desire = static_cast<float>(w) * kAiWeightScale;
        if (desire > s.eventDesire[d]) s.eventDesire[d] = desire;
        if (desire > s.desire[d]) s.desire[d] = desire;
        // `highestdesire` (BaseAI+0x201) is the "this AI is busy" flag, and
        // BaseAIEngineThinkRelated starts it at 0 and only ever raises it from
        // an event -- which is why a freshly loaded AI is idle and its
        // Controller's move type, not its demeanour, is what moves it.
        const float key = desire * s.natureWeight[d];
        if (key > s.highestDesire) s.highestDesire = key;
    }
    if (source >= 0 && source < static_cast<int32_t>(m_entities.size())) {
        bool have = false;
        for (int32_t v : s.visList) {
            if (v == source) { have = true; break; }
        }
        if (!have && m_rng.Range(0.0f, 1.0f) < 0.8f) s.visList.push_back(source);
        for (int k = 0; k < 3; ++k) {
            e.investigate[k] = m_entities[static_cast<size_t>(source)].world[k];
        }
    }
    // The think is pulled forward in proportion to the new desire.
    int32_t which = -1;
    const float top = AiHighestDesire(s, &which);
    if (s.nextEngineThink > now) {
        s.nextEngineThink = now + (1.0 - std::min(1.0f, top)) * (s.nextEngineThink - now);
    }
    if (s.sleeping && top > 0.0f) s.sleeping = false;
}

// `BroadcastHearCombatEventToNearbyEntities @00552530`: a shot raises
// `hearCombat` on everyone inside a radius, and `tookDamage` on the target.
// The radius itself is one of the search-filter parameters the dump does not
// carry; 40 world units is the same engagement reach `SentryAI` uses and it is
// LABELLED as a reconstruction.
void Scene::AiBroadcastCombat(int32_t shooter, int32_t target) {
    if (shooter < 0 || shooter >= static_cast<int32_t>(m_entities.size())) return;
    const LiveEntity& a = m_entities[static_cast<size_t>(shooter)];
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& b = m_entities[i];
        if (!b.isCombatant || b.dead || b.ai < 0) continue;
        if (static_cast<int32_t>(i) == shooter) continue;
        if (BoxGapDistance(a, b) > kSentryMinRange) continue;
        AiRaiseEvent(static_cast<int32_t>(i), kEvHearCombat, shooter);
    }
    if (target >= 0) AiRaiseEvent(target, kEvTookDamage, shooter);
}

// The winning demeanour.  `BaseAI::UpdateDemeanourNatureMoveSpeed @0051e520`
// sorts the ten records and takes the first; `highestdesire` is that record's
// own key.
//
// RECONSTRUCTION, and the one genuinely reconstructed step in this milestone:
// the record holds two floats -- the current desire (from
// `starting_demeanours`, raised by events) and the nature's preference from
// `demeanours_versus_natures` -- and `std::SortEntriesByFloatKeyAscending`
// sorts on a key derived from them whose arithmetic is not in the dump.  The
// PRODUCT is used here because it is the only combination that reproduces
// every nature's stated intent: a Coward (attack 2, flee 5) flees when it
// takes damage where a sum would make it attack; a Turret (attack 10, flee 0)
// can never flee; a Dummy (all zero) never acts at all; and a Looter drops
// everything for a corpse (see 8 x eat 8) exactly as its name says.
float Scene::AiHighestDesire(const BaseAiState& s, int32_t* which) const {
    float best = 0.0f;
    int32_t bestD = -1;
    for (int d = 0; d < kDemeanourCount; ++d) {
        const float key = s.desire[d] * s.natureWeight[d];
        if (key > best) {
            best = key;
            bestD = d;
        }
    }
    if (which != nullptr) *which = bestD;
    return best;
}

// ---------------------------------------------------------------------------
// `BaseAI::PerceiveAndScoreNearbyEntities @0051fa40` -- the perception sweep.
// It builds a search filter around the actor, skips anything unconscious or
// asleep, asks `CanDetectEntity` about each survivor, and scores the ones it
// can see.  Here the detection test is the recovered radius formula and the
// scoring is `ComputeTargetScoreForEntity`; a newly seen hostile raises
// `seeTarget` and a newly seen corpse raises `seeCorpse`.
// ---------------------------------------------------------------------------
void Scene::AiPerceive(BaseAiState& s) {
    LiveEntity& self = m_entities[static_cast<size_t>(s.entity)];
    if (self.dead) return;
    const bool e_hasTarget = s.currentTarget >= 0 &&
                             s.currentTarget < static_cast<int32_t>(m_entities.size()) &&
                             !m_entities[static_cast<size_t>(s.currentTarget)].dead;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (static_cast<int32_t>(i) == s.entity) continue;
        const LiveEntity& other = m_entities[i];
        if (!other.isCombatant) continue;
        if (other.dead) {
            // seeCorpse: only the natures that weight MiscEatCorpse or
            // MiscInvestigate react, which the desire product takes care of.
            DetectTerms dt;
            if (Detect(s.entity, static_cast<int32_t>(i), &dt)) {
                AiRaiseEvent(s.entity, kEvSeeCorpse, static_cast<int32_t>(i));
            }
            continue;
        }
        if (!Hostile(self, other)) continue;
        DetectTerms dt;
        const bool seen = Detect(s.entity, static_cast<int32_t>(i), &dt);
        if (m_traceDetect) {
            char buf[320];
            std::snprintf(buf, sizeof(buf),
                          "DETECT tick=%lld obs=%s(PE %d) tgt=%s dist=%.2fm radius=%.3fm "
                          "base=%.1f facing=%.4f(%.0fdeg) sneak=%.4f(skill %.0f) "
                          "light=%.4f(%.4f) posture=%.2f(%s) action=%.2f blocked=%d -> %s",
                          static_cast<long long>(m_ticks), self.label.c_str(),
                          self.attr.perception, other.label.c_str(), dt.distance, dt.radius,
                          dt.base, dt.facing, dt.angleDeg, dt.sneak, dt.sneakSkill, dt.light,
                          dt.lightLevel, dt.posture, other.posture.c_str(), dt.action,
                          dt.blocked ? 1 : 0, seen ? "DETECTED" : "unseen");
            m_detectLog.emplace_back(buf);
            if (m_detectLog.size() > 4096) m_detectLog.erase(m_detectLog.begin());
            std::printf("%s\n", buf);
        }
        if (!seen) continue;
        bool have = false;
        for (int32_t v : s.visList) {
            if (v == static_cast<int32_t>(i)) { have = true; break; }
        }
        // MILESTONE 17 fix.  The console line is "this AI has now SEEN
        // something", and it belongs to the sighting, not to the list
        // insertion: `AiRaiseEvent` also pushes a source into `visList` (with
        // the 80% roll above), so an AI that hears gunfire before it sees
        // anything already has the shooter in its list and the `!have` branch
        // never runs -- which silenced every detection message in a fight the
        // squad's sentries opened.  `everDetected` is still the once-per-AI
        // latch it always was.
        if (!s.everDetected) {
            s.everDetected = true;
            Logf("%s detects %s at %.1f m (radius %.1f m)", self.label.c_str(),
                 other.label.c_str(), dt.distance, dt.radius);
        }
        if (!have) {
            s.visList.push_back(static_cast<int32_t>(i));
            AiRaiseEvent(s.entity, kEvSeeTarget, static_cast<int32_t>(i));
            // MILESTONE 22 (VIDEO_GAPS M4).  f0246 floats the word "Detected"
            // over a SQUAD MEMBER's head -- it is the player being told that an
            // enemy has just seen one of his own, so the float belongs to the
            // TARGET, not to the observer, and only when the target is the
            // player's.  The word is the shipped string
            // `locale/game/reason.txt`'s `ent_sneakRevealed` = {Detected} --
            // the same table that holds `ent_isLocked` = {Locked}, which f0416
            // floats over a door in exactly this style.
            if (IsPlayerOwned(static_cast<int32_t>(i))) {
                PushStatusFloat(static_cast<int32_t>(i), "ent_sneakRevealed");
            }
        }
    }
    // Nothing in sight and nowhere to investigate: the raised desires lapse
    // and the AI goes back to its Controller.  The original's decay is in
    // `Controller::MaintainRecentPositionHistoryList`'s sleep-record walk,
    // whose timing constants are not in the dump; this is LABELLED.
    if (s.visList.empty() && self.investigate[1] < 0 && !e_hasTarget) {
        for (int d = 0; d < kDemeanourCount; ++d) {
            s.eventDesire[d] = 0.0f;
            s.desire[d] = m_aiTable.Loaded()
                              ? static_cast<float>(m_aiTable.starting[static_cast<size_t>(d)]) *
                                    kAiWeightScale
                              : 0.0f;
        }
        s.highestDesire = 0.0f;
        s.currentTarget = -1;
    }
    // Forget anything dead or gone.
    s.visList.erase(std::remove_if(s.visList.begin(), s.visList.end(),
                                   [this](int32_t v) {
                                       return v < 0 ||
                                              v >= static_cast<int32_t>(m_entities.size()) ||
                                              m_entities[static_cast<size_t>(v)].dead;
                                   }),
                    s.visList.end());
}

// `SelectBestTargetFromEntityList`, the call Actor::AIEvaluateCombatEngagementScore
// makes before anything else: the highest `ComputeTargetScoreForEntity` in the
// vislist wins, and the score is remembered in `targetrating`.
bool Scene::AiSelectTarget(BaseAiState& s) {
    float best = 0.0f;
    int32_t bestI = -1;
    for (int32_t v : s.visList) {
        const float sc = AiTargetScore(s.entity, v);
        if (sc > best) {
            best = sc;
            bestI = v;
        }
    }
    s.currentTarget = bestI;
    s.targetRating = best;
    return bestI >= 0;
}

// A move order that tolerates an unreachable exact point: the nav grid is
// milestone 7's and `OrderMove` already snaps to the nearest open cell.
bool Scene::MoveToward(BaseAiState& s, float wx, float wz, bool run) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (e.moving) return true;
    // `BaseAI+0x1fd` pathattempts: the original gives up on a destination
    // after repeated failures rather than retrying every think.
    if (OrderMove(s.entity, static_cast<int32_t>(wx), static_cast<int32_t>(wz), run)) {
        s.pathAttempts = 0;
        return true;
    }
    ++s.pathAttempts;
    return false;
}

// ---------------------------------------------------------------------------
// The Attack demeanour -- `Actor::AIEvaluateCombatEngagementScore @00519d50`.
// Its shape, in the order the original tests:
//
//   ShouldCowardActorFlee            -> 0, the flee demeanour takes over
//   suppressed (BaseAI+0x239)        -> DecideStanceResponseToThreat
//   SelectBestTargetFromEntityList   -> no target: 0 (or the controller's own
//                                       score when Controller::Vtable150 holds)
//   ChooseWeaponModeAvoidingFriendlyFire
//   out of range / no line           -> ComputeAIThreatDistanceScore, i.e. move
//   ActorCanAttackWithActiveWeapon   -> else ReloadActiveWeaponWithAnyAmmo,
//                                       and failing that unequip and try again
//   otherwise                        -> fire
//
// The approach is steered by the BaseAI's `Tactic`, which is Advance, Retreat
// or Flank in every shipped record.  The three routes below are a
// RECONSTRUCTION: `CombatAI_EvaluateAttackAngleAndApproach @0061a8c0` scores
// candidate approach points and its scoring loop survives, but the point
// GENERATOR it scores is `SphereSearch`, which is not in the dump.
// ---------------------------------------------------------------------------
float Scene::DemeanourAttack(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (!e.weapon.valid) return 0.0f;
    if (!AiSelectTarget(s)) return 0.0f;
    const int32_t tgt = s.currentTarget;
    const LiveEntity& t = m_entities[static_cast<size_t>(tgt)];

    HitTerms terms;
    const bool canHit = ComputeHitChance(s.entity, tgt, &terms);
    const WeaponMode* mode = e.weapon.Mode();
    const float maxRange = mode != nullptr ? mode->maxRange : 0.0f;

    // Dry magazine with rounds in the pack: reload.  The original scores a
    // mode it cannot fire at zero (`CombatAI_EvaluateWeaponModeApproachScore
    // @0061a420`), which is what sends it down this branch.
    if (mode != nullptr && e.weapon.magazine > 0 && e.ammo < mode->ammoUsage) {
        if (ReloadWeapon(e)) return 1.0f;
    }

    if (canHit && terms.total > 0) {
        std::string why;
        // OrderAttack is `Actor::BeginAttackAnEntity @0054a980`, which stops
        // the movement itself -- the same StopMovement the desire gate does.
        if (OrderAttack(s.entity, tgt, &why)) {
            AiBroadcastCombat(s.entity, tgt);
            return 1.0f;
        }
    }

    // Cannot shoot from here: close, flank or back off, per the Tactic.
    if (e.moving) return 1.0f;
    const float dx = static_cast<float>(t.world[0] - e.world[0]);
    const float dz = static_cast<float>(t.world[2] - e.world[2]);
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 0.01f) return 0.0f;
    const float ux = dx / len, uz = dz / len;
    // Stand off at 80% of the weapon's reach, which is where the range
    // penalty term of `ComputeWeaponAttackHitChance` stops growing.  Weapon
    // ranges and `HitTerms::distance` are in METRES (mapview/NOTES.md 14) and
    // the world grid is in quarter-metre units, so the stand-off converts.
    const float standM = maxRange > 1.0f ? maxRange * 0.8f : 1.0f;
    const float stand = standM * kEntityWorldScale;
    float gx = 0.0f, gz = 0.0f;
    if (s.tactic == "Retreat" && terms.distance < standM * 0.5f) {
        gx = static_cast<float>(e.world[0]) - ux * stand;
        gz = static_cast<float>(e.world[2]) - uz * stand;
    } else if (s.tactic == "Flank") {
        // Approach on an arc: the stand-off point rotated 60 degrees around
        // the target, which is what puts flankers off the defender's facing.
        const float a = std::atan2(ux, uz) + 1.0471976f;   // pi/3
        gx = static_cast<float>(t.world[0]) - std::sin(a) * stand;
        gz = static_cast<float>(t.world[2]) - std::cos(a) * stand;
    } else {
        gx = static_cast<float>(t.world[0]) - ux * stand;
        gz = static_cast<float>(t.world[2]) - uz * stand;
    }
    // `Use Cover` diverts the approach through the cover demeanour's search.
    if (s.useCover && DemeanourCover(s) > 0.0f) return 1.0f;
    // In turn-based, moving costs AP; give the turn up when there is none.
    if (m_combatMode != 0 && e.ap < 1.0f) {
        EndTurn();
        return 1.0f;
    }
    return MoveToward(s, gx, gz, true) ? 1.0f : 0.0f;
}

// The MoveFlee demeanour -- `MoveFleeDemeanourThink @0051b1e0`.  It runs for
// the controller's Retreat Zone centre when there is one, and directly away
// from the threat when there is not.
float Scene::DemeanourFlee(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (e.moving) return 1.0f;
    float gx = 0.0f, gz = 0.0f;
    bool have = false;
    if (s.controller >= 0) {
        const ControllerState& c = m_controllers[static_cast<size_t>(s.controller)];
        if (c.haveRetreatCentre) {
            gx = c.retreatCentre[0];
            gz = c.retreatCentre[2];
            have = true;
        }
    }
    if (!have) {
        AiSelectTarget(s);
        if (s.currentTarget < 0) return 0.0f;
        const LiveEntity& t = m_entities[static_cast<size_t>(s.currentTarget)];
        const float dx = static_cast<float>(e.world[0] - t.world[0]);
        const float dz = static_cast<float>(e.world[2] - t.world[2]);
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 0.01f) return 0.0f;
        gx = static_cast<float>(e.world[0]) + dx / len * 40.0f;
        gz = static_cast<float>(e.world[2]) + dz / len * 40.0f;
    }
    if (MoveToward(s, gx, gz, true)) {
        if (s.currentDemeanour != kMoveFlee || !s.announced) {
            s.announced = true;
            Logf("%s flees (%s nature)", e.label.c_str(), s.nature.c_str());
        }
        return 1.0f;
    }
    return 0.0f;
}

// The MoveCover demeanour -- `MoveCoverDemeanourThink @0051c070`, whose body
// is a dispatch into the same approach scorer the Attack demeanour uses.  The
// SEARCH is a reconstruction: cells within twelve world units that stand
// beside a static blocker, keep a line to the target, and are no further from
// it than the actor already is.  The tile flags it consults (Cover, Window,
// Ethereal) are milestone 8's recovered ones.
float Scene::DemeanourCover(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (e.moving) return 1.0f;
    if (s.currentTarget < 0 && !AiSelectTarget(s)) return 0.0f;
    const LiveEntity& t = m_entities[static_cast<size_t>(s.currentTarget)];
    const int32_t cx = m_nav.CellX(e.world[0]), cz = m_nav.CellZ(e.world[2]);
    const float here = BoxGapDistance(e, t) * kEntityWorldScale;   // metres -> world units
    int32_t bestX = 0, bestZ = 0;
    float bestScore = -1.0f;
    const int32_t reach = 12 / kNavCell;
    for (int32_t dz = -reach; dz <= reach; ++dz) {
        for (int32_t dx = -reach; dx <= reach; ++dx) {
            const int32_t nx = cx + dx, nz = cz + dz;
            if (!m_nav.Open(nx, nz, true)) continue;
            // A neighbouring static blocker is what makes the cell cover.
            bool adjacent = false;
            for (int32_t oz = -1; oz <= 1 && !adjacent; ++oz) {
                for (int32_t ox = -1; ox <= 1 && !adjacent; ++ox) {
                    if (ox == 0 && oz == 0) continue;
                    if (!m_nav.In(nx + ox, nz + oz)) continue;
                    const uint8_t l = m_nav.los[m_nav.Idx(nx + ox, nz + oz)];
                    if (l == kLosSolid || l == kLosBonusCover) adjacent = true;
                }
            }
            if (!adjacent) continue;
            const float wx = static_cast<float>(m_nav.CentreX(nx));
            const float wz = static_cast<float>(m_nav.CentreZ(nz));
            const float ddx = wx - static_cast<float>(t.world[0]);
            const float ddz = wz - static_cast<float>(t.world[2]);
            const float d = std::sqrt(ddx * ddx + ddz * ddz);
            if (d > here + 4.0f) continue;
            const float score = 100.0f - d;
            if (score > bestScore) {
                bestScore = score;
                bestX = static_cast<int32_t>(wx);
                bestZ = static_cast<int32_t>(wz);
            }
        }
    }
    if (bestScore < 0.0f) return 0.0f;
    if (bestX == e.world[0] && bestZ == e.world[2]) return 0.0f;
    if (MoveToward(s, static_cast<float>(bestX), static_cast<float>(bestZ), true)) {
        if (!s.announced) {
            s.announced = true;
            Logf("%s takes cover (Use Cover)", e.label.c_str());
        }
        return 1.0f;
    }
    return 0.0f;
}

// The MoveWander demeanour -- `MoveWanderDemeanourThink @0051c1a0`.  A random
// point inside the controller's working zone; without a zone, a short random
// step, which is what the shipped Brahmin do.
float Scene::DemeanourWander(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (e.moving) return 1.0f;
    float gx = static_cast<float>(e.world[0]) + m_rng.Range(-16.0f, 16.0f);
    float gz = static_cast<float>(e.world[2]) + m_rng.Range(-16.0f, 16.0f);
    if (s.controller >= 0) {
        const ControllerState& c = m_controllers[static_cast<size_t>(s.controller)];
        const WorldZone* wz = m_script.Zone(c.workingZone);
        if (wz != nullptr && c.workingZone != "Anywhere") {
            gx = m_rng.Range(wz->box[0], wz->box[3]) * kEntityWorldScale;
            gz = m_rng.Range(wz->box[2], wz->box[5]) * kEntityWorldScale;
        }
    }
    return MoveToward(s, gx, gz, false) ? 1.0f : 0.0f;
}

// The MiscInvestigate demeanour -- `ComputeHideAndSeekRepositionScoreByName
// @0051d650`.  Walk to where the event came from.
float Scene::DemeanourInvestigate(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    if (e.investigate[1] < 0) return 0.0f;
    if (e.moving) return 1.0f;
    const float gx = static_cast<float>(e.investigate[0]);
    const float gz = static_cast<float>(e.investigate[2]);
    const float dx = gx - static_cast<float>(e.world[0]);
    const float dz = gz - static_cast<float>(e.world[2]);
    if (dx * dx + dz * dz < 36.0f) {
        e.investigate[1] = -1;   // arrived
        return 0.0f;
    }
    return MoveToward(s, gx, gz, false) ? 1.0f : 0.0f;
}

// ---------------------------------------------------------------------------
// The Controller move types -- `Controller::SomethingMoveTypeRelated
// @0052e6b0`.  It defaults an empty working zone to the map-wide "Anywhere",
// takes the zone's centre, attaches every unit in the player slot, and then
// dispatches on `Controller Type`:
//
//   Deactivate       DeactivateAllLinkedActorsForPlayer            -- stand down
//   Waypoints        Controller::ValidateAndBuildWaypointList      -- the route in order
//   RandomWaypoints  Controller::ValidateAndBindControllerWaypoints-- the route at random
//   Stationary       Controller_IssueMoveOrderAndMaybeRandomizeAnywhereDestination
//   PerimeterZone    Controller::BuildGuardMoveTypeWaypointList    -- the zone's edge
//   Follow           ResolveUniqueEntityByTag                      -- the tagged unit
//   Flee             Controller::SwapRetreatAndWorkingZones
//   anything else    "Uknown moveType %s"   (the typo is the binary's)
//
// This is what the mission script's Player Move State actions set, and what
// milestone 10 recorded as inert.
// ---------------------------------------------------------------------------
bool Scene::ControllerThink(BaseAiState& s) {
    if (s.controller < 0) return false;
    ControllerState& c = m_controllers[static_cast<size_t>(s.controller)];
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    const double now = static_cast<double>(m_ticks) * 0.07;
    if (e.moving) return true;

    if (c.type == "Deactivate") return false;

    if (c.type == "Waypoints" || c.type == "RandomWaypoints") {
        if (c.route.empty()) {
            // `Controller::ValidateAndBuildWaypointList`'s own complaint.
            return false;
        }
        if (s.waypointResumeAt > now) return true;   // "Pause Time" at the node
        if (s.waypointAt < 0 || s.waypointAt >= static_cast<int32_t>(c.route.size())) {
            s.waypointAt = 0;
        }
        const WaypointNode& w = c.route[static_cast<size_t>(s.waypointAt)];
        const float dx = static_cast<float>(w.world[0] - e.world[0]);
        const float dz = static_cast<float>(w.world[2] - e.world[2]);
        if (dx * dx + dz * dz < 36.0f) {
            // Arrived: run the node's Action and hold for Pause Time.  The
            // shipped actions are None, Look Around, Magic High, Magic Low,
            // Punch and Open Door; only Look Around has an obvious animation
            // and the rest are LOGGED, not played.
            RunWaypointAction(e, w);
            s.waypointResumeAt = now + static_cast<double>(w.pause);
            if (c.type == "RandomWaypoints") {
                s.waypointAt = m_rng.RangeI(0.0f, static_cast<float>(c.route.size()) - 0.001f);
            } else {
                ++s.waypointAt;
                if (s.waypointAt >= static_cast<int32_t>(c.route.size())) s.waypointAt = 0;
            }
            return true;
        }
        return MoveToward(s, static_cast<float>(w.world[0]), static_cast<float>(w.world[2]), false);
    }

    if (c.type == "PerimeterZone") {
        // `BuildGuardMoveTypeWaypointList @0052f230` builds a ring of waypoints
        // round the working zone and walks it.  The ring here is the zone box's
        // four corners, which is the smallest faithful reading of "the
        // perimeter"; the original's node spacing is not recoverable.
        const WorldZone* wz = m_script.Zone(c.workingZone);
        if (wz == nullptr) return false;
        const float corners[4][2] = {{wz->box[0], wz->box[2]},
                                     {wz->box[3], wz->box[2]},
                                     {wz->box[3], wz->box[5]},
                                     {wz->box[0], wz->box[5]}};
        if (s.waypointResumeAt > now) return true;
        const int32_t k = s.waypointAt & 3;
        const float gx = corners[k][0] * kEntityWorldScale;
        const float gz = corners[k][1] * kEntityWorldScale;
        const float dx = gx - static_cast<float>(e.world[0]);
        const float dz = gz - static_cast<float>(e.world[2]);
        if (dx * dx + dz * dz < 64.0f) {
            s.waypointAt = (s.waypointAt + 1) & 3;
            s.waypointResumeAt = now + 1.0;
            return true;
        }
        return MoveToward(s, gx, gz, false);
    }

    if (c.type == "Follow") {
        int32_t target = c.followTarget;
        if (target < 0 && !c.moveTargetTag.empty()) target = FindByTag(c.moveTargetTag);
        if (target < 0 || target == s.entity) return false;
        const LiveEntity& t = m_entities[static_cast<size_t>(target)];
        // `Controller::ComputeFollowOrWaypointStopDistance @0052fdb0` derives
        // the stop distance from the follower's own bounding box; the group
        // spread constant is not in the dump, so ten world units is used and
        // LABELLED.
        const float dx = static_cast<float>(t.world[0] - e.world[0]);
        const float dz = static_cast<float>(t.world[2] - e.world[2]);
        if (dx * dx + dz * dz < 100.0f) return true;
        return MoveToward(s, static_cast<float>(t.world[0]), static_cast<float>(t.world[2]),
                          false);
    }

    if (c.type == "Flee") {
        // `SwapRetreatAndWorkingZones @0052f4b0` literally exchanges the two
        // zones, so a fleeing player's units head for what was the retreat box.
        if (!c.haveRetreatCentre) return false;
        return MoveToward(s, c.retreatCentre[0], c.retreatCentre[2], true);
    }

    // Stationary, and anything unrecognised.  `Rand Move` is what
    // `Controller_IssueMoveOrderAndMaybeRandomizeAnywhereDestination
    // @0052f7c0` reads to decide between "stay put" and "pick somewhere in the
    // zone", so it is the Wander behaviour under a Stationary controller.
    if (c.type != "Stationary") {
        Logf("Uknown moveType %s", c.type.c_str());
        c.type = "Stationary";
    }
    if (c.randMove && s.waypointResumeAt <= now) {
        s.waypointResumeAt = now + kRandMoveHoldSeconds;
        return ControllerRandomMove(s, c) ;
    }
    return false;
}

// ---------------------------------------------------------------------------
// MILESTONE 13.  `BaseAI::ExecuteWaypointAction @005243c0`
// (`bos/game/entity/ai/base_ai_util.cpp`) -- the
// waypoint `Action` strings, which is where an idle NPC's fidget comes from.
// `MoveWanderDemeanourThink @0051c1a0` calls it on EVERY think while the
// node's Pause Time has not run out (`if (World.realtime < BaseAI+0x1f9)
// { ExecuteWaypointAction(); return; }`), so the action repeats for the whole
// pause rather than firing once.
//
//   "Look Around"  two randoms in [-1, 1] on x and z (the pair at
//                  l.7340-7351, `Random__GetRandomValueInFloatRange(rng, -1, 1)`
//                  -- 0xbf80'0000 is -1.0f), normalised, then
//                  BuildOrientationBasisFromForward + UpdateFacing: the actor
//                  turns to a random heading.
//   "Magic Low"    if standing, SetCurrentActionStringMaybe("Magiclow")
//   "Magic High"   if standing, SetCurrentActionStringMaybe("Magichigh")
//                  -- and every character and critter sprite carries
//                  StandMagiclow / StandMagichigh; on TwoHeadedBrahmin.spr
//                  those twelve frames are the head-down graze.
//   "Punch", "Open Door", "Close Door", "None"  -- the rest, logged.
// ---------------------------------------------------------------------------
void Scene::RunWaypointAction(LiveEntity& e, const WaypointNode& w) {
    if (w.action.empty() || w.action == "None") return;
    Logf("%s waypoint %d action '%s'", e.label.c_str(), w.index, w.action.c_str());
    if (w.action == "Look Around") {
        float rx = m_rng.Range(-1.0f, 1.0f), rz = m_rng.Range(-1.0f, 1.0f);
        const float len = std::sqrt(rx * rx + rz * rz);
        if (len > 1e-5f) SetFacingFromVector(e, rx / len, rz / len);
        return;
    }
    if (w.action == "Magic Low" || w.action == "Magic High") {
        if (e.posture != "Stand") return;          // the original's IsStanding gate
        if (!e.action.empty()) return;             // already playing something
        e.action = (w.action == "Magic Low") ? "Magiclow" : "Magichigh";
        if (!ApplyAction(e)) e.action.clear();
        return;
    }
}

// ---------------------------------------------------------------------------
// MILESTONE 13.  `Controller::Controller_IssueMoveOrderAndMaybeRandomizeAnywhereDestination
// @0052f350`.  It issues the ordinary move order
// to every unit in the player slot and then, when the controller's flag at
// +0x278 is set, re-rolls the controller's stored direction:
//
//     toCentre = normalize(zoneCentre - controller.pos)
//     r        = normalize(rand[-0.5, 0.5], 0, rand[-0.5, 0.5])
//     dir      = normalize(2 * toCentre + r)          // stored at +0x26c..0x274
//
// -- a random walk biased two-to-one towards the middle of the working zone,
// which is what keeps a wandering unit inside it.  Two things about the gate:
// the property is `Rand Move` (Controller+0x1c2), whose compile-time default
// `Controller::InitDefaultEntityFields @00527970` sets to FALSE, and the flag
// the wander actually reads is the separate byte at +0x278, which no function
// in any dump writes.  `Rand Move` appears in none of the 74 shipped `.mis`
// files, so no shipped controller wanders -- see NOTES.md section 66.
//
// RECONSTRUCTED: the step length (the original hands the direction to
// `Controller_SetWaypointToTargetUsingVelocity`, whose stride constant is in no
// dump) and the hold between re-rolls.
// ---------------------------------------------------------------------------
bool Scene::ControllerRandomMove(BaseAiState& s, const ControllerState& c) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    float cx = static_cast<float>(e.world[0]), cz = static_cast<float>(e.world[2]);
    float tx = cx, tz = cz;
    if (c.haveCentre) {
        tx = c.centre[0];
        tz = c.centre[2];
    }
    float dx = tx - cx, dz = tz - cz;
    const float dl = std::sqrt(dx * dx + dz * dz);
    if (dl > 1e-5f) {
        dx /= dl;
        dz /= dl;
    } else {
        dx = dz = 0.0f;
    }
    float rx = m_rng.Range(-0.5f, 0.5f), rz = m_rng.Range(-0.5f, 0.5f);
    const float rl = std::sqrt(rx * rx + rz * rz);
    if (rl > 1e-5f) {
        rx /= rl;
        rz /= rl;
    }
    float ox = 2.0f * dx + rx, oz = 2.0f * dz + rz;
    const float ol = std::sqrt(ox * ox + oz * oz);
    if (ol < 1e-5f) return false;
    ox /= ol;
    oz /= ol;
    return MoveToward(s, cx + ox * kRandMoveStep, cz + oz * kRandMoveStep, false);
}

// ---------------------------------------------------------------------------
// The player-side sentry -- `SentryAI::SentryAIMaybeCheckingTargets @0053ef80`
// and `SentryAI::TargetCheckCode1 @0053f480`.  A squad member with no orders
// keeps a list of legal targets inside `max(1600, weaponRange^2)` -- i.e.
// never under 40 world units -- and fires at the nearest.  The three modes are
// keys.cfg's own: `sentryOff` holds fire, `sentryScout` returns fire only,
// `sentryOn` engages anything detected.
// ---------------------------------------------------------------------------
bool Scene::SentryThink(LiveEntity& e) {
    if (e.sentry == kSentryOff) return false;
    if (!e.isCombatant || e.dead || !e.weapon.valid) return false;
    if (e.moving || !e.action.empty()) return false;
    const int32_t self = static_cast<int32_t>(&e - m_entities.data());
    if (m_combatMode != 0 && ActiveTurnEntity() != self) return false;

    const WeaponMode* mode = e.weapon.Mode();
    if (mode != nullptr && e.weapon.magazine > 0 && e.ammo < mode->ammoUsage) {
        if (ReloadWeapon(e)) return true;
    }
    const float reach = std::max(kSentryMinRange, mode != nullptr ? mode->maxRange : 0.0f);
    int32_t best = -1;
    float bestDist = 1e9f;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& t = m_entities[i];
        if (!t.isCombatant || t.dead) continue;
        if (!Hostile(e, t)) continue;
        // Scout mode fires only at whoever has shot at this unit.
        if (e.sentry == kSentryScout && e.lastAttacker != static_cast<int32_t>(i)) continue;
        const float d = BoxGapDistance(e, t);
        if (d > reach) continue;
        DetectTerms dt;
        if (!Detect(self, static_cast<int32_t>(i), &dt) && e.sentry != kSentryScout) continue;
        if (d < bestDist) {
            bestDist = d;
            best = static_cast<int32_t>(i);
        }
    }
    if (best < 0) return false;
    HitTerms terms;
    if (!ComputeHitChance(self, best, &terms) || terms.total <= 0) return false;
    std::string why;
    if (!OrderAttack(self, best, &why)) return false;
    AiBroadcastCombat(self, best);
    return true;
}

// ---------------------------------------------------------------------------
// `BaseAI::DoEngineThink @005128f0`, in its own order:
//
//   thinkrate = highestDesire ? 0.5 : 1.5 + rand[0,1)
//   NextEngineThink = now + thinkrate
//   if (perceive timer expired) { timer = now + 2.0 + rand[0,1.5);
//                                PerceiveAndScoreNearbyEntities() }
//     -- or unconditionally, in turn-based, for a unit that can still act
//   bail out when the actor is not a legal target, has no controller, cannot
//     act, or has no AP left
//   sleeping: stay asleep until something raises a desire
//   UpdateDemeanourNatureMoveSpeed(): sort the ten demeanour records, take the
//     top one, and run its think function
//   in turn-based: think again immediately if nothing was done, else in 0.33 s
// ---------------------------------------------------------------------------
void Scene::AiThink(BaseAiState& s) {
    LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
    const double now = static_cast<double>(m_ticks) * 0.07;

    s.thinkRate = s.highestDesire > 0.0f ? kThinkRateActive
                                        : kThinkRateIdleBase + m_rng.Range(0.0f, 1.0f);
    s.nextEngineThink = now + static_cast<double>(s.thinkRate);

    const bool turnBased = m_combatMode != 0;
    const int32_t self = s.entity;
    if (now >= s.nextPerceive ||
        (turnBased && ActiveTurnEntity() == self)) {
        s.nextPerceive = now + static_cast<double>(kPerceiveInterval) +
                         static_cast<double>(m_rng.Range(0.0f, kPerceiveJitter));
        AiPerceive(s);
    }
    if (e.dead || !e.isCombatant) return;
    if (turnBased && ActiveTurnEntity() != self) return;
    // An animation in flight blocks the think -- except a walk or a run.
    // `BaseAI::BaseAIStopMovementIfHighestDesireSet @00519280` is exactly
    // that exception: with a desire raised it calls `StopMovement(true)` on
    // the attached actor, so a moving AI that spots something stops and acts.
    if (!e.action.empty() && e.action != "Walk" && e.action != "Run") return;

    if (s.sleeping) {
        if (s.highestDesire <= 0.0f) return;
        s.sleeping = false;
        Logf("%s wakes up", e.label.c_str());
    }
    // `DoEngineThink`'s two move-type-priority gates: with no desire raised,
    // an AI whose Controller's `Move Type Priority` is under 6 (under 7 in
    // turn-based) leaves the think entirely and the Controller moves it.
    if (s.highestDesire <= 0.0f) {
        int32_t prio = 0;
        if (s.controller >= 0) prio = m_controllers[static_cast<size_t>(s.controller)].movePriority;
        if (prio < (turnBased ? 7 : 6)) {
            ControllerThink(s);
            // `World::StartNextCombatTurn @00507610` skips anyone who cannot
            // act, so an idle AI must not hold the queue up.
            if (turnBased) EndTurn();
            return;
        }
    }
    // `GetCurrentAP() < 1` -> UpdateVehicleOverwatchAndEvadeBonus, i.e. go on
    // overwatch and stop thinking.  In turn-based that means ending the turn.
    if (e.ap < 1.0f) {
        if (turnBased) EndTurn();
        return;
    }

    // The demeanour.  `UpdateDemeanourNatureMoveSpeed @0051e520` sorts and
    // runs the winner; a changed winner is written to `currentaction` first.
    float acted = 0.0f;
    int32_t top = -1;
    const float desire = AiHighestDesire(s, &top);
    if (desire > 0.0f && top >= 0) {
        if (s.currentDemeanour != top) {
            s.currentDemeanour = top;
            s.currentAction = DemeanourName(top);
            s.announced = false;
        }
        s.highestDesire = desire;
        switch (top) {
            case kAttack: acted = DemeanourAttack(s); break;
            case kMoveFlee: acted = DemeanourFlee(s); break;
            case kMoveCover: acted = DemeanourCover(s); break;
            case kMoveWander: acted = DemeanourWander(s); break;
            case kMiscInvestigate: acted = DemeanourInvestigate(s); break;
            // MiscAssistHeal, MiscEatCorpse, MiscLayTrap, MiscDisarmTrap and
            // MiscTriggerAlarm are recognised and inert: their think functions
            // (`AssistHealDemeanourThink @0051c820`,
            // `ComputeHideAndSeekRepositionScore @0051cc20` and the three
            // stubs at 0051d400 / 0051d480 / 0051d490) all reduce to item and
            // trap verbs fotgame has no model for.
            default: acted = 0.0f; break;
        }
    }
    if (acted <= 0.0f) {
        // Nothing to fight about: the controller's move type takes over, which
        // is the branch `DoEngineThink` gates on
        // `GetControllerMoveTypePriority() < 6`.
        ControllerThink(s);
    }
    if (turnBased) {
        s.nextEngineThink = acted > 0.0f ? now + static_cast<double>(kThinkRateTurnBased) : now;
        if (acted <= 0.0f && !e.moving) EndTurn();
    }
}

// One pass over every BaseAI whose deadline has passed, then the player's own
// sentries.  `World::AdvanceWorldStateOneTick` runs the entity think list once
// per 70 ms tick and each entity's `CheckThinkTimers` gates on its own
// NextEngineThink, which is what this reproduces.
void Scene::AiTick() {
    if (!m_aiEnabled) return;
    const double now = static_cast<double>(m_ticks) * 0.07;
    for (size_t i = 0; i < m_ai.size(); ++i) {
        BaseAiState& s = m_ai[i];
        if (!s.valid || s.entity < 0) continue;
        const LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
        if (e.dead) continue;
        // `DoEngineThink`'s turn-based tail sets `NextEngineThink = realtime`
        // for an entity that has not acted, so the queue's current entry is
        // never held up by its own think deadline.
        const bool active = m_combatMode != 0 && ActiveTurnEntity() == s.entity;
        if (!active && now < s.nextEngineThink) continue;
        AiThink(s);
    }
    // The squad has no BaseAI; it has a SentryAI per member instead.
    for (size_t i = 0; i < m_entities.size(); ++i) {
        LiveEntity& e = m_entities[i];
        if (e.ai >= 0 || !e.isCombatant || e.dead) continue;
        if (m_spawnPlayer >= 0 && e.playerIndex != m_spawnPlayer) continue;
        if (m_ticks < e.aiNextTick) continue;
        e.aiNextTick = m_ticks + 7;   // one sentry sweep every ~0.5 s
        SentryThink(e);
    }
}

void Scene::SetSentry(int32_t index, SentryMode mode) {
    if (index < 0 || index >= static_cast<int32_t>(m_entities.size())) return;
    m_entities[static_cast<size_t>(index)].sentry = mode;
}

// ---------------------------------------------------------------------------
// MILESTONE 18: the item icon -- `WIIBase::Draw @00490b50`.
//
// The original takes the item ENTITY's sprite id (`Entity.e_.unknown2`), looks
// the animation `guibig` / `guismall` up by name in that sprite
// (`FindRecordIndexByName`), reads the record's collection index
// (`GetRecordByIndexOrDefault(idx)+4`), and blits frame 0, direction 0
// UNSCALED, with the entity's own `basecolor` as the tint.  Both animations
// ship in every item sprite -- `Combat Knife.spr` carries `default` (4
// directions, 21 x 12), `guibig` (1 x 1, 126 x 52) and `guismall` (1 x 1,
// 63 x 26) -- so nothing here is a reconstruction except the compositing of
// the four layers, which is milestone 2's own and unchanged.
// ---------------------------------------------------------------------------
bool Scene::ItemIcon(const std::string& spritePath, bool big, const Bitmap** out) {
    *out = nullptr;
    if (spritePath.empty()) return false;
    const std::string key = spritePath + (big ? "|guibig" : "|guismall");
    auto cached = m_iconCache.find(key);
    if (cached != m_iconCache.end()) {
        if (cached->second.w <= 0) return false;
        *out = &cached->second;
        return true;
    }
    Bitmap& bm = m_iconCache[key];          // a zero Bitmap caches the miss
    LoadedSprite* ls = m_sprites->Get(spritePath);
    if (ls == nullptr || !ls->ok) return false;
    const std::string want = big ? "guibig" : "guismall";
    int32_t seq = -1;
    for (size_t i = 0; i < ls->sprite.sequences.size(); ++i) {
        if (Lower(ls->sprite.sequences[i].name) == want) {
            seq = static_cast<int32_t>(i);
            break;
        }
    }
    if (seq < 0) return false;
    const int32_t colIdx = ls->sprite.sequences[static_cast<size_t>(seq)].collectionId;
    if (colIdx < 0 || static_cast<size_t>(colIdx) >= ls->sprite.collections.size()) return false;
    if (!ls->sprite.LoadCollectionImages(ls->file, static_cast<size_t>(colIdx))) return false;
    const Collection& col = ls->sprite.collections[static_cast<size_t>(colIdx)];
    if (col.rects.empty()) return false;
    const Rect& r = col.rects[0];
    const int32_t w = r.Width(), h = r.Height();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    Bitmap tmp;
    tmp.w = w;
    tmp.h = h;
    tmp.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4, 0);
    bool any = false;
    for (int l = 0; l < kLayerCount; ++l) {
        const int32_t idx = col.LayerIndex(0, 0, l);
        if (idx < 0 || static_cast<size_t>(idx) >= col.images.size()) continue;
        const LayerImage& li = col.images[static_cast<size_t>(idx)];
        if (!li.present) continue;
        std::vector<uint8_t> px;
        std::string derr;
        ZarImage& zar = const_cast<ZarImage&>(li.zar);
        if (!zar.DecodeToRGBA(col.palettes[static_cast<size_t>(l)], &px, &derr)) continue;
        const int32_t lw = zar.Width(), lh = zar.Height();
        for (int32_t y = 0; y < lh; ++y) {
            const int32_t dy = y + li.y;
            if (dy < 0 || dy >= h) continue;
            for (int32_t x = 0; x < lw; ++x) {
                const int32_t dx = x + li.x;
                if (dx < 0 || dx >= w) continue;
                const uint8_t* s = &px[(static_cast<size_t>(y) * lw + x) * 4];
                if (s[3] == 0) continue;
                uint8_t* d = &tmp.rgba[(static_cast<size_t>(dy) * w + dx) * 4];
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
                any = true;
            }
        }
    }
    if (!any) return false;
    bm = std::move(tmp);
    *out = &bm;
    return true;
}

namespace {
bool IEq(const std::string& a, const char* b);   // defined below
}  // namespace

// ===========================================================================
// MILESTONE 18 -- talking to people, and the eight skilldex skills.
//
// Both of these are ORDERS in the original: the click handler posts a record
// into the actor's own queue and the executor runs a tick later.  The two
// record builders are next to each other in `.text` and are the same shape:
//
//   `LogRecordFieldChange16 @00631550`  type 0x16 = USE SKILL n ON TARGET
//   `LogRecordFieldChange17 @006315b0`  type 0x17 = TALK TO TARGET
//
// (and 0x03 = move / pick up, 0x04 = use the equipped item on a target,
// 0x0e / 0x0f = the sentry and overwatch modes).  Each carries the same
// "run" bit in the low bit of its last word, computed by the rule in
// section 95.  This port has no order queue, so `OrderTalk` / `UseSkill` do
// what the executor does, with `pendingTalk` / `pendingSkill` as the
// walk-then-do retry that `StepPendingUse` already models for `use`.
// ===========================================================================

bool Scene::IsHostile(int32_t a, int32_t b) const {
    if (a < 0 || b < 0 || static_cast<size_t>(a) >= m_entities.size() ||
        static_cast<size_t>(b) >= m_entities.size()) {
        return false;
    }
    return Hostile(m_entities[static_cast<size_t>(a)], m_entities[static_cast<size_t>(b)]);
}

// `Entity::GetClickSpeechIDString`, entity vtable slot 507.  The stored
// property, unless `ActionSetClickSpeech` has overwritten it this mission.
const std::string& Scene::ClickSpeech(int32_t index) const {
    static const std::string kNone;
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return kNone;
    const auto& over = m_script.ClickSpeechOverrides();
    auto it = over.find(index);
    if (it != over.end()) return it->second;
    return m_entities[static_cast<size_t>(index)].clickSpeech;
}

// `IsEntityValidTalkTarget @004a3070`, transcribed.  The five vtable
// predicates after the click-speech test -- `IsGeneral`, `IsRecruitMaster`,
// `IsQuartermaster`, `CanGambleWith`, `CanBarterWith` -- are all `.ent`
// booleans on entities this port does not carry inside a mission (the bunker's
// quartermaster and recruit master live in the campaign layer, section 57), so
// the click-speech arm is the only one that fires here.  That is not a
// simplification of the rule, it is the shipped data: every talkable in
// Brahmin Wood has a `Click Speech`.
bool Scene::TalkTarget(int32_t index) const {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return false;
    const LiveEntity& t = m_entities[static_cast<size_t>(index)];
    if (!t.isActor || t.hidden) return false;
    if (t.dead) return false;                       // !IsLegalTarget
    if (t.playerIndex == m_spawnPlayer) return false;   // PlayerIndex == 1 arm
    const std::string& sp = ClickSpeech(index);
    return !sp.empty() && !IEq(sp, "None");
}

// `WGameWorld::LogClosestPlayerToPosition @0049aaf0`: among the SELECTED
// members of the local player (flag 0x40), the one with the smallest squared
// 3-D distance to the target, capped at 10000.
int32_t Scene::NearestSelected(int32_t target) const {
    if (target < 0 || static_cast<size_t>(target) >= m_entities.size()) return -1;
    const LiveEntity& t = m_entities[static_cast<size_t>(target)];
    int32_t best = -1;
    float bestD = 10000.0f;
    for (int32_t i : m_sel) {
        if (i < 0 || static_cast<size_t>(i) >= m_entities.size()) continue;
        const LiveEntity& e = m_entities[static_cast<size_t>(i)];
        if (e.dead) continue;
        const float dx = e.wpos[0] - t.wpos[0];
        const float dy = e.wpos[1] - t.wpos[1];
        const float dz = e.wpos[2] - t.wpos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) {
            bestD = d;
            best = i;
        }
    }
    return best;
}

// `WGameWorld::ResolveStealSkillTarget @0049a650`: among the selected members,
// the one with the HIGHEST `Skills::GetSkillValueByName(skill)`, starting from
// -1000 so an unskilled member still qualifies.
int32_t Scene::BestSkilledSelected(const std::string& skill) const {
    const int idx = SkillIndexFromName(skill);
    if (idx < 0) return -1;
    int32_t best = -1, bestV = -1000;
    for (int32_t i : m_sel) {
        if (i < 0 || static_cast<size_t>(i) >= m_entities.size()) continue;
        const LiveEntity& e = m_entities[static_cast<size_t>(i)];
        if (e.dead || !e.isCombatant) continue;
        const int32_t v = e.attr.skills[idx];
        if (v > bestV) {
            bestV = v;
            best = i;
        }
    }
    return best;
}

bool Scene::OrderTalk(int32_t actor, int32_t target, std::string* whyNot) {
    auto fail = [&](const char* w) {
        if (whyNot != nullptr) *whyNot = w;
        return false;
    };
    if (actor < 0 || static_cast<size_t>(actor) >= m_entities.size()) return fail("no actor");
    if (!TalkTarget(target)) return fail("That target is invalid.");
    LiveEntity& a = m_entities[static_cast<size_t>(actor)];
    LiveEntity& t = m_entities[static_cast<size_t>(target)];
    if (a.dead) return fail("That target is invalid.");
    SetFacingFromVector(a, static_cast<float>(t.world[0] - a.world[0]),
                        static_cast<float>(t.world[2] - a.world[2]));
    // REACH.  The order-0x17 executor is not in any dump, so the limit used
    // here is `Actor::CanReachInteractable @0055a840`'s 4.0 m -- the wider of
    // the binary's two, and the one that already gates every other
    // person-to-person interaction.  LABELLED as a reconstruction.
    if (!CanReachToInteract(a, t)) {
        a.pendingTalk = target;
        OrderMove(actor, t.world[0], t.world[2], false);
        if (whyNot != nullptr) *whyNot = "walking over to talk";
        return false;
    }
    a.pendingTalk = -1;
    // The target turns to face the speaker too -- the bubble is over the NPC.
    SetFacingFromVector(t, static_cast<float>(a.world[0] - t.world[0]),
                        static_cast<float>(a.world[2] - t.world[2]));
    const std::string node = ClickSpeech(target);
    m_script.PlaySpeechNode(*this, target, node);
    // `ConditionSpeechOccured` fires on the node's own event names, which
    // `PlaySpeechNode` already raises per line; this is the click itself, so
    // the target's tag goes in too -- that is how the shipped Brahmin Wood
    // triggers ("Charon Spoken To", "Tribals Spoken To After") latch.
    if (!t.tag.empty()) m_script.RaiseSpeechEvent(t.tag);
    Logf("%s talks to %s.", a.ShownName().c_str(), t.ShownName().c_str());
    return true;
}

// ---------------------------------------------------------------------------
// The skills.
// ---------------------------------------------------------------------------
namespace {

// `RollCriticalHitChance @0060fdf0`, the one roll every skill use goes
// through.  `roll = rand() % 100 + 1`; the roll succeeds at or under the
// chance; a success is CRITICAL at or under `chance / 10`, and a failure is
// critical at or under `1 - chance / 10` (Jinxed and Fear the Reaper widen
// that to `clamp(2x, 5, 25)`, neither of which any shipped Brahmin Wood actor
// has).  The decompilation's argument packing for this function is mangled,
// so the two branch thresholds are transcribed and the success test itself is
// the classic percentile compare -- LABELLED.
struct SkillRoll {
    int32_t roll = 0;
    bool success = false;
    bool critical = false;
};
SkillRoll RollSkill(Rng* rng, int32_t chance) {
    SkillRoll r;
    r.roll = rng->Percentile();
    r.success = r.roll <= chance;
    r.critical = r.success ? (r.roll <= chance / 10) : (r.roll <= 1 - chance / 10);
    return r;
}
}  // namespace

// `Actor::PerformSkillUseCheck @00547e90` plus the target's own
// `ApplySkillToTargetEntity`.  The eight names are the table at 0x008a907c
// (`sneak lockpick steal traps firstAid doctor science repair`), which is also
// keys.cfg's `1`..`8` and the skilldex.
bool Scene::UseSkill(int32_t actor, const std::string& skill, int32_t target,
                     std::string* whyNot) {
    std::string msg;
    auto say = [&](const std::string& w) {
        msg = w;
        if (whyNot != nullptr) *whyNot = w;
    };
    if (actor < 0 || static_cast<size_t>(actor) >= m_entities.size()) {
        say("That target is invalid.");
        return false;
    }
    LiveEntity& a = m_entities[static_cast<size_t>(actor)];
    const int skillIdx = SkillIndexFromName(skill);
    if (skillIdx < 0 || a.dead) {
        say("Cannot do that.");
        return false;
    }
    // `Tables::ActionTypes` -- the eight rows are firstAid 10, doctor 15,
    // sneak 2, lockpick 2, steal 2, traps 4, science 2, repair 15.
    const int32_t cost = ActionTypeCost(skill);

    // ----- SNEAK: a MODE with no target ------------------------------------
    // `WGameWorld::EvaluateSneakSkillCondition @0049de10` special-cases the
    // name before anything else: it never sets the skill cursor, it walks the
    // selection and toggles `Entity::IsHidden` on every member at once.  When
    // the selection is MIXED (some hidden, some not) it turns them all on;
    // when it is uniform it flips each.  `desc_sneak` is the rule for what it
    // then costs you: "You cannot run and sneak at the same time."
    if (IEq(skill, "sneak")) {
        if (m_combatMode != 0) {
            if (ActiveTurnEntity() != actor) {
                say("Cannot perform that action.");
                return false;
            }
            if (a.ap < static_cast<float>(cost)) {
                say("Not enough action points.");
                return false;
            }
            a.ap -= static_cast<float>(cost);
        }
        a.sneaking = !a.sneaking;
        if (a.sneaking) a.running = false;      // desc_sneak
        say(a.ShownName() + (a.sneaking ? " is sneaking." : " has been detected."));
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- everything else takes a target ----------------------------------
    if (target < 0 || static_cast<size_t>(target) >= m_entities.size()) {
        say("That target is invalid.");
        return false;
    }
    LiveEntity& t = m_entities[static_cast<size_t>(target)];
    if (t.hidden) {
        say("That target is invalid.");
        return false;
    }

    // `Entity::IsValidTargetForSkill` (vtable), per class.
    const bool isActorT = t.isCombatant;
    const bool isBox = t.isContainer || t.isDoor;
    bool legal = false;
    if (IEq(skill, "lockpick")) legal = isBox;
    else if (IEq(skill, "traps")) legal = isBox || t.isCollectable;
    else if (IEq(skill, "steal")) legal = isActorT && !t.dead;
    else if (IEq(skill, "firstAid") || IEq(skill, "doctor")) {
        legal = isActorT && !t.dead;
    } else if (IEq(skill, "science")) legal = !isActorT;
    else if (IEq(skill, "repair")) legal = !isActorT || t.dead;
    if (!legal) {
        say("That target is invalid.");
        return false;
    }

    SetFacingFromVector(a, static_cast<float>(t.world[0] - a.world[0]),
                        static_cast<float>(t.world[2] - a.world[2]));

    // Reach: `Actor::CheckIfTargetInReachableRange @00557f90`, 1.0 m -- the
    // same test the activate path uses.  Out of reach is a WALK, not a
    // refusal (`PerformSkillUseCheck`'s vtable-0x368 arm), retried by
    // `StepPendingSkill`; `log_skillOutOfReach` is what gives up.
    if (!CanReachToUse(a, t)) {
        a.pendingSkill = skill;
        a.pendingSkillTarget = target;
        OrderMove(actor, t.world[0], t.world[2], false);
        say("walking to " + t.ShownName());
        return false;
    }
    a.pendingSkill.clear();
    a.pendingSkillTarget = -1;

    // `log_healDoesntNeed`.  `Actor::ApplySkillToTargetEntity @00579550` tests
    // this FIRST, before the helper item and before any roll: a target at full
    // hit points does not need healing and nothing is spent on it.
    if ((IEq(skill, "firstAid") || IEq(skill, "doctor")) && t.hp >= t.maxHp &&
        !(IEq(skill, "doctor") && t.knockedDown)) {
        say(t.ShownName() + " does not need healing.");
        return true;
    }

    // The helper item.  `DoesSkillStringMatchHelperItem` over the carried
    // items: a `SkillConsumable` whose `skillType` is this skill.  Only the
    // three healing/repair skills require one -- the three refusals are
    // `log_healRequiresFirstAid`, `log_healRequiresDoctor` and
    // `log_repairRequiresItem`, and there is no such string for any other
    // skill.  Lockpicks are a BONUS instead (`Effect/Skills/lockpick +20`).
    int32_t helper = -1;
    for (size_t i = 0; i < a.inventory.size(); ++i) {
        if (IEq(a.inventory[i].skillType, skill.c_str()) && a.inventory[i].usesLeft > 0) {
            helper = static_cast<int32_t>(i);
            break;
        }
    }
    const bool needsHelper = IEq(skill, "firstAid") || IEq(skill, "doctor") ||
                             IEq(skill, "repair");
    if (needsHelper && helper < 0) {
        say(a.ShownName() + " requires a " +
            (IEq(skill, "repair") ? "repair" : skill) + " item.");
        return false;
    }

    // AP.  `Actor::TryConsumeActionPointsForStance(skill)`; `use_code |= 8`,
    // `log_badAP`.
    if (m_combatMode != 0) {
        if (ActiveTurnEntity() != actor) {
            say("Cannot perform that action.");
            return false;
        }
        if (a.ap < static_cast<float>(cost)) {
            say("Not enough action points.");
            return false;
        }
        a.ap -= static_cast<float>(cost);
    }

    // The chance: the actor's own skill, plus every carried item's
    // `Effect/Skills` bonus for it.
    int32_t chance = a.attr.skills[skillIdx];
    for (const LiveEntity::InvItem& it : a.inventory) {
        chance += it.skillBonus[skillIdx];
    }

    // ----- LOCKPICK: `Container::ApplySkillToTargetEntity @005ad3f0` --------
    if (IEq(skill, "lockpick")) {
        // `chance = actor.lockpick - container.unlockchance` where
        // `unlockchance` is the `.ent`'s own `Chance to Unlock`.
        const int32_t diff = chance - t.unlockChance;
        const SkillRoll r = RollSkill(&m_rng, diff);
        if (!t.isLockable) {
            say("Cannot perform that action.");           // use_code |= 0x20
            return false;
        }
        if (!r.success) {
            // `if (chance < 10) log_lockpickImpossible else log_lockpickFail`
            say(diff < 10 ? a.ShownName() + " failed to pick the lock on the " + t.label +
                                ". It seems very difficult."
                          : a.ShownName() + " failed to pick the lock on the " + t.label + ".");
            Logf("%s", msg.c_str());
            return false;
        }
        if (!t.isLocked) {
            // A successful pick on an UNLOCKED container LOCKS it, and raises
            // its difficulty to `max(10, chance)`.
            t.isLocked = true;
            t.unlockChance = std::max(10, diff);
            say(a.ShownName() + " locked the " + t.label + ".");
        } else {
            t.isLocked = false;
            say(a.ShownName() + " unlocked the " + t.label + ".");
        }
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- FIRST AID / DOCTOR: `Actor::ApplySkillToTargetEntity @00579550` --
    if (IEq(skill, "firstAid") || IEq(skill, "doctor")) {
        const bool doctor = IEq(skill, "doctor");
        const int32_t missing = t.maxHp - t.hp;
        if (missing < 1 && !(doctor && t.knockedDown)) {
            say(t.ShownName() + " does not need healing.");   // log_healDoesntNeed
            return true;
        }
        const SkillRoll r = RollSkill(&m_rng, chance);
        if (!r.success) {
            if (r.critical) {
                // `log_healCriticalFail` = "<location_string> was injured."
                const int32_t hurt = 1 + m_rng.Rand15() % 3;
                t.hp = std::max(1, t.hp - hurt);
                say(t.ShownName() + " was injured.");
            } else {
                say(a.ShownName() + " failed to heal " + t.ShownName() + ".");
            }
            if (helper >= 0) ConsumeHelper(a, helper);
            Logf("%s", msg.c_str());
            return false;
        }
        // MILESTONE 21.  The amount healed IS in the dump, in both arms, and it
        // is a RANGE off the skill, not a flat roll.  `Actor::ApplySkillToTargetEntity@00579550`
        // (first aid) and l.4839 (doctor) build the same base and hand it to
        // `IncreaseAttributeByRange(lo, hi, "hitPoints")`:
        //
        //     base = skill * 35 / 100
        //     first aid:  lo = base * 75 / 100,  hi = base * 125 / 100
        //     doctor:     lo = base,             hi = base * 166 / 100
        //     both:       lo += healer * 4,      hi += healer * 4   (perks.healer)
        //     first aid only: a critical success DOUBLES both ends
        //
        // and the doctor arm's critical success, on a patient with no cripple
        // and no bandage to undo, skips the range entirely and sets hit points
        // to the maximum with `log_healFull`.  Milestone 18 had "first aid 1-5,
        // doctor 4-10, doubled on a critical" here and labelled it a
        // reconstruction; that label can go.
        const int32_t base = chance * 35 / 100;
        int32_t lo = doctor ? base : base * 75 / 100;
        int32_t hi = doctor ? base * 166 / 100 : base * 125 / 100;
        lo += a.attr.healer * 4;
        hi += a.attr.healer * 4;
        if (!doctor && r.critical) { lo *= 2; hi *= 2; }
        if (hi < lo) hi = lo;
        if (doctor && r.critical && !t.knockedDown) {
            // The full-heal branch, l.4903.
            t.hp = t.maxHp;
            say(t.ShownName() + " was fully healed.");              // log_healFull
            if (helper >= 0) ConsumeHelper(a, helper);
            Logf("%s", msg.c_str());
            return true;
        }
        int32_t heal = lo + (hi > lo ? m_rng.Rand15() % (hi - lo + 1) : 0);
        heal = std::min(heal, std::max(0, missing));
        t.hp += heal;
        if (doctor && t.knockedDown) {
            t.knockedDown = false;
            say("Healed all cripples on " + t.ShownName() + ".");   // log_healAllCripples
        } else if (t.hp >= t.maxHp) {
            t.hp = t.maxHp;
            say(t.ShownName() + " was fully healed.");              // log_healFull
        } else {
            say(t.ShownName() + " was healed for " + std::to_string(heal) + " hitpoints.");
        }
        if (helper >= 0) ConsumeHelper(a, helper);
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- REPAIR: `RepairObject::ApplySkillToTargetEntity @005cb220` -------
    if (IEq(skill, "repair")) {
        const SkillRoll r = RollSkill(&m_rng, chance);
        if (helper >= 0) ConsumeHelper(a, helper);
        if (!r.success) {
            say(a.ShownName() + " failed to repair that.");         // log_repairFail
            Logf("%s", msg.c_str());
            return false;
        }
        // A `RepairObject` is a `.ent` class no Brahmin Wood entity has, so
        // there is nothing here to repair and the shipped answer is
        // `log_repairDoesntNeed`.  PLACEHOLDER: the damage model a
        // RepairObject carries (`Vtable521ALLSomethingWithAllFields`) is not
        // reconstructed.
        say(t.ShownName() + " does not need repairing.");
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- SCIENCE: `ScienceSwitch::ApplySkillToTargetEntity @005baac0` -----
    if (IEq(skill, "science")) {
        const bool isConsole = IEq(t.className, "ScienceSwitch") ||
                               IEq(t.className, "Computer");
        if (!isConsole) {
            // `log_thingNeedsScience` / `log_needsScience` are what the game
            // says about something that is not a console.
            say("Looks like it needs a PHD to operate this thing.");
            Logf("%s", msg.c_str());
            return false;
        }
        const SkillRoll r = RollSkill(&m_rng, chance);
        if (!r.success) {
            say(a.ShownName() + " looks stumped by the complexity.");  // log_scienceFailed
            Logf("%s", msg.c_str());
            return false;
        }
        say("Console activated.");                                    // log_usedScience
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- TRAPS: `Trap::ApplySkillToTargetEntity @005cfcf0` ----------------
    if (IEq(skill, "traps")) {
        if (!t.isTrap) {
            say("Cannot trap that.");                                 // log_trapCannot
            Logf("%s", msg.c_str());
            return false;
        }
        if (t.trapDisarmed) {
            say("The " + t.label + " was already disarmed.");          // log_trapAlreadyDisarm
            return true;
        }
        const SkillRoll r = RollSkill(&m_rng, chance);
        if (!r.success) {
            if (r.critical) {
                // `log_trapCriticalFail`: the trap goes off in your hands.
                say(a.ShownName() + " fumbles carelessly and sets off the " + t.label + ".");
                t.trapDisarmed = true;
                Logf("%s", msg.c_str());
                return false;
            }
            say(a.ShownName() + " failed to disarm the " + t.label + ".");
            Logf("%s", msg.c_str());
            return false;
        }
        t.trapDisarmed = true;
        t.hidden = true;          // a disarmed trap leaves the world, as a taken item does
        say(a.ShownName() + " disarmed the " + t.label + ".");         // log_trapDisarm
        Logf("%s", msg.c_str());
        return true;
    }

    // ----- STEAL: `Actor::AttemptSteal @00559920` ---------------------------
    if (IEq(skill, "steal")) {
        if (t.inventory.empty()) {
            say(t.ShownName() + " has nothing to steal.");
            Logf("%s", msg.c_str());
            return false;
        }
        // The contest is the thief's Steal against the mark's own Steal --
        // `Actor::ContestedSkillRollMaybe @005709a0` is the two-sided roll the
        // steal path goes through.  The exact terms of that 108-byte routine
        // are not legible in the dump, so the CONTEST is a RECONSTRUCTION:
        // the thief's chance less the mark's Perception times 5.
        const int32_t contest = chance - t.attr.perception * 5;
        const SkillRoll r = RollSkill(&m_rng, contest);
        if (!r.success) {
            say(a.ShownName() + " was caught stealing.");             // log_stealCaught
            Logf("%s", msg.c_str());
            // Being caught makes the mark hostile, which is the whole point of
            // the message; the AI's own `tookDamage` event is the closest
            // thing this port has to that and is what is raised.
            AiRaiseEvent(target, 0, actor);
            return false;
        }
        LiveEntity::InvItem got = t.inventory.front();
        t.inventory.erase(t.inventory.begin());
        AddToInventory(a, got);
        say(a.ShownName() + " steals " + got.displayName + " from " + t.ShownName() + ".");
        Logf("%s", msg.c_str());
        RefreshAmmoReserves();
        return true;
    }

    say("Cannot do that.");
    return false;
}

// One use of a `SkillConsumable`.  `log_itemUsedUp` = "<thing> was used up."
void Scene::ConsumeHelper(LiveEntity& a, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= a.inventory.size()) return;
    LiveEntity::InvItem& it = a.inventory[static_cast<size_t>(index)];
    if (it.usesLeft > 0) --it.usesLeft;
    if (it.usesLeft > 0) return;
    Logf("%s was used up.", it.displayName.c_str());
    if (it.count > 1) {
        --it.count;
        it.usesLeft = 1;     // the next one in the stack; `Remaining uses` is per item
    } else {
        a.inventory.erase(a.inventory.begin() + index);
    }
}

bool Scene::SetHitPoints(int32_t index, int32_t hp) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return false;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    e.hp = std::max(0, std::min(hp, e.maxHp));
    return true;
}

// The two walk-then-do retries, the same shape as `StepPendingUse`.
void Scene::StepPendingSkill() {
    for (size_t i = 0; i < m_entities.size(); ++i) {
        LiveEntity& a = m_entities[i];
        if (a.pendingTalk >= 0) {
            const int32_t want = a.pendingTalk;
            if (a.dead || static_cast<size_t>(want) >= m_entities.size()) {
                a.pendingTalk = -1;
            } else if (CanReachToInteract(a, m_entities[static_cast<size_t>(want)])) {
                a.pendingTalk = -1;
                a.moving = false;
                a.path.clear();
                std::string why;
                OrderTalk(static_cast<int32_t>(i), want, &why);
            } else if (!a.moving) {
                Logf("%s moved away", m_entities[static_cast<size_t>(want)].ShownName().c_str());
                a.pendingTalk = -1;
            }
        }
        if (a.pendingSkillTarget >= 0) {
            const int32_t want = a.pendingSkillTarget;
            const std::string skill = a.pendingSkill;
            if (a.dead || static_cast<size_t>(want) >= m_entities.size()) {
                a.pendingSkillTarget = -1;
                a.pendingSkill.clear();
            } else if (CanReachToUse(a, m_entities[static_cast<size_t>(want)])) {
                a.pendingSkillTarget = -1;
                a.pendingSkill.clear();
                a.moving = false;
                a.path.clear();
                std::string why;
                UseSkill(static_cast<int32_t>(i), skill, want, &why);
            } else if (!a.moving) {
                // `log_skillOutOfReach` = "<thing> moved away".
                Logf("%s moved away", m_entities[static_cast<size_t>(want)].ShownName().c_str());
                a.pendingSkillTarget = -1;
                a.pendingSkill.clear();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The two script families milestone 10 recorded as inert.
//
// `ActionPlayerMoveState*` sets a player's Controller Type and its move
// target; `ActionPlayerPriority*` sets Aggression / Communicate and the
// KillPlayer / KillUnit target, which is the `Move Target Tag` that
// `ComputeTargetScoreForEntity`'s +10 bonus reads.
// ---------------------------------------------------------------------------
bool Scene::ScriptSetMoveState(int32_t player, const std::string& type,
                               const std::string& targetTag, const std::string& zone) {
    bool any = false;
    for (ControllerState& c : m_controllers) {
        if (c.playerIndex != player) continue;
        if (!type.empty()) c.type = type;
        if (!targetTag.empty()) {
            c.moveTargetTag = targetTag;
            if (type == "Follow") c.followTarget = FindByTag(targetTag);
        }
        if (!zone.empty()) {
            c.workingZone = zone;
            const WorldZone* wz = m_script.Zone(zone);
            if (wz != nullptr) {
                for (int k = 0; k < 3; ++k) {
                    c.centre[k] = (wz->box[k] + wz->box[k + 3]) * 0.5f * kEntityWorldScale;
                }
                c.haveCentre = true;
            }
        }
        if (type == "Flee") {
            // SwapRetreatAndWorkingZones.
            std::swap(c.workingZone, c.retreatZone);
            for (int k = 0; k < 3; ++k) std::swap(c.centre[k], c.retreatCentre[k]);
            std::swap(c.haveCentre, c.haveRetreatCentre);
        }
        any = true;
    }
    // Waking the units is `Controller::ControllerWakeUpUnitList @0052ecb0`.
    for (BaseAiState& s : m_ai) {
        if (s.controller >= 0 && m_controllers[static_cast<size_t>(s.controller)].playerIndex ==
                                     player) {
            s.waypointAt = 0;
            s.waypointResumeAt = -1.0;
            s.nextEngineThink = 0.0;
        }
    }
    if (any) Logf("player %d move state -> %s%s%s", player, type.c_str(),
                  targetTag.empty() ? "" : " target ", targetTag.c_str());
    return any;
}

bool Scene::ScriptSetPriority(int32_t player, const std::string& kind, int32_t value) {
    bool any = false;
    for (ControllerState& c : m_controllers) {
        if (c.playerIndex != player) continue;
        if (kind == "Aggression") c.aggression = value;
        else if (kind == "Communication") c.communicate = value;
        else if (kind == "Mobility") c.mobility = value;
        any = true;
    }
    if (any) Logf("player %d priority %s = %d", player, kind.c_str(), value);
    return any;
}

// ---------------------------------------------------------------------------
std::string Scene::DescribeAI() const {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "aitable: %zu natures, %zu events, %zu demeanours\n"
                  "starting_demeanours:",
                  m_aiTable.natures.size(), m_aiTable.events.size(),
                  m_aiTable.demeanours.size());
    out += buf;
    for (int d = 0; d < kDemeanourCount; ++d) {
        std::snprintf(buf, sizeof(buf), " %s=%d", DemeanourName(d), m_aiTable.starting[d]);
        out += buf;
    }
    out += "\n\n--- controllers ---\n";
    for (const ControllerState& c : m_controllers) {
        std::snprintf(buf, sizeof(buf),
                      "  player %-3d %-16s target='%s' prio=%d agg=%d comm=%d mob=%d "
                      "zone='%s' retreat='%s' group=%d rand=%d never=%d route=%zu units=%zu\n",
                      c.playerIndex, c.type.c_str(), c.moveTargetTag.c_str(), c.movePriority,
                      c.aggression, c.communicate, c.mobility, c.workingZone.c_str(),
                      c.retreatZone.c_str(), c.groupMove ? 1 : 0, c.randMove ? 1 : 0,
                      c.neverLeaveZone ? 1 : 0, c.route.size(), c.units.size());
        out += buf;
        for (const WaypointNode& w : c.route) {
            std::snprintf(buf, sizeof(buf), "      wp %d at %d,%d pause %.1f action '%s'\n",
                          w.index, w.world[0], w.world[2], w.pause, w.action.c_str());
            out += buf;
        }
    }
    out += "\n--- base AI ---\n";
    for (const BaseAiState& s : m_ai) {
        const LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
        int32_t which = -1;
        const float top = AiHighestDesire(s, &which);
        std::snprintf(buf, sizeof(buf),
                      "  %-22s plr %-3d nature %-10s tactic %-8s cover=%d sneak=%d sleep=%d "
                      "PE %d sneakSkill %d -> %s (%.2f)\n",
                      e.label.c_str(), e.playerIndex, s.nature.c_str(), s.tactic.c_str(),
                      s.useCover ? 1 : 0, s.useSneak ? 1 : 0, s.sleeping ? 1 : 0,
                      e.attr.perception, e.attr.skills[kSkillSneak],
                      which >= 0 ? DemeanourName(which) : "-", top);
        out += buf;
    }
    return out;
}

std::string Scene::TraceAI() const {
    std::string out;
    char buf[512];
    for (const BaseAiState& s : m_ai) {
        if (s.entity < 0) continue;
        const LiveEntity& e = m_entities[static_cast<size_t>(s.entity)];
        if (e.dead) continue;
        int32_t which = -1;
        const float top = AiHighestDesire(s, &which);
        std::snprintf(buf, sizeof(buf),
                      "AI tick=%lld %-22s nature=%-10s tactic=%-8s demeanour=%-10s (%.2f) "
                      "target=%s vis=%zu hp=%d ap=%.1f wp=%d moving=%d\n",
                      static_cast<long long>(m_ticks), e.label.c_str(), s.nature.c_str(),
                      s.tactic.c_str(), which >= 0 ? DemeanourName(which) : "-", top,
                      s.currentTarget >= 0
                          ? m_entities[static_cast<size_t>(s.currentTarget)].label.c_str()
                          : "-",
                      s.visList.size(), e.hp, e.ap, s.waypointAt, e.moving ? 1 : 0);
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------
void Scene::Logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log.emplace_back(buf);
    if (m_log.size() > 256) m_log.erase(m_log.begin());
}

std::string Scene::TraceCombat() const {
    std::string out;
    char buf[512];
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        if (!e.isCombatant) continue;
        std::snprintf(buf, sizeof(buf),
                      "COMBAT tick=%lld i=%zu %-22s player=%d hp=%d/%d ap=%.1f/%d ac=%d "
                      "weapon=%s mode=%s ammo=%d+%d at=%d,%d %s%s\n",
                      static_cast<long long>(m_ticks), i, e.label.c_str(), e.playerIndex, e.hp,
                      e.maxHp, e.ap, e.maxAp, e.attr.armorClass,
                      e.weapon.valid ? e.weapon.displayName.c_str() : "-",
                      (e.weapon.Mode() != nullptr && !e.weapon.Mode()->modeName.empty())
                          ? e.weapon.Mode()->modeName.c_str()
                          : "-",
                      e.ammo, e.ammoReserve, e.world[0], e.world[2], e.dead ? "DEAD " : "",
                      e.sequenceName.c_str());
        out += buf;
    }
    return out;
}

// ---------------------------------------------------------------------------
// MILESTONE 9: the interface's model of the equipment slots.
//
// The original's slot model is `Actor.ct_.slot_equip[0..2]` -- hand 1, hand 2,
// armour; those are the only three indices used anywhere (`Actor::Actor_HandleGoToCommandOrQueueAttack@00546100` and
// `Actor::UpdateSprite@00540e50`, the last of which reads `slot_equip[2]` immediately before `GetSexString` to
// build the actor's sprite path -- i.e. slot 2 is the armour and it is what
// drives the paperdoll).  The verbs that move an item between them are vtable
// slots 323 / 324 (`EquipItemFromInventoryToSlot`,
// `UnequipItemFromSlotAndPutInInventory`) and neither body is in any dump, and
// neither is `Inventory::Add`.  So the transfer itself is a RECONSTRUCTION.
//
// What is NOT reconstructed is the legality test's SHAPE:
// `WIIHand::HandleEquipSlotItemInteraction @0046f940` resolves a drop entirely
// on the receiving side, with no size grid and no type bitmask, by asking the
// target `Entity::CanReachInteractable` / `Entity::CanUseItem` for a
// `UseItemResponse` and refusing when `use_code != 0` -- and it special-cases
// exactly one thing before that, `if (item->IsArmour() && droppedOnThisPanel)
// reject`.  The rule below is that one special case plus the only other
// constraint the shipped classes make unavoidable.
// ---------------------------------------------------------------------------
namespace {

bool SlotAcceptsClass(int32_t slot, const std::string& cls) {
    if (slot == 2) return cls == "Armour";       // the IsArmour() special case
    if (slot == 0 || slot == 1) return cls == "Weapon";
    return true;                                  // the inventory list, and the ground
}

}  // namespace

// ---------------------------------------------------------------------------
// MILESTONE 12: vehicles.
//
// `Vehicle::Move @0057f080` is an
// acceleration/velocity integrator, not the actors' animation-driven walk: it
// takes the vehicle's own forward basis vector (`Entity.pad2d[3..5]`), scales
// it by `Vehicle.ct_.acceleration`, negates it when `ct_.inreverse`, adds
// gravity `_DAT_0080ca58` on the y axis when the collision test fails, and
// integrates with the timestep `_DAT_0080ca54`.  Both of those data globals
// are unresolved, so the integration below uses the scene's own 70 ms tick and
// clamps to the `.ent`'s own "Max Speed" -- the two properties that ARE
// recoverable.  A metre is four world units (mapview/NOTES.md 14).
// ---------------------------------------------------------------------------
// Attach every standalone `VehicleWeapon` to the nearest `Vehicle`.
void Scene::AttachTurrets() {
    for (LiveEntity& t : m_entities) {
        if (!t.isTurret || !t.vehicleWeapon.valid) continue;
        int32_t best = -1;
        int64_t bestD = 64 * 64;
        for (size_t i = 0; i < m_entities.size(); ++i) {
            if (!m_entities[i].isVehicle) continue;
            const int64_t dx = m_entities[i].world[0] - t.world[0];
            const int64_t dz = m_entities[i].world[2] - t.world[2];
            const int64_t d = dx * dx + dz * dz;
            if (d < bestD) {
                bestD = d;
                best = static_cast<int32_t>(i);
            }
        }
        if (best >= 0 && !m_entities[static_cast<size_t>(best)].vehicleWeapon.valid) {
            m_entities[static_cast<size_t>(best)].vehicleWeapon = t.vehicleWeapon;
            m_entities[static_cast<size_t>(best)].gunnerEnabled = true;
        }
    }
}

std::vector<int32_t> Scene::Vehicles() const {
    std::vector<int32_t> out;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].isVehicle) out.push_back(static_cast<int32_t>(i));
    }
    return out;
}

bool Scene::VehicleMount(int32_t actor, int32_t vehicle, std::string* whyNot) {
    auto fail = [&](const char* w) {
        if (whyNot) *whyNot = w;
        return false;
    };
    if (actor < 0 || static_cast<size_t>(actor) >= m_entities.size()) return fail("no actor");
    if (vehicle < 0 || static_cast<size_t>(vehicle) >= m_entities.size()) return fail("no vehicle");
    LiveEntity& a = m_entities[static_cast<size_t>(actor)];
    LiveEntity& v = m_entities[static_cast<size_t>(vehicle)];
    if (!v.isVehicle) return fail("not a vehicle");
    if (a.dead) return fail("dead");
    if (a.inVehicle >= 0) return fail("already aboard");
    // `Vehicle::IsPassengerCapacityReached @00584f60`: the seat count is the
    // `.ent`'s "Num Passangers".
    const int32_t taken = (v.driver >= 0 ? 1 : 0) + (v.gunner >= 0 ? 1 : 0) +
                          static_cast<int32_t>(v.passengers.size());
    if (taken >= std::max(1, v.seats)) return fail("the vehicle is full");
    if (v.driver < 0) {
        v.driver = actor;
    } else if (v.gunnerEnabled && v.gunner < 0) {
        v.gunner = actor;
    } else {
        v.passengers.push_back(actor);
    }
    a.inVehicle = vehicle;
    a.moving = false;
    a.path.clear();
    Logf("%s boards %s (%s)", a.label.c_str(), v.label.c_str(),
         v.driver == actor ? "driver" : (v.gunner == actor ? "gunner" : "passenger"));
    return true;
}

bool Scene::VehicleDismount(int32_t actor, std::string* whyNot) {
    if (actor < 0 || static_cast<size_t>(actor) >= m_entities.size()) {
        if (whyNot) *whyNot = "no actor";
        return false;
    }
    LiveEntity& a = m_entities[static_cast<size_t>(actor)];
    if (a.inVehicle < 0) {
        if (whyNot) *whyNot = "not aboard";
        return false;
    }
    LiveEntity& v = m_entities[static_cast<size_t>(a.inVehicle)];
    // `Vehicle::VehiclePassengerDisembarkAllowed @00587ef0` searches the box
    // around the vehicle for a free cell; the search bounds come from
    // `Vehicle::ComputeDisembarkSearchBounds @005884f0`.  Here the first
    // walkable navigation cell within three tiles wins.
    int32_t bestX = v.world[0], bestZ = v.world[2];
    bool found = false;
    for (int32_t r = 4; r <= 12 && !found; r += 4) {
        for (int32_t dz = -r; dz <= r && !found; dz += 3) {
            for (int32_t dx = -r; dx <= r && !found; dx += 3) {
                const int32_t cx = m_nav.CellX(v.world[0] + dx);
                const int32_t cz = m_nav.CellZ(v.world[2] + dz);
                if (!m_nav.Open(cx, cz)) continue;
                bestX = v.world[0] + dx;
                bestZ = v.world[2] + dz;
                found = true;
            }
        }
    }
    if (v.driver == actor) v.driver = -1;
    if (v.gunner == actor) v.gunner = -1;
    v.passengers.erase(std::remove(v.passengers.begin(), v.passengers.end(), actor),
                       v.passengers.end());
    a.inVehicle = -1;
    PlaceEntity(actor, bestX, bestZ);
    Logf("%s leaves %s at world (%d, %d)", a.label.c_str(), v.label.c_str(), bestX, bestZ);
    return true;
}

bool Scene::VehicleDrive(int32_t vehicle, int32_t worldX, int32_t worldZ, std::string* whyNot) {
    if (vehicle < 0 || static_cast<size_t>(vehicle) >= m_entities.size()) {
        if (whyNot) *whyNot = "no vehicle";
        return false;
    }
    LiveEntity& v = m_entities[static_cast<size_t>(vehicle)];
    if (!v.isVehicle) {
        if (whyNot) *whyNot = "not a vehicle";
        return false;
    }
    // `Vehicle::Move` bails at once when the driver slot is empty or the driver
    // is unconscious (l.6345-6371).
    if (v.driver < 0) {
        if (whyNot) *whyNot = "no driver";
        return false;
    }
    v.vdest[0] = static_cast<float>(worldX);
    v.vdest[1] = static_cast<float>(v.world[1]);
    v.vdest[2] = static_cast<float>(worldZ);
    v.destReached = false;
    Logf("%s drives to world (%d, %d)", v.label.c_str(), worldX, worldZ);
    return true;
}

bool Scene::VehicleFire(int32_t vehicle, int32_t target, std::string* whyNot) {
    if (vehicle < 0 || static_cast<size_t>(vehicle) >= m_entities.size()) {
        if (whyNot) *whyNot = "no vehicle";
        return false;
    }
    LiveEntity& v = m_entities[static_cast<size_t>(vehicle)];
    if (!v.vehicleWeapon.valid) {
        if (whyNot) *whyNot = "this vehicle has no main weapon";
        return false;
    }
    if (v.gunner < 0) {
        if (whyNot) *whyNot = "no gunner";
        return false;
    }
    // `VehicleWeapon::BeginAttackAnEntity @005d64f0` fires the turret; the
    // resolution is `VehicleWeapon::ResolveCombatMessageForAttacker @005d70c0`,
    // which is the same combat message the actors use.  Here the gunner takes
    // the shot with the turret in hand, which is what the damage numbers say
    // it amounts to.
    LiveEntity& g2 = m_entities[static_cast<size_t>(v.gunner)];
    const WeaponDef saved = g2.weapon;
    g2.weapon = v.vehicleWeapon;
    const bool ok = OrderAttack(v.gunner, target, whyNot);
    if (!ok) g2.weapon = saved;
    return ok;
}

// One tick of `Vehicle::Move`.
bool Scene::VehicleStep(LiveEntity& v) {
    if (v.destReached || v.driver < 0) return false;
    const float dx = v.vdest[0] - v.wpos[0] * kEntityWorldScale;
    const float dz = v.vdest[2] - v.wpos[2] * kEntityWorldScale;
    const float dist = std::sqrt(dx * dx + dz * dz);
    // "Max Speed" is metres per second; a metre is four world units and a tick
    // is 70 ms (NOTES.md 2, 14).
    const float top = (v.maxSpeed > 0 ? v.maxSpeed : 2.0f) * 4.0f * kTickMs / 1000.0f;
    const float a = (v.accel > 0 ? v.accel : 1.0f) * 4.0f * kTickMs / 1000.0f;
    float speed = std::sqrt(v.vel[0] * v.vel[0] + v.vel[2] * v.vel[2]);
    speed = std::min(top, speed + a);
    if (dist <= speed || dist <= 1.0f) {
        int32_t nx = static_cast<int32_t>(v.vdest[0]), nz = static_cast<int32_t>(v.vdest[2]);
        PlaceEntity(static_cast<int32_t>(&v - m_entities.data()), nx, nz);
        v.destReached = true;
        v.vel[0] = v.vel[2] = 0;
        Logf("%s arrives at world (%d, %d)", v.label.c_str(), nx, nz);
    } else {
        v.vel[0] = dx / dist * speed;
        v.vel[2] = dz / dist * speed;
        const int32_t nx = v.world[0] + static_cast<int32_t>(v.vel[0]);
        const int32_t nz = v.world[2] + static_cast<int32_t>(v.vel[2]);
        PlaceEntity(static_cast<int32_t>(&v - m_entities.data()), nx, nz);
        SetFacingFromVector(v, v.vel[0], v.vel[2]);
    }
    // The occupants ride along.
    const int32_t self = static_cast<int32_t>(&v - m_entities.data());
    for (LiveEntity& p : m_entities) {
        if (p.inVehicle == self) {
            for (int k = 0; k < 3; ++k) p.world[k] = v.world[k];
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// MILESTONE 17: entity references, containers, corpses and ground items.
// ---------------------------------------------------------------------------
namespace {

std::string Fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return std::string(buf);
}

bool IEq(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return i == a.size() && b[i] == '\0';
}

}  // namespace

//
// A `.mis` stores an entity's sub-objects as SEPARATE SLOTS in the
// `<entity_file>` table and points at them with a `kEshEntityRef` (type 12)
// property whose four bytes are `u16 slot ; u16 sequenceNumber` -- the same
// `EntityID { id ; id_seqnum }` pair every decompiled function compares against
// `WorldGlobal->entitystart[id].sequence_num`.  A zero slot is the null entity.
// Verified on `campaigns/missions/core/mission01.mis`: the chest at slot 46
// carries `Inventory = ab01 1a00` -> slot 0x1ab = 427, which IS an `Inventory`
// whose slot record's second i16 is 0x1a = 26, and whose `ILink0 = 0502 0800`
// -> slot 517, an `afterBurner` Consumable with sequence number 8.
const EntityInstance* Scene::ResolveRef(const Esh& owner, const std::string& prop) const {
    const EshProp* p = owner.Find(prop);
    if (p == nullptr || p->type != kEshEntityRef || p->payload.size() < 4) return nullptr;
    const uint32_t slot = static_cast<uint32_t>(p->payload[0]) |
                          (static_cast<uint32_t>(p->payload[1]) << 8);
    const uint32_t seq = static_cast<uint32_t>(p->payload[2]) |
                         (static_cast<uint32_t>(p->payload[3]) << 8);
    if (slot == 0) return nullptr;
    const auto it = m_slotIndex.find(static_cast<int32_t>(slot));
    if (it == m_slotIndex.end()) return nullptr;
    const EntityInstance& e = m_world.entityFile.entities[it->second];
    // The sequence-number check every `GetEntityByID` does.  A mismatch is the
    // original's "stale handle" and resolves to the null entity.
    if (static_cast<uint32_t>(e.idB) != seq) return nullptr;
    return &e;
}

void Scene::ReadItemInstance(const EntityInstance& inst, LiveEntity::InvItem* out) const {
    out->cls = inst.className;
    out->entPath = inst.entPath;
    inst.esh.GetString("Display Name", &out->displayName);
    inst.esh.GetString("Item Type", &out->itemType);
    inst.esh.GetString("Tag Name", &out->tagName);
    inst.esh.GetString("Sprite", &out->spritePath);
    inst.esh.GetString("skillType", &out->skillType);       // MILESTONE 18
    inst.esh.GetInt("Remaining uses", &out->usesLeft);
    inst.esh.GetInt("Count", &out->count);
    inst.esh.GetFloat("Weight", &out->weight);
    // The prototype supplies everything the instance left at its default --
    // the same override rule `ApplyEntOverride` runs for the drawing
    // properties (`EntityStreamWriteInteger@005f7890`).
    std::string perr;
    const EntProto* proto = m_protos->Get(inst.entPath, &perr);
    const Esh* proto_esh = proto != nullptr ? &proto->block.esh : nullptr;
    if (proto_esh != nullptr) {
        if (out->displayName.empty()) proto_esh->GetString("Display Name", &out->displayName);
        if (out->itemType.empty()) proto_esh->GetString("Item Type", &out->itemType);
        if (out->spritePath.empty()) proto_esh->GetString("Sprite", &out->spritePath);
        if (out->skillType.empty()) proto_esh->GetString("skillType", &out->skillType);
        if (out->usesLeft <= 0) proto_esh->GetInt("Remaining uses", &out->usesLeft);
        if (out->weight == 0.0f) proto_esh->GetFloat("Weight", &out->weight);
        if (out->count <= 0) proto_esh->GetInt("Count", &out->count);
    }
    if (out->count <= 0) out->count = 1;
    // MILESTONE 18: the `Effect/Skills` bonus (lockPicks +20 lockpick, ...).
    {
        Esh effect;
        std::string eerr;
        Attributes bonus;
        const Esh* from = nullptr;
        if (inst.esh.GetNested("Effect", &effect, &eerr)) from = &effect;
        else if (proto_esh != nullptr && proto_esh->GetNested("Effect", &effect, &eerr)) {
            from = &effect;
        }
        if (from != nullptr) {
            ReadAttributes(*from, &bonus, false);
            for (int i = 0; i < kSkillCount; ++i) out->skillBonus[i] = bonus.skills[i];
        }
    }
    if (out->displayName.empty()) out->displayName = out->cls;
    // A weapon or armour carries its whole definition with it so a round trip
    // through a pack, a container or a corpse is lossless.  The instance's own
    // block wins; the prototype fills the rest in.
    if (out->cls == "Weapon") {
        if (!ReadWeapon(inst.esh, &out->weapon) && proto_esh != nullptr) {
            ReadWeapon(*proto_esh, &out->weapon);
        }
        out->weapon.path = inst.entPath;
        if (out->weapon.displayName.empty()) out->weapon.displayName = out->displayName;
    } else if (out->cls == "Armour" || out->cls == "Armor") {
        if (!ReadArmour(inst.esh, &out->armour) && proto_esh != nullptr) {
            ReadArmour(*proto_esh, &out->armour);
        }
        out->armour.path = inst.entPath;
        if (out->armour.displayName.empty()) out->armour.displayName = out->displayName;
    }
}

void Scene::ReadInventoryList(const Esh& inv, std::vector<LiveEntity::InvItem>* out) const {
    int32_t n = 0;
    inv.GetInt("IListSz", &n);
    for (int32_t i = 0; i < n && i < 256; ++i) {
        char key[24];
        std::snprintf(key, sizeof(key), "ILink%d", i);
        const EntityInstance* item = ResolveRef(inv, key);
        if (item == nullptr) continue;
        LiveEntity::InvItem it;
        ReadItemInstance(*item, &it);
        // `Inventory::Inventory_BuildSerializedItemPathCountList @005b4b20`
        // emits `(path, count, nonLootableCount)`, and the owning list mirrors
        // the stack size in `ILinkCnt<n>`; when the item entity states no
        // Count of its own that mirror is the count.
        int32_t mirrored = 0;
        std::snprintf(key, sizeof(key), "ILinkCnt%d", i);
        if (inv.GetInt(key, &mirrored) && mirrored > 0) it.count = mirrored;
        out->push_back(it);
    }
}

// ---------------------------------------------------------------------------
// The unarmed tables.
// ---------------------------------------------------------------------------
// `GameData::GameData__GetRaceUnarmedTableEntry @0061f0f0` takes the entity's
// `Entity Character Type` (the `.ent`'s "Race Type"), finds its index in the
// race table, then walks the run of unarmed rows carrying that race index and
// keeps the LAST one whose four minima -- level, unarmed skill, strength,
// agility (row offsets +8, +0xc, +0x10, +0x14) -- are all met.  The row's
// second field indexes `punch_entities` / `kick_entities`, and
// `Actor::LevelUpUnarmedAttacks @0054f4e0` turns that name into
// `entities/weapons/Punches/<name>.ent` (or `Kicks/`) and stores the result in
// `Actor.ct_.unarmed_left` / `unarmed_right`.
//
// All four tables and the race list are `tables/races.txt`, shipped
// uncompressed, and are read verbatim here.
namespace {

// races.txt is brace-structured: `name = { ... }`, rows `{ a, b, c }`, strings
// in braces, `//` comments.
std::string StripComments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool comment = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!comment && s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') comment = true;
        if (s[i] == '\n') comment = false;
        out.push_back(comment ? ' ' : s[i]);
    }
    return out;
}

// The body of `<name> = { ... }`, brace-balanced.
bool BraceBlock(const std::string& text, const std::string& name, std::string* out) {
    size_t at = 0;
    while ((at = text.find(name, at)) != std::string::npos) {
        size_t p = at + name.size();
        // Only a whole-word match followed by '='.
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) ||
                       text[at - 1] == '_')) {
            ++at;
            continue;
        }
        while (p < text.size() && std::isspace(static_cast<unsigned char>(text[p]))) ++p;
        if (p >= text.size() || text[p] != '=') {
            ++at;
            continue;
        }
        ++p;
        while (p < text.size() && text[p] != '{') ++p;
        if (p >= text.size()) return false;
        int depth = 0;
        const size_t start = p + 1;
        for (; p < text.size(); ++p) {
            if (text[p] == '{') ++depth;
            else if (text[p] == '}' && --depth == 0) {
                *out = text.substr(start, p - start);
                return true;
            }
        }
        return false;
    }
    return false;
}

// Every `{...}` group in a block, as its raw body.
std::vector<std::string> BraceRows(const std::string& body) {
    std::vector<std::string> rows;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '{') {
            if (depth++ == 0) start = i + 1;
        } else if (body[i] == '}') {
            if (--depth == 0) rows.push_back(body.substr(start, i - start));
        }
    }
    return rows;
}

std::vector<std::string> QuotedStrings(const std::string& body) {
    std::vector<std::string> out;
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string::npos) {
        const size_t e = body.find('"', i + 1);
        if (e == std::string::npos) break;
        out.push_back(body.substr(i + 1, e - i - 1));
        i = e + 1;
    }
    return out;
}

std::vector<int32_t> Ints(const std::string& s) {
    std::vector<int32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) ||
            (s[i] == '-' && i + 1 < s.size() &&
             std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            out.push_back(static_cast<int32_t>(std::strtol(s.c_str() + i, nullptr, 10)));
            if (s[i] == '-') ++i;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace

bool Scene::LoadRaceTables() {
    if (!m_punchTable.empty()) return true;
    // `tables/races.txt` ships uncompressed beside `tables/aitable.txt`, which
    // `LoadAI` resolves the same way.
    const std::string full =
        (m_opt.entDir.empty() ? std::string(".") : m_opt.entDir) + "/tables/races.txt";
    std::vector<uint8_t> bytes;
    std::string rerr;
    if (!ft::ReadWholeFile(full, &bytes, &rerr)) {
        std::fprintf(stderr, "  races.txt: %s (no unarmed attacks)\n", rerr.c_str());
        return false;
    }
    const std::string text =
        StripComments(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    std::string body;
    if (BraceBlock(text, "races", &body)) {
        // `{ {Race}, {PhysicalType}, {TraitType}, }` -- the first braced word of
        // each row is the race name and the row's ordinal is its RaceIndex.
        for (const std::string& row : BraceRows(body)) {
            const size_t o = row.find('{');
            const size_t c = row.find('}');
            if (o == std::string::npos || c == std::string::npos || c < o) continue;
            m_raceNames.push_back(row.substr(o + 1, c - o - 1));
        }
    }
    if (BraceBlock(text, "punch_entities", &body)) m_punchEnts = QuotedStrings(body);
    if (BraceBlock(text, "kick_entities", &body)) m_kickEnts = QuotedStrings(body);
    auto rows = [&](const char* name, std::vector<UnarmedRow>* out) {
        std::string b;
        if (!BraceBlock(text, name, &b)) return;
        for (const std::string& row : BraceRows(b)) {
            const std::vector<int32_t> v = Ints(row);
            if (v.size() < 6) continue;
            UnarmedRow r;
            r.race = v[0];
            r.entity = v[1];
            r.level = v[2];
            r.unarmed = v[3];
            r.strength = v[4];
            r.agility = v[5];
            out->push_back(r);
        }
    };
    rows("punch_table", &m_punchTable);
    rows("kick_table", &m_kickTable);
    return !m_punchTable.empty();
}

std::string Scene::UnarmedEntityFor(const std::string& race, const Attributes& a, int32_t level,
                                    bool kick) const {
    const std::vector<UnarmedRow>& table = kick ? m_kickTable : m_punchTable;
    const std::vector<std::string>& names = kick ? m_kickEnts : m_punchEnts;
    if (table.empty() || names.empty()) return std::string();
    int32_t raceIndex = -1;
    for (size_t i = 0; i < m_raceNames.size(); ++i) {
        if (IEq(race, m_raceNames[i].c_str())) {
            raceIndex = static_cast<int32_t>(i);
            break;
        }
    }
    // An actor whose `.ent` states no `Race Type` takes the property's
    // compile-time default, which is not in any dump -- but the shipped data
    // settles it: of the 70 `entities/recruits/*.ent`, the 34 that are ghouls,
    // deathclaws, raiders, reavers, tribals, mutants, robots and the pipboy all
    // NAME their race, and the 36 that are ordinary Brotherhood humans -- along
    // with every `entities/Actors/BOS/*.ent` -- name none.  `races.txt` lists
    // BOS as RaceIndex 0.  So the default is index 0, and without it a recruit
    // has no punch and no kick, which is exactly the "no weapons, cannot attack
    // back" the player reported.
    if (raceIndex < 0 && race.empty()) raceIndex = 0;
    // "No race set for entity %s" / "Unable to find race %s in race table" are
    // the original's own two errors here; both return -1.
    if (raceIndex < 0) return std::string();
    // The equal_range over the race column, then the last row whose four
    // minima are met.  The original starts at the run's first row, so a race
    // whose first row is already unmet still yields that first row.
    int32_t best = -1;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].race != raceIndex) continue;
        if (best < 0) best = static_cast<int32_t>(i);
        if (table[i].level <= level && table[i].unarmed <= a.skills[kSkillUnarmed] &&
            table[i].strength <= a.strength && table[i].agility <= a.agility) {
            best = static_cast<int32_t>(i);
        }
    }
    if (best < 0) return std::string();
    const int32_t idx = table[static_cast<size_t>(best)].entity;
    if (idx < 0 || static_cast<size_t>(idx) >= names.size()) return std::string();
    return std::string("entities/weapons/") + (kick ? "Kicks/" : "Punches/") +
           names[static_cast<size_t>(idx)] + ".ent";
}

void Scene::EquipUnarmed(LiveEntity& e, const std::string& race) {
    auto load = [&](bool kick, WeaponDef* out) {
        if (out->valid) return;   // the `.ent` already carried one
        const std::string path = UnarmedEntityFor(race, e.attr, e.level, kick);
        if (path.empty()) return;
        std::string perr;
        const EntProto* proto = m_protos->Get(path, &perr);
        if (proto == nullptr) return;
        if (!ReadWeapon(proto->block.esh, out)) return;
        out->path = path;
        if (out->displayName.empty()) out->displayName = kick ? "kick" : "punch";
    };
    load(false, &e.unarmedPunch);
    load(true, &e.unarmedKick);
    // `Actor::GetIDOfEquippedItemInActiveSlot` hands the fire path whatever is
    // in the active slot; with the hand empty that is the unarmed weapon, and
    // `Weapon::IsUnarmedAttack @005d7a40` is what every later branch keys on.
    // Hand 1 gets the punch and hand 2 the kick, which is the order
    // `LevelUpUnarmedAttacks` fills `unarmed_left` and `unarmed_right` in.
    if (!e.weapon.valid && e.unarmedPunch.valid) {
        e.weapon = e.unarmedPunch;
        e.unarmedHand[0] = true;
    }
    if (!e.weapon2.valid && e.unarmedKick.valid) {
        e.weapon2 = e.unarmedKick;
        e.unarmedHand[1] = true;
    }
}

// ---------------------------------------------------------------------------
// Reach, use, and the loot window.
// ---------------------------------------------------------------------------
// `Actor::CheckIfTargetInReachableRange @00557f90` and
// `Actor::CanReachInteractable @0055a840` both measure the same quantity: the
// straight-line distance between the two entities' positions, less the summed
// x+z extents of their two bounding boxes times `_DAT_008bc660 * _DAT_007fd598`
// (0.5 * 0.25 -- `BoxGapDistance`), floored at 0.001 m.  They differ only in
// the limit: `DAT_00824d04` == 1.0 m for the first and `_DAT_00824d08` == 4.0 m
// for the second, both read out of `.rdata`.
//
// DEVIATION, and it is the only one on this path.  The binary's distance
// includes the Y term, because in the original both entities' `pos_y` come from
// the same place.  Here they do not: a map entity's elevation is its own stored
// `frame` translation, while an ACTOR's comes from milestone 7's navigation
// grid, whose surface is the top of the tallest static tile over the cell
// (section 12).  Those are different quantities, and the shipped data shows how
// far apart: every one of Brahmin Wood's twelve containers stores y = 128 while
// the floor the actor stands on beside it reads 139-142, a 3.2 m difference
// that would put every container in the game permanently out of reach.  The
// gap used for reach is therefore measured in the X/Z plane only.  It is
// exactly the binary's formula with the Y term dropped, and it is the
// navigation grid that should be corrected, not this test.
float Scene::ReachGap(const LiveEntity& a, const LiveEntity& b) const {
    const float dx = b.wpos[0] - a.wpos[0];
    const float dz = b.wpos[2] - a.wpos[2];
    const float len = std::sqrt(dx * dx + dz * dz);
    const float extentsA = static_cast<float>(a.bbox[0] + a.bbox[2]);
    const float extentsB = static_cast<float>(b.bbox[0] + b.bbox[2]);
    const float gap = len - (extentsA + extentsB) * k::kHalf * k::kMetresPerUnit;
    return gap < kGapFloor ? kGapFloor : gap;
}

bool Scene::CanReachToUse(const LiveEntity& a, const LiveEntity& t) const {
    // The 2D-bounding-box guard the original opens with is a "has this entity
    // been placed" test and every live entity here has been.
    return ReachGap(a, t) <= 1.0f;
}

bool Scene::CanReachToInteract(const LiveEntity& a, const LiveEntity& t) const {
    // `Actor::CanReachInteractable @0055a840`: the same gap against
    // `_DAT_00824d08` == 4.0 m, plus the line-of-sight test `FUN_005e31e0`,
    // which is milestone 8's `CountLineBlockers` here.
    if (ReachGap(a, t) > 4.0f) return false;
    bool blocked = false;
    CountLineBlockers(a, t, &blocked);
    return !blocked;
}

float Scene::CarriedWeight(const LiveEntity& e) const {
    // `Actor::GetWeight @005550c0` = `Inventory::GetWeight` + the three
    // `slot_equip` entries.  `Collectable::GetWeight @00590160` is
    // `max(0, count - nonLootableCount) * weight`; nothing in the shipped data
    // sets a non-lootable count, so it is the whole stack.
    float w = 0.0f;
    for (const LiveEntity::InvItem& it : e.inventory) {
        w += it.weight * static_cast<float>(std::max(0, it.count));
    }
    // An unarmed weapon is not in a slot -- it lives in `unarmed_left` /
    // `unarmed_right` -- so it never weighs anything.
    if (e.weapon.valid && !e.unarmedHand[0]) w += e.weapon.weight;
    if (e.weapon2.valid && !e.unarmedHand[1]) w += e.weapon2.weight;
    return w;
}

std::vector<int32_t> Scene::Containers() const {
    std::vector<int32_t> out;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].isContainer) out.push_back(static_cast<int32_t>(i));
    }
    return out;
}

std::vector<int32_t> Scene::GroundItems() const {
    std::vector<int32_t> out;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        if (m_entities[i].isCollectable) out.push_back(static_cast<int32_t>(i));
    }
    return out;
}

void Scene::CloseLoot() {
    m_lootTarget = -1;
    m_lootActor = -1;
}

// `HandleActivateOrLootCommand @00549120`.  See the note in scene.h.
// The queued activate: `Actor+0x1037` recorded a target this actor could not
// reach, so every tick it is retried until the walk brings it into range.
void Scene::StepPendingUse() {
    for (size_t i = 0; i < m_entities.size(); ++i) {
        LiveEntity& a = m_entities[i];
        if (a.pendingUse < 0) continue;
        if (a.dead || static_cast<size_t>(a.pendingUse) >= m_entities.size()) {
            a.pendingUse = -1;
            continue;
        }
        const LiveEntity& t = m_entities[static_cast<size_t>(a.pendingUse)];
        if (!CanReachToUse(a, t)) {
            // The walk ended without arriving: give up rather than loop.
            if (!a.moving) {
                Logf("%s cannot reach %s.", a.ShownName().c_str(), t.ShownName().c_str());
                a.pendingUse = -1;
            }
            continue;
        }
        const int32_t want = a.pendingUse;
        a.pendingUse = -1;
        a.moving = false;
        a.path.clear();
        std::string why;
        bool opened = false;
        if (!UseEntity(static_cast<int32_t>(i), want, &why, &opened)) {
            Logf("%s", why.c_str());
        } else if (opened) {
            a.lootJustOpened = true;
        }
    }
}

bool Scene::UseEntity(int32_t actor, int32_t target, std::string* whyNot, bool* openedLoot) {
    if (openedLoot != nullptr) *openedLoot = false;
    auto fail = [&](const char* w) {
        if (whyNot != nullptr) *whyNot = w;
        return false;
    };
    if (actor < 0 || static_cast<size_t>(actor) >= m_entities.size()) return fail("no actor");
    if (target < 0 || static_cast<size_t>(target) >= m_entities.size()) return fail("no target");
    if (actor == target) return fail("Can not do that.");
    LiveEntity& a = m_entities[static_cast<size_t>(actor)];
    if (!a.isCombatant || a.dead) return fail("Can not do that.");
    LiveEntity& t = m_entities[static_cast<size_t>(target)];

    // A corpse: `Actor::IsLegalTarget` is false once an actor is dead, and the
    // `HandleActivateOrLootCommand` arm that follows is the loot one.  There is
    // no separate "make the inventory lootable" step in the binary -- the
    // inventory simply stays where it is; what death does is
    // `RandomizeDeadEntityLoot @006ae030`, which runs at the moment of death
    // (see ApplyHit).
    const bool corpse = t.isCombatant && t.dead;
    const bool container = t.isContainer;
    const bool ground = t.isCollectable;
    if (!corpse && !container && !ground && !t.isDoor) {
        // `Entity::CanBeActivated` false: `use_code |= 0x40`.
        return fail("That does nothing.");
    }

    // 1. face the target (BuildOrientationBasisFromForward +
    //    Entity::Entity_UpdateFacingFromVelocityAndBounds).
    SetFacingFromVector(a, static_cast<float>(t.world[0] - a.world[0]),
                        static_cast<float>(t.world[2] - a.world[2]));

    // 2. reach: `use_code |= 0x100`, `log_outOfRange`.
    if (!CanReachToUse(a, t)) {
        // `use_code |= 0x100`.  The original does not simply refuse: its tail
        // records the target at `Actor+0x1037` and calls vtable slot 0x368, the
        // walk-then-activate.  So the order becomes "go there and use it", and
        // `StepPendingUse` retries every tick until the actor is in reach or
        // the walk gives up.
        a.pendingUse = target;
        OrderMove(actor, t.world[0], t.world[2], false);
        if (whyNot != nullptr) {
            *whyNot = Fmt("Out of range. (gap %.2f m) -- walking to it",
                          static_cast<double>(ReachGap(a, t)));
        }
        return false;
    }

    // 5. `Tables::ActionTypes["activate"]` (2 AP) -- or "pickup" (1) for a
    //    ground item, which is the action `Actor::ActorPickupItem @0055df60`
    //    names.  `use_code |= 8`, `log_badAP`.
    const int32_t cost = ActionTypeCost(ground ? "pickup" : "activate");
    if (m_combatMode != 0) {
        if (ActiveTurnEntity() != actor) return fail("not this entity's turn");
        if (a.ap < static_cast<float>(cost)) return fail("Not enough action points.");
    }

    if (t.isDoor && !container) {
        if (m_combatMode != 0) a.ap -= static_cast<float>(cost);
        // `Door::SetActivationState @005b2690` -- the same four-state string
        // machine a Container uses.
        SetDoorState(t, t.doorOpen ? "Closing" : "Opening");
        return true;
    }

    if (ground) {
        // `Actor::ActorPickupItem @0055df60`: face the item, spend the AP, play
        // `<Posture>Pickup` (`SetCurrentActionStringMaybe(GBL_StringPickup)`),
        // and take the stack.  A count of 1 skips the quantity prompt outright
        // (`if (GetCount() == 1) count = 1;`).
        if (!t.lootable) return fail("That cannot be removed.");
        if (m_combatMode != 0) a.ap -= static_cast<float>(cost);
        a.action = "Pickup";
        if (!SetAnimationType(a, a.posture + "Pickup")) {
            // No art for the posture: the original's variant search drops the
            // suffixes and lands on the bare posture, which is a no-op here.
            a.action.clear();
        } else {
            a.nextThinkMs = m_ticks * kTickMs;
        }
        const bool wasEncumbered = IsEncumbered(a);
        AddToInventory(a, t.item);
        Logf("%s picks up %s%s", a.ShownName().c_str(), t.item.displayName.c_str(),
             t.item.count >= 2 ? Fmt(" x%d", t.item.count).c_str() : "");
        if (!wasEncumbered && IsEncumbered(a)) Logf("%s is encumbered.", a.ShownName().c_str());
        // The ground entity is consumed.  `SplitAndRemoveEntityForPickup
        // @00508760` splits the stack first when only part of it is taken; the
        // whole stack is taken here, so the entity goes.
        // `SplitAndRemoveEntityForPickup @00508760` removes the world entity
        // once the whole stack has been taken.  There is no entity-destruction
        // path in this port, so the sprite is dropped and the entity stops
        // being pickable, drawn and hit-tested.
        t.isCollectable = false;
        t.lootable = false;
        t.hidden = true;
        return true;
    }

    // A container or a corpse: `Container::MaybeUseEntity @005aca10`.
    if (container) {
        // The lock.  A container with a Tag Name and `islocked` set opens only
        // for an actor who carries or has equipped an item with that same tag
        // (`ContainsItemWithTagName` / `PlayerEquippedItemWithTagName`); the
        // key then unlocks it permanently (`islocked = false`, `nowUnlocked`).
        if (t.isLocked) {
            // `Container::MaybeUseEntity @005aca10`: a container that has a
            // `Tag Name` and is locked opens for an actor who CARRIES or has
            // EQUIPPED an item with that same tag -- the key -- and then stays
            // unlocked (`islocked = false`, `log_nowUnlocked`).  Without the
            // key it plays sound 1 and answers `log_isLocked`.  A container
            // with no tag has no key and can only be picked, which is
            // `Container::ApplySkillToTargetEntity @005ad3f0` and the `.ent`'s
            // own `Chance to Unlock` -- the skill path is not implemented here.
            bool haveKey = false;
            if (!t.tag.empty()) {
                for (const LiveEntity::InvItem& it : a.inventory) {
                    haveKey |= IEq(it.tagName, t.tag.c_str());
                }
                haveKey = haveKey || (a.weapon.valid && IEq(a.weapon.displayName, t.tag.c_str()));
            }
            if (!haveKey) {
                return fail(t.tag.empty() ? "The container is locked. (no key exists; picking "
                                            "the lock is not implemented)"
                                          : "The container is locked.");
            }
            t.isLocked = false;
            Logf("The %s is now unlocked.", t.label.c_str());
        }
        if (m_combatMode != 0) a.ap -= static_cast<float>(cost);
        if (t.alwaysOpen) {
            // No animation at all: the loot window goes up immediately.
            t.openedBy = actor;
            m_lootTarget = target;
            m_lootActor = actor;
            if (openedLoot != nullptr) *openedLoot = true;
            PlaySoundStem(t, "sound/game/Butn_Std", 2, "container open");
            return true;
        }
        if (t.isOpen && t.action == "Opened") {
            // A second use closes it.
            t.action = "Closing";
            t.isOpen = false;
            SetAnimationType(t, "Closing");
            if (m_lootTarget == target) CloseLoot();
            return true;
        }
        // "Opening" now; `Container::FinishedAnimationSequenceMaybe @005ac780`
        // sets isopen, switches to "Opened" and opens the window for the actor
        // it recorded.
        t.action = "Opening";
        t.openedBy = actor;
        if (SetAnimationType(t, "Opening")) {
            t.openWhenAnimDone = true;
            t.nextThinkMs = m_ticks * kTickMs;
        } else {
            t.action = "Opened";
            t.isOpen = true;
            m_lootTarget = target;
            m_lootActor = actor;
            if (openedLoot != nullptr) *openedLoot = true;
        }
        return true;
    }

    // A corpse.
    if (m_combatMode != 0) a.ap -= static_cast<float>(cost);
    m_lootTarget = target;
    m_lootActor = actor;
    if (openedLoot != nullptr) *openedLoot = true;
    return true;
}

// `Inventory::AddToInventory @005b4030` plus `Collectable::MergeEntities
// @005c1910`: a stack of the same entity path merges rather than becoming a
// second row.
void Scene::AddToInventory(LiveEntity& e, const LiveEntity::InvItem& item) {
    for (LiveEntity::InvItem& have : e.inventory) {
        // `Collectable::MaybeCheckingIfMergeableAndMatching @005c1b20` compares
        // the two entities; the strongest identity the port carries is the
        // `.ent` path, and a weapon with its own magazine state never merges.
        if (have.entPath == item.entPath && !have.entPath.empty() && item.cls != "Weapon" &&
            item.cls != "Armour" && item.cls != "Armor") {
            have.count += item.count;
            return;
        }
    }
    e.inventory.push_back(item);
}

bool Scene::TransferItem(int32_t from, int32_t to, int32_t index, int32_t count,
                         std::string* whyNot) {
    auto fail = [&](const char* w) {
        if (whyNot != nullptr) *whyNot = w;
        return false;
    };
    if (from < 0 || static_cast<size_t>(from) >= m_entities.size()) return fail("no source");
    if (to < 0 || static_cast<size_t>(to) >= m_entities.size()) return fail("no target");
    LiveEntity& src = m_entities[static_cast<size_t>(from)];
    LiveEntity& dst = m_entities[static_cast<size_t>(to)];
    if (index < 0 || static_cast<size_t>(index) >= src.inventory.size()) {
        return fail("nothing there");
    }
    LiveEntity::InvItem item = src.inventory[static_cast<size_t>(index)];
    if (count <= 0 || count > item.count) count = item.count;
    if (count < 1) return fail("Cannot do anything with less than 1 item.");

    // `Container::AddToInventory @005ac050` is the ONE weight refusal in the
    // game: `GetWeight(inventory) + GetWeight(item) > Container.maxweight`
    // yields `use_code = 1` and `log_tooHeavy`.
    const float moving = item.weight * static_cast<float>(count);
    if (dst.isContainer && dst.maxWeight > 0) {
        float have = 0.0f;
        for (const LiveEntity::InvItem& it : dst.inventory) {
            have += it.weight * static_cast<float>(it.count);
        }
        if (have + moving > static_cast<float>(dst.maxWeight)) {
            return fail("That item is too heavy.");
        }
    }
    // An Actor never refuses: `Actor::AddToInventory @00555180` compares
    // `IsEncumbered` before and after and raises the `encumbered` event when
    // the add crosses `ST * 25`.
    const bool wasEncumbered = dst.isCombatant && IsEncumbered(dst);

    // `Collectable::SplitEntity @005c17a0` when only part of a stack moves.
    if (count == item.count) {
        src.inventory.erase(src.inventory.begin() + index);
    } else {
        src.inventory[static_cast<size_t>(index)].count -= count;
    }
    item.count = count;
    AddToInventory(dst, item);
    Logf("%s -> %s: %s%s", src.ShownName().c_str(), dst.ShownName().c_str(),
         item.displayName.c_str(), count >= 2 ? Fmt(" x%d", count).c_str() : "");
    if (dst.isCombatant && !wasEncumbered && IsEncumbered(dst)) {
        Logf("%s is encumbered.", dst.ShownName().c_str());
    }
    return true;
}

int32_t Scene::TakeAll(int32_t from, int32_t to, std::string* whyNot) {
    if (from < 0 || static_cast<size_t>(from) >= m_entities.size()) return 0;
    int32_t moved = 0;
    // `equipMenu_takeAll`.  It walks the list forward, which is the order every
    // consumer in the dumps walks it in, and stops at the first refusal.
    while (!m_entities[static_cast<size_t>(from)].inventory.empty()) {
        if (!TransferItem(from, to, 0, 0, whyNot)) break;
        ++moved;
        if (moved > 256) break;
    }
    return moved;
}

std::string Scene::TraceLoot() const {
    std::string s;
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        if (!e.isContainer && !e.isCollectable && !(e.isCombatant && e.dead)) continue;
        s += Fmt("%zu %s %s world=(%d,%d,%d) open=%d locked=%d items=%zu\n", i,
                 e.isContainer ? "container" : (e.isCollectable ? "ground" : "corpse"),
                 e.label.c_str(), e.world[0], e.world[1], e.world[2], e.isOpen ? 1 : 0,
                 e.isLocked ? 1 : 0, e.inventory.size());
        if (e.isCollectable) {
            s += Fmt("    %-24s x%-4d %.2f kg  %s\n", e.item.displayName.c_str(), e.item.count,
                     static_cast<double>(e.item.weight), e.item.entPath.c_str());
        }
        for (const LiveEntity::InvItem& it : e.inventory) {
            s += Fmt("    %-24s x%-4d %.2f kg  %s\n", it.displayName.c_str(), it.count,
                     static_cast<double>(it.weight), it.entPath.c_str());
        }
    }
    for (size_t i = 0; i < m_entities.size(); ++i) {
        const LiveEntity& e = m_entities[i];
        if (!e.isCombatant) continue;
        s += Fmt("%zu actor %s hp=%d/%d hand1=%s%s hand2=%s%s wt=%.1f/%d%s\n", i,
                 e.ShownName().c_str(), e.hp, e.maxHp,
                 e.weapon.valid ? e.weapon.displayName.c_str() : "-",
                 e.unarmedHand[0] ? " (unarmed)" : "",
                 e.weapon2.valid ? e.weapon2.displayName.c_str() : "-",
                 e.unarmedHand[1] ? " (unarmed)" : "",
                 static_cast<double>(CarriedWeight(e)), CarryCapacity(e),
                 IsEncumbered(e) ? " ENCUMBERED" : "");
        for (const LiveEntity::InvItem& it : e.inventory) {
            s += Fmt("    %-24s x%-4d %.2f kg  %s\n", it.displayName.c_str(), it.count,
                     static_cast<double>(it.weight), it.entPath.c_str());
        }
    }
    return s;
}

// MILESTONE 22.  Wearing or removing armour has to move the wearer's derived
// stats, not just the slot.  The `.ent` load path (`LoadCombatant`) already
// folded the armour's `Effect` into `derived.armorclass` / the six thresholds
// and resistances; the quartermaster's issue, `--issue` and every drag in the
// inventory screen went through `GiveItemByPath` / `MoveItem`, which set the
// slot and left the sheet alone.  That is why Mick came out of a `--campaign`
// New Game at AC 8 (his bare agility) where the original logs him at 23:
// agility 8 plus the 15 of the `bosLeather` the quartermaster hands him.
void ApplyArmourToDerived(LiveEntity& e, const ArmourDef& a, int sign) {
    if (!a.valid) return;
    e.attr.armorClass += sign * a.armorClass;
    for (int i = 0; i < kDamageTypeCount; ++i) {
        e.attr.thresh[i] += sign * a.thresh[i];
        e.attr.resist[i] += sign * a.resist[i];
    }
    // MILESTONE 25: the Effect block's skill deltas move with the slot too.
    for (int i = 0; i < kSkillCount; ++i) e.attr.skills[i] += sign * a.skills[i];
}

bool Scene::ForceEquipByPath(int32_t index, const std::string& entPath,
                             std::string* whyNot) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) {
        if (whyNot) *whyNot = "no actor";
        return false;
    }
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    std::string perr;
    const EntProto* proto = m_protos->Get(entPath, &perr);
    if (proto == nullptr) {
        if (whyNot) *whyNot = perr;
        return false;
    }
    WeaponDef w;
    if (!ReadWeapon(proto->block.esh, &w) || !w.valid) {
        if (whyNot) *whyNot = "not a weapon";
        return false;
    }
    e.weapon = w;
    e.weapon.path = entPath;      // ReadWeapon fills the stats, not the path
    e.unarmedHand[0] = false;
    e.ammo = w.magazine;
    RefreshAmmoReserves();
    return true;
}

bool Scene::GiveItemByPath(int32_t entity, const std::string& entPath, int32_t slot,
                           int32_t count, std::string* whyNot) {
    if (entity < 0 || static_cast<size_t>(entity) >= m_entities.size()) {
        if (whyNot) *whyNot = "no actor";
        return false;
    }
    LiveEntity& e = m_entities[static_cast<size_t>(entity)];
    std::string perr;
    const EntProto* proto = m_protos->Get(entPath, &perr);
    if (proto == nullptr) {
        if (whyNot) *whyNot = perr;
        return false;
    }
    LiveEntity::InvItem it;
    it.entPath = entPath;
    it.count = count;
    proto->block.esh.GetString("Display Name", &it.displayName);
    proto->block.esh.GetString("Item Type", &it.itemType);
    proto->block.esh.GetString("Sprite", &it.spritePath);
    it.cls = proto->block.className;
    proto->block.esh.GetFloat("Weight", &it.weight);
    // MILESTONE 18: the two helper-item shapes, as `ReadItemInstance` reads
    // them -- a `SkillConsumable`'s `skillType` / `Remaining uses`, and the
    // `Effect/Skills` bonus block.
    proto->block.esh.GetString("skillType", &it.skillType);
    proto->block.esh.GetInt("Remaining uses", &it.usesLeft);
    {
        Esh effect;
        std::string eerr;
        Attributes bonus;
        if (proto->block.esh.GetNested("Effect", &effect, &eerr)) {
            ReadAttributes(effect, &bonus, false);
            for (int i = 0; i < kSkillCount; ++i) it.skillBonus[i] = bonus.skills[i];
        }
    }
    if (it.cls == "Weapon") ReadWeapon(proto->block.esh, &it.weapon);
    if (it.cls == "Armor" || it.cls == "Armour") ReadArmour(proto->block.esh, &it.armour);
    // MILESTONE 17: an unarmed weapon does not hold the slot against a real
    // one -- it is not in `slot_equip` at all.
    if (slot == 0 && it.weapon.valid && (!e.weapon.valid || e.unarmedHand[0])) {
        e.weapon = it.weapon;
        if (e.weapon.path.empty()) e.weapon.path = entPath;
        e.unarmedHand[0] = false;
        e.ammo = it.weapon.magazine;
        return true;
    }
    if (slot == 1 && it.weapon.valid && (!e.weapon2.valid || e.unarmedHand[1])) {
        e.weapon2 = it.weapon;
        if (e.weapon2.path.empty()) e.weapon2.path = entPath;
        e.unarmedHand[1] = false;
        return true;
    }
    if (slot == 2 && it.armour.valid && !e.armour.valid) {
        e.armour = it.armour;
        if (e.armour.path.empty()) e.armour.path = entPath;
        ApplyArmourToDerived(e, e.armour, +1);
        return true;
    }
    e.inventory.push_back(it);
    return true;
}

void Scene::RefreshAmmoReserves() {
    for (LiveEntity& e : m_entities) {
        if (!e.isCombatant) continue;
        e.ammoReserve = 0;
        if (!e.weapon.valid || e.weapon.ammoType.empty()) continue;
        for (const LiveEntity::InvItem& it : e.inventory) {
            if (it.cls != "Ammo") continue;
            std::string perr;
            const EntProto* proto = m_protos->Get(it.entPath, &perr);
            if (proto == nullptr) continue;
            std::string type;
            proto->block.esh.GetString("Ammo Type", &type);
            if (type != e.weapon.ammoType) continue;
            e.ammoReserve += it.count;
            std::string variant;
            if (proto->block.esh.GetString("Ammo Variant", &variant) && !variant.empty()) {
                e.weapon.ammoVariant = variant;
            }
        }
        if (e.ammo > e.weapon.magazine) e.ammo = e.weapon.magazine;
    }
}

bool Scene::MoveItem(int32_t entity, int32_t from, int32_t to, std::string* whyNot) {
    auto fail = [&](const char* why) {
        if (whyNot != nullptr) *whyNot = why;
        return false;
    };
    if (entity < 0 || static_cast<size_t>(entity) >= m_entities.size()) return fail("no actor");
    LiveEntity& e = m_entities[static_cast<size_t>(entity)];
    if (!e.isCombatant) return fail("not an actor");
    if (from == to) return fail("same slot");

    // What is in `from`?
    LiveEntity::InvItem item;
    if (from == 0 || from == 1) {
        const WeaponDef& w = from == 0 ? e.weapon : e.weapon2;
        if (!w.valid) return fail("hand is empty");
        // MILESTONE 17.  An unarmed weapon is not in `slot_equip` at all -- it
        // lives in `Actor.ct_.unarmed_left` / `unarmed_right` -- and every
        // punch and kick `.ent` sets `CanBeRemoved = false` and
        // `Lootable = false`.  It cannot be dragged out of the hand.
        if (e.unarmedHand[from]) return fail("That cannot be removed.");
        item.cls = "Weapon";
        item.displayName = w.displayName;
        item.itemType = w.itemType;
        item.entPath = w.path;
        item.weight = w.weight;
        item.weapon = w;
    } else if (from == 2) {
        if (!e.armour.valid) return fail("no armour worn");
        item.cls = "Armour";
        item.displayName = e.armour.displayName;
        item.itemType = e.armour.itemType;
        item.entPath = e.armour.path;
        item.armour = e.armour;
    } else if (from >= 3 && static_cast<size_t>(from - 3) < e.inventory.size()) {
        item = e.inventory[static_cast<size_t>(from - 3)];
    } else {
        return fail("nothing there");
    }

    if (!SlotAcceptsClass(to, item.cls)) {
        // The refusal the original surfaces is the `use_response_string` the
        // UseItemResponse carries; there is no such table here, so the message
        // is this port's.
        return fail(item.cls == "Armour" ? "armour goes in the armour slot"
                                         : "that does not go there");
    }
    if (to == 0 || to == 1) {
        const WeaponDef& w = to == 0 ? e.weapon : e.weapon2;
        // An unarmed weapon does not occupy the slot: equipping into that hand
        // simply displaces it, and unequipping puts it back (below).
        if (w.valid && !e.unarmedHand[to]) return fail("hand is full");
    }
    if (to == 2 && e.armour.valid) return fail("already wearing armour");

    // Take it out.  A hand emptied of a real weapon falls back to the unarmed
    // one, which is what `Actor::GetIDOfEquippedItemInActiveSlot` would then
    // hand the fire path.
    if (from == 0) {
        e.weapon = e.unarmedPunch;
        e.unarmedHand[0] = e.unarmedPunch.valid;
    } else if (from == 1) {
        e.weapon2 = e.unarmedKick;
        e.unarmedHand[1] = e.unarmedKick.valid;
    }
    else if (from == 2) {
        ApplyArmourToDerived(e, e.armour, -1);
        e.armour = ArmourDef();
    }
    else e.inventory.erase(e.inventory.begin() + (from - 3));

    // Put it in.  A weapon or armour that came out of the inventory has to be
    // re-read from its own `.ent`: the list only carries the identity.
    if (to == 0 || to == 1) {
        WeaponDef w = item.weapon;
        if (!w.valid) {
            std::string perr;
            const EntProto* proto = m_protos->Get(item.entPath, &perr);
            if (proto != nullptr) ReadWeapon(proto->block.esh, &w);
        }
        if (!w.valid) {
            w.valid = true;
            w.displayName = item.displayName;
            w.itemType = item.itemType;
            w.path = item.entPath;
        }
        (to == 0 ? e.weapon : e.weapon2) = w;
        e.unarmedHand[to] = false;
        if (to == 0) e.ammo = w.magazine;
    } else if (to == 2) {
        ArmourDef a = item.armour;
        if (!a.valid) {
            std::string perr;
            const EntProto* proto = m_protos->Get(item.entPath, &perr);
            if (proto != nullptr) ReadArmour(proto->block.esh, &a);
        }
        if (!a.valid) {
            a.valid = true;
            a.displayName = item.displayName;
            a.itemType = item.itemType;
            a.path = item.entPath;
        }
        e.armour = a;
        ApplyArmourToDerived(e, e.armour, +1);
    } else if (to >= 3 || to == -1) {
        if (to != -1) {
            const size_t at = std::min(static_cast<size_t>(std::max(0, to - 3)),
                                       e.inventory.size());
            e.inventory.insert(e.inventory.begin() + static_cast<long>(at), item);
        }
        // to == -1 is a drop: the item leaves the actor.  There is no ground
        // container in this port, so it is simply gone, and the log says so.
    }
    Logf("%s: %s -> %s", e.label.c_str(), item.displayName.c_str(),
         to == -1 ? "the ground"
                  : (to == 0 ? "left hand" : (to == 1 ? "right hand"
                                                      : (to == 2 ? "armour" : "pack"))));
    return true;
}

bool Scene::SwapHands(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return false;
    LiveEntity& e = m_entities[static_cast<size_t>(index)];
    if (!e.isCombatant) return false;
    e.handSelected = e.handSelected == 0 ? 1 : 0;
    return true;
}

bool Scene::Reload(int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= m_entities.size()) return false;
    return ReloadWeapon(m_entities[static_cast<size_t>(index)]);
}

// MILESTONE 13 (verification only).  See scene.h.
size_t Scene::SetControllerRandMove(int32_t playerIndex, bool on) {
    size_t n = 0;
    for (ControllerState& c : m_controllers) {
        if (playerIndex >= 0 && c.playerIndex != playerIndex) continue;
        c.randMove = on;
        ++n;
    }
    return n;
}


// ===========================================================================
// MILESTONE 13 -- sound.  See NOTES.md section 69.
//
// The engine funnels every effect through `SoundManager::PlaySound @0071cfd0`
// with a world position, a reference distance, a cutoff distance and a channel;
// game/audio.cpp is that function.  What is here is the *caller* side: which
// event names which file.  The one naming function the dumps contain is
// `Actor::PlayActorAttackSound @00562910`; `Actor::SoundRelated1`, which names
// the footstep, hit, death and fidget files, is a vtable slot with no body, so
// game/audio_names.cpp reconstructs it from the shipped file names.
// ===========================================================================

void Scene::SetListener(int32_t worldX, int32_t worldZ) {
    const int32_t cx = m_nav.CellX(worldX), cz = m_nav.CellZ(worldZ);
    const int32_t surf = m_nav.Surface(cx, cz);
    const float y = surf == kNoFloor ? 0.0f
                                     : static_cast<float>(surf) / ft::kEntityWorldScale;
    const float x = static_cast<float>(worldX) / ft::kEntityWorldScale;
    const float z = static_cast<float>(worldZ) / ft::kEntityWorldScale;
    m_listener[0] = x;
    m_listener[1] = y;
    m_listener[2] = z;
    if (m_audio != nullptr) {
        // `SoundManager::Set3DListenerOrientation @0071e580`.  The listener's
        // right vector: on this projection screen-x is 2*kIsoHalfWidth*(x - z)
        // (mapview/iso.h), so world +x and world -z both move a source to the
        // right, and the normalised right vector is (1, 0, -1)/sqrt(2).
        m_audio->SetListener(x, y, z, 0.70710678f, -0.70710678f);
    }
}

bool Scene::PlaySoundStem(const LiveEntity& e, const std::string& stem, int channel,
                          const char* why) {
    if (m_audio == nullptr || !m_audio->Enabled() || stem.empty()) {
        return false;
    }
    const std::string pick = m_audio->PickVariant(stem);
    if (pick.empty()) {
        return false;
    }
    SoundRequest r;
    r.file = pick;
    r.entityId = static_cast<int32_t>(&e - m_entities.data());
    r.positional = true;
    r.x = e.wpos[0];
    r.y = e.wpos[1];
    r.z = e.wpos[2];
    r.channel = channel;
    r.why = why;
    return m_audio->Play(r);
}

// `Actor::CheckThinkTimers @00541?` (`Entity::ProcessAnimation@005e2b00`): opcodes
// -0x2c (-44), -0x29 (-41) and -0x28 (-40) all call
// `SoundRelated1(this, opcode)`.  Which family each names is a reconstruction
// pinned by where the opcode appears in the shipped sprites:
//   -44 sits at frame 0 of Death*, Dodge*, Fallback, GetUp, Attack* and of a
//       door's Opening/Closing -- the sequence's own sound;
//   -41 and -40 appear ONLY in Walk/Run/Climb sequences, twice per cycle at
//       opposite phases -- the two footfalls.
// (2706 -44 markers, 365 -41 and 367 -40, over the 918 shipped sprites.)
void Scene::FrameEventSound(LiveEntity& e, int opcode) {
    if (m_audio == nullptr || !m_audio->Enabled()) {
        return;
    }
    const std::string& seq = e.sequenceName;
    if (opcode == -41 || opcode == -40) {
        if (!e.isActor) {
            return;
        }
        // The material of the cell the walker is standing on.
        const int32_t cx = m_nav.CellX(e.world[0]), cz = m_nav.CellZ(e.world[2]);
        const int material = static_cast<int>(m_nav.Material(cx, cz));
        // A climb is on a ladder, whatever the tile under it says.
        const bool ladder = seq.find("Climb") != std::string::npos;
        std::string stem =
            snd::Footstep(e.posture, e.spritePath,
                          e.armour.valid ? e.armour.itemType : std::string(), material);
        if (ladder) {
            stem = "sound/game/" + snd::FootPosture(e.posture) +
                   snd::WeightClass(e.spritePath) +
                   snd::ArmourTier(e.armour.valid ? e.armour.itemType : std::string()) +
                   "Ladder";
        }
        PlaySoundStem(e, stem, kChanDefault, "footstep");
        return;
    }
    if (opcode != -44) {
        return;   // -45 pickup and -6 effect have no sound of their own
    }
    // The sequence's own sound.  A door's Opening/Closing is the door family;
    // an actor's is its voice set plus the sequence's suffix.
    if (e.isDoor) {
        return;   // SetDoorState already played it, at the state change
    }
    if (e.voice.empty()) {
        return;
    }
    if (seq.compare(0, 5, "Death") == 0) {
        PlaySoundStem(e, snd::DeathStem(e.voice, seq.substr(5)), kChanSpeech, "death");
    } else if (seq.find("Fallback") != std::string::npos ||
               seq.find("Dodge") != std::string::npos) {
        PlaySoundStem(e, snd::HitStem(e.voice), kChanSpeech, "hit-reaction");
    } else if (seq.find("GetUp") != std::string::npos) {
        PlaySoundStem(e, "sound/game/" + e.voice + "GetUp", kChanSpeech, "getup");
    } else if (seq.find("Breathe") != std::string::npos ||
               seq.find("Fidget") != std::string::npos) {
        PlaySoundStem(e, snd::FidgetStem(e.voice), kChanSpeech, "fidget");
    }
}

// `UpdateWorldZonesTimeAndAmbientSound @004fd4e0`: a
// per-tick pass over the world's zones that runs `MaybePlayRandomAmbientSound`
// for each, then `UpdateCombatMusicState`.  The zone/time-of-day selector
// `FUN_004dccf0` and `UpdateCombatMusicState` are both call-site-only in the
// dumps, so the phase test and the combat switch below are RECONSTRUCTIONS.
void Scene::UpdateAmbient(int64_t deltaMs) {
    (void)deltaMs;
    if (m_audio == nullptr || !m_audio->Enabled()) {
        return;
    }
    StartMissionMusic();

    // The time-of-day phase.  `UpdateWorldZonesTimeAndAmbientSound` computes
    // one of 0 (none), 1, 2, 3, 4 from the world clock; the four predicates
    // (FUN_006ad8c0 / 8a0 / 7a0 / 770) have no bodies.  The shipped records
    // only ever carry 0 and 4, and the one documented value in
    // `ambientExample.txt` is `night`, so 4 is read as night here.
    const int hour = m_opt.haveTime ? m_opt.timeHour : 12;
    const int phase = (hour >= 20 || hour < 6) ? 4 : 0;

    for (AmbientSound& a : m_script.Ambients()) {
        if (a.files.empty()) {
            continue;
        }
        // The zone test.  `_zone` names a <world_zone>; the bed is live when
        // the LISTENER is inside it, which is what makes an interior bed fade
        // in as the camera enters the building.
        if (!a.zone.empty() && a.zone != "none") {
            const WorldZone* z = m_script.Zone(a.zone);
            if (z != nullptr && !z->Contains(m_listener)) {
                continue;
            }
        }
        if (a.timeOfDay != 0) {
            const bool match = a.timeOfDay == phase;
            if (match == a.notTimeOfDay) {
                continue;
            }
        }
        // `MaybePlayRandomAmbientSound`: when the
        // next-play time is unset, roll it in [minTime, maxTime] ahead of
        // World.realtime; return until it is reached.  A looping bed starts
        // once and is never re-rolled.
        if (a.looping && a.started) {
            continue;
        }
        if (a.nextMs == 0) {
            const int32_t span = a.maxTime > a.minTime ? a.maxTime - a.minTime : 0;
            const int32_t wait =
                a.minTime + (span > 0 ? static_cast<int32_t>(m_audio->NextRandom() %
                                                             static_cast<uint32_t>(span))
                                      : 0);
            a.nextMs = m_nowMs + static_cast<int64_t>(wait) * 1000;
            if (a.looping) {
                a.nextMs = m_nowMs;   // a bed starts at once
            }
        }
        if (!a.looping && m_nowMs < a.nextMs) {
            continue;
        }
        // l.1409-1424: `_random` picks with the LCG, otherwise the list is
        // walked in order.
        int32_t idx = a.lastFile;
        const int32_t n = static_cast<int32_t>(a.files.size());
        if (n > 1) {
            if (a.random) {
                idx = static_cast<int32_t>(m_audio->NextRandom() %
                                           static_cast<uint32_t>(n));
            } else {
                idx = (idx + 1) % n;
            }
        } else {
            idx = 0;
        }
        a.lastFile = idx;

        SoundRequest r;
        r.file = a.files[static_cast<size_t>(idx)];
        r.entityId = -1 - a.id;      // a stable, entity-free id
        r.channel = kChanAmbient;
        r.looping = a.looping;
        r.why = "ambient";
        // l.1466-1476: a `_useRadius` record is positional at `_position`
        // with (`_minDist`, `_maxDist`); everything else is the flat
        // `PlaySoundByFilename` path.
        if (a.useRadius) {
            r.positional = true;
            r.x = a.pos[0];
            r.y = a.pos[1];
            r.z = a.pos[2];
            r.refDist = static_cast<float>(a.minDist);
            r.cutoff = static_cast<float>(a.maxDist);
        }
        // l.1480-1489: `_volumeVar` takes a random bite out of `_volume`.
        int vol = a.volume;
        if (a.volumeVar > 0.0f) {
            const float f = static_cast<float>(m_audio->NextRandom() % 1000u) / 1000.0f;
            vol = static_cast<int>(static_cast<float>(a.volume) * (1.0f - f * a.volumeVar));
        }
        r.baseVolume = vol;
        m_audio->Play(r);
        a.started = true;
        if (!a.looping) {
            const int32_t span = a.maxTime > a.minTime ? a.maxTime - a.minTime : 0;
            const int32_t wait =
                a.minTime + (span > 0 ? static_cast<int32_t>(m_audio->NextRandom() %
                                                             static_cast<uint32_t>(span))
                                      : 0);
            a.nextMs = m_nowMs + static_cast<int64_t>(wait) * 1000;
        }
    }

    // `UpdateCombatMusicState` -- the last line of the same function, and a
    // call site with no body anywhere in the dumps.  RECONSTRUCTION: the
    // mission's `ambientMusic` bed plays out of combat, and a `music/battle`
    // track plays while any squad member has a live hostile target.  The
    // battle track names (MX_BA_RAI, MX_BA_GHO, MX_BA_MUT, MX_BA_ROB,
    // MX_BA_REA, MX_BA_TRI, MX_BA_CRI, MX_BA_GEN, MX_BA_FIN) mirror the race
    // names, which is the only evidence there is; MX_BA_GEN is the generic one
    // and is what this picks.
    bool inCombat = false;
    for (const LiveEntity& e : m_entities) {
        if (e.isCombatant && !e.dead && e.attackTarget >= 0) {
            inCombat = true;
            break;
        }
    }
    // A hold-off, so a burst that briefly clears every target does not restart
    // the stream.  The real one is inside `UpdateCombatMusicState`, which has
    // no body; 10 s is a PLACEHOLDER.
    if (inCombat) {
        m_combatUntilMs = m_nowMs + 10000;
    }
    const bool wantCombat = m_nowMs < m_combatUntilMs;
    if (wantCombat != m_combatMusic) {
        m_combatMusic = wantCombat;
        if (wantCombat) {
            m_audio->PlayMusic("battle/MX_BA_GEN.MP3", -1, "combat-music");
        } else {
            m_musicStarted = false;   // fall back to the mission bed
            StartMissionMusic();
        }
    }
}

// `LoadAmbientMusicAndOverlays @004f3e90`: the mission names one base track and
// N overlays.  The base ships as a numbered pair (MX_ENV_DES1.MP3 /
// MX_ENV_DES2.MP3), which is what `OpenRandomAmbientMusicStream @0071b820`'s
// random pick is for; the overlays (MX_ENV_DES_SS1 / _SS2 / _SP1) are separate
// files with no numbering.  Only the base is played here: the mixing rule for
// an overlay is not in any dump.  PLACEHOLDER -- NOTES.md 69.6.
void Scene::StartMissionMusic() {
    if (m_musicStarted || m_audio == nullptr || !m_audio->Enabled()) {
        return;
    }
    m_musicStarted = true;
    const std::string& base = m_script.AmbientMusic();
    if (base.empty()) {
        return;
    }
    const int pick = static_cast<int>(m_audio->NextRandom() % 2u) + 1;
    const std::string a = "mission/" + base + std::to_string(pick) + ".MP3";
    const std::string b = "mission/" + base + std::to_string(3 - pick) + ".MP3";
    if (!m_audio->PlayMusic(a, -1, "mission-music")) {
        m_audio->PlayMusic(b, -1, "mission-music");
    }
}

// ---------------------------------------------------------------------------
// MILESTONE 22: writing a real world save.
//
// `World::Save @004ea450` takes its `param_3 != 0` branch when a save-game
// header is supplied: it writes the source
// map's path and a `<sgd>` block in place of the whole `<mapManager>`, and then
// every other section exactly as the editor wrote it -- entity records
// included.  `World::SaveEntityRecordsWithProgress @004e5860` hands each entity
// to `ResolveEntityRefAndFlagDirty`, whose body is in no dump, and NOTES 59
// concluded from that the payload was unrecoverable.  It is not: the shipped
// save proves the payload is the ORDINARY `<esh>` record, byte for byte the
// same codec an editor-written `.mis` uses (NOTES 116).  So the save is the
// world that was loaded, with the runtime state written back into the entities'
// own properties.
// ---------------------------------------------------------------------------
bool Scene::WriteWorldSave(const std::string& mapPath, std::vector<uint8_t>* out,
                           std::string* errorOut) {
    World copy = m_world;

    // An entity's carried items are SEPARATE SLOTS pointed at by a
    // `kEshEntityRef` (see `Scene::ResolveRef` and NOTES 89) -- that is how the
    // shipped save stores a squad member's rifle, and it is how this writes one
    // back.  `AddItem` appends the item's own record and returns its slot.
    auto classIndexFor = [&](const std::string& name) -> int32_t {
        for (size_t c = 0; c < copy.entityFile.classNames.size(); ++c) {
            if (copy.entityFile.classNames[c] == name) return static_cast<int32_t>(c);
        }
        copy.entityFile.classNames.push_back(name);
        return static_cast<int32_t>(copy.entityFile.classNames.size() - 1);
    };
    auto addItem = [&](const std::string& entPath, const std::string& cls) -> int32_t {
        if (entPath.empty()) return 0;
        std::string perr;
        const EntProto* proto = m_protos->Get(entPath, &perr);
        if (proto == nullptr) return 0;
        EntityInstance item;
        item.className = proto->block.className.empty() ? cls : proto->block.className;
        item.classIndex = classIndexFor(item.className);
        item.esh = proto->block.esh;
        item.esh.PutString("EntPath", entPath, 4);
        item.entPath = entPath;
        item.idA = 0;
        item.idB = 1;
        EntitySlot row;
        row.idA = 0;
        row.idB = 1;
        row.classIndex = static_cast<uint16_t>(item.classIndex);
        copy.entityFile.slots.push_back(row);
        item.slot = static_cast<int32_t>(copy.entityFile.slots.size());
        copy.entityFile.entities.push_back(std::move(item));
        return copy.entityFile.entities.back().slot;
    };
    auto setRef = [](Esh* owner, const char* prop, int32_t slot) {
        EshProp* p = owner->FindMut(prop);
        if (p == nullptr) {
            EshProp np;
            np.name = prop;
            np.type = kEshEntityRef;
            owner->props.push_back(std::move(np));
            p = &owner->props.back();
        }
        p->type = kEshEntityRef;
        p->payload = {static_cast<uint8_t>(slot & 0xFF), static_cast<uint8_t>((slot >> 8) & 0xFF),
                      1, 0};
    };
    for (const LiveEntity& e : m_entities) {
        if (e.misIndex >= copy.entityFile.entities.size()) continue;

        // Every item record is appended FIRST: `addItem` pushes onto the same
        // vector the record being edited lives in, so no reference into it may
        // be held across the calls.
        int32_t hand1 = 0, magazine = 0, hand2 = 0, armour = 0;
        if (e.weapon.valid) {
            hand1 = addItem(e.weapon.path, "Weapon");
            if (hand1 != 0 && e.weapon.magazine > 0) {
                // The magazine: an `Ammo` entity of its own whose `Count` is
                // the rounds left, exactly as the shipped save stores it.
                EntityInstance mag;
                mag.className = "Ammo";
                mag.classIndex = classIndexFor("Ammo");
                mag.esh.version = 1;
                mag.esh.PutInt("Count", e.ammo);
                mag.esh.PutString("Ammo Type", e.weapon.ammoType, 9);
                mag.idB = 1;
                EntitySlot mrow;
                mrow.idB = 1;
                mrow.classIndex = static_cast<uint16_t>(mag.classIndex);
                copy.entityFile.slots.push_back(mrow);
                mag.slot = static_cast<int32_t>(copy.entityFile.slots.size());
                magazine = mag.slot;
                copy.entityFile.entities.push_back(std::move(mag));
            }
        }
        if (e.weapon2.valid) hand2 = addItem(e.weapon2.path, "Weapon");
        if (!e.armour.path.empty()) armour = addItem(e.armour.path, "Armour");

        if (hand1 != 0 && magazine != 0) {
            for (EntityInstance& rec : copy.entityFile.entities) {
                if (rec.slot == hand1) {
                    setRef(&rec.esh, "Ammo", magazine);
                    break;
                }
            }
        }

        Esh& esh = copy.entityFile.entities[e.misIndex].esh;

        // The "frame" property: a 3x3 orientation then the position, in metres
        // (`kEntityWorldScale` world units to the metre).
        float frame[12];
        const EntityInstance& src = m_world.entityFile.entities[e.misIndex];
        for (int k = 0; k < 9; ++k) frame[k] = src.rot[k];
        for (int k = 0; k < 3; ++k) {
            frame[9 + k] = static_cast<float>(e.world[k]) / kEntityWorldScale;
        }
        esh.SetFrame("frame", frame);

        // `Current Attributes` is the live ActorStatus (the reader takes
        // hitPoints / actionPoints straight out of it), so it is where the
        // damage taken this mission belongs.
        std::string err;
        Esh cur;
        if (esh.GetNested("Current Attributes", &cur, &err)) {
            cur.SetInt("hitPoints", e.dead ? 0 : e.hp);
            cur.SetFloat("actionPoints", e.ap);
            esh.SetNested("Current Attributes", cur);
        }

        // What the actor is holding.  Only the three worn slots are written:
        // the backpack is an `Inventory` entity with its own `ILink<n>` chain
        // and is still carried by the roster instead (NOTES 118).
        if (hand1 != 0) {
            setRef(&esh, "Hand1", hand1);
            // `Hand1` is only read when the instance does NOT claim an
            // override, so clear the flag.
            if (EshProp* ov = esh.FindMut("Hand1_override")) {
                ov->type = kEshBool;
                ov->payload.assign(1, 0);
            }
        }
        if (hand2 != 0) setRef(&esh, "Hand2", hand2);
        if (armour != 0) setRef(&esh, "Armour", armour);
    }
    copy.entityFile.slotCount = static_cast<uint16_t>(copy.entityFile.slots.size() + 1);
    copy.entityFile.liveCount = static_cast<uint16_t>(copy.entityFile.entities.size());

    // The clock: `<sgd>` +0x2e7 and `<SSG>` are seconds, and the port's tick is
    // `kTickMs` milliseconds (NOTES 2).
    const float seconds = static_cast<float>(m_ticks) * (kTickMs / 1000.0f);
    std::vector<std::string> strings;
    std::vector<std::vector<std::string>> groups;
    const std::vector<uint8_t> prefix = World::BuildSavePrefix(
        mapPath.empty() ? m_misPath : mapPath, seconds, static_cast<int32_t>(m_ticks),
        seconds, strings, groups);

    return copy.BuildSaveWorld(out, prefix, errorOut);
}

}  // namespace game

