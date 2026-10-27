// campaign.h -- milestone 12: the campaign layer.
//
// Three things live here:
//
//   * `CampaignFile`  -- the binary `campaigns/bos.cam`, whose whole wire
//     format is `Campaign::SaveCampaignProperties @0069f330`
//     (`Campaign::SaveCampaignProperties@0069f330`, `Campaign::SaveMissionToCampaign@006a0020`) read as a symmetric serializer,
//     with the four record codecs it calls out to:
//       `SerializeCampaignTileProperties @0069c120`  -> "<campaign_tile>" 2
//       `SerializeRandomForceProperties  @0069baf0`  -> "<random_force>" 2
//       `VarTable::SaveVarTableProperties @00500b50` -> "varTableHeader" 1
//       `CoreMission::Serialize @0069b220`           -> one "<esh>" per record
//     (the last three were exported from the binary for this milestone).
//
//   * `CampaignTables` -- `tables/campaign.txt`, the DESIGNER source for the
//     same material, parsed by `Campaign::LoadCampaignCoresSection @006a3f80`,
//     `...SpecialsSection @006a4840`, `...ForcesSection @006a4f40`,
//     `...LandscapesSection @006a5c20` and `...PrefabsSection @006a5f90`.
//     The shipped `.cam` already carries the merged result of all five, so the
//     text file is parsed only for the two sections the `.cam` stores without
//     their designer names (`quarterMaster`, `recruitsPool` are in both) and
//     as a cross-check that the binary parse agrees.
//
//   * `CampaignState` -- the running campaign: where the squad is on the world
//     map, what time it is, which locations are covered/uncovered/visited, the
//     roster and its carried-over state, the recruit pool, the quartermaster
//     stock, and the campaign variable table.  This is what a `.sav` holds.
//
// COORDINATES.  The world map is a `gridW x gridH` array of `tilePx`-square
// tiles: 65 x 34 of 75 px on the shipped campaign, so 4875 x 2550 px.  Core
// mission `Loc x` / `Loc y` are in those pixels, and so is the squad marker
// (`Campaign+0xa5`/`+0xa9`, floats, 4322/867 in the shipped file, which is
// mission01's own 4326/868 within rounding).

#ifndef GAME_CAMPAIGN_H
#define GAME_CAMPAIGN_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "esh.h"

