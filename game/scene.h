// scene.h -- milestone 6: the live scene.
//
// A `Scene` owns one loaded mission and everything derived from it that the
// per-frame path needs: the decoded tile prototypes, the resolved entities with
// their animation cursors, and the light list.  `Update(ms)` advances the
// simulation; `Render(...)` paints one window-sized frame; `Pick(...)` maps a
// screen point to an entity.
//
// Everything geometric and photometric here is mapview's, reused verbatim --
// mapview/main.cpp's RenderWorld is the source for the tile/entity placement,
// the tint pipeline and the two draw passes, and the pieces of it that live in
// its anonymous namespace are copied into scene.cpp with a comment naming the
// origin (see NOTES.md "What was copied out of mapview/main.cpp").
//
// What is NEW in milestone 6, and where it comes from, is in NOTES.md:
//   * the tick model   World::AdvanceWorldSimulationTimeStep @004fbf50, and the
//                      0.07f at World+0x2c1 that World::World @005046b0 stores
//   * the frame clock  Entity::ProcessAnimation @005e2b00, Entity::CheckThinkTimers
//                      @005e7500, Entity::SetAnimationRelated @005df6d0
//   * the idle rule    Actor::FinishedAnimationSequenceMaybe @00542ac0 ->
//                      Actor::Vtable225ThinkRelated @00541f90 ("Breathe")
//   * the viewport     UpdateScreenOffsetFromWorldPosition @006e13d0,
//                      TransformWorldToScreen @006e14f0
//   * the pick         WGameWorld::FindPrevVisibleTileOfType @0049d500, the
//                      reverse walk of the list CollectVisibleEntitiesInViewport
//                      @006e22a0 built
//   * the marker       NOT recoverable; see NOTES.md section 6

#ifndef GAME_SCENE_H
#define GAME_SCENE_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ai.h"
#include "combat.h"
#include "ent.h"
#include "move.h"
#include "shared.h"
#include "esh.h"
#include "iso.h"
#include "light.h"
#include "mis.h"
#include "script.h"
#include "audio.h"
#include "audio_names.h"
#include "sort.h"
#include "spr.h"
#include "til.h"

namespace game {

using namespace ft;

// The engine tick.  `World::World @005046b0` stores
// 0x3D8F5C29 == 0.07f at World+0x2c1 and 0x3DCCCCCD == 0.1f at World+0x2bd;
// `World::AdvanceWorldSimulationTimeStep @004fbf50`
// clamps the real-time delta to the second, accumulates it into World+0x2e7,
// and runs one `AdvanceWorldStateOneTick` per 0.07 s while the accumulator is
// ahead of `World.realtime` (World+0x2f7), which it then sets to
// `tickCount * 0.07`.  So the simulation clock only ever takes the values
// n * 70 ms, and every animation deadline is quantised up to one of them.
constexpr int64_t kTickMs = 70;         // World+0x2c1
constexpr int64_t kMaxFrameMs = 100;    // World+0x2bd

// `Entity::SetAnimationRelated @005df6d0` sets
// sequencedelay to 0x42 == 66 on every sequence change, and the ev_time_of_display
// opcode overrides it per frame.  The unit is milliseconds: ProcessAnimation
// computes `NextEngineThink = sequencedelay * _DAT_0080a6e8 + World.realtime`
// with _DAT_0080a6e8 == 0.001f.
constexpr int16_t kDefaultSequenceDelayMs = 66;

// MILESTONE 13.  How many rings `Entity::Entity_FindNearestFreeGroundTileAndReposition
// @00587690` walks before giving up.  The original's bound is
// `param_1_00[5] - param_1_00[2]`, two ints of the search object whose field
// layout is in no dump.  RECONSTRUCTION: eight rings, which at kNavCell == 1
// is eight world units -- a little over a tile -- and is enough to seat a
// six-member squad around one point.
constexpr int32_t kFreeTileRings = 8;

// How many consecutive ticks an actor stands blocked before it replans.
// RECONSTRUCTION: the original replans off the blocker recorded at the search
// object's +0x80 and no dump shows the debounce.
constexpr int32_t kBlockedTicksBeforeRepath = 3;

// MILESTONE 13.  The Rand Move wander: how far one leg goes and how long the
// unit rests before the direction is re-rolled.  Both are RECONSTRUCTIONS --
// `Controller_SetWaypointToTargetUsingVelocity`'s stride and the controller's
// own re-roll cadence are in no dump.
constexpr float kRandMoveStep = 24.0f;         // world units, four tiles
constexpr double kRandMoveHoldSeconds = 4.0;

// MILESTONE 14.  RENDER-SIDE INTERPOLATION -- a PORT ADDITION, and NOT a
// reconstruction of anything the binary does.  See NOTES.md section 72: the
// original advances an actor's position exactly once per 70 ms tick, inside
// `Actor::DoEngineThink @0056c090`, which calls `Entity::ProcessAnimation` and
// then `Actor::Move @0055ba20` with no delta time at all -- the step length is
// the current animation frame's own `frameData2` entry.  Nothing on the render
// side extrapolates: `UpdateEntityRenderTransform @006e1c10`
// hands the entity's CURRENT world triple straight to
// `TransformWorldToScreen`.  At the ~14 fps the original drew at, position and
// animation stepped together and it read as smooth; at fotgame's 100+ fps the
// same 70 ms hold is a visible stutter.  So the render position is linearly
// interpolated between the previous tick's and the current tick's positions.
// A step longer than this is a teleport, not a walk, and is not interpolated:
// the longest real step is StandRun's 0.5 m == 2 world units per tick.
constexpr float kInterpSnapWorldUnits = 8.0f;


// kLuma*, DrawTint, kEntityHalfExtent/kEntityHeight, DecodedTile,
// LoadedSprite and SpriteCache come from engine/shared.h (milestone 7 step 0).

// ---------------------------------------------------------------------------
// Per-frame RGBA of one image, decoded once and cached.
// ---------------------------------------------------------------------------
struct Bitmap {
    int32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
};



// ---------------------------------------------------------------------------
// One live entity.
// ---------------------------------------------------------------------------
struct LiveEntity {
    size_t misIndex = 0;
    std::string className, label, spritePath;
    // The `.ent`'s "Race Type" (`Entity::GetEntityCharacterTypeString`): the
    // unarmed table is keyed on it, and so is whether armour can be worn at
    // all -- a size-variant body ("RaiderLarge", "RaiderMaleHuge") has no
    // armoured character sprite.
    std::string raceType;

    LoadedSprite* ls = nullptr;
    int32_t sequence = -1, collection = -1, direction = 0, dirCount = 1;
    std::string sequenceName;

    // The animation cursor.  These are Entity's own fields:
    //   cursor  == Entity.e_.sequenceframe, an index into the RAW int16 frame
    //              table (event markers and their parameters included).  It
    //              starts at the 0xffff sentinel, which ProcessAnimation's
    //              leading `++` turns into 0.
    //   frame   == Entity.e_.animationframe, the collection frame index.
    //   delayMs == Entity.e_.sequencedelay.
    //   nextThinkMs == Entity.ct_.NextEngineThink, in World.realtime.
    const std::vector<int16_t>* table = nullptr;   // the reconstructed raw table
    int32_t cursor = -1;
    int32_t frame = 0;
    int16_t delayMs = kDefaultSequenceDelayMs;
    int64_t nextThinkMs = 0;
    bool animates = false;     // more than one real frame in the sequence
    std::string posture = "Stand";

    // --- milestone 7: movement -------------------------------------------
    // `Actor.currentaction` (Actor+0xf8e).  "" is the idle that
    // Vtable225ThinkRelated re-selects Breathe from; "Walk" and "Run" are the
    // two moving actions of Actor::SelectDestinationAnimationState; a Door
    // holds "Opening" / "Opened" / "Closing" / "Closed" here instead
    // (Door::SetActivationState @005b2690).
    std::string action;
    bool isActor = false;
    bool isDoor = false;
    // Sprite+0x0c..0x0e, which Entity::UpdateSprite turns into the entity's
    // bounding box: box = pos +/- bbox/2 on x
    // and z, pos .. pos + bbox.y vertically.
    int32_t bbox[3] = {2 * kEntityHalfExtent, kEntityHeight, 2 * kEntityHalfExtent};

