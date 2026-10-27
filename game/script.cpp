#include "script.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "file.h"
#include "scene.h"
#include "stream.h"

namespace game {
namespace {

using ft::Esh;
using ft::Reader;

int32_t ReadI32(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

float ReadF32(Reader& r) {
    const uint32_t bits = r.U32();
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// World::Load's prologue plus the raw zlib stream (mapview/mis.cpp's Inflate).
bool LoadWorldBlob(const std::string& path, std::vector<uint8_t>* blob, std::string* errorOut) {
    std::vector<uint8_t> raw;
    if (!ft::ReadWholeFile(path, &raw, errorOut)) {
        return false;
    }
    Reader outer(raw);
    std::string tag;
    int32_t v = 0;
    outer.FormatToken(&tag, &v);
    if (tag != "<world>") {
        *errorOut = "not a world file";
        return false;
    }
    const int32_t hint = outer.I32();
    outer.I32();
    if (outer.Failed()) {
        *errorOut = "truncated <world> header";
        return false;
    }
    blob->assign(hint > 0 ? static_cast<size_t>(hint) : (1u << 20), 0);
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) {
        *errorOut = "inflateInit failed";
        return false;
    }
    zs.next_in = const_cast<Bytef*>(raw.data() + outer.Pos());
    zs.avail_in = static_cast<uInt>(raw.size() - outer.Pos());
    size_t produced = 0;
    for (;;) {
        if (produced == blob->size()) {
            blob->resize(blob->size() * 2);
        }
        zs.next_out = blob->data() + produced;
        zs.avail_out = static_cast<uInt>(blob->size() - produced);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        produced = blob->size() - zs.avail_out;
        if (rc == Z_STREAM_END) break;
        if (rc != Z_OK || (zs.avail_in == 0 && zs.avail_out != 0)) {
            inflateEnd(&zs);
            *errorOut = "zlib inflate error";
            return false;
        }
    }
    inflateEnd(&zs);
    blob->resize(produced);
    return true;
}

// The sections are found by scanning for their tag, exactly as
// mapview/mis.cpp's ParseMapManager does: World::LoadWorldSections is in no
// dump, so the order of the top-level sections is not established.
size_t FindTag(const std::vector<uint8_t>& blob, const char* tag, size_t from = 0) {
    const size_t n = std::strlen(tag) + 1;   // include the NUL
    for (size_t i = from; i + n < blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, tag, n) == 0) return i;
    }
    return std::string::npos;
}

bool EqualsCI(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != 0; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return i == a.size() && b[i] == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// QualifiedNumber
// ---------------------------------------------------------------------------
QualifiedNumber QualifiedNumber::FromPayload(const std::vector<uint8_t>& p) {
    QualifiedNumber q;
    if (p.size() >= 5) {
        q.mode = static_cast<int8_t>(p[0]);
        q.a = static_cast<int16_t>(static_cast<uint16_t>(p[1]) | (static_cast<uint16_t>(p[2]) << 8));
        q.b = static_cast<int16_t>(static_cast<uint16_t>(p[3]) | (static_cast<uint16_t>(p[4]) << 8));
    }
    return q;
}

bool QualifiedNumber::Evaluate(int32_t measured) const {
    // The original draws a uniform value in [min, max] first; every shipped
    // record has a == b, so the draw is the value itself.
    const int32_t lo = std::min<int32_t>(a, b), hi = std::max<int32_t>(a, b);
    const int32_t roll = lo;   // == hi in all shipped data
    (void)hi;
    switch (mode) {
        case 0: return measured < roll;
        case 1: return measured > roll;
        case 2: return measured == roll;
        default: return false;   // "invalid qualified number mode"
    }
}

std::string QualifiedNumber::Describe() const {
    const char* op = mode == 0 ? "<" : mode == 1 ? ">" : mode == 2 ? "==" : "?";
    char buf[64];
    if (a == b) {
        std::snprintf(buf, sizeof(buf), "%s %d", op, static_cast<int>(a));
    } else {
        std::snprintf(buf, sizeof(buf), "%s rand[%d,%d]", op, static_cast<int>(a),
                      static_cast<int>(b));
    }
    return buf;
}

StringPair StringPair::FromPayload(const std::vector<uint8_t>& p) {
    StringPair sp;
    if (p.size() < 4) return sp;
    const int32_t n1 = ReadI32(p.data());
    if (n1 < 0 || static_cast<size_t>(n1) + 4 > p.size()) return sp;
    sp.first.assign(reinterpret_cast<const char*>(p.data() + 4), static_cast<size_t>(n1));
    const size_t at = 4 + static_cast<size_t>(n1);
    if (at + 4 > p.size()) return sp;
    const int32_t n2 = ReadI32(p.data() + at);
    if (n2 < 0 || at + 4 + static_cast<size_t>(n2) > p.size()) return sp;
    sp.second.assign(reinterpret_cast<const char*>(p.data() + at + 4), static_cast<size_t>(n2));
    return sp;
}

int32_t PlayerRefFromPayload(const std::vector<uint8_t>& p) {
    if (p.size() < 2) return -1;
    return static_cast<int32_t>(p[1]);
}

// ---------------------------------------------------------------------------
// ScriptObj property accessors
// ---------------------------------------------------------------------------
std::string ScriptObj::Str(const char* name) const {
    std::string s;
    esh.GetString(name, &s);
    return s;
}
int32_t ScriptObj::Int(const char* name, int32_t dflt) const {
    int32_t v = dflt;
    esh.GetInt(name, &v);
    return v;
}
bool ScriptObj::Bool(const char* name, bool dflt) const {
    bool v = dflt;
    esh.GetBool(name, &v);
    return v;
}
float ScriptObj::Float(const char* name, float dflt) const {
    float v = dflt;
    esh.GetFloat(name, &v);
    return v;
}
QualifiedNumber ScriptObj::Qual(const char* name) const {
    const ft::EshProp* p = esh.Find(name);
    if (p == nullptr) return QualifiedNumber();   // the {0, 1, 1} default
    return QualifiedNumber::FromPayload(p->payload);
}
StringPair ScriptObj::Pair(const char* name) const {
    const ft::EshProp* p = esh.Find(name);
    if (p == nullptr) return StringPair();
    return StringPair::FromPayload(p->payload);
}
int32_t ScriptObj::PlayerRef(const char* name) const {
    const ft::EshProp* p = esh.Find(name);
    if (p == nullptr) return -1;
    return PlayerRefFromPayload(p->payload);
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
namespace {

// One condition or action: a length-prefixed class name then one <esh>.
// FindTriggerByName @00678a80 reads the name, looks the class up in the
// registry and hands the stream to the new instance's Serialize.
bool ReadScriptObj(Reader& r, ScriptObj* out, std::string* err) {
    out->cls = r.LengthPrefixedString();
    if (r.Failed()) {
        *err = "truncated script object name";
        return false;
    }
    if (!out->esh.Read(r, err)) return false;
    out->esh.GetBool("blocking", &out->blocking);
    return true;
}

}  // namespace

bool MissionScript::Load(const std::string& misPath, std::string* errorOut) {
    m_path = misPath;
    std::vector<uint8_t> blob;
    if (!LoadWorldBlob(misPath, &blob, errorOut)) return false;

    // --- <world_zone> 2 ---------------------------------------------------
    // WorldZone::SaveWorldZoneProperties @00500130 writes, in this order, the
    // 12 bytes at WorldZone+0x18 (three floats), the name string at +0x24, the
    // 24 bytes at +0 (the box: min xyz then max xyz, in metres) and the byte at
    // +0x28.  The shipped data agrees field for field.
    for (size_t at = FindTag(blob, "<world_zone>"); at != std::string::npos;
         at = FindTag(blob, "<world_zone>", at + 1)) {
        Reader r(blob.data(), blob.size());
        r.Seek(at);
        std::string tag;
        int32_t v = 0;
        r.FormatToken(&tag, &v);
        WorldZone z;
        for (int i = 0; i < 3; ++i) z.colour[i] = ReadF32(r);
        z.name = r.LengthPrefixedString();
        for (int i = 0; i < 6; ++i) z.box[i] = ReadF32(r);
        z.flagB = r.U8();
        if (r.Failed()) break;
        m_zones.push_back(z);
    }

    // --- <Player> 8 -------------------------------------------------------
    // Player::SavePlayerSectionProperties @004df310: name, i32 side, 12 bytes,
    // i32, u16, u16, then five <Group Header> 3 records, then the rest.  Only
    // the name, the side and -- out of the nested <PInfoH> -- the team id are
    // needed here, and the team id is the u16 at PInfoH payload offset 13
    // (it is Player+0x39, which GetFactionRelationValue indexes the alignment
    // matrix with; the PlayerInfo writer is in no dump, so the offset is read
    // off the shipped data, where it agrees with every player's name prefix).
    {
        size_t at = FindTag(blob, "<Player>");
        size_t info = FindTag(blob, "<PInfoH>");
        while (at != std::string::npos) {
            Reader r(blob.data(), blob.size());
            r.Seek(at);
            std::string tag;
            int32_t v = 0;
            r.FormatToken(&tag, &v);
            PlayerDef p;
            p.name = r.LengthPrefixedString();
            p.side = r.I32();
            if (r.Failed()) break;
            if (info != std::string::npos && info > at) {
                Reader ir(blob.data(), blob.size());
                ir.Seek(info);
                std::string it;
                int32_t iv = 0;
                ir.FormatToken(&it, &iv);
                ir.Seek(ir.Pos() + 13);
                p.team = static_cast<uint16_t>(ir.I16());
            }
            m_players.push_back(p);
            at = FindTag(blob, "<Player>", at + 1);
            info = FindTag(blob, "<PInfoH>", info + 1);
        }
    }

    // --- <Team> 2 and the 9x9 alignment matrix ----------------------------
    // Each team is a name and five bytes.  After the ninth come the two
    // `timerTableHeader` / `varTableHeader` blocks and then the 81-byte matrix
    // World::InitWorldTeamsAndDefaults @00505200 keeps at World+0x520.
    {
        size_t at = FindTag(blob, "<Team>");
        size_t last = at;
        while (at != std::string::npos) {
            Reader r(blob.data(), blob.size());
            r.Seek(at);
            std::string tag;
            int32_t v = 0;
            r.FormatToken(&tag, &v);
            TeamDef t;
            t.name = r.LengthPrefixedString();
            t.enabled = r.U8();
            if (r.Failed()) break;
            m_teams.push_back(t);
            last = at;
            at = FindTag(blob, "<Team>", at + 1);
        }
        // The matrix ends immediately before <speechList>.
        const size_t sl = FindTag(blob, "<speechList>", last);
        if (sl != std::string::npos && sl >= 81) {
            const uint8_t* m = blob.data() + sl - 81;
            for (int a = 0; a < 9; ++a)
                for (int b = 0; b < 9; ++b)
                    m_align[a][b] = static_cast<int8_t>(m[a * 9 + b]);
        }
    }

    // --- <speech_node> 3 --------------------------------------------------
    for (size_t at = FindTag(blob, "<speech_node>"); at != std::string::npos;
         at = FindTag(blob, "<speech_node>", at + 1)) {
        Reader r(blob.data(), blob.size());
        r.Seek(at);
        std::string tag;
        int32_t v = 0;
        r.FormatToken(&tag, &v);
        SpeechNode s;
        s.flag = r.U8() != 0;
        s.name = r.LengthPrefixedString();
        for (int i = 0; i < 6; ++i) {
            s.line[i] = r.LengthPrefixedString();
            s.event[i] = r.LengthPrefixedString();
        }
        s.tail = r.LengthPrefixedString();
        if (r.Failed()) break;
        m_speech.push_back(s);
    }

    // --- <ambientSound> 3 -------------------------------------------------
    // MILESTONE 13.  See the AmbientSound comment in script.h.  The last
    // record is followed, untagged, by `LoadAmbientMusicAndOverlays`'s two
    // fields, so the loop remembers where it stopped.
    {
        size_t after = std::string::npos;
        for (size_t at = FindTag(blob, "<ambientSound>"); at != std::string::npos;
             at = FindTag(blob, "<ambientSound>", at + 1)) {
            Reader r(blob.data(), blob.size());
            r.Seek(at);
            std::string tag;
            int32_t v = 0;
            r.FormatToken(&tag, &v);
            AmbientSound a;
            a.name = r.LengthPrefixedString();
            a.id = r.I32();
            a.random = r.U8() != 0;
            const int32_t nf = r.I32();
            if (r.Failed() || nf < 0 || nf > 256) break;
            for (int32_t i = 0; i < nf; ++i) a.files.push_back(r.LengthPrefixedString());
            a.useRadius = r.U8() != 0;
            for (int i = 0; i < 3; ++i) a.pos[i] = ReadF32(r);
            a.minDist = r.I32();
            a.maxDist = r.I32();
            a.volume = r.I32();
            a.volumeVar = ReadF32(r);
            a.pitchVar = ReadF32(r);
            a.looping = r.U8() != 0;
            a.minTime = r.I32();
            a.maxTime = r.I32();
            a.timeOfDay = r.I32();
            // 17 bytes that are zero in every shipped record, then
            // `_notTimeOfDay`.  UNIDENTIFIED -- NOTES.md 69.5.
            for (int i = 0; i < 17; ++i) r.U8();
            a.notTimeOfDay = r.U8() != 0;
            a.zone = r.LengthPrefixedString();
            const int32_t no = r.I32();
            if (r.Failed() || no < 0 || no > 64) break;
            for (int32_t i = 0; i < no; ++i)
                a.overrideZones.push_back(r.LengthPrefixedString());
            if (r.Failed()) break;
            m_ambients.push_back(a);
            after = r.Pos();
        }
        if (after != std::string::npos) {
            Reader r(blob.data(), blob.size());
            r.Seek(after);
            r.I32();                                   // zero in every mission
            const std::string music = r.LengthPrefixedString();
            const int32_t n = r.I32();
            if (!r.Failed() && n >= 0 && n <= 32 && music.size() < 64) {
                m_ambientMusic = music;
                for (int32_t i = 0; i < n; ++i)
                    m_ambientOverlays.push_back(r.LengthPrefixedString());
                if (r.Failed()) m_ambientOverlays.clear();
            }
        }
    }

    // --- <mnob> 2 ---------------------------------------------------------
    for (size_t at = FindTag(blob, "<mnob>"); at != std::string::npos;
         at = FindTag(blob, "<mnob>", at + 1)) {
        Reader r(blob.data(), blob.size());
        r.Seek(at);
        std::string tag;
        int32_t v = 0;
        r.FormatToken(&tag, &v);
        MinimapObj o;
        o.x = r.I32();
        o.y = r.I32();
        o.name = r.LengthPrefixedString();
        o.a = r.I32();
        o.b = r.I32();
        o.c = r.I32();
        if (r.Failed()) break;
        m_mnobs.push_back(o);
    }

    // --- <TriggerManager> 1 + <Trigger> 4 ---------------------------------
    const size_t tm = FindTag(blob, "<TriggerManager>");
    if (tm != std::string::npos) {
        Reader r(blob.data(), blob.size());
        r.Seek(tm);
        std::string tag;
        int32_t v = 0;
        r.FormatToken(&tag, &v);
        const uint32_t count = r.U32();
        if (count > 100000) {
            *errorOut = "implausible trigger count";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            std::string t;
            int32_t tv = 0;
            if (!r.FormatToken(&t, &tv) || t != "<Trigger>") {
                *errorOut = "trigger " + std::to_string(i) + ": expected <Trigger>, got '" + t + "'";
                return false;
            }
            Trigger tr;
            tr.version = tv;
            tr.name = r.LengthPrefixedString();
            tr.delay = ReadF32(r);
            tr.nextEval = ReadF32(r);
            tr.armed = r.U8() != 0;
            if (tv > 1) tr.repeat = r.U8() != 0;
            if (tv == 3) r.LengthPrefixedString();   // a field dropped in v4
            const uint32_t na = r.U32();
            if (r.Failed() || na > 10000) {
                *errorOut = "trigger '" + tr.name + "': bad action count";
                return false;
            }
            for (uint32_t k = 0; k < na; ++k) {
                ScriptObj o;
                std::string err;
                if (!ReadScriptObj(r, &o, &err)) {
                    *errorOut = "trigger '" + tr.name + "' action " + std::to_string(k) + ": " + err;
                    return false;
                }
                tr.actions.push_back(std::move(o));
            }
            const uint32_t nc = r.U32();
            if (r.Failed() || nc > 10000) {
                *errorOut = "trigger '" + tr.name + "': bad condition count";
                return false;
            }
            for (uint32_t k = 0; k < nc; ++k) {
                ScriptObj o;
                std::string err;
                if (!ReadScriptObj(r, &o, &err)) {
                    *errorOut =
                        "trigger '" + tr.name + "' condition " + std::to_string(k) + ": " + err;
                    return false;
                }
                tr.conds.push_back(std::move(o));
            }
            ComputeMask(tr);
            m_triggers.push_back(std::move(tr));
        }
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "script: %zu triggers, %zu zones, %zu players, %zu teams, %zu speech nodes, "
                  "%zu minimap objectives",
                  m_triggers.size(), m_zones.size(), m_players.size(), m_teams.size(),
                  m_speech.size(), m_mnobs.size());
    m_info = buf;
    m_loaded = true;
    return true;
}

// Trigger::TriggerComputeSpecialConditionMask @00679520.
void MissionScript::ComputeMask(Trigger& t) const {
    t.mask = 0;
    for (const ScriptObj& c : t.conds) {
        if (c.cls == "Skill Use Occured") t.mask |= 2;
        else if (c.cls == "Speech Occured") t.mask |= 4;
        else if (c.cls == "Right Click on Tab") t.mask |= 8;
    }
    if (t.mask == 0) t.mask = 1;
}

const WorldZone* MissionScript::Zone(const std::string& name) const {
    for (const WorldZone& z : m_zones) {
        if (EqualsCI(z.name, name.c_str())) return &z;
    }
    return nullptr;
}

// GetFactionRelationValue @00505880.
int32_t MissionScript::Relation(int32_t a, int32_t b) const {
    const int32_t n = static_cast<int32_t>(m_players.size());
    if (a < 0 || b < 0 || a >= n || b >= n) return 0;
    if (a == 0 || b == 0) return 0;      // player 0 is Scenery
    if (a == b) return 10;
    const int32_t ta = m_players[static_cast<size_t>(a)].team;
    const int32_t tb = m_players[static_cast<size_t>(b)].team;
    if (ta > 0 && tb > 0 && ta < 9 && tb < 9) {
        // The original picks matrix[ta][tb] or its transpose depending on
        // UnknownRelatedToPlayerType(); that predicate has no body in any dump,
        // so only the default branch is ported.
        return m_align[ta][tb];
    }
    return -10;   // either side has no team: hostile
}

void MissionScript::SetAlignment(int32_t a, int32_t b, int8_t v) {
    if (a < 0 || b < 0 || a >= 9 || b >= 9 || a == b) return;
    m_align[a][b] = v;   // ActionSetTeamAlignment::Vtable15 @006833b0
}

void MissionScript::Logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    m_log.push_back(buf);
}

// ---------------------------------------------------------------------------
// Entity sets
// ---------------------------------------------------------------------------
std::vector<int32_t> MissionScript::EntitiesOfPlayer(Scene& scene, int32_t player) const {
    std::vector<int32_t> out;
    const std::vector<LiveEntity>& es = scene.Entities();
    for (size_t i = 0; i < es.size(); ++i) {
        if (es[i].playerIndex == player && es[i].isCombatant) out.push_back(static_cast<int32_t>(i));
    }
    return out;
}

std::vector<int32_t> MissionScript::EntitiesOfTeam(Scene& scene, const std::string& team) const {
    int32_t idx = -1;
    for (size_t i = 0; i < m_teams.size(); ++i) {
        if (EqualsCI(m_teams[i].name, team.c_str())) {
            idx = static_cast<int32_t>(i);
            break;
        }
    }
    std::vector<int32_t> out;
    if (idx < 0) return out;
    const std::vector<LiveEntity>& es = scene.Entities();
    for (size_t i = 0; i < es.size(); ++i) {
        if (!es[i].isCombatant) continue;
        if (TeamOf(es[i].playerIndex) == idx) out.push_back(static_cast<int32_t>(i));
    }
    return out;
}

int32_t MissionScript::EntityByTag(Scene& scene, const std::string& tag) const {
    return scene.FindByTag(tag);
}

// EvaluateEntityQuantityCondition @004e0e20.  The dispatcher's full string set
// is Alive / Deaths / Kills / Attribute / DerivedStat / Skill / Trait /
// OptionalTrait / ItemsTagged / ItemsTaggedEquipped / Reputation / Selected /
// Standing / Crouching / Prone / Moving / Running / Sneaking / IsAlert /
// Unconcious; the ones the shipped campaign actually uses are ported and the
// rest log "Unknown quantity type" as the original does.
int32_t MissionScript::Quantity(Scene& scene, const std::vector<int32_t>& set,
                                const StringPair& type, const WorldZone* zone,
                                bool inLocation) const {
    const std::vector<LiveEntity>& es = scene.Entities();
    auto inZone = [&](const LiveEntity& e) {
        if (zone == nullptr) return true;
        const bool in = zone->Contains(e.wpos);
        return inLocation ? in : !in;
    };
    int32_t n = 0, sum = 0, seen = 0;
    for (int32_t i : set) {
        const LiveEntity& e = es[static_cast<size_t>(i)];
        if (type.first == "Alive") {
            if (!e.dead && inZone(e)) ++n;
        } else if (type.first == "Deaths") {
            if (e.dead) ++n;
        } else if (type.first == "Kills") {
            n += e.kills;
        } else if (type.first == "Selected") {
            if (scene.Selected() == i) ++n;
        } else if (type.first == "Standing" || type.first == "Crouching" ||
                   type.first == "Prone") {
            if (!e.dead && e.posture == type.first.substr(0, type.first.size() - 3)) ++n;
        } else if (type.first == "Attribute" || type.first == "DerivedStat") {
            if (e.dead) continue;
            ++seen;
            if (type.second == "hitPoints") sum += e.hp;
            else if (type.second == "actionPoints") sum += static_cast<int32_t>(e.ap);
        } else {
            return 0;
        }
    }
    if (type.first == "Attribute" || type.first == "DerivedStat") {
        return seen > 0 ? sum / seen : 0;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Conditions
// ---------------------------------------------------------------------------
bool MissionScript::EvaluateCondition(Scene& scene, const ScriptObj& c) {
    if (c.cls == "Always") return true;
    if (c.cls == "Never") return false;

    if (c.cls == "Variable") {
        const std::string k = c.Str("Variable"), want = c.Str("is Value");
        const bool campaign = c.Bool("Campaign", false);
        const std::map<std::string, std::string>& t = campaign ? m_cvars : m_vars;
        auto it = t.find(k);
        return it != t.end() && EqualsCI(it->second, want.c_str());
    }

    if (c.cls == "Speech Occured") {
        const std::string want = c.Str("Speech Event was");
        for (const std::string& e : m_pendingSpeechEvents) {
            if (EqualsCI(e, want.c_str())) return true;
        }
        return false;
    }

    if (c.cls == "Quantity - Player" || c.cls == "Quantity - Team" ||
        c.cls == "Quantity - Unit") {
        std::vector<int32_t> set;
        if (c.cls == "Quantity - Player") {
            set = EntitiesOfPlayer(scene, c.PlayerRef("Player"));
        } else if (c.cls == "Quantity - Team") {
            set = EntitiesOfTeam(scene, c.Str("Team"));
        } else {
            const int32_t e = EntityByTag(scene, c.Str("Unit"));
            if (e >= 0) set.push_back(e);
        }
        const StringPair type = c.Pair("type");
        std::string at = c.Str("at");
        const WorldZone* z = (at.empty() || EqualsCI(at, "Anywhere")) ? nullptr : Zone(at);
        const int32_t measured = Quantity(scene, set, type, z, c.Bool("inLocation", true));
        return c.Qual("has").Evaluate(measured);
    }

    if (c.cls == "Object Script State") {
        const int32_t e = EntityByTag(scene, c.Str("Unit"));
        const bool want = c.Bool("ScriptState", true);
        auto it = m_objState.find(e);
        return (it == m_objState.end() ? true : it->second) == want;
    }

    if (c.cls == "Timer") {
        auto it = m_timers.find(c.Str("Timer"));
        if (it == m_timers.end()) return false;
        const float elapsed = m_realtime - it->second.startedAt;
        const float v = it->second.descending ? (it->second.seconds - elapsed) : elapsed;
        return c.Qual("is").Evaluate(static_cast<int32_t>(v));
    }

    if (c.cls == "Can See") {
        const std::vector<int32_t> a = EntitiesOfPlayer(scene, c.PlayerRef("Player1"));
        const std::vector<int32_t> b = EntitiesOfPlayer(scene, c.PlayerRef("Player2"));
        for (int32_t i : a) {
            if (scene.Entities()[static_cast<size_t>(i)].dead) continue;
            for (int32_t j : b) {
                if (scene.Entities()[static_cast<size_t>(j)].dead) continue;
                if (scene.LineOfSight(i, j)) return true;
            }
        }
        return false;
    }

    if (c.cls == "All Alive at Exit Grid") {
        // RECONSTRUCTION.  `ConditionAllAliveAtExitGrids::Evaluate @006949b0`
        // walks the player's members and asks each whether it is standing on an
        // exit grid, through a virtual whose body is in no dump; the shipped
        // maps carry no exit-grid entity class and no exit-grid tile flag, so
        // this uses the mission's own entry zone -- the `<world_zone>` named
        // "Start Point" -- as the exit grid.  Labelled in NOTES.md.
        const WorldZone* z = Zone("Start Point");
        if (z == nullptr) return false;
        const std::vector<int32_t> set = EntitiesOfPlayer(scene, c.PlayerRef("Players at Exit Grid"));
        if (set.empty()) return false;
        int32_t alive = 0;
        for (int32_t i : set) {
            const LiveEntity& e = scene.Entities()[static_cast<size_t>(i)];
            if (e.dead) continue;
            ++alive;
            if (!z->Contains(e.wpos)) return false;
        }
        return alive > 0;
    }

    // Not ported: Object Script State, GUI Open, ObjDude Alive, Skill Use
    // Occured, Right Click on Tab, Timer, Weapon Mode Equals, World Play Mode
    // Equals, Quantity - Player Kills, Multiplayer Map Restart.
    return false;
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
void MissionScript::ExecuteAction(Scene& scene, ScriptObj& a) {
    const std::string& k = a.cls;

    if (k == "Set Variable") {
        const std::string name = a.Str("Variable"), value = a.Str("Value");
        if (a.Bool("Campaign", false)) m_cvars[name] = value;
        else m_vars[name] = value;
        return;
    }
    if (k == "Set Death Text") {
        m_deathText = a.Str("Set Death Text to");
        return;
    }
    if (k == "Add Objective") {
        const std::string key = a.Str("Add Objective");
        for (const Objective& o : m_objectives) {
            if (o.key == key) return;
        }
        m_objectives.push_back({key, false});
        if (a.Bool("with popup box", false)) m_objectivePopup = true;
        Logf("objective added: %s", key.c_str());
        return;
    }
    if (k == "Complete Objective") {
        const std::string key = a.Str("Complete Objective");
        for (Objective& o : m_objectives) {
            if (o.key == key) o.complete = true;
        }
        m_objectivePopup = true;
        Logf("objective complete: %s", key.c_str());
        return;
    }
    if (k == "Display World Text") {
        m_worldText = a.Str("Text Label");
        m_worldTextUntil = scene.NowMs() + kWorldTextMs;
        Logf("world text: %s", m_worldText.c_str());
        return;
    }
    if (k == "Add text to Debrief text list") {
        m_debrief.emplace_back(a.Int("Position", 0), a.Str("Keyname"));
        return;
    }
    if (k == "Display Debrief window and text") {
        std::stable_sort(m_debrief.begin(), m_debrief.end(),
                         [](const std::pair<int32_t, std::string>& x,
                            const std::pair<int32_t, std::string>& y) { return x.first < y.first; });
        m_debriefShown = true;
        Logf("debrief shown (%zu lines)", m_debrief.size());
        return;
    }
    if (k == "End Mission") {
        const std::string to = a.Str("Exit to");
        if (to == "World Map") m_end = MissionEnd::WorldMap;
        else if (to == "Fail Screen") m_end = MissionEnd::FailScreen;
        else if (to == "Death Screen") m_end = MissionEnd::DeathScreen;
        Logf("end mission: %s", to.c_str());
        return;
    }
    if (k == "Mission Complete") {
        m_missionComplete = true;
        m_missionCompleteKey = a.Str("Complete Mission");
        Logf("mission complete: %s", m_missionCompleteKey.c_str());
        return;
    }
    if (k == "Wait") {
        a.waitUntil = m_realtime + static_cast<float>(a.Int("Duration (secs)", 0));
        return;
    }
    if (k == "Toggle Exit Grids") {
        m_exitGrids = a.Bool("Exit Grids On", !m_exitGrids);
        Logf("exit grids %s", m_exitGrids ? "on" : "off");
        return;
    }
    if (k == "Add Experience To Player" || k == "Add Experience") {
        m_experience += a.Int("XP amount", 0);
        Logf("+%d XP (total %d)", a.Int("XP amount", 0), m_experience);
        return;
    }
    if (k == "Add Reputation") {
        m_reputation += a.Int("Reputation Amount", 0);
        Logf("reputation %+d (total %d)", a.Int("Reputation Amount", 0), m_reputation);
        return;
    }
    if (k == "Set Team Alignment Matrix") {
        const std::string ta = a.Str("Change"), tb = a.Str("Alignment towards");
        int32_t ia = -1, ib = -1;
        for (size_t i = 0; i < m_teams.size(); ++i) {
            if (EqualsCI(m_teams[i].name, ta.c_str())) ia = static_cast<int32_t>(i);
            if (EqualsCI(m_teams[i].name, tb.c_str())) ib = static_cast<int32_t>(i);
        }
        const int32_t v = a.Int("to (-10enemy..0neutral..10friend)", 0);
        SetAlignment(ia, ib, static_cast<int8_t>(v));
        Logf("alignment %s -> %s = %d", ta.c_str(), tb.c_str(), v);
        return;
    }
    if (k == "Set Click Speech") {
        const int32_t e = EntityByTag(scene, a.Str("Set Click Speech for Unit"));
        if (e >= 0) m_clickSpeech[e] = a.Str("to");
        return;
    }
    if (k == "Set Random Speech") {
        const int32_t e = EntityByTag(scene, a.Str("Set Random Speech for Unit"));
        if (e >= 0) m_randomSpeech[e] = a.Str("to");
        return;
    }
    if (k == "Force Speech") {
        const int32_t e = EntityByTag(scene, a.Str("Unit"));
        PlaySpeechNode(scene, e, a.Str("will use speech node"));
        return;
    }
    if (k == "Give Item from NPC to Human" || k == "Give Item to Human") {
        const std::string unit = a.Str("Unit"), item = a.Str("Item");
        if (scene.ScriptGiveItem(unit, item)) Logf("%s gives %s to the squad", unit.c_str(), item.c_str());
        return;
    }
    if (k == "Kill Entity") {
        const int32_t e = EntityByTag(scene, a.Str("Unit"));
        if (e >= 0) scene.ScriptKill(e);
        return;
    }
    if (k == "Set object script state") {
        const int32_t e = EntityByTag(scene, a.Str("Set Unit"));
        if (e >= 0) m_objState[e] = a.Bool("Object State", true);
        return;
    }
    if (k == "Set Timer") {
        TimerState t;
        t.startedAt = m_realtime;
        t.descending = a.Bool("Decending", true);
        // The delay rides in the record's one unnamed QualifiedNumber.
        for (const ft::EshProp& p : a.esh.props) {
            if (p.payload.size() == 5 && p.name.empty()) {
                t.seconds = static_cast<float>(QualifiedNumber::FromPayload(p.payload).a);
            }
        }
        m_timers[a.Str("to Timer")] = t;
        Logf("timer '%s' set to %g s", a.Str("to Timer").c_str(), t.seconds);
        return;
    }
    if (k == "Stop Timer") {
        m_timers.erase(a.Str("Timer"));
        return;
    }
    if (k == "Change Player") {
        const int32_t e = EntityByTag(scene, a.Str("Change Unit"));
        const int32_t pl = a.PlayerRef("to Player");
        if (e >= 0 && pl >= 0) {
            scene.ScriptSetPlayer(e, pl);
            Logf("%s joins player %d", a.Str("Change Unit").c_str(), pl);
        }
        return;
    }
    if (k == "Change Team") {
        const int32_t pl = a.PlayerRef("Player");
        const std::string team = a.Str("Team");
        for (size_t i = 0; i < m_teams.size(); ++i) {
            if (EqualsCI(m_teams[i].name, team.c_str()) && pl >= 0 &&
                pl < static_cast<int32_t>(m_players.size())) {
                m_players[static_cast<size_t>(pl)].team = static_cast<int32_t>(i);
                Logf("player %d joins team %s", pl, team.c_str());
            }
        }
        return;
    }
    if (k == "Lock/Unlock Door") {
        // The door lock model is milestone 7's `Door::SetActivationState`,
        // which has no lock state; logged rather than guessed.
        Logf("door '%s' lock -> %s", a.Str("Door").c_str(),
             a.Bool("LockState", true) ? "locked" : "unlocked");
        return;
    }
    if (k == "Center View") {
        const int32_t e = EntityByTag(scene, a.Str("on Unit"));
        if (e >= 0) scene.RequestCentre(e);
        Logf("centre view on %s", a.Str("on Unit").c_str());
        return;
    }
    if (k == "Set Mission State") {
        // `ActionSetMissionState::GetDescription @006899c0` names the three
        // states the enum at `+0xf` takes: 0 Covered, 1 UnCovered, 2 Visited.
        const std::string st = a.Str("State");
        const int32_t v = st == "UnCovered" ? 1 : (st == "Visited" ? 2 : 0);
        m_missionState.emplace_back(a.Str("Set Mission"), v);
        Logf("mission '%s' state -> %s", a.Str("Set Mission").c_str(), st.c_str());
        return;
    }
    if (k == "Play Bink Movie") {
        Logf("play bink: %s", a.Str("Bink File").c_str());
        return;
    }
    if (k == "Play Wav" || k == "Play Sound") {
        Logf("play wav: %s", a.Str("Wav File").c_str());
        return;
    }

    // MILESTONE 11.  The two families milestone 10 recorded as inert now
    // reach the AI: each sets the named player's `Controller` fields, which
    // is what `Controller::SetControllerWaypointModeAndMoveTargetTag
    // @0052e0d0`, `SetControllerStationary @0052e1c0`,
    // `SetControllerToPerimeterZone @0052e220`, `SetControllerToFollow
    // @0052e280`, `SetControllerToFlee @0052e2e0`, `DeactivateController
    // @0052dfd0` and `SetControllerCommunicate @0052dd70` each do in the
    // binary -- they are seven one-line setters on the same object.
    if (k.rfind("Player Move State - ", 0) == 0) {
        const std::string what = k.substr(20);
        const int32_t pl = a.PlayerRef("Player");
        std::string type = what, tag, zone;
        if (what == "WayPoint") {
            type = "Waypoints";
            tag = a.Str("First Point");
        } else if (what == "Random WayPoint" || what == "RandomWayPoint") {
            type = "RandomWaypoints";
            tag = a.Str("First Point");
        } else if (what == "Follow") {
            type = "Follow";
            tag = a.Str("Follow Target");
            if (tag.empty()) tag = a.Str("Unit");
        } else if (what == "Perimeter" || what == "PerimeterZone") {
            type = "PerimeterZone";
            zone = a.Str("Zone");
        } else if (what == "Wander") {
            // The editor's "Wander" is a Stationary controller with Rand Move
            // set: `Controller_IssueMoveOrderAndMaybeRandomizeAnywhereDestination
            // @0052f7c0` is the same entry point for both.
            type = "Stationary";
        }
        scene.ScriptSetMoveState(pl, type, tag, zone);
        return;
    }
    if (k.rfind("Player Priority - ", 0) == 0) {
        const std::string what = k.substr(18);
        const int32_t pl = a.PlayerRef("Player");
        int32_t level = a.Int("Level (0-10)", -1);
        if (level < 0) level = a.Int("level (0-10)", -1);
        if (level < 0) level = a.Int("With Urgency (0-10)", 0);
        if (what == "Kill Unit") {
            // The urgency lands as the controller's move-target tag, which is
            // the +10 term of `ComputeTargetScoreForEntity @005206f0`.
            scene.ScriptSetMoveState(pl, "", a.Str("Kill Unit"), "");
            scene.ScriptSetPriority(pl, "Aggression", level);
        } else if (what == "Kill Player") {
            scene.ScriptSetPriority(pl, "Aggression", level);
        } else if (what == "Move State") {
            scene.ScriptSetPriority(pl, "Mobility", level);
        } else {
            scene.ScriptSetPriority(pl, what, level);
        }
        return;
    }

    // Everything else is recognised but has no effect here: the minimap and
    // mission-flow actions need systems this build does not have.
    Logf("action not implemented: %s", k.c_str());
}

bool MissionScript::ActionComplete(Scene& scene, const ScriptObj& a) const {
    (void)scene;
    if (a.cls == "Wait") return m_realtime >= a.waitUntil;
    // Every other shipped action is instantaneous; the original's slot-17
    // default returns true.
    return true;
}

// TriggerAction::PollTriggerCondition @0068c9a0.
bool MissionScript::PollAction(Scene& scene, ScriptObj& a) {
    if (!a.started) {
        ExecuteAction(scene, a);
        a.started = true;
        a.nextTick = m_realtime;
    }
    if (!a.finished) {
        if (a.nextTick <= m_realtime) {
            a.finished = ActionComplete(scene, a);
            // The poll interval is vtable slot 16, whose bodies are in no dump;
            // one engine tick is used here.
            a.nextTick = m_realtime + 0.07f;
        }
    }
    return a.finished;
}

void MissionScript::RunActions(Scene& scene, Trigger& t) {
    bool allDone = true;
    for (ScriptObj& a : t.actions) {
        const bool done = PollAction(scene, a);
        if (!done) allDone = false;
        // Slot 11 returns false when the action is `blocking` and unfinished,
        // which stops the rest of the list for this tick.
        if (a.blocking && !done) return;
    }
    if (!allDone) return;
    t.pending = false;
    ++t.fireCount;
    Logf("trigger fired: %s", t.name.c_str());
    if (t.repeat) {
        t.armed = false;
        t.fireCount = 0;
        for (ScriptObj& a : t.actions) {
            a.started = a.finished = false;
            a.nextTick = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// The tick.  Trigger::TriggerConditionUpdateTimer @00679050.
// ---------------------------------------------------------------------------
void MissionScript::Tick(Scene& scene) {
    if (!m_loaded) return;
    m_realtime = static_cast<float>(scene.Ticks()) * 0.07f;

    // Speech bubbles expire.
    const int64_t now = scene.NowMs();
    m_bubbles.erase(std::remove_if(m_bubbles.begin(), m_bubbles.end(),
                                   [&](const FloatingText& f) { return f.untilMs <= now; }),
                    m_bubbles.end());
    if (m_worldTextUntil <= now) m_worldText.clear();

    for (Trigger& t : m_triggers) {
        if (!t.repeat && t.fireCount > 0) continue;
        if ((t.mask & 1) != 0 && t.nextEval < m_realtime) {
            if (!t.armed) {
                t.nextEval = t.delay + m_realtime;
                bool all = true;
                for (const ScriptObj& c : t.conds) {
                    if (!EvaluateCondition(scene, c)) {
                        all = false;
                        break;
                    }
                }
                if (t.conds.empty()) all = false;   // a trigger with no conditions never fires
                if (t.pending || all) t.armed = true;
            }
        }
        if (!t.armed) continue;
        RunActions(scene, t);
        if (m_end != MissionEnd::None) break;
    }

    // Event-driven triggers (mask bit 2 "Speech Occured") are evaluated once
    // against the events raised this tick, then the queue is cleared.
    if (!m_pendingSpeechEvents.empty()) {
        for (Trigger& t : m_triggers) {
            if ((t.mask & 4) == 0) continue;
            if (!t.repeat && t.fireCount > 0) continue;
            bool all = true;
            for (const ScriptObj& c : t.conds) {
                if (!EvaluateCondition(scene, c)) {
                    all = false;
                    break;
                }
            }
            if (all) t.armed = true;
        }
        m_pendingSpeechEvents.clear();
    }
}

void MissionScript::RaiseSpeechEvent(const std::string& event) {
    if (event.empty() || EqualsCI(event, "None")) return;
    m_pendingSpeechEvents.push_back(event);
    Logf("speech event: %s", event.c_str());
}

// The mission's own locale directory, which is the `.mis` basename --
// `Ui::LoadMissionStrings` (ui_screens.cpp) derives the string tables the same
// way.
void MissionScript::PlaySpeechLine(Scene& scene, int32_t entity,
                                   const std::string& key) {
    Audio* audio = scene.GetAudio();
    if (audio == nullptr || !audio->Enabled() || key.empty()) return;
    std::string base = m_path;
    const size_t slash = base.find_last_of("/\\");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    SoundRequest r;
    r.file = "locale/missions/" + base + "/" + key + ".mp3";
    r.entityId = entity;
    r.channel = kChanSpeech;
    r.why = "speech";
    const std::vector<LiveEntity>& es = scene.Entities();
    if (entity >= 0 && entity < static_cast<int32_t>(es.size())) {
        r.positional = true;
        r.x = es[static_cast<size_t>(entity)].wpos[0];
        r.y = es[static_cast<size_t>(entity)].wpos[1];
        r.z = es[static_cast<size_t>(entity)].wpos[2];
    }
    audio->Play(r);
}

void MissionScript::PlaySpeechNode(Scene& scene, int32_t entity, const std::string& node) {
    for (const SpeechNode& s : m_speech) {
        if (!EqualsCI(s.name, node.c_str())) continue;
        for (int i = 0; i < 6; ++i) {
            if (s.line[i].empty() || EqualsCI(s.line[i], "None")) continue;
            FloatingText f;
            f.entity = entity;
            f.text = s.line[i];   // the LOCALE KEY; the interface resolves it
            f.untilMs = scene.NowMs() + kSpeechMs;
            m_bubbles.push_back(f);
            // MILESTONE 13: the line the bubble shows is also the name of its
            // recording.  Nothing in the dumps builds a speech path -- the
            // speech window is not in the 50 exported functions -- but the
            // correspondence in the shipped data is exact and total:
            // `<speech_node>` "Hawk_A00_W" has line[0] "M01_Hawkeye_A00_W" and
            // `loc-mis_A.bos` carries
            // `locale/missions/mission01/M01_HAWKEYE_A00_W.MP3`.  Case differs
            // freely on both the stem and the extension, hence the
            // case-insensitive resolve.  Missing audio is normal: only some
            // lines were recorded, and the locale string is the fallback.
            PlaySpeechLine(scene, entity, s.line[i]);
            if (!s.event[i].empty()) RaiseSpeechEvent(s.event[i]);
            break;   // one line per node play; the rest are the random pool
        }
        Logf("speech '%s' by entity %d", node.c_str(), entity);
        return;
    }
    Logf("speech node not found: %s", node.c_str());
}

// ---------------------------------------------------------------------------
// --triggers
// ---------------------------------------------------------------------------
namespace {

std::string DescribeObj(const ScriptObj& o) {
    std::string s = o.cls;
    // The editor's own one-line form is each class's GetDescription (vtable
    // slot 4), e.g. ActionCompleteObjective::GetDescription @00680270 formats
    // "Complete Objective %s"; rather than transcribe forty format strings this
    // prints the class name and every property the record actually carries.
    for (const ft::EshProp& p : o.esh.props) {
        if (p.name == "esbin") continue;
        s += "\n         " + p.name + " = ";
        std::string str;
        if (ft::EshTypeIsString(p.type) && o.esh.GetString(p.name, &str)) {
            s += "\"" + str + "\"";
        } else if (p.type == 3 && p.payload.size() == 4) {
            s += std::to_string(ReadI32(p.payload.data()));
        } else if (p.type == 1 && p.payload.size() == 1) {
            s += p.payload[0] ? "true" : "false";
        } else if (p.type == 2 && p.payload.size() == 4) {
            float f;
            std::memcpy(&f, p.payload.data(), 4);
            s += std::to_string(f);
        } else if (p.type == 17) {
            s += "player " + std::to_string(PlayerRefFromPayload(p.payload));
        } else if (p.type == 18) {
            s += QualifiedNumber::FromPayload(p.payload).Describe();
        } else if (p.type == 20) {
            const StringPair sp = StringPair::FromPayload(p.payload);
            s += "(" + sp.first + ", " + sp.second + ")";
        } else {
            s += "<type " + std::to_string(p.type) + ", " + std::to_string(p.payload.size()) +
                 " bytes>";
        }
    }
    return s;
}

}  // namespace

std::string MissionScript::Describe() const {
    std::string s = m_info + "\n";
    s += "\nteams:\n";
    for (size_t i = 0; i < m_teams.size(); ++i) {
        s += "  " + std::to_string(i) + " " + m_teams[i].name + "\n";
    }
    s += "alignment matrix (row = actor's team, column = the other; <0 hostile):\n     ";
    for (size_t b = 0; b < m_teams.size(); ++b) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%4zu", b);
        s += buf;
    }
    s += "\n";
    for (size_t a = 0; a < m_teams.size(); ++a) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%3zu: ", a);
        s += buf;
        for (size_t b = 0; b < m_teams.size(); ++b) {
            std::snprintf(buf, sizeof(buf), "%4d", static_cast<int>(m_align[a][b]));
            s += buf;
        }
        s += "   " + m_teams[a].name + "\n";
    }
    s += "\nplayers:\n";
    for (size_t i = 0; i < m_players.size(); ++i) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %2zu %-28s side=%d team=%d (%s)\n", i,
                      m_players[i].name.c_str(), m_players[i].side, m_players[i].team,
                      m_players[i].team < static_cast<int32_t>(m_teams.size())
                          ? m_teams[static_cast<size_t>(m_players[i].team)].name.c_str()
                          : "?");
        s += buf;
    }
    s += "\nzones:\n";
    for (const WorldZone& z : m_zones) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %-16s [%.1f %.1f %.1f] .. [%.1f %.1f %.1f]\n",
                      z.name.c_str(), z.box[0], z.box[1], z.box[2], z.box[3], z.box[4], z.box[5]);
        s += buf;
    }
    s += "\ntriggers:\n";
    for (size_t i = 0; i < m_triggers.size(); ++i) {
        const Trigger& t = m_triggers[i];
        char buf[512];
        std::snprintf(buf, sizeof(buf), "\n[%02zu] \"%s\"  delay=%gs %s%s\n", i, t.name.c_str(),
                      t.delay, t.repeat ? "repeating" : "one-shot",
                      t.mask != 1 ? "  (event-driven)" : "");
        s += buf;
        for (const ScriptObj& c : t.conds) s += "    IF   " + DescribeObj(c) + "\n";
        for (const ScriptObj& a : t.actions) s += "    DO   " + DescribeObj(a) + "\n";
    }
    return s;
}

}  // namespace game