namespace game {

// ---------------------------------------------------------------------------
// `<random_force>` 2, and the `forces = { ... }` section of campaign.txt.
// The text form is
//     { "name;align;min;max;enabled;cdCode",
//       "entities/....ent;level;chance", ... }
// and the binary form is name, align, i32 min, i32 max, u8 enabled, a string
// vector of member paths, an int vector of levels, an int vector of chances,
// and the cd code -- field for field the same thing.
// ---------------------------------------------------------------------------
struct ForceMember {
    std::string entPath;
    int32_t level = 0;
    int32_t chance = 0;
};

struct ForceDef {
    std::string name;
    std::string align;      // "critter" / "bad" / "good" / ...
    int32_t minLevel = 0;   // the squad-level band this force may be rolled for
    int32_t maxLevel = 0;
    bool enabled = false;
    std::string cdCode;     // "Z", "A".."E" -- which CD the art is on
    std::vector<ForceMember> members;
};

// ---------------------------------------------------------------------------
// `CoreMission` -- one `<esh>` record, written by `CoreMission::Serialize
// @0069b220` on top of `CampaignMission::Serialize`.  Properties seen in the
// shipped file: "Name" (the locale key), "Loc x", "Loc y", "Radius",
// "MissionFile", "State" and the nested "esbin".  `State` defaults to
// "Covered" and `Is Bunker` to false, which is why only the records that
// differ carry them.
// ---------------------------------------------------------------------------
enum class MissionState : int32_t { Covered = 0, UnCovered = 1, Visited = 2 };

struct CoreMission {
    std::string nameKey;        // "mission_name_01"
    std::string misPath;        // "campaigns/missions/core/mission01.mis"
    int32_t locX = 0, locY = 0; // world-map pixels
    int32_t radius = 37;
    MissionState state = MissionState::Covered;
    bool isBunker = false;
    bool vehicleMission = false;
    bool specialMission = false;
};

// `SpecialMission`, the unique random encounters.  Same esh shape minus the
// position (they are placed where the encounter fires) plus "Should Morph".
struct SpecialMission {
    std::string nameKey;
    std::string misPath;
    int32_t radius = 20;
    bool morph = false;      // "Should Morph": becomes a permanent core marker
    bool onceOnly = true;
    bool used = false;       // runtime
    int32_t locX = -1, locY = -1;
};

// `{ tag ; count ; path }` in campaign.txt's `quarterMaster`; on the wire a
// (string, string, int) triple.  The tag is a campaign variable name; the
// entry becomes buyable once that tag is set.
struct StockEntry {
    std::string tag;
    std::string entPath;
    int32_t count = 0;
};

// `{ tag ; op ; path }` in campaign.txt's `recruitsPool`; on the wire a
// (string, string, string) triple in the order (tag, path, op).
struct RecruitEntry {
    std::string tag;
    std::string entPath;
    std::string op;          // "add" / "remove"
};

// One `<campaign_tile>` 2: three parallel weight vectors, one entry per
// random force, per special mission and per landscape, plus an optional
// `<zar>` of the tile's own art.  The vectors are the ENCOUNTER TABLE for the
// square: the shipped values are 0/5/10/15/20.
struct CampaignTile {
    // `tile+0x00`, the fog-of-war state: 0 unexplored, 1 and 2 seen.  It is
    // NOT in the `<campaign_tile>` record -- `SaveCampaignProperties` writes a
    // second gridW x gridH int array of them after the world bitmap
    // (l.17475-17495), and every one of them is 0 in the shipped file, i.e.
    // the campaign starts with the whole map dark.  `RevealFogOfWarAroundTile`
    // (called from `WWorldWindow::Update` l.23129) is what clears it.
    int32_t fog = 0;
    std::vector<int32_t> forceChance;      // tile+0x04, one per CampaignFile::forces
    std::vector<int32_t> specialChance;    // tile+0x14, one per CampaignFile::specials
    std::vector<int32_t> landscapeChance;  // tile+0x24, one per CampaignFile::landscapes
    // tile+0x34, the tile's own 75 x 75 `<zar>`: the world map picture is this
    // grid of them, not the bitmap (see WorldTerrain).
    bool hasArt = false;
    int32_t artW = 0, artH = 0;
    size_t artOffset = 0, artSize = 0;     // into CampaignFile::raw
};

// `Campaign+0x1a5`, the payload `WriteBitmapToDeviceTimed` writes after the
// tile array: an 8-bit GREYSCALE Windows BMP, 2450 x 1292 in the shipped file,
// which is exactly half the 4875 x 2550 world extent.  It is the TERRAIN SPEED
// map, not the picture: `WWorldWindow::Update` samples it at
// `pos * _DAT_007fd93c` (= 0.5, l.23104), divides the byte by 255
// (`_DAT_007fd78c`, l.23115) and multiplies the travel step by the result;
// below `_DAT_007fe074` the square is impassable and travel stops (l.23117).
struct WorldTerrain {
    int32_t w = 0, h = 0;
    std::vector<uint8_t> grey;
    bool ok = false;
    // The speed multiplier at a world-map pixel, 0..1.
    float Speed(double px, double py) const;
};

class CampaignFile {
public:
    bool Load(const std::string& path, std::string* errorOut);

    int32_t gridW = 0, gridH = 0, tilePx = 0;
    std::vector<CampaignTile> tiles;   // [x * gridH + y]
    WorldTerrain terrain;
    std::vector<uint8_t> raw;          // the whole file, so tile art can be decoded lazily
    // The tile's own art, decoded to RGBA on first use.
    bool TileArt(int32_t tx, int32_t ty, std::vector<uint8_t>* rgba) const;

    std::string startMission;          // Campaign+0x10
    float viewX = 0, viewY = 0;        // Campaign+0x9d, +0xa1
    float squadX = 0, squadY = 0;      // Campaign+0xa5, +0xa9

    std::vector<CoreMission> cores;
    std::vector<SpecialMission> specials;
    std::vector<ForceDef> forces;
    std::vector<std::string> landscapes;   // Campaign+0x1fd
    std::vector<std::string> prefabs;      // Campaign+0x1bd
    std::vector<StockEntry> quarterMaster; // Campaign+0x141
    std::vector<RecruitEntry> recruitPool; // Campaign+0x171
    std::vector<std::string> varNames, varValues;

    int32_t fileVersion = 0;
    std::string info;

    // World-map pixel extent.
    int32_t PixelW() const { return gridW * tilePx; }
    int32_t PixelH() const { return gridH * tilePx; }
    const CampaignTile* TileAtPixel(int32_t px, int32_t py) const;
    // The core mission whose radius contains (px, py), or -1.
    int32_t CoreAtPixel(int32_t px, int32_t py) const;
};

// ---------------------------------------------------------------------------
// `tables/campaign.txt`, the designer source.  Only used as a cross-check and
// as the fallback when no `.cam` is present.
// ---------------------------------------------------------------------------
struct CampaignTables {
    std::vector<ForceDef> forces;
    std::vector<CoreMission> cores;
    std::vector<SpecialMission> specials;
    std::vector<std::string> landscapes, prefabs;
    std::vector<StockEntry> quarterMaster;
    std::vector<RecruitEntry> recruitPool;