    std::vector<PathPoint> path;   // world units, current node first
    size_t pathAt = 0;
    bool moving = false;
    bool running = false;
    int32_t goal[3] = {0, 0, 0};   // the ordered destination, world units
    // The stance change in flight, if any: the sequence is playing and
    // `pendingPosture` is what the actor becomes when it ends.
    std::string pendingPosture;
    bool hasOrder = false;         // has ever been given a move order (debug)
    int64_t arrivedTick = -1;      // the tick StepMovement finished the path
    // MILESTONE 13: local avoidance.  `ComputeDirectionToFocusWithObstacleCheck
    // @00536410` records the entity blocking the step it was about to take at
    // search+0x80; these are how long this actor has been standing on such a
    // block and when it last replanned around one.
    int32_t blockedTicks = 0;
    int64_t repathTick = -1;

    // Doors.
    bool doorOpen = false;
    bool doorOutwards = false;
    int64_t doorHoldUntilMs = 0;

    // --- milestone 8: combat -------------------------------------------
    bool isCombatant = false;
    Attributes attr;
    WeaponDef weapon;          // Hand1, the active slot
    ArmourDef armour;          // the Armour slot
    int32_t hp = 0, maxHp = 0;
    // `Actor+0x289` is a float and `Actor::AdjustAP @0056fcf0` clamps it with a
    // 0.05 deadband, so AP is a float here too.
    float ap = 0.0f;
    int32_t maxAp = 0;
    int32_t ammo = 0;          // rounds in the magazine
    int32_t ammoReserve = 0;   // spare rounds, summed out of the Inventory block
    int32_t playerIndex = -1;  // the .mis Player_Index; the team, for hostility
    // The `.mis` "Tag Name" property, which is what every `<Trigger>` Unit
    // reference names ('Elder', 'Hawkeye', 'Leader', 'Captive-1' ...).
    std::string tag;
    // MILESTONE 18.  The `.mis` / `.ent` **"Click Speech"** string property --
    // `Entity::GetClickSpeechIDString`, entity vtable slot 507 -- which names a
    // `<speech_node>`.  It is what `IsEntityValidTalkTarget @004a3070` tests
    // to decide whether an actor can be talked to at all, and every one of
    // Brahmin Wood's villagers, captives, the Elder and Hawkeye carries one
    // ("Cap06_B_F", "Char_A00_W", "Tribal_A_F", "Junkie_A_F"...).
    // `ActionSetClickSpeech` overwrites it at run time; `Scene::ClickSpeech`
    // applies that override.  "Random Speech" is the sibling the idle chatter
    // uses.
    std::string clickSpeech, randomSpeech;
    // MILESTONE 18: Sneak is a MODE, not a one-shot -- `WGameWorld::
    // EvaluateSneakSkillCondition @0049de10` toggles it over the whole
    // selection with no target at all.  This is `Entity::IsHidden`.
    bool sneaking = false;
    // The talk order in flight (`LogRecordFieldChange17`, order type 0x17) and
    // the skill order in flight (`LogRecordFieldChange16`, type 0x16), each
    // retried a tick at a time by the same walk-then-do machinery
    // `pendingUse` uses.
    int32_t pendingTalk = -1;
    int32_t pendingSkillTarget = -1;
    std::string pendingSkill;
    int32_t kills = 0;         // for the "Kills" quantity condition
    // MILESTONE 12: the campaign carries this between missions.  `Add
    // Experience` (`ActionAddExperience`) credits the PLAYER, not a unit, so
    // the per-member figure is the mission total shared out at the debrief.
    int32_t xp = 0;
    int32_t level = 1;
    bool dead = false;
    bool knockedDown = false;
    std::string deathType;     // the suffix of the Death<type> sequence
    // MILESTONE 13: the actor's voice set plus its variant index, e.g.
    // "NormalM3" -- `Actor::GetSoundType`'s value.  See audio_names.h.
    std::string voice;
    // The frame-event sound the animation asked for while the frame it belongs
    // to had not yet been displayed.  `Actor::CheckThinkTimers`
    // (`Entity::ProcessAnimation@005e2b00`) parks the opcode at `ct_.unk13+4` and fires
    // it at the top of the next think; -1 is "nothing parked".
    int32_t pendingSoundEvent = -1;
    bool frameShown = false;

    // --- milestone 9: what the interface needs ---------------------------
    // The `.ent` this instance resolved to, its `Display Name` -- which is also
    // the portrait key, `gui/char/<Display Name>.zar` -- and its `Rank`, which
    // is `gui/rank/<Rank>.zar`.  Every shipped recruit carries all three
    // (`entities/recruits/hm01.ent` has "CORE_rec_hm01" and "Initiate", and
    // `gui/char/CORE_rec_hm01.zar` and `gui/rank/Initiate.zar` both ship).
    std::string entPath, displayName, rank;
    // MILESTONE 16: the LOCALISED display name -- `name_<Display Name>` out of
    // `locale/game/characters.txt`, per `GetLocalizedEntityName @0064f5d0`.
    // Empty until main.cpp resolves it; `label` stays the tag, because
    // `Scene::FindByTag` and the script layer key off that.
    std::string name;
    // The name the interface shows: the localised one when there is one.
    const std::string& ShownName() const { return name.empty() ? label : name; }
    // Hand2.  Milestone 8 only ever read Hand1, because that is the slot
    // `Actor::GetIDOfEquippedItemInActiveSlot` returns; the game bar draws both
    // (`WGameBar::LayoutGameBarControls @0047f420` builds two `WIIHand`s), so
    // the second one is read here.  `handSelected` is Actor.ct_.handselected.
    WeaponDef weapon2;
    int32_t handSelected = 0;
    // One entry of the actor's `Inventory` block.  The container is a
    // doubly-linked circular list walked FORWARD from head->next by every
    // consumer in the dumps (`WIIHand::Update @004924b0`,
    // `WBarterTable::PrepareBarterTransactionData @004558a0`, and the save
    // writer `Inventory::Inventory_BuildSerializedItemPathCountList @005b4b20`),
    // so display order is the list's own order, which is the order the `ILink<n>`
    // keys are numbered in.  Whether that is oldest- or newest-first is NOT
    // recoverable: `Inventory::Add` is in no dump.
    struct InvItem {
        std::string cls;          // Weapon / Ammo / Armour / Chem / ...
        std::string displayName;  // `Display Name`
        std::string itemType;     // `Item Type`
        // MILESTONE 17: the `Tag Name`.  `Container::MaybeUseEntity @005aca10`
        // unlocks a tagged container for an actor whose inventory
        // `ContainsItemWithTagName` (`Inventory::ContainsItemWithTagName
        // @005b4dc0`) or who has one equipped -- i.e. the key IS an item whose
        // Tag Name matches the container's.
        std::string tagName;
        std::string entPath;
        std::string spritePath;   // the item's own sprite, which carries guibig
        // MILESTONE 18: the two shapes a "helper item" takes in the shipped
        // `entities/items/Enhancers/*.ent`.
        //   * a `SkillConsumable` -- `firstAidKit`, `fieldMedicKit`,
        //     `paramedicsBag`, `doctorsBag`, `toolKit`, `superToolKit` -- with
        //     a `skillType` string ("firstAid" / "doctor" / "repair") and
        //     `Remaining uses`.  `Actor::PerformSkillUseCheck @00547e90` asks
        //     the carried items `DoesSkillStringMatchHelperItem` and refuses
        //     with `log_healRequiresFirstAid` / `log_healRequiresDoctor` /
        //     `log_repairRequiresItem` when none matches.
        //   * a plain item whose `Effect/Skills` block carries a bonus --
        //     `lockPicks` and `electronicLockPick` are +20 lockpick,
        //     `expandedLockPicks` and the Mk II more.  Not required, just
        //     added, which is exactly what `desc_lockpick` says.
        std::string skillType;
        int32_t usesLeft = 0;
        int32_t skillBonus[kSkillCount] = {0};
        int32_t count = 1;
        float weight = 0.0f;
        // When an item is unequipped its whole definition rides with it, so a
        // round trip through the pack is lossless.  An item that STARTED in the
        // pack has neither and is re-read from its own `.ent` on equip.
        WeaponDef weapon;
        ArmourDef armour;
    };
    std::vector<InvItem> inventory;

