// ai.h -- milestone 11: the non-player AI.
//
// Three classes carry it in the shipped engine and all three are entities in
// the `.mis` slot table, so the map's own data drives everything here:
//
//   BaseAI      one per AI actor, `Attached` to it.  Holds the actor's
//               `Nature`, `Tactic`, `Use Cover`, `Sleeping`, its current
//               target and the ten per-demeanour desires.  Its think is
//               `BaseAI::DoEngineThink @005128f0`.
//   Controller  one per PLAYER slot (not per actor).  Holds the player's
//               `Controller Type` -- the Player Move State the mission script
//               sets -- plus `Move Target Tag`, `Working Zone`, `Retreat
//               Zone`, `Aggression`, `Communicate`, `Mobility` and the
//               waypoint chain.  Its dispatch is
//               `Controller::SomethingMoveTypeRelated @0052e6b0`.
//   SentryAI    the player-side overwatch mode -- the game bar's sentry
//               column, `sentryOff` / `sentryScout` / `sentryOn` in keys.cfg.
//               `SentryAI::SentryAIMaybeCheckingTargets @0053ef80`.
//
// The desire model is `tables/aitable.txt`, which the engine reads verbatim:
// ten natures, nine events, ten demeanours, one starting-desire row, a
// nature x demeanour preference matrix and an event x demeanour reaction
// matrix.  `BaseAI::BuildDemeanourNatureFunctionTable @0051e8a0` builds one
// record per demeanour from `starting_demeanours[d] * 0.1` and
// `demeanours_versus_natures[nature][d] * 0.1` and binds each to its think
// function; `BaseAI::BaseAIEngineThinkRelated @0051e1d0` raises them from
// `demeanours_versus_events[event][d] * 0.1`; and
// `BaseAI::UpdateDemeanourNatureMoveSpeed @0051e520` sorts the records and
// runs the top one.  The 0.1 is `_DAT_0080a6fc`, read out of `.rdata`.
//
// Sources for everything numeric are named at the point of use in ai.cpp and
// in scene.cpp's AI section; NOTES.md section 44 collects them.

#ifndef GAME_AI_H
#define GAME_AI_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace game {

// The ten demeanours, in `tables/aitable.txt`'s own column order -- which is
// also the order `BuildDemeanourNatureFunctionTable` tests the names in.
enum Demeanour {
    kAttack = 0,
    kMoveFlee,
    kMoveCover,
    kMoveWander,
    kMiscAssistHeal,
    kMiscEatCorpse,
    kMiscLayTrap,
    kMiscDisarmTrap,
    kMiscTriggerAlarm,
    kMiscInvestigate,
    kDemeanourCount,
};
const char* DemeanourName(int d);

// The nine events, in the table's row order.
enum AiEvent {
    kEvTookDamage = 0,
    kEvSeeTarget,
    kEvSeeCorpse,
    kEvSeeTrap,
    kEvHearCombat,
    kEvCaughtThief,
    kEvOverridePassify,
    kEvOverrideFlee,
    kEvOverrideAttack,
    kAiEventCount,
};
const char* AiEventName(int e);

// `tables/aitable.txt`, parsed.  The file's own header says "Order of natures,
// Demeanours, and Events must be kept relative to column/row in tables", and
// `BaseAI::BuildDemeanourNatureFunctionTable` refuses to run at all unless the
// demeanour list has exactly ten entries ("Only %d of %d listed in AI Table").
struct AiTable {
    std::vector<std::string> natures;      // 10 rows of the two matrices
    std::vector<std::string> events;       // 9
    std::vector<std::string> demeanours;   // 10, must match the enum above
    std::array<int32_t, kDemeanourCount> starting{};
    std::vector<std::array<int32_t, kDemeanourCount>> vsNature;   // [nature][demeanour]
    std::vector<std::array<int32_t, kDemeanourCount>> vsEvent;    // [event][demeanour]

    bool Load(const std::string& path, std::string* errorOut);
    int32_t NatureIndex(const std::string& name) const;
    bool Loaded() const { return !demeanours.empty(); }
};

// One waypoint of a patrol route.  `WayPoint` entities carry `Tag Name` (the
// route's name -- every node of one route shares it), `Waypoint Index`,
// `Next Waypoint List`, `Pause Time` and `Action`; the shipped actions are
// None, Look Around, Magic High, Magic Low, Punch and Open Door.
struct WaypointNode {
    int32_t entity = -1;         // index into Scene::m_entities
    int32_t index = 0;           // "Waypoint Index"
    float pause = 0.0f;          // "Pause Time", seconds
    std::string action;          // "Action"
    std::string nextList;        // "Next Waypoint List"
    int32_t world[3] = {0, 0, 0};
};

