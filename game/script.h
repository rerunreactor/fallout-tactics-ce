// script.h -- milestone 10: mission scripting.
//
// The `.mis` world stream carries, after the entity file, a run of scripting
// sections that milestones 2-9 skipped:
//
//   <world_zone> 2      named axis-aligned boxes in metres
//   <Player> 8          one per player slot; carries the side flag and, in its
//                       nested <PInfoH>, the team id
//   <Team> 2            nine team names, then the 9x9 alignment matrix
//   <speechList> 1      <speech_node> 3 records: six (line, event) pairs each
//   <mnob> 2            minimap objective markers
//   <TriggerManager> 1  a count and that many <Trigger> 4 records
//
// Every layout below is read off the binary's own writers; the function that
// establishes each one is named at the point of use.  The two that matter most:
//
//   Trigger::SavePropertiesAndCheckDirty @00679a60 and its reader
//     Trigger::ReadTriggerFromStream @00679830 give the <Trigger> record:
//     name, float delay, float nextEval, bool armed, bool repeat (v > 1),
//     u32 actionCount + that many actions, u32 conditionCount + that many
//     conditions.  An action or a condition is a length-prefixed CLASS NAME
//     followed by one <esh> block -- the same tagged-property format
//     mapview/esh.* already reads -- so the whole scripting layer decodes with
//     no new codec.
//
//   GetFactionRelationValue @00505880 is hostility, and it is a
//     table lookup, not a heuristic: 0 when either side is player 0, 10 when
//     both are the same player, matrix[teamA][teamB] when both teams are
//     non-zero, and -10 otherwise.  The matrix is World+0x520, a 9x9 signed
//     byte block that World::InitWorldTeamsAndDefaults @00505200 fills with
//     the identity (10 on the diagonal, -10 at [0][0]) before the file
//     overwrites it.  ActionSetTeamAlignment::Vtable15 @006833b0 writes
//     matrix[a*9 + b], which pins the index order.
//
// The evaluation model is Trigger::TriggerConditionUpdateTimer @00679050:
//
//     if (!repeat && fireCount > 0) return;                  // one-shot
//     if ((mask & 1) && nextEval < World.realtime) {         // not event-driven
//         if (!armed) {
//             nextEval = delay + World.realtime;             // delay is 1.0 s
//             if (pending || AllConditionsTrue()) armed = true;
//         }
//     }
//     if (!armed) return;
//     allDone = true;
//     for (a : actions) {
//         if (!a->Poll()) allDone = false;                   // slot 10
//         if (!a->Continue()) return;                        // slot 11, blocking
//     }
//     if (!allDone) return;
//     pending = false; ++fireCount;
//     if (repeat) Rearm();
//
// and one action is TriggerAction::PollTriggerCondition @0068c9a0:
//
//     if (!started) { Execute(); started = true; nextTick = realtime; }
//     if (!finished && nextTick < realtime) {
//         finished = IsComplete();
//         nextTick = PollInterval() + realtime;
//     }
//     return finished;
//
// so `started`/`finished`/`blocking`/`nextTick` (TriggerAction::Serialize
// @0068c8f0) are the base class's four fields and every subclass's own
// properties start after them.
//
// The condition mask is Trigger::TriggerComputeSpecialConditionMask @00679520:
// bit 1 is "ordinary, poll me on the timer", bit 2 "Skill Use Occured", bit 4
// "Speech Occured", bit 8 "Right Click on Tab".  A trigger with an event
// condition is not polled; the event fires it.

#ifndef GAME_SCRIPT_H
#define GAME_SCRIPT_H

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "esh.h"

namespace game {

class Scene;

// ---------------------------------------------------------------------------
// QualifiedNumber -- the "has" operand of every quantity condition.
//
// `QualifiedNumber::EvaluateAgainstRandomValue @005ec730`: five bytes on the
// wire, { int8 mode; int16 a; int16 b }, and the comparison draws a uniform
// integer from [min(a,b), max(a,b)] first:
//
//     mode 0  ->  measured <  roll
//     mode 1  ->  measured >  roll
//     mode 2  ->  measured == roll
//     else       "invalid qualified number mode"
//
// The DEFAULT, which is what an absent property means, is { 0, 1, 1 } -- the
// packed literal 0x01000100 that ConditionQuantityCheckTeam::Serialize
// @006909d0 passes -- i.e. "measured < 1", which for a count is "== 0".  That
// is what makes `Quantity - Unit, Unit 'Elder', type Alive` with no `has`
// property read as "the Elder is dead".
// ---------------------------------------------------------------------------
struct QualifiedNumber {
    int8_t mode = 0;
    int16_t a = 1, b = 1;