    // --- MILESTONE 17: looting -------------------------------------------
    // `Container` (`Container::Container @005ab1e0`), whose `.ent` properties
    // are all plain and all read by name: "Maximum Weight" (`Container.maxweight`,
    // the only weight test in the game -- `Container::AddToInventory @005ac050`
    // refuses with `tooHeavy` when `GetWeight(inv) + GetWeight(item) >
    // maxweight`), "Is Open", "Is Always Open", "Is Locked", "Action" (the same
    // `currentaction` string a Door uses: Closed / Opening / Opened / Closing)
    // and "Type of sounds".
    bool isContainer = false;
    bool alwaysOpen = false;
    bool isOpen = false;
    bool isLocked = false;
    // MILESTONE 18: `Container::ApplySkillToTargetEntity @005ad3f0` reads two
    // more `.ent` properties -- "Is Lockable" and "Chance to Unlock", which is
    // `Container.unlockchance`, the number the lockpick roll subtracts.
    // Brahmin Wood's one locked chest has `Is Locked = True`,
    // `Is Lockable = True`, `Chance to Unlock = 10` and NO Tag Name, i.e. no
    // key exists for it and picking is the only way in.
    bool isLockable = false;
    int32_t unlockChance = 0;
    // `Trap` -- `Trap::ApplySkillToTargetEntity @005cfcf0` / `IsValidTargetForSkill
    // @005d3190`.  `Entity::IsTrap` is the vtable predicate the world click
    // already uses to arm the Traps cursor automatically.
    bool isTrap = false;
    bool trapDisarmed = false;
    int32_t maxWeight = 0;
    // `Collectable` -- an item lying in the world rather than in a list.
    // `Entity::IsCollectable` is vtable slot 43 and `Collectable::GetWeight
    // @00590160` is `max(0, count - countNonLootable) * weight`, so the
    // non-lootable part of a stack weighs nothing and cannot be taken.
    bool isCollectable = false;
    InvItem item;                 // what this ground entity IS, when isCollectable
    // `Lootable` is a shipped `.ent` boolean (false on every punch/kick
    // entity); `Inventory::Inventory_BuildSerializedItemPathCountList
    // @005b4b20` writes a `nonLootableCount` beside every stack's count.
    bool lootable = true;
    // MILESTONE 17.  `SplitAndRemoveEntityForPickup @00508760` removes the
    // world entity once its whole stack has been taken.  This port has no
    // entity-destruction path, so a picked-up item is hidden instead: it stops
    // being drawn, picked and hit-tested, and keeps its sprite so nothing
    // downstream has to cope with a null one.
    bool hidden = false;
    // Container::MaybeUseEntity records the actor that opened it and
    // `Container::FinishedAnimationSequenceMaybe @005ac780` opens the loot
    // window for that actor when the "Opening" sequence ends.
    int32_t openedBy = -1;
    bool openWhenAnimDone = false;
    // `HandleActivateOrLootCommand @00549120`'s tail records the target it
    // could not reach in the actor's own fields at `Actor+0x1037` (the two
    // EntityIDs) and calls vtable slot 0x368, the walk-then-activate; the four
    // early refusals all CLEAR those same four words first.  This is that
    // record: the entity this actor is walking to in order to use it.
    int32_t pendingUse = -1;
    // Set when a queued use finally ran and opened a loot window, so main.cpp
    // can put the screen up on the tick it happens.
    bool lootJustOpened = false;
    // The two invisible weapons `Actor::LevelUpUnarmedAttacks @0054f4e0` keeps
    // in `Actor.ct_.unarmed_left` / `unarmed_right`: the race's punch and kick
    // out of `tables/races.txt`.  They are NOT in `slot_equip`, are not
    // lootable and cannot be dropped; `unarmedHand[i]` says the weapon
    // currently in hand `i` is one of them.
    WeaponDef unarmedPunch, unarmedKick;
    bool unarmedHand[2] = {false, false};

    // The attack in flight.  `Actor::BeginAttackAnEntity @0054a980` fills
    // Actor.ct_.combatmsg2 and sets currentaction to "Attack"; the resolution
    // happens later, when the animation reaches its ev_fire/ev_hit marker and
    // `Actor::ResolveCombatMessageForAttacker @0054dc40` runs.
    int32_t attackTarget = -1;
    bool attackResolved = false;    // Actor.ct_.unk26a, the once-per-attack latch
    int32_t attackChance = 0;       // cached for the console line
    int32_t attackShots = 0;
    // BaseAI: the tick this actor may next choose an action.
    int64_t aiNextTick = 0;
    bool aiEngaged = false;

    // --- milestone 12: vehicles -------------------------------------------
    // Every field here is a serialized `.ent` property of the shipped
    // `entities/vehicles/*.ent`, read by name: "Acceleration", "Max Speed",
    // "Max Run Speed", "Turning Rate", "Num Passangers" (sic), "Vehicle Type",
    // "Gunner Enabled" and the nested "Main Weapon" `VehicleWeapon` entity.
    bool isVehicle = false;
    bool isTurret = false;
    std::string vehicleType;
    float accel = 0.0f, maxSpeed = 0.0f, maxRunSpeed = 0.0f, turnRate = 0.0f;
    int32_t seats = 0;
    bool gunnerEnabled = false;
    WeaponDef vehicleWeapon;          // "Main Weapon", the turret
    int32_t driver = -1;              // Vehicle.ct_.driver
    int32_t gunner = -1;
    std::vector<int32_t> passengers;
    float vel[3] = {0, 0, 0};         // Vehicle.ct_.velx/vely/velz
    bool destReached = true;          // Vehicle.ct_.destreached
    bool inReverse = false;           // Vehicle.ct_.inreverse
    float vdest[3] = {0, 0, 0};       // Vehicle.ct_.destx/desty/destz
    int32_t inVehicle = -1;           // the vehicle this actor is riding, or -1

    // --- milestone 11 ------------------------------------------------------
    // The index into Scene::m_ai of the BaseAI attached to this actor, or -1.
    int32_t ai = -1;
    // The player-side sentry mode (SentryAI+0x1ad), for squad members.
    SentryMode sentry = kSentryScout;
    // Set by ApplyHit; the sentry's "return fire" test and the AI's
    // `tookDamage` event both read it.
    int32_t lastAttacker = -1;
    // Where the AI last saw or heard something worth walking to, for
    // MiscInvestigate.  -1 in [1] means "none".
    int32_t investigate[3] = {0, -1, 0};

    // Static geometry, mapview/main.cpp step 1b.
    int32_t world[3] = {0, 0, 0};
    float wpos[3] = {0, 0, 0};
    int32_t wmin[3] = {0, 0, 0}, wmax[3] = {0, 0, 0};
    int32_t sx = 0, sy = 0;              // projected anchor
    int32_t spriteOffX = 0, spriteOffY = 0;
    DrawTint tint[kLayerCount];

    // Refreshed whenever `frame` changes.
    int32_t x0 = 0, y0 = 0;
    int32_t rect[4] = {0, 0, 0, 0};      // absolute screen AABB, element+0x41..+0x51
    struct Layer {
        bool present = false;
        int32_t x = 0, y = 0;
        const Bitmap* bmp = nullptr;     // baked (tinted) pixels
        const Bitmap* raw = nullptr;     // untinted, for the per-pixel pick test
    };
    std::array<Layer, kLayerCount> layers;
    uint32_t tintKey = 0xffffffffu;      // quantised ambient of the last bake
    bool dirty = true;

    // MILESTONE 14, render-side interpolation (a PORT ADDITION -- see
    // kInterpSnapWorldUnits).  `prevWpos` is where this entity stood before the
    // most recent tick moved it; `interp` says that tick moved it by a
    // plausible walking distance, and `interpApplied` that `sx`/`sy` currently
    // hold an interpolated value rather than the tick-exact projection.
    float prevWpos[3] = {0, 0, 0};
    bool interp = false;
    bool interpApplied = false;
};

// One placed tile instance that survived the load-time filters.
struct PlacedTile {
    size_t instance = 0;
    int32_t protoId = 0;
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int64_t depth1 = 0, depth0 = 0;
    int32_t elev = 0;
    bool floor = false;
};

struct SceneOptions {
    std::string tileDir, spriteDir, entDir;
    bool haveInside = false;
    int64_t inside[3] = {0, 0, 0};
    double insideK = 3.0;
    bool haveTime = false;
    int timeHour = 0, timeMinute = 0;
    bool lights = true;
    // MILESTONE 19 (NOTES.md 25.13): the rig the renderer reads is
    // ComputeTimeOfDayLightingState's, which World::UpdateWorldTimeAndLighting
    // @004f2c10 recomputes from the world clock every frame -- NOT the `.mis`'s
    // stored <mapManager> triple, which is only the EDITOR's snapshot of that
    // same computation and is a NIGHT snapshot on ten maps, Brahmin Wood
    // included.  `--light-base map` is that snapshot, `white` the literal
    // DAT_008bda50 == 0 branch.
    LightBase lightBase = LightBase::kClock;
    bool drawMarkers = false;
    bool showInvisibleTiles = false;
    float ambient[3] = {1.0f, 1.0f, 1.0f};
    // --squad / --campaign-start, ported from mapview (NOTES.md 19, 24.5).
    std::vector<std::string> squad;
    bool campaignStart = false;
    // MILESTONE 22: a world written by `World::Save` -- a `<world>` 70 file
    // image out of a `.sav` container, ours or the shipped game's.  It carries
    // no map geometry (World::Save writes the source `.mis`'s PATH instead), so
    // `Load` reads the map named in `misPath` as usual and then substitutes the
    // save's entity records for the map's own.  That is exactly what the
    // original does: `World::Load` re-reads the map and then
    // `World::LoadWorldSections` overwrites the entities from the stream.
    const std::vector<uint8_t>* saveWorld = nullptr;
};

struct RenderStats {
    size_t tilesDrawn = 0, entitiesDrawn = 0, sortElems = 0;
};

class Scene {
public:
    bool Load(const std::string& misPath, const SceneOptions& opt, std::string* errorOut);