// The per-player Controller.  The `Controller Type` values that
// `Controller::SomethingMoveTypeRelated @0052e6b0` dispatches on are exactly
// Deactivate, Waypoints, RandomWaypoints, Stationary, PerimeterZone, Follow
// and Flee; anything else logs "Uknown moveType %s" (the typo is the
// binary's).  Absent means Stationary, which is the constructor's default.
struct ControllerState {
    int32_t entity = -1;
    int32_t playerIndex = -1;
    std::string type = "Stationary";
    std::string moveTargetTag;      // "Move Target Tag": the route or the follow target
    int32_t movePriority = 0;       // "Move Type Priority", the gate in DoEngineThink
    int32_t aggression = 0;         // "Aggression"
    int32_t communicate = 0;        // "Communicate"
    int32_t mobility = 0;           // "Mobility"
    std::string workingZone = "Anywhere";
    std::string retreatZone = "Anywhere";
    bool groupMove = false;
    bool randMove = false;
    bool neverLeaveZone = false;
    int32_t followTarget = -1;

    std::vector<int32_t> units;         // the actors in this player slot
    std::vector<WaypointNode> route;    // built at load, ordered by Waypoint Index
    // The working zone's centre, which `SomethingMoveTypeRelated` computes as
    // `(min + max) * 0.5` of the zone box and every move type steers by.
    float centre[3] = {0, 0, 0};
    bool haveCentre = false;
    float retreatCentre[3] = {0, 0, 0};
    bool haveRetreatCentre = false;
};

// The per-actor BaseAI.
struct BaseAiState {
    bool valid = false;
    int32_t entity = -1;        // the actor this is `Attached` to
    int32_t controller = -1;    // index into Scene::m_controllers, by player slot

    // Straight off the `.mis` BaseAI record.
    std::string nature = "Default";
    std::string tactic = "Advance";     // Advance | Retreat | Flank
    bool useCover = false;
    bool useSneak = false;
    bool sleeping = false;
    int32_t natureIndex = 0;

    // `BaseAI+0x201` highestdesire, `+0x205` targetrating, `+0x1e5`
    // currenttarget, `+0x1f5` thinkrate, `+0x1fd` pathattempts, `+0x1c5`
    // currentaction (the demeanour's own name).
    float desire[kDemeanourCount] = {0};   // the base = starting * 0.1, raised by events
    // The event contribution alone.  `highestdesire` (BaseAI+0x201) starts at
    // 0 and is raised only by `BaseAIEngineThinkRelated`, so it is the flag
    // that says whether the AI is reacting to something or is idle and under
    // its Controller's move type.
    float eventDesire[kDemeanourCount] = {0};
    float natureWeight[kDemeanourCount] = {0};
    float highestDesire = 0.0f;
    float targetRating = 0.0f;
    int32_t currentTarget = -1;
    int32_t currentDemeanour = -1;
    std::string currentAction;
    float thinkRate = 1.5f;
    int32_t pathAttempts = 0;
    bool suppressed = false;
    bool announced = false;    // the log line for the current demeanour has run

    // Deadlines, in World.realtime seconds.
    double nextEngineThink = 0.0;
    double nextPerceive = 0.0;

    // `BaseAI+0x254` vislist: the entities this AI has seen, which is what the
    // Attack demeanour picks its target out of.
    std::vector<int32_t> visList;

    // Patrol progress.  The Controller owns the route; the position along it
    // is per-unit (`Controller::ControllerWaypointCheckingRepeatFlag` keys its
    // per-entity records on the unit's id).
    int32_t waypointAt = 0;
    double waypointResumeAt = -1.0;
    bool waypointForward = true;
    bool everDetected = false;
};

// The player-side sentry, i.e. the game bar's sentry column.  `SentryAI` has
// `mode` (SentryAI+0x1ad), `accuracy` and `location`; keys.cfg binds the three
// modes to comma / period / slash as sentryOff / sentryScout / sentryOn.
enum SentryMode {
    kSentryOff = 0,     // hold fire
    kSentryScout = 1,   // return fire only
    kSentryOn = 2,      // engage anything in range
};

// One detection evaluation, kept whole so the verification runs can print the
// inputs.  Every field names its term in
// `Actor::Actor_TrySneakPastDetection @00544770`.
struct DetectTerms {
    float base = 0.0f;        // (PE * deathSense + 4) * 3
    float perception = 0.0f;
    float facing = 1.0f;      // 1 inside 60 deg, then 1 - (deg - 60) / 120
    float angleDeg = 0.0f;
    float sneak = 1.0f;       // 100 / ((sneakSkill - observerSneak * 0.5) + 100)
    float sneakSkill = 0.0f;
    float light = 1.0f;       // (lightLevel + 0.5) * 0.6666667
    float lightLevel = 1.0f;
    float posture = 1.0f;     // 1 stand, 0.8 crouch, 0.6 prone
    float action = 1.0f;      // 2 quiet attack, 1.33 skill use, 1.2 / 1.1, else 1
    float level = 1.0f;       // 0.75 when CanDetectEntity says 2
    float radius = 0.0f;      // the product
    float distance = 0.0f;    // box-gap distance, the same measure to-hit uses
    bool blocked = false;     // no line
    bool detected = false;
};

}  // namespace game

#endif  // GAME_AI_H