    bool Load(const std::string& path, std::string* errorOut);
};

// ---------------------------------------------------------------------------
// The running campaign.
// ---------------------------------------------------------------------------

// One squad member's carried state.  These are the fields `World::Save`'s
// entity records keep for a player-side Actor and that the next mission's
// spawn has to restore: see NOTES.md.
struct RosterItem {
    std::string cls, displayName, itemType, entPath, spritePath;
    int32_t count = 1;
};

struct RosterMember {
    std::string entPath;          // "entities/recruits/hm01.ent"
    std::string displayName;      // the portrait key
    std::string rank;
    std::string tag;              // the in-mission tag, if any
    int32_t hp = 0, maxHp = 0;
    int32_t xp = 0;
    int32_t level = 1;
    bool dead = false;
    // The equipped slots, by ent path, plus the pack.
    std::string hand1, hand2, armour;
    int32_t ammo = 0;
    std::vector<RosterItem> inventory;
    bool inSquad = false;         // one of the six deployed
};

struct EncounterRoll {
    bool fired = false;
    std::string forceName;
    std::string mapPath;
    int32_t roll = 0, threshold = 0;
    int32_t special = -1;             // index into CampaignFile::specials, or -1
    std::vector<std::string> spawn;   // entity paths
    std::vector<int32_t> spawnLevel;
};

// The save-game header, `SaveGameHeader::SaveGameHeaderProperties @004bfea0`.
struct SaveGameHeader {
    // MILESTONE 24: these are the five strings the shipped file carries, in
    // its own order.  See `WriteSaveHeader`.
    std::string name;         // +0x05, the save file's own stem
    std::string playerName;   // +0x09, "Mick"
    std::string missionName;  // +0x0d, "Brahmin Wood"
    std::string timestamp;    // +0x11, "Jan 1 2197.  07:45"
    int64_t timeMs = 0;
    int32_t version = 0;
    int32_t squadCount = 0;
};

class CampaignState {
public:
    // `campaignRoot` is the directory holding `campaigns/` and `tables/`.
    bool New(const std::string& assetsDir, std::string* errorOut);

    CampaignFile file;
    CampaignTables tables;
    std::string campaignPath;

    // The per-run copy of the mission markers: `Campaign+0x1cd`, the vector a
    // save carries and `ActionSetMissionState` mutates.
    std::vector<CoreMission> cores;

    // Where the squad is, in world-map pixels, and where it is heading.
    double posX = 0, posY = 0;
    double destX = 0, destY = 0;
    double viewX = 0, viewY = 0;
    bool travelling = false;
    int32_t destCore = -1;
    uint32_t rng = 1;

    // The screenshot a save carries in its `<saveh>` block.
    std::vector<uint8_t> thumb;
    int32_t thumbW = 0, thumbH = 0;

    // MILESTONE 24: the three things the shipped game's Load Game row draws,
    // and the six portraits beside them.  A save written without these showed
    // an absolute Mac path where the real game shows
    // "Mick / Brahmin Wood / Jan 1 2197.  07:45" and drew no faces at all.
    // `SaveGameHeader::SaveGameHeaderProperties @004bfea0` writes the strings
    // at +0x09 / +0x0d and the six `ImageZAR`s at +0x21; the 24 bytes at +0x39
    // are six floats and the shipped save's are the members' hit points.
    std::string playerName;    // the chosen pre-made character, "Mick"
    std::string missionName;   // the mission's DISPLAY name, "Brahmin Wood"
    std::string missionNameKey;// its locale KEY, "mission_name_01"; a <campaign> tail field
    struct SavePortrait {
        int32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;   // straight alpha, RGBA byte order
        float hp = 0.0f;
    };
    std::vector<SavePortrait> portraits;   // up to six, in squad order
    std::string saveLabel;

    // Campaign time.  `Campaign+0x48` is an int64 the save carries; the world
    // clock the missions get is derived from it.
    int64_t timeMs = 0;

    std::vector<RosterMember> roster;
    std::vector<std::string> pool;        // recruit .ent paths available now
    std::map<std::string, int32_t> stock; // quartermaster: ent path -> count
    std::map<std::string, std::string> vars;   // the campaign VarTable

    std::string currentMission;           // "" on the world map
    int32_t reputation = 0;