    // Advance the simulation by `deltaMs` of real time.  See NOTES.md: the
    // engine's own step is a fixed 100 ms tick with a real-time millisecond
    // clock alongside it, and both are driven from here.
    void Update(int64_t deltaMs);

    // Paint one W x H frame.  (camX, camY) is the world-screen coordinate of
    // the window's top-left pixel -- the negation of the engine's screen
    // offset (NOTES.md "the viewport").
    void Render(int64_t camX, int64_t camY, int32_t W, int32_t H, uint8_t* out,
                RenderStats* stats);

    // Screen point -> entity index, or -1.  Uses the draw list the last Render
    // built, walked front to back.
    int32_t Pick(int64_t camX, int64_t camY, int32_t px, int32_t py) const;

    // --- milestone 7 ------------------------------------------------------
    // Order entity `index` to walk (or run) to the world point (x, z).  The
    // elevation is the navigation grid's own surface there.  Returns false when
    // no path exists at all.
    bool OrderMove(int32_t index, int32_t worldX, int32_t worldZ, bool run);
    void StopMove(int32_t index);
    // "Stand" / "Crouch" / "Prone".  Plays the transition sequence.
    bool SetPosture(int32_t index, const std::string& posture);
    const NavGrid& Nav() const { return m_nav; }
    // The path last computed for `index`, for the --path overlay.
    const std::vector<PathPoint>* PathOf(int32_t index) const;
    // Debug: one line per moving actor.
    std::string TraceMovement() const;
    // World point under a screen point, on the navigation grid's surface.
    bool ScreenToGround(int64_t camX, int64_t camY, int32_t px, int32_t py, int32_t* wx,
                        int32_t* wz) const;

    // --- milestone 8: combat ---------------------------------------------
    // The breakdown one to-hit computation produced, for the console line and
    // for the verification harness.  Every field names the term in
    // `ComputeWeaponAttackHitChance @00615e80` it comes from.
    struct HitTerms {
        int32_t skill = 0;            // Skills::GetSkillValueByName
        int32_t accuracyBonus = 0;    // WeaponMode.accuracybonus
        int32_t rangeClass = 0;       // 2 short/melee, 4 Long, 5 Scoped/OnlyLong
        int32_t rangePenalty = 0;     // (dist - rangeClass*(PE+2*Sharpshooter)*light) * 4
        int32_t closeBonus = 0;       // (1 - dist/6) * 30, short-range weapons only
        int32_t armourClass = 0;      // target derived.armorclass, capped at 80
        int32_t meleeDodge = 0;       // target's unarmed/melee skill inside 3 units
        int32_t coverPenalty = 0;     // min(nBlockers * 5, 20)
        int32_t calledShot = 0;       // FUN_0062f890, 0 for an uncalled shot
        int32_t targetDown = 0;       // +40 fallen, +20 crippled legs
        int32_t bigTarget = 0;        // +15 when the bbox volume exceeds 81
        int32_t difficulty = 0;       // trunc((difficultyMultiplier - 1) * 40)
        int32_t total = 0;            // after the [0, 95] clamp
        float distance = 0.0f;        // world units between the bounding boxes
        float light = 1.0f;           // TraceLightVisibilityToEntity
        bool blocked = false;         // no line of fire
        bool outOfRange = false;
        bool noAmmo = false;
        bool notEnoughAp = false;
        std::string reason;           // why an attack was refused, if it was
    };
    // The damage band a hit is rolled from -- the weapon's field through the
    // mode multiplier and the melee/ranged bonus -- plus the ammunition's
    // damage and DT/DR multipliers.  `--hitcalc` reports it so a session's
    // `DMG` rolls can be checked against the band they came out of.
    // `centreMetres` is the CENTRE-to-centre range the shot is taken at -- the
    // distance `ApplyAreaDamageAttackWithFactionCheck @00615100` measures, not
    // the bounding-box gap the to-hit sum uses.  It only matters for a `Cone`
    // spread weapon, whose band widens as the range shortens.  Pass a negative
    // range to get the band with no cone term at all.
    void DamageBandOf(const LiveEntity& attacker, int32_t* minOut, int32_t* maxOut,
                      float* ammoMultOut, float* drModOut, float centreMetres = -1.0f) const;
    // The chance `attacker` has to hit `target` right now, with the terms.
    bool ComputeHitChance(int32_t attacker, int32_t target, HitTerms* out) const;
    // Order an attack.  Faces the target, spends nothing yet: the AP and the
    // ammo go at the animation's fire marker, as they do in the original.
    bool OrderAttack(int32_t attacker, int32_t target, std::string* whyNot);
    // Weapon::SetNextWeaponMode @005da0d0 -- cycle single/burst.
    bool SetWeaponMode(int32_t index, const std::string& modeName);
    // Turn-based.  `World+0x427` is 0 continuous, 1 individual, 2 squad;
    // `World::StartCombat @00506b40` picks 1 or 2 from the campaign setting and
    // `World::EndCombatAndGrantSquadXP @00506fe0` puts it back to 0.
    void SetTurnBased(bool on);
    bool TurnBased() const { return m_combatMode != 0; }
    void EndTurn();
    int32_t ActiveTurnEntity() const;
    const std::vector<int32_t>& TurnOrder() const { return m_turnQueue; }
    // The console line, newest last.  `CombatMessage` has no UI yet.
    const std::vector<std::string>& Log() const { return m_log; }
    void ClearLog() { m_log.clear(); }
    void SeedRng(uint32_t seed) { m_rng.Seed(seed); }
    // MILESTONE 14.  Render-side interpolation of moving entities, on by
    // default.  A PORT ADDITION, not original behaviour: see
    // kInterpSnapWorldUnits and NOTES.md section 72.
    void SetInterpolation(bool on) { m_interpolate = on; }
    bool Interpolation() const { return m_interpolate; }
    // The screen anchor Render() last drew entity `i` at, for verification.
    bool EntityScreen(int32_t i, int32_t* sx, int32_t* sy) const {
        if (i < 0 || static_cast<size_t>(i) >= m_entities.size()) return false;
        *sx = m_entities[static_cast<size_t>(i)].sx;
        *sy = m_entities[static_cast<size_t>(i)].sy;
        return true;
    }
    // --- milestone 13: sound ----------------------------------------------
    void SetAudio(Audio* a) { m_audio = a; }
    Audio* GetAudio() const { return m_audio; }
    // The listener is the camera; main.cpp feeds it the world point under the
    // middle of the viewport, in world units.  The elevation comes from the
    // navigation grid, because the ambient <world_zone> boxes are 3D and a
    // listener parked at y = 0 is inside none of them.
    void SetListener(int32_t worldX, int32_t worldZ);
    // One positional effect at an entity, picking among numbered variants.
    // `stem` is asset-relative and carries no number and no extension.
    bool PlaySoundStem(const LiveEntity& e, const std::string& stem, int channel,
                       const char* why);
    // `Actor::SoundRelated1`'s frame-event half: opcodes -44 (the sequence's
    // own sound), -41 and -40 (the two footsteps of a walk cycle).
    void FrameEventSound(LiveEntity& e, int opcode);
    // `UpdateWorldZonesTimeAndAmbientSound @004fd4e0` -- the ambient beds and
    // the mission's music.
    void UpdateAmbient(int64_t deltaMs);
    void StartMissionMusic();
    void DisableAI() { m_aiEnabled = false; }
    // --- verification harness ---------------------------------------------
    // Put an entity down at a world (x, z), on the navigation grid's surface
    // there.  This is a TEST AFFORDANCE, not something the game does: it is
    // how the hit-rate runs get an attacker at an exact range.
    bool PlaceEntity(int32_t index, int32_t worldX, int32_t worldZ);
    // Put a weapon in an actor's first hand whatever is already there.  Also a
    // TEST AFFORDANCE, and for the same reason as PlaceEntity: replaying a
    // `HIT` record from the original needs the attacker holding the weapon the
    // record names.  `GiveItemByPath` deliberately will not displace a held
    // weapon, which is the right rule for the quartermaster and the wrong one
    // here.
    bool ForceEquipByPath(int32_t index, const std::string& entPath, std::string* whyNot);
    // Run `n` independent hit rolls of the attack `attacker` would make against
    // `target` right now, through the same `RollHits` the live path uses, and
    // report how many landed.  Nothing is damaged and no AP or ammo is spent.
    bool HitRateTest(int32_t attacker, int32_t target, int32_t n, HitTerms* terms,
                     int64_t* hits, int64_t* shots);
    // Debug: one line per combatant.
    std::string TraceCombat() const;
    // The blood decals CombatFX spawns; drawn under the entities.
    struct Decal {
        int32_t world[3] = {0, 0, 0};
        int32_t sx = 0, sy = 0;
        int32_t kind = 0;    // 0 spurt, 1 spray, 2 gush, 3 pool
        int64_t tick = 0;
    };
    const std::vector<Decal>& Decals() const { return m_decals; }

