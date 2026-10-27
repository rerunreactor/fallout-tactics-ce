// ent.h -- entity instances in a `.mis`, `.ent` prototypes, and the sprite /
// sequence / direction selection that decides what to draw for one.
//
// Sources, all in the Ghidra export unless noted:
//
//   World::ReadEntityFileRecordsFromStream @004e52d0 (l.1590) --
//     the <entity_file> section: the class-name table, then a slot table.
//   Entity::ClassType::Serialize @005e3450 (l.15030) -- the per-instance
//     property set: "frame" (l.15106-15125), "EntPath" (l.15284), "Sprite"
//     (l.15211), "Player_Index" (l.15206), "sequenceIndex"/"animationFrame"/
//     "animationRotation" (l.15214-15234).
//   Entity::UpdateSprite (l.14234) and Entity::SetAnimationType (l.15008) --
//     a freshly loaded entity's animation is the sprite's sequence *named*
//     "default" (l.14269: FOTString::operator=(&param_1, s_default_00897f14)).
//   Entity::Entity_UpdateFacingFromVelocityAndBounds (l.14315) -- the direction
//     index and its clamp against the collection's direction count.
//   SoftwareRender::RenderLitObject -- the blit
//     position: four layers at direction-record +0x10/+0x20/+0x30/+0x40, each
//     drawn at (layer.x + screenX, layer.y + screenY).

#ifndef MAPVIEW_ENT_H
#define MAPVIEW_ENT_H

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "esh.h"
#include "spr.h"

namespace ft {

// A case-insensitive index of a directory tree, keyed by the path relative to
// the root with '\' folded to '/'.  The names inside a `.mis` are written with
// whatever case and separators the map's author typed, which do not match what
// came out of the .bos archives.  Generalised from mapview's original
// TileIndex, which did the same job for `tiles/` only.
class PathIndex {
public:
    PathIndex() = default;
    // `stripPrefix` is the leading directory the archive names carry but the
    // extracted tree may not, e.g. "tiles/", "entities/", "sprites/".  Lookup
    // tries the name as given and then with the prefix removed.
    PathIndex(const std::string& root, const std::string& stripPrefix,
              const std::string& extensionFilter);

    bool Find(const std::string& name, std::string* out) const;
    size_t Size() const { return m_byRel.size(); }
    const std::string& Root() const { return m_root; }

private:
    std::string m_root, m_strip;
    std::unordered_map<std::string, std::string> m_byRel;
};

// The four 5-float colour records every entity carries, in the order they sit
// in EntityMembers: basecolor +0xB3, haircolor +0xC7, skincolor +0xDB,
// teamcolor +0xEF (Entity::InitDefaultEntityFields l.14489-14523; the stride is
// 0x14 = 5 floats, pinned by the raw-offset sibling at l.13919-13966).  The
// `.mis` names them "Base", "Hair", "Skin" and "Team".
enum EntityColourSlot { kColourBase = 0, kColourHair = 1, kColourSkin = 2, kColourTeam = 3 };
extern const char* const kEntityColourNames[4];

struct EntityInstance {
    // Slot-table bookkeeping.
    int32_t slot = 0;
    uint16_t idA = 0, idB = 0;  // EntityTable[i]+4 and +6; one is the id, one the sequence number
    int32_t classIndex = -1;
    std::string className;

    Esh esh;  // the instance's own property block, as stored in the .mis

    // Pulled out of `esh` (and, for the prototype-side ones, out of the `.ent`).
    std::string entPath, displayName, internalName, tagName, itemType, raceType, posture;
    std::string spritePath;      // after the prototype override rule
    std::string spriteFromMis;   // what the .mis itself said

    bool hasFrame = false;
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float pos[3] = {0, 0, 0};    // the "frame" property's translation, in metres
    int32_t world[3] = {0, 0, 0};  // pos * kEntityWorldScale, rounded -- tile units

    bool hasRotation = false;
    int32_t animationRotation = 0;
    int32_t sequenceIndex = -1, animationFrame = -1;
    int32_t playerIndex = -1, desiredTeam = -1;