    static QualifiedNumber FromPayload(const std::vector<uint8_t>& p);
    // `rng` is only consulted when a != b; every shipped record has a == b.
    bool Evaluate(int32_t measured) const;
    std::string Describe() const;
};

// A string pair, esh type 20 -- the `type` property of the quantity
// conditions, which is (quantity kind, attribute/skill name).
struct StringPair {
    std::string first, second;
    static StringPair FromPayload(const std::vector<uint8_t>& p);
};

// esh type 17, two bytes on the wire: a validity byte and the player index.
// Every `Player` / `Player1` / `Players at Exit Grid` property is one of these
// and the shipped data always writes 0x01 in the first byte.
int32_t PlayerRefFromPayload(const std::vector<uint8_t>& p);

// ---------------------------------------------------------------------------
// One condition or one action: the class name the factory looked up
// (`FindTriggerByName @00678a80` walks a registry comparing vtable slot 6's
// name string) and the <esh> the object then deserialised from.
// ---------------------------------------------------------------------------
struct ScriptObj {
    std::string cls;
    ft::Esh esh;

    // TriggerAction's four base fields (TriggerAction::Serialize @0068c8f0).
    bool blocking = false;

    // Runtime, reset by Trigger::Rearm (FUN_006791d0 @006791d0, vtable slot 9
    // on every action).
    bool started = false;
    bool finished = false;
    float nextTick = 0.0f;
    // ActionWait's deadline, in seconds of World.realtime.
    float waitUntil = 0.0f;

    std::string Str(const char* name) const;
    int32_t Int(const char* name, int32_t dflt = 0) const;
    bool Bool(const char* name, bool dflt = false) const;
    float Float(const char* name, float dflt = 0.0f) const;
    QualifiedNumber Qual(const char* name) const;
    StringPair Pair(const char* name) const;
    int32_t PlayerRef(const char* name) const;
};

struct Trigger {
    std::string name;
    float delay = 1.0f;      // Trigger+4, the poll interval; 1.0 everywhere
    float nextEval = 0.0f;   // Trigger+8
    bool armed = false;      // Trigger+0x25
    bool repeat = false;     // Trigger+0xc
    int32_t version = 4;
    std::vector<ScriptObj> actions;    // Trigger+0x11 / +0x15
    std::vector<ScriptObj> conds;      // Trigger+0x1d / +0x21

    // Runtime.
    uint32_t mask = 1;       // Trigger+0x26
    bool pending = false;    // Trigger+0x2a
    int32_t fireCount = 0;   // Trigger+0x2b
};

struct WorldZone {
    std::string name;
    uint8_t flagA = 0;       // leading byte
    float colour[3] = {1, 1, 1};
    float box[6] = {0, 0, 0, 0, 0, 0};   // min xyz, max xyz, in metres
    uint8_t flagB = 0;