    // --- travel ----------------------------------------------------------
    // `WWorldWindow::Update @006ba780` is a
    // straight-line constant-speed lerp in world-map pixels:
    //
    //   dt   = clamp(realSeconds, <= 0.1)                      l.23067-23069
    //   step = dt * 30.0 * campaign.speed * terrain * K        l.23088-23092
    //   pos += normalise(dest - pos) * step                    l.23094-23127
    //   arrive when |pos - dest| < step on both axes           l.23131-23140
    //
    // 30.0 is `_DAT_0080afc0`, which milestone 8 already resolved
    // (game/combat.h's kCloseBonus reads the same address).  `campaign.speed`
    // is the serialized property `Campaign+0x23` and is 1.0f in the shipped
    // `bos.cam`.  `terrain` is `WorldTerrain::Speed`.  K is `_DAT_0082daec`,
    // an unresolved data global with no cross-reference anywhere in the dumps
    // -- it is a PLACEHOLDER at 1.0 here, which makes the model 30 px/s on
    // open ground and puts the whole 4,875 px map about three minutes across.
    static constexpr double kBaseSpeed = 30.0;        // _DAT_0080afc0
    static constexpr double kSpeedFactorK = 1.0;      // _DAT_0082daec, PLACEHOLDER
    static constexpr double kMaxFrameSeconds = 0.1;   // _DAT_007ff0c8
    // `_DAT_007fe074`: below this terrain multiplier the square is impassable
    // and travel stops.  Unresolved; 1/255 (one grey level) is the smallest
    // value that keeps every non-black square passable.
    static constexpr float kImpassable = 1.0f / 255.0f;
    // `_DAT_0082dae8`, the master encounter-probability scale used by both
    // rolls (l.23260, l.23388, l.23397).  Unresolved and un-cross-referenced;
    // PLACEHOLDER.
    // With the roll's own shape -- (maxWeight/100) * K * step * 100 per tick,
    // and a step of about one world pixel -- this value gives about one
    // encounter per 500 pixels of travel on the shipped map's typical
    // weight-15 square, i.e. roughly one every seven campaign tiles.
    static constexpr double kEncounterScale = 1.5e-4;
    // The encounter check runs on a global ~10 ms clock (l.23206-23210) --
    // `_DAT_00809560 / _DAT_0082daf0`, the numerator inferred as 1000.0 from
    // its use elsewhere as a seconds->ms factor.
    static constexpr int64_t kEncounterTickMs = 10;
    // `WWorldWindow__ProcessWorldMapTick @006bcaa0` l.24237: the marker blinks
    // and the mission is entered on the eighth tick.
    static constexpr int32_t kEncounterCountdown = 8;

    void SetDestination(double px, double py);
    // One frame of world-map time.  `deltaMs` is real time; the return value
    // carries an encounter when one fires.
    EncounterRoll Step(double deltaMs);
    bool encountersEnabled = true;
    float speed = 1.0f;              // Campaign+0x23
    bool alwaysEncounter = false;    // Campaign+0x22
    int32_t avoidChance = 0;         // Campaign+0x1c
    int32_t campaignLevel = 10;      // Campaign+0x18
    int64_t encounterClock = 0;
    double travelled = 0;
    int32_t SquadLevel() const;
    // The MSVC LCG `WWorldWindow` seeds per window (l.24246): state = state *
    // 0x343fd + 0x269ec3, value = (state >> 16) & 0x7fff.
    int32_t Rand();
    void RevealFog(double px, double py);

    // --- progression -----------------------------------------------------
    void SetMissionState(const std::string& nameKey, MissionState st);
    int32_t FindCore(const std::string& nameKeyOrPath) const;
    void ApplyTag(const std::string& tag);      // recruit pool + stock gates
    void SetVar(const std::string& k, const std::string& v);
    std::string Var(const std::string& k) const;

    std::vector<std::string> DeployedPaths() const;

    // --- saves -----------------------------------------------------------
    bool Save(const std::string& path, const std::string& label,
              std::string* errorOut) const;
    bool LoadSave(const std::string& path, std::string* errorOut);
    // What the save browser reads out of a slot without loading it
    // (`WSaveGameItem::EnsureSaveGameHeaderLoaded @004bf560`: a plain read
    // from offset 0, no seek).
    static bool ReadHeaderOnly(const std::string& path, SaveGameHeader* out,
                               std::string* errorOut);

    // The in-mission world blob a save carries, when it was taken mid-mission.
    std::vector<uint8_t> missionBlob;
    // MILESTONE 22: the name the blob is stored under inside the `.sav`
    // container -- `user/$$current$$/<missionBase>.sav`, which is the name
    // `Campaign::MatchesMissionName @006a0a10` accepts.
    std::string missionBlobName;

    std::vector<std::string> log;
    void Logf(const char* fmt, ...);
};

}  // namespace game

#endif  // GAME_CAMPAIGN_H