    // --- milestone 9: the interface's model ------------------------------
    // Move an item between an actor's slots.  `from` and `to` are 0 hand1,
    // 1 hand2, 2 armour, and >= 3 the inventory list entry (index - 3); -1 is
    // "the ground", i.e. a drop.  The original resolves legality on the
    // receiving side with `Entity::CanReachInteractable` / `Entity::CanUseItem`
    // (`WIIHand::HandleEquipSlotItemInteraction @0046f940`), whose bodies are
    // in no dump; the rule applied here is the one the shipped classes make
    // unavoidable -- Armour only into the armour slot, Weapons only into a
    // hand -- and it is a RECONSTRUCTION, labelled at the point of use.
    bool MoveItem(int32_t entity, int32_t from, int32_t to, std::string* whyNot);
    // MILESTONE 12: issue one item, by `.ent` path, into `slot` (0 hand1,
    // 1 hand2, 2 armour, -1 the pack).  This is what the bunker's
    // quartermaster does; `WBarterTable::PrepareBarterTransactionData
    // @004558a0` walks the same inventory list on the trade side, but the
    // transfer itself is `Inventory::Add`, whose body is in no dump, so the
    // slot rules here are `MoveItem`'s own.
    bool GiveItemByPath(int32_t entity, const std::string& entPath, int32_t slot,
                        int32_t count, std::string* whyNot);
    // Recompute every combatant's magazine and spare-round count from what it
    // is actually carrying -- `Weapon::FindSuitableAmmoInInventory @005dae40`
    // matches an `Ammo` stack's `Ammo Type` against the equipped weapon's.
    // Needed after anything hands an actor a weapon or ammunition outside the
    // load path (the quartermaster, `--issue`, and every loot transfer).
    void RefreshAmmoReserves();

    // --- MILESTONE 17: containers, corpses and ground items ---------------
    //
    // `HandleActivateOrLootCommand @00549120` is the whole of "left click on an
    // object" (`locale/gui/help.txt`: "Left Click on Object: Use").  In order it
    //   1. turns the actor to face the target;
    //   2. refuses with `use_code |= 0x100` when the target is out of reach;
    //   3. refuses with `0x40` when `Entity::CanBeActivated` is false;
    //   4. refuses with `0x20` when the target's own
    //      `IsValidTargetForDefaultAction` says no;
    //   5. spends `Tables::ActionTypes["activate"]` AP, refusing with `8`;
    //   6. and finally calls the target's `MaybeUseEntity`.
    // The refusal strings are `locale/game/reason.txt`'s `log_*` keys, which is
    // where every message this path produces comes from.
    //
    // Reach is the binary's two different tests, both recovered:
    //   `Actor::CheckIfTargetInReachableRange @00557f90` -- box gap <=
    //   `DAT_00824d04` = 1.0 m -- gates opening a container and picking an item
    //   up, and `Actor::CanReachInteractable @0055a840` -- box gap <=
    //   `_DAT_00824d08` = 4.0 m plus a line-of-sight test -- gates the equip
    //   screen's drag.  The gap itself is `BoxGapDistance`, the same
    //   `(sum of both boxes' x+z extents) * 0.5 * 0.25` metres the fire path
    //   uses.
    float ReachGap(const LiveEntity& a, const LiveEntity& b) const;
    bool CanReachToUse(const LiveEntity& a, const LiveEntity& t) const;
    bool CanReachToInteract(const LiveEntity& a, const LiveEntity& t) const;
    // "Use" the entity under the cursor.  Opens a container, opens a corpse,
    // picks a ground item up, opens or closes a door.  Returns false and fills
    // `whyNot` with the `reason.txt` message the original would have shown.
    // `openedLoot` is set when the call put a loot window up.
    bool UseEntity(int32_t actor, int32_t target, std::string* whyNot, bool* openedLoot);
    // The queued activate, run once a tick: an actor that was out of reach
    // walked to the object and uses it on arrival.
    void StepPendingUse();
    // Consumed by main.cpp: which actor's queued use just opened a window.
    int32_t TakeLootJustOpened() {
        for (LiveEntity& e : m_entities) {
            if (!e.lootJustOpened) continue;
            e.lootJustOpened = false;
            return m_lootTarget;
        }
        return -1;
    }
    // Which entity, if any, has a loot window open, and for whom.
    int32_t LootTarget() const { return m_lootTarget; }
    int32_t LootActor() const { return m_lootActor; }
    void CloseLoot();
    // Move item `index` of `from`'s inventory into `to`'s, `count` of them
    // (0 = the whole stack).  `Inventory::AddToInventory @005b4030` merges into
    // a matching stack (`Collectable::MergeEntities @005c1910`) and
    // `Collectable::SplitEntity @005c17a0` splits one; a Container refuses over
    // its own `Maximum Weight` with `tooHeavy`, and an Actor never refuses --
    // `Actor::AddToInventory @00555180` only raises the `encumbered` event when
    // the add pushes it over `ST * 25`.
    bool TransferItem(int32_t from, int32_t to, int32_t index, int32_t count,
                      std::string* whyNot);
    // `equipMenu_takeAll`, the loot window's own button.
    int32_t TakeAll(int32_t from, int32_t to, std::string* whyNot);
    // `Actor::GetWeight @005550c0`: the inventory plus the three equip slots.
    float CarriedWeight(const LiveEntity& e) const;
    // `Attributes::SumFinalAttributes @006088d0`: `maxcarryweight += ST * 25`.
    int32_t CarryCapacity(const LiveEntity& e) const {
        return e.attr.strength * 25;
    }
    bool IsEncumbered(const LiveEntity& e) const {
        return CarriedWeight(e) > static_cast<float>(CarryCapacity(e));
    }
    // Every container / corpse / ground item in the map, for the harness.
    std::vector<int32_t> Containers() const;
    std::vector<int32_t> GroundItems() const;
    std::string TraceLoot() const;

    // --- milestone 12: vehicles -------------------------------------------
    // `Vehicle::MountPilotAndCreateController @0058ca40` and
    // `Vehicle::Vehicle_AssignPilotToSeat @0058cde0` put an actor in the
    // driver's seat or a passenger slot, refusing when
    // `Vehicle::IsPassengerCapacityReached @00584f60` says the vehicle is full;
    // `Vehicle::EjectAllOccupantsAndPark @0058d2a0` and
    // `Vehicle::VehiclePassengerDisembarkAllowed @00587ef0` are the way out.
    bool VehicleMount(int32_t actor, int32_t vehicle, std::string* whyNot);
    bool VehicleDismount(int32_t actor, std::string* whyNot);
    // `Vehicle::SetDestination @005800b0`.
    bool VehicleDrive(int32_t vehicle, int32_t worldX, int32_t worldZ, std::string* whyNot);
    // `VehicleWeapon::BeginAttackAnEntity @005d64f0` through the gunner.
    bool VehicleFire(int32_t vehicle, int32_t target, std::string* whyNot);
    std::vector<int32_t> Vehicles() const;
    void AttachTurrets();
    // Actor.ct_.handselected, which `WIIHand::Draw` greys the other hand by.
    bool SwapHands(int32_t index);
    // The active weapon of the selected hand.
    const WeaponDef& ActiveWeapon(const LiveEntity& e) const {
        return e.handSelected == 1 ? e.weapon2 : e.weapon;
    }
    bool Reload(int32_t index);