    bool Contains(const float p[3]) const {
        return p[0] >= box[0] && p[0] <= box[3] && p[1] >= box[1] && p[1] <= box[4] &&
               p[2] >= box[2] && p[2] <= box[5];
    }
};

struct PlayerDef {
    std::string name;
    int32_t side = 0;        // 0 scenery, 1 AI, 2 the human squad
    int32_t team = 0;        // <PInfoH> +13, u16
};

struct TeamDef {
    std::string name;
    uint8_t enabled = 0;
};

// <speech_node> 3, from SpeechNode::SaveSpeechNodeProperties @004ff230: a
// leading bool, the node name, six (line-key, event-name) pairs and a trailing
// string.  "None" in a line slot means the slot is unused.
struct SpeechNode {
    std::string name;
    bool flag = false;
    std::string line[6], event[6];
    std::string tail;
};

// ---------------------------------------------------------------------------
// MILESTONE 13: <ambientSound> 3 -- the ambient sound zones.
// ---------------------------------------------------------------------------
// `LoadAmbientSoundSection @004dd0b0` reads these out
// of the `.mis`; `ParseAmbientSoundZoneConfig @004daf40` is the *editor* side,
// which compiles `data/core/editor/ambientExample.txt`'s
// `<name>_numOfFiles` / `_file_%d` / `_minDist` / `_maxDist` / `_volume` /
// `_volumeVar` / `_pitchVar` / `_looping` / `_random` / `_minTime` / `_maxTime`
// / `_timeOfDay` / `_notTimeOfDay` / `_useRadius` / `_position` / `_zone` /
// `_overrideZone` keys into exactly the record below.  Only the compiled form
// ships (no mission carries a sidecar `.txt`), so the field ORDER here is read
// off the shipped records and checked against those key names one for one --
// see NOTES.md 69.5.
struct AmbientSound {
    std::string name;
    int32_t id = 0;               // 5000 + the table index (ParseAmbientSoundTable)
    bool random = true;           // `_random`: pick, versus walk the list in order
    std::vector<std::string> files;
    bool useRadius = false;       // `_useRadius`: a point source, not a zone bed
    float pos[3] = {0, 0, 0};     // `_position`, Integer3, in metres
    int32_t minDist = 5;          // `_minDist` -- PlaySound's `volume` argument
    int32_t maxDist = 25;         // `_maxDist` -- PlaySound's `cutoff_distance`
    int32_t volume = 100;         // `_volume`, 0..100
    float volumeVar = 0;          // `_volumeVar`
    float pitchVar = 0;           // `_pitchVar` -- NOT applied; see NOTES.md 69.8
    bool looping = false;         // `_looping`
    int32_t minTime = 20;         // `_minTime`, seconds between plays
    int32_t maxTime = 120;        // `_maxTime`
    int32_t timeOfDay = 0;        // `_timeOfDay`: 0 always, else a phase index
    bool notTimeOfDay = false;    // `_notTimeOfDay`: invert the phase test
    std::string zone;             // `_zone`, a <world_zone> name ("" == everywhere)
    std::vector<std::string> overrideZones;  // `_overrideZone`, ';'-separated

    // Runtime, mirroring AmbientSound+0x55 (the next play time, in
    // World.realtime) and +0x71 (the last file index).
    int64_t nextMs = 0;
    int32_t lastFile = -1;
    bool started = false;
};

struct MinimapObj {
    int32_t x = 0, y = 0;
    std::string name;
    int32_t a = 0, b = 0, c = 0;
};

// ---------------------------------------------------------------------------
// What the interface has to draw.
// ---------------------------------------------------------------------------
struct Objective {
    std::string key;        // the locale key, e.g. "M01_OBJ_01"
    bool complete = false;
};

// ActionForceSpeech / the click and random speech tables put one of these over
// an entity.  The bubble's dwell time is NOT recoverable -- `WSpeechBubble`
// is a call site only -- so kSpeechMs is a labelled placeholder.
struct FloatingText {
    int32_t entity = -1;
    std::string text;
    int64_t untilMs = 0;
};

enum class MissionEnd { None, WorldMap, FailScreen, DeathScreen };

// ---------------------------------------------------------------------------
// The engine.
// ---------------------------------------------------------------------------
class MissionScript {
public:
    // Re-reads and re-inflates the `.mis` and parses every scripting section.
    // (mapview's `World::Load` keeps only the map manager and the entity file;
    // rather than change a structure three other programs share, this reads the
    // file again -- a 4 MB inflate, about 30 ms.)
    bool Load(const std::string& misPath, std::string* errorOut);
    bool Loaded() const { return m_loaded; }

    // One engine tick, called from Scene::Update once per 70 ms step.
    void Tick(Scene& scene);

    // Events the rest of the game raises into the script layer.
    void RaiseSpeechEvent(const std::string& event);
    // "Speech Occured" triggers fire on the event, not on the timer.
    void PlaySpeechNode(Scene& scene, int32_t entity, const std::string& node);
    // MILESTONE 13: `locale/missions/<mission>/<line key>.mp3`.
    void PlaySpeechLine(Scene& scene, int32_t entity, const std::string& key);

