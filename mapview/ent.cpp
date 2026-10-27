#include "ent.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>

#include "file.h"
#include "vfs.h"

namespace fs = std::filesystem;

namespace ft {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string Slashes(std::string s) {
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    return s;
}

constexpr double kTwoPi = 6.283185307179586;

}  // namespace

const char* const kEntityColourNames[4] = {"Base", "Hair", "Skin", "Team"};

// ---------------------------------------------------------------------------
// PathIndex
// ---------------------------------------------------------------------------
PathIndex::PathIndex(const std::string& root, const std::string& stripPrefix,
                     const std::string& extensionFilter)
    : m_root(root), m_strip(Lower(stripPrefix)) {
    const std::string ext = Lower(extensionFilter);
    // MILESTONE 15.  When a resource layer is mounted it is the whole truth:
    // it already merges the `.bos` archives with any loose tree, in the right
    // precedence order, under exactly the names this index is keyed by. Walking
    // the directory again on top of it would be both redundant and wrong (it
    // would let a loose file that an archive supersedes win by arriving last).
    //
    // `m_root` is still set from the constructor argument, because `Root()`
    // being empty is how SpriteCache and EntProtoCache report "no --sprites
    // directory"; with a mount, the root is the mount and is never empty.
    if (VfsMounted()) {
        if (m_root.empty()) m_root = "<resources>";
        VfsEnumerate(m_strip, ext, [&](const std::string& key) {
            std::string handle;
            if (VfsResolve(key, &handle)) m_byRel.emplace(key, handle);
        });
        return;
    }
    if (root.empty()) {
        return;
    }
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        if (!it->is_regular_file(ec)) {
            continue;
        }
        if (!ext.empty() && Lower(it->path().extension().string()) != ext) {
            continue;
        }
        const std::string full = it->path().string();
        std::string rel = full.substr(root.size());
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) {
            rel.erase(rel.begin());
        }
        m_byRel.emplace(Lower(Slashes(rel)), full);
    }
}