    // --- milestone 10: mission scripting ---------------------------------
    MissionScript& Script() { return m_script; }
    const MissionScript& Script() const { return m_script; }
    void EnableScript(bool on) { m_scriptEnabled = on; }
    // The Unit references in triggers and actions are Tag Names.
    int32_t FindByTag(const std::string& tag) const;
    // `ConditionCanSee` reduced to "an unblocked line between the two boxes,
    // inside the looker's sight range".  RECONSTRUCTION: the original's
    // `Entity::CanSeeEntity` is in no dump; this reuses milestone 8's line-of-
    // fire walk and the same 40-world-unit engagement radius the AI uses.
    bool LineOfSight(int32_t a, int32_t b) const;
    // ActionKillEntity.
    bool ScriptKill(int32_t index);
    // ActionChangePlayer.
    bool ScriptSetPlayer(int32_t index, int32_t player);
    // ActionGiveItemToHuman: move the named item out of the named NPC's
    // inventory into the first living squad member's pack.
    bool ScriptGiveItem(const std::string& unitTag, const std::string& itemTag);
    // ActionCenterView asks the window to recentre; `main` reads and clears it.
    void RequestCentre(int32_t index) { m_centreRequest = index; }
    int32_t TakeCentreRequest() {
        const int32_t v = m_centreRequest;
        m_centreRequest = -1;
        return v;
    }

    // --- milestone 22: floating status text (VIDEO_GAPS M4) -------------
    // f0246 shows the word "Detected" floating over a squad member's head in
    // the same white-outlined style the speech uses.  The game raises it when
    // an enemy first sees that unit, which is the `!have` arm of the detection
    // sweep below.  `WFloatingText` is in no dump, so the dwell is a labelled
    // PLACEHOLDER, shared with `kSpeechMs`.
    struct StatusFloat {
        int32_t entity = -1;
        std::string text;
        int64_t untilMs = 0;
    };
    const std::vector<StatusFloat>& StatusFloats() const { return m_statusFloats; }
    void PushStatusFloat(int32_t entity, const std::string& text, int64_t ms = 3000);
    // Which `<Player>` slot the human is playing.  The SpawnPoint's own slot
    // when the map has one (`World::ValidateSpawnPointsAndAssignTeamColors
    // @004f53d0` leaves the squad standing there), and otherwise the slot of
    // whatever is selected -- the four tutorial maps place their `player`
    // directly and carry no spawn point at all, so `m_spawnPlayer` is -1 there.
    int32_t PlayerSide() const;
    // Whether an entity belongs to the human player: the `<Player>` record's
    // own `side == 2` where the map has a player list, else `PlayerSide()`.
    bool IsPlayerOwned(int32_t index) const;

    // --- milestone 11: the AI -------------------------------------------
    // `Actor::Actor_TrySneakPastDetection @00544770`'s detection radius,
    // evaluated for one (observer, target) pair.  `actionFactor` is the
    // action the target is performing, 1.0 for "just standing there";
    // ai.h's DetectTerms carries every input back for the log.
    bool Detect(int32_t observer, int32_t target, DetectTerms* out,
                float actionFactor = 1.0f) const;
    // `ComputeTargetScoreForEntity @005206f0`.
    float AiTargetScore(int32_t self, int32_t target) const;
    // Raise one of aitable.txt's nine events on an AI, which is what
    // `BaseAI::BaseAIEngineThinkRelated @0051e1d0` does with the event's
    // demeanour row.  `source` is the entity that caused it, or -1.
    void AiRaiseEvent(int32_t entity, int event, int32_t source);
    // `BroadcastHearCombatEventToNearbyEntities @00552530`.
    void AiBroadcastCombat(int32_t shooter, int32_t target);
    const std::vector<BaseAiState>& AiStates() const { return m_ai; }
    const std::vector<ControllerState>& Controllers() const { return m_controllers; }
    const AiTable& Table() const { return m_aiTable; }
    // The two script families milestone 10 left inert.
    bool ScriptSetMoveState(int32_t player, const std::string& type,
                            const std::string& targetTag, const std::string& zone);
    bool ScriptSetPriority(int32_t player, const std::string& kind, int32_t value);
    // MILESTONE 13 (verification only).  Turn a player controller's `Rand Move`
    // on or off at runtime.  No shipped mission writes the property, so this is
    // the only way to exercise the wander; see NOTES.md section 66.
    size_t SetControllerRandMove(int32_t playerIndex, bool on);
    void SetSentry(int32_t index, SentryMode mode);

    // -----------------------------------------------------------------------
    // MILESTONE 18: talking, and the skills.  See NOTES.md sections 95-99.
    // -----------------------------------------------------------------------
    bool IsHostile(int32_t a, int32_t b) const;
    // `Entity::GetClickSpeechIDString` (vtable slot 507): the entity's own
    // `Click Speech` property, or the `ActionSetClickSpeech` override.
    const std::string& ClickSpeech(int32_t index) const;
    // `IsEntityValidTalkTarget @004a3070`, whole:
    //   IsActor && IsLegalTarget && PlayerIndex != <the local player>
    //   && (click speech is set and not "None"
    //       || IsGeneral || IsRecruitMaster || IsQuartermaster
    //       || CanGambleWith || CanBarterWith)
    bool TalkTarget(int32_t index) const;
    // The talk order, `WGameWorld::LogClosestPlayerToPosition @0049aaf0` plus
    // order type 0x17: walk into reach, face the target, play its click-speech
    // node.  Returns false with `whyNot` when the target is not talkable.
    bool OrderTalk(int32_t actor, int32_t target, std::string* whyNot);

    // The skill-use order, type 0x16.  `skill` is one of the eight skilldex
    // names (`sneak lockpick steal traps firstAid doctor science repair`,
    // the table at 0x008a907c, which is also keys.cfg's `1`..`8`).  `target`
    // is -1 for `sneak`, which takes none.  The message goes to the combat
    // console and is a `locale/game/reason.txt` key's value.
    bool UseSkill(int32_t actor, const std::string& skill, int32_t target,
                  std::string* whyNot);
    // `Actor::PerformSkillUseCheck @00547e90`'s retry: an out-of-reach skill
    // target is walked to, not refused, exactly like `StepPendingUse`.
    void StepPendingSkill();
    // Which selected member performs a skill: `WGameWorld::ResolveStealSkillTarget
    // @0049a650` walks the selection and picks the HIGHEST value of that skill.
    int32_t BestSkilledSelected(const std::string& skill) const;
    // The nearest selected member to `target`, which is who talks
    // (`LogClosestPlayerToPosition`).
    int32_t NearestSelected(int32_t target) const;
    // One use of a `SkillConsumable`; `log_itemUsedUp`.
    void ConsumeHelper(LiveEntity& a, int32_t index);
    // A test hook, for the healing skills: set an actor's hit points.
    bool SetHitPoints(int32_t index, int32_t hp);

    // -----------------------------------------------------------------------
    // MILESTONE 18: the item icon.
    //
    // `WIIBase::Draw @00490b50` blits the ITEM ENTITY'S OWN SPRITE: it takes
    // the entity's sprite id, asks the sprite for the animation named
    // `guibig` or `guismall` (`FindRecordIndexByName(this, "guibig"/"guismall")`,
    // chosen by the byte at `WIIBase+0x170` -- 0 is guibig), reads that
    // record's collection index, and blits frame 0 / direction 0 unscaled,
    // centred vertically in the cell, tinted by the entity's own basecolor.
    // Every shipped item sprite carries both: `Combat Knife.spr` has
    // `default` (the 4-direction world sprite), `guibig` (126 x 52) and
    // `guismall` (63 x 26), which is exactly the 140 x 69 hand cell and a
    // list row.
    //
    // Returns false when the sprite, the animation or its images are missing.
    // The result is owned by the scene's own image cache and stays valid.
    bool ItemIcon(const std::string& spritePath, bool big, const Bitmap** out);
    // Debug: the parsed AI layer, and one line per thinking actor.
    std::string DescribeAI() const;
    std::string TraceAI() const;
    // Every detection event this run, "tick obs tgt terms...", when enabled.
    void TraceDetection(bool on) { m_traceDetect = on; }
    void SetDifficultyMultiplier(float m) { m_difficultyMultiplier = m; }
    float DifficultyMultiplier() const { return m_difficultyMultiplier; }