    // --- what Load parsed -------------------------------------------------
    const std::vector<Trigger>& Triggers() const { return m_triggers; }
    const std::vector<WorldZone>& Zones() const { return m_zones; }
    const std::vector<PlayerDef>& Players() const { return m_players; }
    const std::vector<TeamDef>& Teams() const { return m_teams; }
    const std::vector<SpeechNode>& Speech() const { return m_speech; }
    // MILESTONE 18: `Scene::ClickSpeech` needs the `ActionSetClickSpeech`
    // override, which is this table.
    const std::unordered_map<int32_t, std::string>& ClickSpeechOverrides() const {
        return m_clickSpeech;
    }
    const std::vector<MinimapObj>& MinimapObjs() const { return m_mnobs; }
    // MILESTONE 13.  Non-const: the ambient records carry their own timers.
    std::vector<AmbientSound>& Ambients() { return m_ambients; }
    const std::vector<AmbientSound>& Ambients() const { return m_ambients; }
    // `LoadAmbientMusicAndOverlays @004f3e90`'s two keys, which the `.mis`
    // carries as an untagged record right after the last <ambientSound>.
    const std::string& AmbientMusic() const { return m_ambientMusic; }
    const std::vector<std::string>& AmbientOverlays() const { return m_ambientOverlays; }
    // The `.mis` basename, which is also the locale/speech directory name.
    const std::string& MisPath() const { return m_path; }
    const WorldZone* Zone(const std::string& name) const;

    // GetFactionRelationValue @00505880.  Returns 0 for an unknown player.
    int32_t Relation(int32_t playerA, int32_t playerB) const;
    int32_t TeamOf(int32_t player) const {
        return (player >= 0 && player < static_cast<int32_t>(m_players.size()))
                   ? m_players[static_cast<size_t>(player)].team
                   : 0;
    }
    void SetAlignment(int32_t teamA, int32_t teamB, int8_t v);

    // --- what the interface reads ----------------------------------------
    const std::vector<Objective>& Objectives() const { return m_objectives; }
    const std::vector<std::pair<int32_t, std::string>>& DebriefLines() const {
        return m_debrief;
    }
    bool DebriefShown() const { return m_debriefShown; }
    const std::string& WorldText() const { return m_worldText; }
    int64_t WorldTextUntil() const { return m_worldTextUntil; }
    // VIDEO_GAPS L7: the tutorial popup's DONE button.  `gui/back/Tutor.zar`'s
    // apron carries one control and the capture (f0373) shows the panel waiting
    // on it, so the timer is only the fallback.
    void DismissWorldText() {
        m_worldText.clear();
        m_worldTextUntil = 0;
    }
    bool ObjectivePopup() const { return m_objectivePopup; }
    void ClearObjectivePopup() { m_objectivePopup = false; }
    const std::vector<FloatingText>& Bubbles() const { return m_bubbles; }
    MissionEnd Ended() const { return m_end; }
    const std::string& DeathTextKey() const { return m_deathText; }
    bool ExitGridsOn() const { return m_exitGrids; }
    int32_t Reputation() const { return m_reputation; }
    int32_t Experience() const { return m_experience; }
    bool MissionComplete() const { return m_missionComplete; }
    // MILESTONE 12: `ActionSetMissionState`'s requests, queued for the
    // campaign layer to apply to its own CoreMission vector.  The pair is
    // (mission name key, the 0/1/2 the action's `State` enum resolves to).
    const std::vector<std::pair<std::string, int32_t>>& MissionStateRequests() const {
        return m_missionState;
    }
    void ClearMissionStateRequests() { m_missionState.clear(); }
    const std::string& MissionCompleteKey() const { return m_missionCompleteKey; }

    // Mission variables and the campaign CVARs, both `map<string,string>`
    // (ActionSetVariable's `Value` is a string in every shipped record).
    const std::map<std::string, std::string>& Vars() const { return m_vars; }
    const std::map<std::string, std::string>& CVars() const { return m_cvars; }
    void SetVar(const std::string& k, const std::string& v) { m_vars[k] = v; }
    // MILESTONE 12: a mid-mission save has to restore the objective list and
    // the exit-grid flag as well as the variables.
    void RestoreObjectives(const std::vector<Objective>& o) { m_objectives = o; }
    void SetExitGrids(bool on) { m_exitGrids = on; }