    bool hasColour[4] = {false, false, false, false};
    float colour[4][5] = {};

    // Prototype resolution.
    bool entFound = false;
    std::string entResolvedPath, entError;
};

// The world stream stores an entity's position in metres; the tile map stores
// integers.  `_DAT_0082431c` in Entity::ClassType::Serialize l.15133-15139 is
// the float-to-tile scale and Ghidra did not resolve it; it is 4, pinned by the
// data -- see NOTES.md.
constexpr float kEntityWorldScale = 4.0f;

// One row of the slot table: the two ids and the class index, or 0xffff for a
// slot that holds nothing.  Kept in full so the table can be written back.
struct EntitySlot {
    uint16_t idA = 0, idB = 0, classIndex = 0xffff;
};

struct EntityFile {
    int32_t version = 0;
    std::vector<std::string> classNames;
    // Whether the class-name table was stored in the 16-bit string form: a
    // world written by `World::Save` uses it, an editor-written `.mis` does not.
    bool classNamesWide = false;
    // Slots 1 .. slotCount-1, in order.  `entities[i].slot` indexes into this
    // table (offset by one), which is what lets the section be re-emitted.
    std::vector<EntitySlot> slots;
    uint16_t slotCount = 0;       // includes the reserved slot 0
    uint16_t liveCount = 0;       // version >= 3 only
    uint16_t worldEntityCount = 0;
    std::vector<EntityInstance> entities;   // the occupied slots, in slot order
    size_t emptySlots = 0;
    std::string error;

    // Reads from the "<entity_file>" token onwards.
    bool Read(Reader& stream, std::string* errorOut);

    // `World::SaveEntityRecordsWithProgress @004e5860`: the token, the class
    // table, the three counts, then per slot the two ids, the class index and
    // -- for an occupied slot -- the entity's own <esh>.  Re-emitting a section
    // read by `Read` reproduces its bytes exactly.
    void Write(std::vector<uint8_t>* out) const;
};

// One loaded `.ent` prototype.
struct EntProto {
    std::string path;
    EntityBlock block;
    std::string error;
};

class EntProtoCache {
public:
    explicit EntProtoCache(const PathIndex* index) : m_index(index) {}
    // Returns nullptr when the path is empty or the file cannot be found/read;
    // `errorOut` then says why.
    const EntProto* Get(const std::string& entPath, std::string* errorOut);
    size_t Size() const { return m_cache.size(); }

private:
    const PathIndex* m_index;
    std::map<std::string, EntProto> m_cache;
};

// Applies the prototype-override rule to one instance: the `.ent` pass reruns
// the same serializers with stream+0x39 set, and every property whose flag mask
// has bit 0 clear is taken from the prototype (`EntityStreamWriteInteger@005f7890`).
// `Sprite` (flags 2) and `Display Name` (flags 2) are prototype-side;
// `Player_Index`, `Tag_Name` and `animationRotation` (flags 3) and the "frame"
// position (guarded outright at l.15126) stay with the instance.
void ApplyEntOverride(const EntProto* proto, EntityInstance* inst);

// What to draw for one entity, once its sprite is loaded.
struct SpritePick {
    bool ok = false;
    std::string error;
    int32_t sequence = -1;     // index into Sprite::sequences
    std::string sequenceName;
    int32_t collection = -1;
    int32_t frame = 0;         // collection frame index
    int32_t direction = 0;
    int32_t dirCount = 0;
};

// Entity::UpdateSprite -> SetAnimationType("default"); the direction comes from
// `animationRotation` when the instance stored one and otherwise from the
// "frame" property's orientation matrix.
SpritePick PickSprite(const Sprite& sprite, const EntityInstance& inst);

// Normalises a stored sprite name to a lookup key: '\' to '/', and a ".spr"
// extension appended when absent (Actor::UpdateSprite l.3586 stores
// "sprites\critters\BrahminTippable" with no extension).
std::string NormaliseSpriteName(const std::string& raw);

}  // namespace ft

#endif  // MAPVIEW_ENT_H