    // MILESTONE 16: selection is a SET, not one entity.
    //
    // `WGameWorld::ToggleEntitySelection @0049fb40` takes an entity id and an
    // "exclusive" flag: when the flag is set and any other member of the local
    // player's list disagrees with "is this the clicked one", it calls
    // `RemoveSelectionMarkerFromEntity` over the whole list first and then
    // toggles flag 0x40 on the clicked one -- an exclusive select.  With the
    // flag clear it goes straight to `ToggleEntitySelectionFlag @0049fa30`,
    // which flips 0x40 on that one entity alone -- an add/remove.  Both end at
    // `SetSpeechTargetEntity`, which is why there is still exactly one
    // "current" member: `m_selected` is that one, `m_sel` is the set.
    void Select(int32_t index) {
        m_selected = index;
        m_sel.clear();
        if (index >= 0) m_sel.push_back(index);
    }
    // The add/remove half of ToggleEntitySelection (exclusive == 0).
    void SelectToggle(int32_t index) {
        if (index < 0) return;
        for (size_t i = 0; i < m_sel.size(); ++i) {
            if (m_sel[i] == index) {
                m_sel.erase(m_sel.begin() + static_cast<long>(i));
                if (m_selected == index) m_selected = m_sel.empty() ? -1 : m_sel.front();
                return;
            }
        }
        m_sel.push_back(index);
        m_selected = index;
    }
    void SelectMany(const std::vector<int32_t>& v) {
        m_sel = v;
        m_selected = v.empty() ? -1 : v.front();
    }
    int32_t Selected() const { return m_selected; }
    const std::vector<int32_t>& Selection() const { return m_sel; }
    bool IsSelected(int32_t i) const {
        for (int32_t s : m_sel) {
            if (s == i) return true;
        }
        return false;
    }
    // MILESTONE 16: the localised display name.
    //
    // `GetLocalizedEntityName @0064f5d0` returns the entity's own override
    // string (`Entity+0x14e`) when it is set, and otherwise looks up
    // `"name_" + <Display Name>` in the game's string tables, then in the
    // mission's own table, and falls back to the raw Display Name.  The
    // recruits' Display Names ARE those keys -- `entities/recruits/hm01.ent`
    // carries "CORE_rec_hm01" and `locale/game/characters.txt` has
    // `name_CORE_rec_hm01 = {Robin}` -- which is why the tabs read
    // "CORE_rec_h" until this is applied.  The lookup itself lives in the UI
    // (it owns the `Locale`), so main.cpp pushes the answer down here.
    void SetLocalName(int32_t i, const std::string& n) {
        if (i >= 0 && static_cast<size_t>(i) < m_entities.size()) m_entities[static_cast<size_t>(i)].name = n;
    }

    const std::vector<LiveEntity>& Entities() const { return m_entities; }
    // MILESTONE 12: the campaign layer restores a squad member's carried state
    // (HP, XP, inventory) onto the freshly spawned entity after Load, and
    // harvests it back when the mission ends.
    std::vector<LiveEntity>& EntitiesMut() { return m_entities; }
    const World& GetWorld() const { return m_world; }
    // MILESTONE 22: `World::Save @004ea450`'s save-game branch, written into a
    // real `<world>` 70 file image.  The runtime state the port models is
    // written back into the entity records' own properties -- position,
    // current hit points and action points -- and every other section of the
    // world is re-emitted exactly as it was read.
    // `mapPath` is the name the save stores for the map, and must be the
    // CAMPAIGN-RELATIVE one (`campaigns/missions/core/mission01.mis`) that the
    // original writes and that `World::Load` re-opens through the format
    // manager -- not the resolved filesystem path this process was handed.
    bool WriteWorldSave(const std::string& mapPath, std::vector<uint8_t>* out,
                        std::string* errorOut);
    const std::string& MisPath() const { return m_misPath; }
    // The screen-space bounding box of the whole map.
    void ScreenBounds(int64_t b[4]) const {
        for (int i = 0; i < 4; ++i) b[i] = m_bounds[i];
    }
    int64_t NowMs() const { return m_nowMs; }
    int64_t Ticks() const { return m_ticks; }
    const std::string& Info() const { return m_info; }
    // The world position the map's SpawnPoint stands on, and its player slot.
    bool SpawnPoint(int32_t out[3]) const {
        if (!m_haveSpawn) return false;
        for (int i = 0; i < 3; ++i) out[i] = m_spawn[i];
        return true;
    }
    // Indices of the entities --squad placed, in roster order.
    const std::vector<int32_t>& Squad() const { return m_squad; }
    // The SpawnPoint's player slot -- the player-controlled side.
    int32_t SpawnPlayer() const { return m_spawnPlayer; }
    // Whether the last OrderMove reached the cell it was asked for, and how
    // many nodes the search expanded.
    bool LastPathExact() const { return m_lastPathExact; }
    size_t LastPathExpanded() const { return m_lastPathExpanded; }
    // Debug: the state of one cell.
    bool CellState(int32_t worldX, int32_t worldZ, int32_t* surfaceOut, bool* blockedOut) const {
        const int32_t cx = m_nav.CellX(worldX), cz = m_nav.CellZ(worldZ);
        if (!m_nav.In(cx, cz)) return false;
        *surfaceOut = m_nav.Surface(cx, cz);
        *blockedOut = m_nav.stat[m_nav.Idx(cx, cz)] != 0 || m_nav.dyn[m_nav.Idx(cx, cz)] != 0;
        return true;
    }
    // Debug: one line per animated entity, for the verification runs.
    std::string TraceAnimation() const;
    // Every asset file the load actually opened, absolute (`--manifest`).
    const std::vector<std::string>& Manifest() const { return m_manifest; }

private:
    void ResolveEntities();
    void RefreshEntityFrame(LiveEntity& e);
    // Entity::ProcessAnimation @005e2b00 and the idle re-selection it ends in.
    void ProcessAnimation(LiveEntity& e);
    void FinishedAnimationSequence(LiveEntity& e);
    bool SetAnimationType(LiveEntity& e, const std::string& name);
    const std::vector<int16_t>* RawTable(const LiveEntity& e, int32_t sequence);
    // milestone 7
    void BuildNavGrid();
    void RefreshDynamicBlockers();
    // MILESTONE 13.  DirSearch's translated-bounding-box occupancy test, and
    // `Entity::Entity_FindNearestFreeGroundTileAndReposition @00587690`.
    bool ActorBoxFree(const LiveEntity& e, int32_t wx, int32_t wz, bool ignoreActors) const;
    bool FindNearestFreeGroundTile(const LiveEntity& e, int32_t wx, int32_t wz, int32_t* outX,
                                   int32_t* outZ) const;
    void SetEntityWorldPos(LiveEntity& e, const float pos[3]);
    // MILESTONE 14: the render-side interpolation pass, run at the top of
    // Render().  A PORT ADDITION; see kInterpSnapWorldUnits.
    void ApplyRenderInterpolation();
    void SetFacingFromVector(LiveEntity& e, float dx, float dz);
    void StepMovement(LiveEntity& e);
    void SelectDestinationAnimationState(LiveEntity& e);
    void DoorThink(LiveEntity& e);
    bool VehicleStep(LiveEntity& e);
    void SetDoorState(LiveEntity& e, const std::string& state);
    // MILESTONE 17: `Inventory::AddToInventory @005b4030` plus
    // `Collectable::MergeEntities @005c1910` -- an identical stack merges.
    void AddToInventory(LiveEntity& e, const LiveEntity::InvItem& item);
    bool ApplyAction(LiveEntity& e);
    // milestone 8
    void LoadCombatant(LiveEntity& e, const ft::Esh& instance, const ft::EntProto* proto);
    // Entity::ResolveCombatMessageForAttacker: the ev_fire / ev_hit marker.
    void ResolveAttack(LiveEntity& e);
    // Actor::ReloadActiveWeaponWithAnyAmmo @0054ec40.
    bool ReloadWeapon(LiveEntity& e);
    int32_t RollHits(int32_t chance, int32_t shots, bool burst, int32_t critBonus,
                     bool betterCriticals, int32_t* critFlagsOut);
    // ResolveBurstAttackTargets @00614ac0: the shot allotment across the cone.
    void ResolveBurst(LiveEntity& e, const HitTerms& primaryTerms, int32_t shots,
                      float apBefore, int32_t ammoBefore);
    // ApplyRadiusDamageToTarget + Actor::ApplyAttackResultToEntity + ApplyDamage.
    void ApplyHit(LiveEntity& attacker, LiveEntity& target, int32_t hits, int32_t critFlags);
    void KillEntity(LiveEntity& e, const std::string& damageType, int32_t damage, int32_t hits);
    void SpawnBlood(const LiveEntity& target, int32_t damage);
    float BoxGapDistance(const LiveEntity& a, const LiveEntity& b) const;
    // The raw centre-to-centre distance, which is what the FTSE logger's
    // `gap_m` is and what the cone damage multiplier measures.
    float CentreDistance(const LiveEntity& a, const LiveEntity& b) const;
    // The line of fire.  `CheckLineOfSightBetweenEntities @004ff9b0` walks the
    // tile grid between the two eye points; this counts static blockers.
    int32_t CountLineBlockers(const LiveEntity& a, const LiveEntity& b, bool* blocked) const;
    bool Hostile(const LiveEntity& a, const LiveEntity& b) const;
    // milestone 11
    void LoadAI();
    void AiTick();
    void AiThink(BaseAiState& s);
    void AiPerceive(BaseAiState& s);
    float AiHighestDesire(const BaseAiState& s, int32_t* which) const;
    bool AiSelectTarget(BaseAiState& s);
    // The demeanour think functions.  Each returns the score the original's
    // returns: > 0 means "I acted", 0 means "fall through".
    float DemeanourAttack(BaseAiState& s);
    float DemeanourFlee(BaseAiState& s);
    float DemeanourCover(BaseAiState& s);
    float DemeanourWander(BaseAiState& s);
    float DemeanourInvestigate(BaseAiState& s);
    // `Controller::SomethingMoveTypeRelated @0052e6b0`: the move type.
    bool ControllerThink(BaseAiState& s);
    // MILESTONE 13: BaseAI::ExecuteWaypointAction @005243c0, and the Rand Move
    // wander of Controller_IssueMoveOrderAndMaybeRandomizeAnywhereDestination.
    void RunWaypointAction(LiveEntity& e, const WaypointNode& w);
    bool ControllerRandomMove(BaseAiState& s, const ControllerState& c);
    bool SentryThink(LiveEntity& e);
    bool MoveToward(BaseAiState& s, float wx, float wz, bool run);
    float EntityLightLevel(const LiveEntity& e) const;
    void RebuildTurnQueue();
    void BeginTurnFor(int32_t index);
    void Logf(const char* fmt, ...);
    float FrameMoveDistance(const LiveEntity& e) const;
    void BakeEntity(LiveEntity& e);
    void DrawDecals(int64_t camX, int64_t camY, int32_t W, int32_t H, uint8_t* out);
    const Bitmap* DecodeImage(const LiveEntity& e, const Collection& col, int32_t layer);