    // The running log of what the script did, newest last -- the milestone's
    // verification surface.
    const std::vector<std::string>& Log() const { return m_log; }

    // A readable dump of every trigger, for `--triggers`.
    std::string Describe() const;
    // The parse summary line.
    const std::string& Info() const { return m_info; }

private:
    bool EvaluateCondition(Scene& scene, const ScriptObj& c);
    // The ordinary path: run the actions of an armed trigger for one tick.
    void RunActions(Scene& scene, Trigger& t);
    // TriggerAction::PollTriggerCondition @0068c9a0.
    bool PollAction(Scene& scene, ScriptObj& a);
    // vtable slot 15: the action's effect, once.
    void ExecuteAction(Scene& scene, ScriptObj& a);
    // vtable slot 17: has it finished?
    bool ActionComplete(Scene& scene, const ScriptObj& a) const;
    void ComputeMask(Trigger& t) const;

    // EvaluateEntityQuantityCondition @004e0e20, reduced to the quantity kinds
    // the shipped campaign uses.
    int32_t Quantity(Scene& scene, const std::vector<int32_t>& set, const StringPair& type,
                     const WorldZone* zone, bool inLocation) const;
    std::vector<int32_t> EntitiesOfPlayer(Scene& scene, int32_t player) const;
    std::vector<int32_t> EntitiesOfTeam(Scene& scene, const std::string& team) const;
    int32_t EntityByTag(Scene& scene, const std::string& tag) const;

    void Logf(const char* fmt, ...);

    std::string m_path, m_info;
    bool m_loaded = false;

    std::vector<Trigger> m_triggers;
    std::vector<WorldZone> m_zones;
    std::vector<PlayerDef> m_players;
    std::vector<TeamDef> m_teams;
    std::vector<SpeechNode> m_speech;
    std::vector<MinimapObj> m_mnobs;
    std::vector<AmbientSound> m_ambients;    // MILESTONE 13
    std::string m_ambientMusic;
    std::vector<std::string> m_ambientOverlays;
    int8_t m_align[9][9] = {};

    std::map<std::string, std::string> m_vars, m_cvars;
    std::vector<Objective> m_objectives;
    std::vector<std::pair<int32_t, std::string>> m_debrief;
    std::vector<FloatingText> m_bubbles;
    // Set Click Speech / Set Random Speech: the node an entity says when
    // clicked, and the pool it says at random.
    std::unordered_map<int32_t, std::string> m_clickSpeech, m_randomSpeech;
    std::vector<std::string> m_pendingSpeechEvents;
    // `Set object script state` / `Object Script State`: a per-entity boolean
    // the mission scripts use as a general-purpose latch.  The serializer only
    // writes the property when it differs from its compile-time default, and
    // every shipped record writes `false`, so the default is true.
    std::unordered_map<int32_t, bool> m_objState;
    // `Set Timer` / `Stop Timer` / the `Timer` condition.  The delay is a
    // QualifiedNumber the writer leaves unnamed;
    // `ActionSetTimer::GetRandomTimerDelay @00679xxx` reads its two int16s and
    // draws a value between them, which is the same codec.
    struct TimerState {
        float startedAt = 0.0f;
        float seconds = 0.0f;
        bool descending = true;
    };
    std::map<std::string, TimerState> m_timers;
    std::vector<std::string> m_log;

    std::string m_worldText, m_deathText, m_missionCompleteKey;
    int64_t m_worldTextUntil = 0;
    bool m_objectivePopup = false;
    bool m_debriefShown = false;
    bool m_exitGrids = false;
    bool m_missionComplete = false;
    int32_t m_reputation = 0, m_experience = 0;
    std::vector<std::pair<std::string, int32_t>> m_missionState;
    MissionEnd m_end = MissionEnd::None;
    float m_realtime = 0.0f;
};

// How long a speech bubble and a world-text banner stay up.  PLACEHOLDER:
// neither `WSpeechBubble` nor the world-text window has a body in any dump.
constexpr int64_t kSpeechMs = 4000;
constexpr int64_t kWorldTextMs = 5000;

}  // namespace game

#endif  // GAME_SCRIPT_H