bool PathIndex::Find(const std::string& name, std::string* out) const {
    std::string key = Lower(Slashes(name));
    // A handful of shipped maps store an absolute path from the developer's
    // build machine -- `F:/bos/core/entities/Actors/Mutants/sm_03_Melee.ent`
    // and two others.  The engine's resource layer is rooted at `core`, so
    // anything up to and including a `/core/` segment is not part of the
    // resource name.  Everything after it resolves normally.
    const size_t core = key.find("/core/");
    if (core != std::string::npos && key.find(':') < core) {
        key.erase(0, core + 6);
    }
    auto it = m_byRel.find(key);
    if (it != m_byRel.end()) {
        *out = it->second;
        return true;
    }
    if (!m_strip.empty() && key.rfind(m_strip, 0) == 0) {
        it = m_byRel.find(key.substr(m_strip.size()));
        if (it != m_byRel.end()) {
            *out = it->second;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// <entity_file>
// ---------------------------------------------------------------------------
//
// World::ReadEntityFileRecordsFromStream @004e52d0:
//
//   token "<entity_file>"  version v   (v == 3 in every shipped map)
//     u32     classNameCount                                        l.1650-1666
//     string  className[classNameCount]
//     [v < 2] u32 n2; string[n2]                        -- read and discarded
//     u16     slotCount                                             l.1726
//     [v > 2] u16 liveCount            -- progress-bar denominator  l.1748-1749
//     u16     worldEntityCount                                      l.1751
//     for i = 1 .. slotCount-1:                                     l.1753-1781
//         u16 idA          -> EntityTable[i]+4                      l.1768
//         u16 idB          -> EntityTable[i]+6                      l.1769
//         <one entity record>                                       l.1770
//
// `ReadAndConstructRegisteredClassFromStream` (l.1770) is not in the supplied
// decompilation, so the record's own framing was recovered from the data: it is
// a u16 class index into the name table, and 0xffff means "this slot is empty"
// -- which is the branch at l.1772 where the slot pointer comes back null and
// is replaced with World's dummy entity.  An occupied slot is followed by one
// <esh>.  Checked over all 87 shipped maps: the occupied-slot count equals
// `liveCount` exactly in every one, and the walk lands precisely on the byte
// where the next section (`<world_zone>`) begins.  See NOTES.md.
bool EntityFile::Read(Reader& stream, std::string* errorOut) {
    std::string tag;
    if (!stream.FormatToken(&tag, &version)) {
        *errorOut = "truncated <entity_file> token";
        return false;
    }
    if (tag != "<entity_file>") {
        *errorOut = "expected <entity_file>, got " + tag;
        return false;
    }
    if (version > 3) {
        // l.1668: `if (3 < local_20)` takes ProcessEntityRecordsWithProgress,
        // an entirely different path that is not in the decompilation.
        *errorOut = "<entity_file> version " + std::to_string(version) + " not supported";
        return false;
    }
    const uint32_t classCount = stream.U32();
    if (stream.Failed() || classCount > 4096) {
        *errorOut = "bad <entity_file> class count";
        return false;
    }
    classNames.resize(classCount);
    for (uint32_t i = 0; i < classCount; ++i) {
        classNames[i] = stream.TaggedString(&classNamesWide);
    }
    if (version < 2) {
        const uint32_t n2 = stream.U32();
        if (stream.Failed() || n2 > 65536) {
            *errorOut = "bad <entity_file> second table count";
            return false;
        }
        for (uint32_t i = 0; i < n2; ++i) {
            stream.TaggedString();  // read and thrown away by the original
        }
    }
    slotCount = static_cast<uint16_t>(stream.I16());
    if (version > 2) {
        liveCount = static_cast<uint16_t>(stream.I16());
    }
    worldEntityCount = static_cast<uint16_t>(stream.I16());
    if (stream.Failed()) {
        *errorOut = "truncated <entity_file> slot header";
        return false;
    }

    entities.clear();
    slots.clear();
    emptySlots = 0;
    for (int32_t i = 1; i < static_cast<int32_t>(slotCount); ++i) {
        const uint16_t a = static_cast<uint16_t>(stream.I16());
        const uint16_t b = static_cast<uint16_t>(stream.I16());
        const uint16_t classIdx = static_cast<uint16_t>(stream.I16());
        if (stream.Failed()) {
            *errorOut = "truncated slot " + std::to_string(i);
            return false;
        }
        EntitySlot row;
        row.idA = a;
        row.idB = b;
        row.classIndex = classIdx;
        slots.push_back(row);
        if (classIdx == 0xffff) {
            ++emptySlots;
            continue;
        }
        if (classIdx >= classNames.size()) {
            *errorOut = "slot " + std::to_string(i) + ": class index " +
                        std::to_string(classIdx) + " out of range";
            return false;
        }
        EntityInstance inst;
        inst.slot = i;
        inst.idA = a;
        inst.idB = b;
        inst.classIndex = classIdx;
        inst.className = classNames[classIdx];
        std::string eshError;
        if (!inst.esh.Read(stream, &eshError)) {
            *errorOut = "slot " + std::to_string(i) + " (" + inst.className + "): " + eshError;
            return false;
        }

        // Entity::ClassType::Serialize, the properties a viewer needs.
        inst.esh.GetString("EntPath", &inst.entPath);
        inst.esh.GetString("Display Name", &inst.displayName);
        inst.esh.GetString("Internal Name", &inst.internalName);
        inst.esh.GetString("Tag Name", &inst.tagName);
        inst.esh.GetString("Item Type", &inst.itemType);
        inst.esh.GetString("Race Type", &inst.raceType);
        inst.esh.GetString("Posture", &inst.posture);
        if (inst.esh.GetString("Sprite", &inst.spriteFromMis)) {
            inst.spritePath = inst.spriteFromMis;
        }
        float frame[12];
        if (inst.esh.GetFrame("frame", frame)) {
            inst.hasFrame = true;
            for (int k = 0; k < 9; ++k) {
                inst.rot[k] = frame[k];
            }
            for (int k = 0; k < 3; ++k) {
                inst.pos[k] = frame[9 + k];
                // l.15133-15139: ROUND((pos + spriteAdjust) * scale).  The
                // sprite adjust is a runtime field and zero at load.
                inst.world[k] =
                    static_cast<int32_t>(std::lround(inst.pos[k] * kEntityWorldScale));
            }
        }
        int32_t v = 0;
        if (inst.esh.GetInt("animationRotation", &v)) {
            inst.hasRotation = true;
            // l.15233: the field is a signed char widened to an int for the
            // stream, so truncate back the same way.
            inst.animationRotation = static_cast<int8_t>(v & 0xff);
        }
        if (inst.esh.GetInt("sequenceIndex", &v)) {
            inst.sequenceIndex = v;
        }
        if (inst.esh.GetInt("animationFrame", &v)) {
            inst.animationFrame = v;
        }
        if (inst.esh.GetInt("Player Index", &v)) {
            inst.playerIndex = v;
        }
        if (inst.esh.GetInt("desiredTeam", &v)) {
            inst.desiredTeam = v;
        }
        for (int c = 0; c < 4; ++c) {
            inst.hasColour[c] = inst.esh.GetColour(kEntityColourNames[c], inst.colour[c]);
        }
        entities.push_back(std::move(inst));
    }
    return true;
}

void EntityFile::Write(std::vector<uint8_t>* out) const {
    auto u16 = [&](uint16_t v) {
        out->push_back(static_cast<uint8_t>(v & 0xFF));
        out->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    };
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    };
    static const char kTok[] = "<entity_file>";
    out->insert(out->end(), kTok, kTok + sizeof(kTok));
    const std::string ver = std::to_string(version);
    out->insert(out->end(), ver.begin(), ver.end());
    out->push_back(0);

    u32(static_cast<uint32_t>(classNames.size()));
    for (const std::string& n : classNames) {
        WriteTaggedString(out, n, classNamesWide);
    }
    u16(slotCount);
    if (version > 2) u16(liveCount);
    u16(worldEntityCount);

    size_t next = 0;
    for (size_t i = 0; i < slots.size(); ++i) {
        const EntitySlot& row = slots[i];
        u16(row.idA);
        u16(row.idB);
        u16(row.classIndex);
        if (row.classIndex == 0xffff) continue;
        // `entities` holds the occupied slots in slot order, so one cursor
        // walks both tables.
        if (next < entities.size() &&
            entities[next].slot == static_cast<int32_t>(i) + 1) {
            entities[next].esh.Write(out);
            ++next;
        }
    }
}

// ---------------------------------------------------------------------------
// .ent prototypes
// ---------------------------------------------------------------------------
const EntProto* EntProtoCache::Get(const std::string& entPath, std::string* errorOut) {
    if (entPath.empty()) {
        *errorOut = "no EntPath";
        return nullptr;
    }
    auto it = m_cache.find(entPath);
    if (it == m_cache.end()) {
        EntProto proto;
        if (m_index == nullptr || m_index->Root().empty()) {
            proto.error = "no --ents directory";
        } else if (!m_index->Find(entPath, &proto.path)) {
            proto.error = "not found in ent directory";
        } else {
            std::vector<uint8_t> file;
            std::string err;
            if (!ReadWholeFile(proto.path, &file, &err)) {
                proto.error = err;
            } else {
                Reader r(file);
                if (!proto.block.Read(r, &err)) {
                    proto.error = err;
                }
            }
        }
        it = m_cache.emplace(entPath, std::move(proto)).first;
    }
    if (!it->second.error.empty()) {
        *errorOut = it->second.error;
        return nullptr;
    }
    return &it->second;
}

void ApplyEntOverride(const EntProto* proto, EntityInstance* inst) {
    if (proto == nullptr) {
        return;
    }
    inst->entFound = true;
    inst->entResolvedPath = proto->path;
    const Esh& p = proto->block.esh;
    // Prototype-side properties (flag mask bit 0 clear): the .ent pass
    // overwrites whatever the .mis had.  `EntityStreamWriteInteger@005f7890` -- the read
    // returns early *only* when bit 0 is set, so everything else is taken from
    // the prototype, and if the prototype omits it the field is reset to its
    // compile-time default (l.20124).  A viewer cannot reset to a C++ default
    // it does not have, so an absent prototype property leaves the .mis value
    // standing; the difference is invisible for the fields drawn here because
    // every actor's .mis record already carries the same value.
    std::string s;
    if (p.GetString("Sprite", &s)) {  // flags 2, l.15211
        inst->spritePath = s;
    }
    if (p.GetString("Display Name", &s)) {  // flags 2, l.15161
        inst->displayName = s;
    }
    if (p.GetString("Internal Name", &s)) {
        inst->internalName = s;
    }
    if (p.GetString("Item Type", &s)) {
        inst->itemType = s;
    }
    if (p.GetString("Race Type", &s)) {
        inst->raceType = s;
    }
    // Deliberately NOT taken from the prototype:
    //   "frame" (position/orientation) -- guarded at l.15126, a .ent cannot
    //     move an instance;
    //   "EntPath"                      -- guarded by DAT_008bc4d8 at l.15276;
    //   "Player Index", "Tag Name", "animationRotation", "desiredTeam"
    //                                  -- flag mask 3 / 1, instance wins.
}

// ---------------------------------------------------------------------------
// Sprite / sequence / direction
// ---------------------------------------------------------------------------
std::string NormaliseSpriteName(const std::string& raw) {
    std::string out = Slashes(raw);
    if (out.size() < 4 || Lower(out.substr(out.size() - 4)) != ".spr") {
        out += ".spr";
    }
    return out;
}

SpritePick PickSprite(const Sprite& sprite, const EntityInstance& inst) {
    SpritePick pick;

    // Entity::UpdateSprite l.14268-14272: sequenceindex is set to 0xffff and
    // then SetAnimationType("default") is called, so a freshly loaded entity
    // shows the sprite's sequence *named* "default".  SetAnimationRelated
    // l.14081-14107 also falls back to "default" on any name miss and reports
    // "No default sequence for sprite" if that is missing too.
    for (size_t i = 0; i < sprite.sequences.size(); ++i) {
        if (Lower(sprite.sequences[i].name) == "default") {
            pick.sequence = static_cast<int32_t>(i);
            break;
        }
    }
    if (pick.sequence < 0) {
        // No "default": the original errors out.  Fall back to whatever the
        // .mis stored so something is still drawn, and say so.
        if (inst.sequenceIndex >= 0 &&
            inst.sequenceIndex < static_cast<int32_t>(sprite.sequences.size())) {
            pick.sequence = inst.sequenceIndex;
            pick.error = "no 'default' sequence; used stored sequenceIndex";
        } else if (!sprite.sequences.empty()) {
            pick.sequence = 0;
            pick.error = "no 'default' sequence; used sequence 0";
        } else {
            pick.error = "sprite has no sequences";
            return pick;
        }
    }
    const Sequence& seq = sprite.sequences[static_cast<size_t>(pick.sequence)];
    pick.sequenceName = seq.name;
    pick.collection = seq.collectionId;
    if (pick.collection < 0 || pick.collection >= static_cast<int32_t>(sprite.collections.size())) {
        pick.error = "sequence collection id out of range";
        return pick;
    }
    const Collection& col = sprite.collections[static_cast<size_t>(pick.collection)];
    pick.dirCount = col.dirCount;
    if (col.frameCount <= 0 || col.dirCount <= 0) {
        pick.error = "empty collection";
        return pick;
    }

    // At rest, sequenceframe is the 0xffff sentinel (l.14197) and
    // ProcessAnimation -- which is not in the decompilation -- advances it.
    // InitDefaultEntityFields zeroes both cursors (l.14479-14483), so the rest
    // frame is the sequence's first real table entry.  Event markers have
    // negative ids and are not frames.
    pick.frame = 0;
    for (const Frame& f : seq.frames) {
        if (f.id >= 0) {
            pick.frame = f.id;
            break;
        }
    }
    if (pick.frame >= col.frameCount) {
        pick.frame = 0;
    }

    // Direction.  Entity_UpdateFacingFromVelocityAndBounds l.14343-14350:
    //   dir = round(theta / 2pi * dirCount)
    //   if (dir >= dirCount) dir -= dirCount;
    //   if (dir >= dirCount) dir = 0;
    // The angle is about +Y and the inverse used to rebuild the matrix is
    // `angle = dir * 2pi / dirCount` (l.15487-15488).  The quantiser's own
    // degenerate branch (l.14333-14340) fires on m02 == 0 and takes its
    // +-pi/2 from the sign of m00, so the angle is atan2(m00, m02) -- x over
    // z.  MILESTONE 13 corrected this from atan2(m02, m00); see game/NOTES.md
    // section 63.
    int32_t dir = 0;
    if (inst.hasRotation) {
        dir = inst.animationRotation;
    } else if (inst.hasFrame) {
        double theta = std::atan2(static_cast<double>(inst.rot[0]),
                                  static_cast<double>(inst.rot[2]));
        if (theta < 0.0) {
            theta += kTwoPi;
        }
        dir = static_cast<int32_t>(
            std::floor(theta / kTwoPi * static_cast<double>(col.dirCount) + 0.5));
    }
    // The original's cheap modulo, reproduced including the second test --
    // which is what makes an out-of-range stored index collapse to 0 rather
    // than wrap (l.15366-15373 does the same on load).
    if (dir < 0) {
        dir = 0;
    }
    if (dir >= col.dirCount) {
        dir -= col.dirCount;
    }
    if (dir >= col.dirCount) {
        dir = 0;
    }
    pick.direction = dir;
    pick.ok = true;
    return pick;
}

}  // namespace ft