    // MILESTONE 17.  The `<entity_file>` slot table indexed by slot number, so
    // a `kEshEntityRef` (type 12) property can be resolved.  A type-12 payload
    // is four bytes -- `u16 slot ; u16 sequenceNumber` -- and the slot record's
    // own second `i16` is that sequence number; a zero slot means "no entity".
    // This is how a Container reaches its `Inventory`, an `Inventory` reaches
    // its `ILink<n>` items, and a map Actor reaches `Hand1` / `Hand2` /
    // `Armour` / `unarmedSlot1` / `unarmedSlot2`.  Nothing before milestone 17
    // followed those references, which is why every map actor's loadout came
    // from its prototype and every container looked empty.
    std::unordered_map<int32_t, size_t> m_slotIndex;
    const EntityInstance* ResolveRef(const Esh& owner, const std::string& prop) const;
    // Reads one item entity (a Collectable subclass -- Weapon, Ammo, Armour,
    // Consumable, GenericItem ...) into an InvItem.
    void ReadItemInstance(const EntityInstance& inst, LiveEntity::InvItem* out) const;
    // Walks an `Inventory` entity's `IListSz` / `ILink<n>` / `ILinkCnt<n>` list
    // and appends every item to `out`, in the list's own forward order.
    void ReadInventoryList(const Esh& inv, std::vector<LiveEntity::InvItem>* out) const;
    // `tables/races.txt`'s four unarmed tables and the lookup
    // `GameData::GameData__GetRaceUnarmedTableEntry @0061f0f0` runs over them.
    struct UnarmedRow {
        int32_t race = 0, entity = 0, level = 0, unarmed = 0, strength = 0, agility = 0;
    };
    std::vector<std::string> m_raceNames, m_punchEnts, m_kickEnts;
    std::vector<UnarmedRow> m_punchTable, m_kickTable;
    bool LoadRaceTables();
    std::string UnarmedEntityFor(const std::string& race, const Attributes& a, int32_t level,
                                 bool kick) const;
    void EquipUnarmed(LiveEntity& e, const std::string& race);

    int32_t m_lootTarget = -1;
    int32_t m_lootActor = -1;

    World m_world;
    std::string m_misPath;
    size_t m_squadAdded = 0;   // squad records appended to the world's own table   // the path Load was given; the save form names it
    SceneOptions m_opt;
    std::unique_ptr<PathIndex> m_tileIndex, m_spriteIndex, m_entIndex;
    std::unique_ptr<SpriteCache> m_sprites;
    std::unique_ptr<EntProtoCache> m_protos;

    std::vector<DecodedTile> m_tiles;      // by prototype id, frame 0
    // MILESTONE 13.  An ANIMATED TILE: `.til` carries `frameCount` ZAR images
    // (mapview/NOTES.md section 1) and 26 of mission01's prototypes have more
    // than one -- `Generic tiles/Generic objects/Fire/Flame Normal.til` and
    // `.../Fire Barrel/Burning Barrel.til` have fifteen each, which is the
    // raider camp's fires.  Frames 1..n-1 live here; frame 0 stays in m_tiles
    // so every existing rect and cull stays exactly as it was.
    std::vector<std::vector<DecodedTile>> m_tileFrames;   // by prototype id
    size_t m_animatedTileProtos = 0;
    std::vector<PlacedTile> m_placed;
    std::vector<LiveEntity> m_entities;
    SceneLighting m_scene;
    std::vector<int32_t> m_lightRate;      // "Rate of change (ms)" per light
    std::vector<int64_t> m_lightAccum;

    std::unordered_map<uint64_t, Bitmap> m_imageCache;   // raw decoded layers
    // MILESTONE 18: one composited `guibig`/`guismall` icon per (sprite, size).
    std::map<std::string, Bitmap> m_iconCache;
    std::unordered_map<uint64_t, Bitmap> m_bakedCache;   // tint applied
    std::unordered_map<uint64_t, std::vector<int16_t>> m_tableCache;

    int64_t m_nowMs = 0;
    int64_t m_ticks = 0;
    int64_t m_tickAccumMs = 0;
    bool m_interpolate = true;      // MILESTONE 14, --no-interp turns it off
    int64_t m_bounds[4] = {0, 0, 0, 0};
    int32_t m_selected = -1;
    std::vector<int32_t> m_sel;
    std::string m_info;
    std::vector<std::string> m_manifest;

    NavGrid m_nav;
    int32_t m_spawn[3] = {0, 0, 0};
    int32_t m_spawnPlayer = -1;
    bool m_haveSpawn = false;
    std::vector<int32_t> m_squad;
    bool m_lastPathExact = false;
    size_t m_lastPathExpanded = 0;

    // The draw list Render last built, for Pick.
    std::vector<int32_t> m_lastEntityOrder;  // front-to-back is reverse iteration

    // milestone 8
    Rng m_rng{1};
    int32_t m_combatMode = 0;              // World+0x427
    std::vector<int32_t> m_turnQueue;      // World+0x42C, ordered by the key below
    std::vector<float> m_turnKey;          // TurnQueueEntry::RecomputeOrderKey
    size_t m_turnAt = 0;                   // World+0x43C
    int64_t m_round = 0;
    std::vector<std::string> m_log;
    std::vector<Decal> m_decals;
    bool m_aiEnabled = true;

    // milestone 10
    MissionScript m_script;
    // --- milestone 13 -----------------------------------------------------
    Audio* m_audio = nullptr;          // not owned; main.cpp holds it
    float m_listener[3] = {0, 0, 0};
    bool m_musicStarted = false;
    bool m_combatMusic = false;
    int64_t m_combatUntilMs = 0;
    bool m_scriptEnabled = true;
    int32_t m_centreRequest = -1;

    // milestone 11
    AiTable m_aiTable;
    std::vector<BaseAiState> m_ai;              // one per BaseAI in the .mis
    std::vector<ControllerState> m_controllers; // one per Controller, by player
    std::vector<StatusFloat> m_statusFloats;
    std::vector<std::string> m_detectLog;
    bool m_traceDetect = false;
    // The campaign difficulty multiplier `GetDifficultyModifierForAttackerTarget`
    // returns; 1.0 is Normal.  `SetDifficulty` is what `--difficulty` moves.
    float m_difficultyMultiplier = 1.0f;
};

}  // namespace game

#endif  // GAME_SCENE_H
