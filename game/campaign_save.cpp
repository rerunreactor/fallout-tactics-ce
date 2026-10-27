// campaign_save.cpp -- the `.sav` container, and the campaign state it holds.
//
// The container is `Campaign::SaveCampaign @006a1600`
// (`Campaign::SaveCampaign@006a1600`) and its inverse
// `Campaign::ReadCampaignSaveHeader @006a0c00` (l.18405-18845):
//
//     "<saveh>" 2            SaveGameHeader::SaveGameHeaderProperties @004bfea0
//     "<campaign_save>" 1
//     u32 fileCount
//     fileCount x { i32len+path ; u32 size ; size bytes }
//
// Each embedded blob is a complete file image.  `SaveCampaign` gets them by
// listing the campaign scratch directory and keeping every name
// `Campaign::MatchesMissionName @006a0a10` accepts: the campaign file itself,
// the mission currently in progress, and one file per core mission that has
// been played (`Campaign::SaveMissionToCampaign @006a0020` writes
// `<missionBaseName>.<ext>` there through `World::Save`).  On load the stored
// DIRECTORY is discarded and only the name+extension are reused, re-rooted at
// the scratch directory (l.18607-18609), with a 20,000,001-byte per-file cap
// (l.18642).
//
// `<saveh>` 2 is, in order:
//     u8   isCampaignSave      (1 from Campaign::SaveCampaign l.18956,
//                               0 from World::Save l.18451)
//     str  worldName           World::Save l.18452 copies World+0x2c9
//     str  fileName            overwritten from the path by the browser
//     str  line2, str line3, str line4     the three text lines the item draws
//     <zar> imageA, <zar> imageB           (Image vtable slot 0x7c)
//     6 x <zar>                            the six squad portraits
//     24 bytes                             six floats, one per portrait
// A sixth string exists in the struct (+0x15) and is DRAWN but never written
// (l.2180-2243 writes five), so it is recomputed at display time.
//
// WHAT IS AND IS NOT INTERCHANGEABLE.  The container, the `<saveh>` block and
// the embedded `<campaign>` file are written here exactly as the binary writes
// them, including the `headerOnly` short form the game uses inside a save.
// The per-mission world blobs are NOT: `World::Save`'s section order is known
// (see NOTES.md) but the per-entity payload it emits goes through
// `ResolveEntityRefAndFlagDirty`, whose body is in no dump, so a byte-exact
// `<entity_file>` 3 record cannot be produced honestly.  This port therefore
// writes its mission state under its own `<f2ce_mission>` 1 tag inside the
// same container; `MatchesMissionName` would not accept that name, so a real
// game reading one of our saves gets the campaign and no mission state rather
// than a corrupt world.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "campaign.h"
#include "file.h"
#include "light.h"
#include "stream.h"

namespace game {
namespace {

using ft::Reader;

// MILESTONE 22.  Every string in a `.sav` is read through `Reader::TaggedString`,
// not `LengthPrefixedString`: bit 31 of the length prefix means the characters
// are 16 bit, and the shipped game writes EVERY string in a save that way.  Read
// with the plain rule the length is negative and a real save is refused at its
// first field.  The writer here keeps the narrow form, which the original's own
// reader accepts -- the flag is on the wire, not in the schema.

struct Writer {
    std::vector<uint8_t> b;
    void Bytes(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        b.insert(b.end(), q, q + n);
    }
    void U8(uint8_t v) { b.push_back(v); }
    void U32(uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void I64(int64_t v) {
        U32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
        U32(static_cast<uint32_t>((v >> 32) & 0xFFFFFFFFu));
    }
    void F32(float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        U32(u);
    }
    // MILESTONE 24: the WIDE form.  Every string the shipped game writes into a
    // `.sav` -- the `<saveh>` fields, the container's entry paths, the
    // `<campaign>` fields, every entity property name -- carries bit 31 in its
    // length prefix and is 16 bit on the wire.  This wrote the narrow form,
    // which the original's tagged codec does accept, but the player's Load Game
    // list still showed our row wrong and there is no reason to differ from the
    // file the game itself produces.
    void Str(const std::string& s) {
        I32(static_cast<int32_t>(s.size()) | static_cast<int32_t>(0x80000000u));
        for (char c : s) {
            b.push_back(static_cast<uint8_t>(c));
            b.push_back(0);
        }
    }
    void NarrowStr(const std::string& s) {
        I32(static_cast<int32_t>(s.size()));
        Bytes(s.data(), s.size());
    }
    void Token(const char* tag, int version) {
        Bytes(tag, std::strlen(tag) + 1);
        const std::string v = std::to_string(version);
        Bytes(v.data(), v.size() + 1);
    }
    void StrVec(const std::vector<std::string>& v) {
        I32(static_cast<int32_t>(v.size()));
        for (const std::string& s : v) Str(s);
    }
};

// A `<zar>` 4 with its own palette.  `ImageZAR::ReadFromStream @006f7d50` is
// the reader (common/zar.cpp); this is its inverse, emitting one opaque run
// (control byte `(count << 2) | 1`) per <=63-pixel span.  The palette is a
// 3-3-2 RGB cube, which is a choice this port makes -- the original's own
// quantiser is not in the dumps and none is needed to produce a legal file.
void WriteZar(Writer& w, int32_t width, int32_t height, const uint8_t* rgba) {
    w.Token("<zar>", 4);
    w.I32(width);
    w.I32(height);
    if (width <= 0 || height <= 0 || rgba == nullptr) {
        w.U8(0);
        w.U32(0);
        return;
    }
    w.U8(1);
    w.U32(256);
    for (int i = 0; i < 256; ++i) {
        const uint32_t r = static_cast<uint32_t>((i >> 5) & 7) * 255 / 7;
        const uint32_t g = static_cast<uint32_t>((i >> 2) & 7) * 255 / 7;
        const uint32_t bl = static_cast<uint32_t>(i & 3) * 255 / 3;
        w.U32(0xFF000000u | (r << 16) | (g << 8) | bl);
    }
    w.U8(0);   // m_constantIndex

    std::vector<uint8_t> data;
    for (int32_t y = 0; y < height; ++y) {
        int32_t x = 0;
        while (x < width) {
            const int32_t n = std::min<int32_t>(63, width - x);
            data.push_back(static_cast<uint8_t>((n << 2) | 1));
            for (int32_t i = 0; i < n; ++i) {
                const uint8_t* px = rgba + (static_cast<size_t>(y) * width + x + i) * 4;
                data.push_back(static_cast<uint8_t>(((px[0] >> 5) << 5) | ((px[1] >> 5) << 2) |
                                                    (px[2] >> 6)));
            }
            x += n;
        }
    }
    w.U32(static_cast<uint32_t>(data.size()));
    w.Bytes(data.data(), data.size());
}

bool SkipZarBlock(Reader& r) {
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<zar>") return false;
    r.I32();
    r.I32();
    if (r.U8() != 0) {
        const uint32_t n = r.U32();
        if (n > 256) return false;
        r.Take(static_cast<size_t>(n) * 4);
        if (v > 3) r.U8();
    }
    const uint32_t ds = r.U32();
    return r.Take(ds) != nullptr && !r.Failed();
}

// Nearest-neighbour resample of the captured RGBA screenshot.  The shipped
// header carries the picture at two sizes and this makes both from one grab.
std::vector<uint8_t> ResampleRgba(const uint8_t* src, int32_t sw, int32_t sh, int32_t dw,
                                  int32_t dh) {
    std::vector<uint8_t> out(static_cast<size_t>(dw) * static_cast<size_t>(dh) * 4, 0);
    if (src == nullptr || sw <= 0 || sh <= 0) return out;
    for (int32_t y = 0; y < dh; ++y) {
        const int32_t sy = y * sh / dh;
        for (int32_t x = 0; x < dw; ++x) {
            const int32_t sx = x * sw / dw;
            std::memcpy(&out[(static_cast<size_t>(y) * dw + x) * 4],
                        &src[(static_cast<size_t>(sy) * sw + sx) * 4], 4);
        }
    }
    return out;
}

// The `<saveh>` +0x11 string: "Jan 1 2197.  07:45" in the shipped save.  The
// date is the world clock through the one port of `ConvertMsecToDaysHMS
// @006ad6a0` + `NormalizeTimeForYearsAndMonths @006ada90` (mapview/light.cpp),
// which is what the `printDate` console command prints and what drives the
// lighting; world time 0 is 06:29 on Jan 1 of 2197.  The separator is a full
// stop and TWO spaces, measured off the file.
std::string FormatSaveTimestamp(int64_t ms) {
    static const char* const kMon[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const ft::WorldTime t = ft::WorldTime::FromMillis(ms);
    const int mi = (t.month >= 1 && t.month <= 12) ? t.month - 1 : 0;
    char out[64];
    std::snprintf(out, sizeof(out), "%s %lld %d.  %02d:%02d", kMon[mi],
                  static_cast<long long>(t.days), t.year, t.hour, t.minute);
    return out;
}

// A path this process opened, mapped back into the game's own namespace: the
// assets root is a command-line argument here and is nowhere in a shipped save.
std::string GameRelativePath(const std::string& p) {
    std::string out = p;
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    static const char* const kRoots[] = {"campaigns/", "missions/", "entities/", "locale/",
                                         "tables/",    "gui/",      "core/"};
    for (const char* root : kRoots) {
        const size_t at = out.find(std::string("/") + root);
        if (at != std::string::npos) return out.substr(at + 1);
        if (out.rfind(root, 0) == 0) return out;
    }
    const size_t sl = out.find_last_of('/');
    return sl == std::string::npos ? out : out.substr(sl + 1);
}

// `SaveGameHeader::SaveGameHeaderProperties @004bfea0`, written.
//
// MILESTONE 24.  The five strings are settled by the shipped
// `New Save Game.sav` and by what the game's Load Game list draws beside it --
// "Mick / Brahmin Wood / Jan 1 2197.  07:45 / 277K":
//
//     +0x01  empty in every shipped save
//     +0x05  the save file's own stem; NOTES 59 already had the browser
//            overwriting this from the path, and the shipped file agrees
//            ("New Save Game")
//     +0x09  the PLAYER CHARACTER's name    -- "Mick"
//     +0x0d  the mission's DISPLAY name     -- "Brahmin Wood"
//     +0x11  the in-game date and time      -- "Jan 1 2197.  07:45"
//
// The size at the end of the row is computed at display time, which is the
// sixth `FOTString` in the struct that §59 noted is drawn and never written.
// This port wrote the campaign's absolute filesystem path into +0x09 and the
// `.mis` path into +0x0d, which is exactly what the player saw in the list.
//
// The two pictures are 280x165 and 128x76 in the shipped file, and the six
// portraits are 25x33 -- the size of `gui/char/<Display Name>.zar`.
void WriteSaveHeader(Writer& w, const SaveGameHeader& h, int32_t thumbW, int32_t thumbH,
                     const uint8_t* thumb,
                     const std::vector<CampaignState::SavePortrait>& portraits) {
    w.Token("<saveh>", 2);
    w.U8(1);                       // isCampaignSave
    w.Str("");                     // +0x01
    w.Str(h.name);                 // +0x05, the save's own stem
    w.Str(h.playerName);           // +0x09
    w.Str(h.missionName);          // +0x0d
    w.Str(h.timestamp);            // +0x11

    const std::vector<uint8_t> big = ResampleRgba(thumb, thumbW, thumbH, 280, 165);
    const std::vector<uint8_t> small = ResampleRgba(thumb, thumbW, thumbH, 128, 76);
    if (thumb != nullptr && thumbW > 0 && thumbH > 0) {
        WriteZar(w, 280, 165, big.data());    // +0x19
        WriteZar(w, 128, 76, small.data());   // +0x1d
    } else {
        WriteZar(w, 0, 0, nullptr);
        WriteZar(w, 0, 0, nullptr);
    }
    for (int i = 0; i < 6; ++i) {           // +0x21, the six portraits
        if (static_cast<size_t>(i) < portraits.size() &&
            portraits[static_cast<size_t>(i)].w > 0) {
            const CampaignState::SavePortrait& p = portraits[static_cast<size_t>(i)];
            WriteZar(w, p.w, p.h, p.rgba.data());
        } else {
            WriteZar(w, 0, 0, nullptr);
        }
    }
    for (int i = 0; i < 6; ++i) {           // +0x39, 24 bytes: six floats
        w.F32(static_cast<size_t>(i) < portraits.size() ? portraits[static_cast<size_t>(i)].hp
                                                        : 0.0f);
    }
}

bool ReadSaveHeader(Reader& r, SaveGameHeader* h) {
    std::string tag;
    int32_t v = 0;
    const size_t at = r.Pos();
    r.FormatToken(&tag, &v);
    if (tag != "<saveh>") {
        r.Seek(at);
        return false;
    }
    h->version = v;
    r.U8();
    r.TaggedString();                    // +0x01
    h->name = r.TaggedString();          // +0x05
    h->playerName = r.TaggedString();    // +0x09
    h->missionName = r.TaggedString();   // +0x0d
    h->timestamp = r.TaggedString();     // +0x11
    for (int i = 0; i < 8; ++i) {
        if (!SkipZarBlock(r)) return false;
    }
    r.Take(24);
    return !r.Failed();
}

// `Campaign::SaveCampaignProperties @0069f330` in its `headerOnly` form -- the
// `cVar3 != 0` branch at l.17470, which writes the campaign's own path in
// place of the tile array and the world-map bitmap.  Version 21 is what the
// shipped executable writes (`local_18 = 0x15`, l.17424); the shipped
// `bos.cam` is version 19 and ends exactly where the three version-21 tail
// fields begin (an int64 and two string vectors), which is how that delta was
// established.
void WriteCampaignSection(Writer& w, const CampaignState& s) {
    const CampaignFile& f = s.file;
    w.Token("<campaign>", 21);
    w.U8(1);                                  // headerOnly
    // MILESTONE 24.  Both of these are paths INSIDE THE GAME'S OWN NAMESPACE,
    // and writing this process's filesystem paths into them is what broke the
    // player's test.  The shipped save holds `campaigns/bos.cam` here, not
    // `/path/to/core/campaigns/bos.cam`.
    w.Str(f.info.empty() ? std::string() : GameRelativePath(s.campaignPath));
    w.I32(f.gridW);
    w.I32(f.gridH);
    for (int32_t i = 0; i < f.gridW * f.gridH; ++i) w.I32(0);
    // +0x10, THE CURRENT MISSION -- and this is the field the real game failed
    // on.  `Campaign::ReadCampaignSaveHeader @006a0c00` extracts every
    // `<campaign_save>` entry into the campaign scratch directory and then
    // opens the current mission BY THIS NAME.  The shipped save holds
    // `user/$$current$$/mission01.sav`: the scratch path of the world blob it
    // just wrote out, `.sav` and all.  This port wrote the SOURCE MAP's path,
    // `campaigns/missions/core/mission01.mis`, so the game looked for
    // `user/$$current$$/mission01.mis`, which is not one of the entries, and
    // reported "Failed to open mission file: user/$$current$$/mission01.mis"
    // -- the player's exact error, field for field.
    w.Str(s.missionBlobName.empty() ? std::string() : s.missionBlobName);
    w.I32(0);                                 // +0x1c
    w.I64(s.timeMs);                          // +0x48
    w.I32(0);                                 // +0x65, the StringFile list count

    w.Token("varTableHeader", 1);
    {
        std::vector<std::string> names, values;
        for (const auto& kv : s.vars) {
            names.push_back(kv.first);
            values.push_back(kv.second);
        }
        // MILESTONE 25: NAMES FIRST.  `VarTable::SaveVarTableProperties
        // @00500b50` writes two parallel string vectors and nothing in the
        // dumps says which is which; the shipped save settles it -- its first
        // vector is `SPRINGFIELD_EXISTS, CVAR_M01_COMPLETE, ...` and its second
        // is `FALSE, FALSE, ...`.  This wrote them the other way round, so the
        // original read our values as names and our names as values.
        w.StrVec(names);
        w.StrVec(values);
    }

    w.F32(static_cast<float>(s.viewX));
    w.F32(static_cast<float>(s.viewY));
    w.F32(static_cast<float>(s.posX));
    w.F32(static_cast<float>(s.posY));

    // The core and special mission records, as `<esh>` documents.
    w.I32(static_cast<int32_t>(s.cores.size()));
    for (const CoreMission& c : s.cores) {
        Writer e;
        int32_t n = 0;
        auto prop = [&](const char* name, uint32_t type, const std::vector<uint8_t>& pay) {
            e.Str(name);
            e.U32(type);
            e.U32(static_cast<uint32_t>(pay.size()));
            e.Bytes(pay.data(), pay.size());
            ++n;
        };
        auto strPay = [](const std::string& v) {
            Writer t;
            t.Str(v);
            return t.b;
        };
        auto intPay = [](int32_t v) {
            Writer t;
            t.I32(v);
            return t.b;
        };
        auto boolPay = [](bool v) { return std::vector<uint8_t>{static_cast<uint8_t>(v ? 1 : 0)}; };
        prop("Name", 4, strPay(c.nameKey));
        prop("Loc x", 3, intPay(c.locX));
        prop("Loc y", 3, intPay(c.locY));
        prop("Radius", 3, intPay(c.radius));
        prop("MissionFile", 25, strPay(c.misPath));
        prop("State", 9,
             strPay(c.state == MissionState::Visited
                        ? "Visited"
                        : (c.state == MissionState::UnCovered ? "UnCovered" : "Covered")));
        prop("Is Bunker", 1, boolPay(c.isBunker));
        Writer doc;
        doc.Token("<esh>", 1);
        doc.I32(n);
        doc.Bytes(e.b.data(), e.b.size());
        w.Bytes(doc.b.data(), doc.b.size());
    }
    w.I32(static_cast<int32_t>(s.file.specials.size()));
    for (const SpecialMission& sp : s.file.specials) {
        Writer e;
        int32_t n = 0;
        auto prop = [&](const char* name, uint32_t type, const std::vector<uint8_t>& pay) {
            e.Str(name);
            e.U32(type);
            e.U32(static_cast<uint32_t>(pay.size()));
            e.Bytes(pay.data(), pay.size());
            ++n;
        };
        Writer t1;
        t1.Str(sp.nameKey);
        prop("Name", 4, t1.b);
        Writer t2;
        t2.I32(sp.radius);
        prop("Radius", 3, t2.b);
        Writer t3;
        t3.Str(sp.misPath);
        prop("MissionFile", 25, t3.b);
        prop("Should Morph", 1, {static_cast<uint8_t>(sp.morph ? 1 : 0)});
        Writer doc;
        doc.Token("<esh>", 1);
        doc.I32(n);
        doc.Bytes(e.b.data(), e.b.size());
        w.Bytes(doc.b.data(), doc.b.size());
    }

    // `<random_force>` 2 records.
    w.I32(static_cast<int32_t>(f.forces.size()));
    for (const ForceDef& fd : f.forces) {
        w.Token("<random_force>", 2);
        w.Str(fd.name);
        w.Str(fd.align);
        w.I32(fd.minLevel);
        w.I32(fd.maxLevel);
        w.U8(fd.enabled ? 1 : 0);
        w.I32(static_cast<int32_t>(fd.members.size()));
        for (const ForceMember& m : fd.members) w.Str(m.entPath);
        w.I32(static_cast<int32_t>(fd.members.size()));
        for (const ForceMember& m : fd.members) w.I32(m.level);
        w.I32(static_cast<int32_t>(fd.members.size()));
        for (const ForceMember& m : fd.members) w.I32(m.chance);
        w.Str(fd.cdCode);
    }

    w.StrVec(f.landscapes);
    w.StrVec(f.prefabs);
    w.I32(static_cast<int32_t>(f.quarterMaster.size()));
    for (const StockEntry& e : f.quarterMaster) {
        w.Str(e.tag);
        w.Str(e.entPath);
        w.I32(e.count);
    }
    w.I32(0);   // +0x151
    w.I32(0);   // +0x161
    w.I32(static_cast<int32_t>(f.recruitPool.size()));
    for (const RecruitEntry& e : f.recruitPool) {
        w.Str(e.tag);
        w.Str(e.entPath);
        w.Str(e.op);
    }
    w.I32(0);   // +0x185, the StringFile list
    w.I32(0);   // +0x195, the string/int tree
    w.I32(0);
    w.I32(0);
    w.I32(0);
    w.I32(0);                 // +0x101, +0x111, +0x121, +0x131
    w.U8(0);
    w.U8(0);
    w.U8(0);                  // +0x20, +0x21, +0x22
    w.I32(0);                 // +0x58
    w.Str("");                // +0x69
    for (int i = 0; i < 6; ++i) {
        // Six {string, int} slots.  Every one in the shipped save is an empty
        // string and 0xCDCDCDCD -- MSVC's uninitialised-heap fill written
        // straight to disk.  They are not a squad table; see NOTES 147.
        w.Str("");
        w.I32(0);
    }
    w.F32(1.0f);              // +0x23
    w.U8(0);                  // +0x5c
    // +0xd1 is a list of strings, 16 bytes on from the movie lists at +0xb1
    // and +0xc1, which hold four entries in the shipped save. This one holds
    // one there, the current mission's name key, so the count is 1. Writing 0
    // with the key after it misplaced every later field for a reader that
    // follows the layout, as the save editor on rerunreactor.com does.
    w.I32(1);

    // MILESTONE 25.  EVERYTHING FROM HERE WAS WRONG, and it is why the shipped
    // game answered "Unable to load campaign": this port wrote 148 bytes of
    // tail where the original writes 598, so `ParseCampaignFileToken` -- the
    // reader whose returning 0 is the ONLY thing that raises
    // `campaign_failedload` on the load path (`Campaign::
    // OpenAndParseCampaignFile @0069d3d0`) -- ran
    // off the end of the block.  The layout below is read straight off the
    // shipped save's own 598 tail bytes, which it now accounts for in full.
    w.Str(s.missionNameKey);  // "mission_name_01" in the shipped save
    w.I32(10);                // +0x18
    w.I32(0);                 // +0xe1
    w.I32(0);                 // +0xf1
    w.I32(1);                 // +0x14
    w.I32(70);                // +0x209; 0x46 in the shipped save
    for (int i = 0; i < 7; ++i) w.I32(0);
    // Two parallel string vectors: the movie KEYS and the movie FILES.  The
    // shipped save carries the four tutorial-2 movies; they are runtime state
    // (they are in neither `bos.cam` nor any `tables/` file), so this port has
    // nothing to put in them and writes the empty pair -- a queue with no
    // movies in it, not a malformed one.
    w.StrVec(std::vector<std::string>());
    w.StrVec(std::vector<std::string>());
}

// The roster and the rest of the runtime state the `<campaign>` block has no
// field for.  Written under this port's own tag; see the header comment.
void WriteRoster(Writer& w, const CampaignState& s) {
    w.Token("<f2ce_roster>", 1);
    w.I32(static_cast<int32_t>(s.roster.size()));
    for (const RosterMember& m : s.roster) {
        w.Str(m.entPath);
        w.Str(m.displayName);
        w.Str(m.rank);
        w.Str(m.tag);
        w.I32(m.hp);
        w.I32(m.maxHp);
        w.I32(m.xp);
        w.I32(m.level);
        w.U8(m.dead ? 1 : 0);
        w.U8(m.inSquad ? 1 : 0);
        w.Str(m.hand1);
        w.Str(m.hand2);
        w.Str(m.armour);
        w.I32(m.ammo);
        w.I32(static_cast<int32_t>(m.inventory.size()));
        for (const RosterItem& it : m.inventory) {
            w.Str(it.cls);
            w.Str(it.displayName);
            w.Str(it.itemType);
            w.Str(it.entPath);
            w.Str(it.spritePath);
            w.I32(it.count);
        }
    }
    w.StrVec(s.pool);
    w.I32(static_cast<int32_t>(s.stock.size()));
    for (const auto& kv : s.stock) {
        w.Str(kv.first);
        w.I32(kv.second);
    }
    w.I32(s.reputation);
    w.U8(s.encountersEnabled ? 1 : 0);
    w.I32(s.destCore);
    w.U8(s.travelling ? 1 : 0);
    w.F32(static_cast<float>(s.destX));
    w.F32(static_cast<float>(s.destY));
    w.U32(s.rng);
}

bool ReadRoster(Reader& r, CampaignState* s) {
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<f2ce_roster>") return false;
    s->roster.clear();
    const int32_t n = r.I32();
    for (int32_t i = 0; i < n && !r.Failed(); ++i) {
        RosterMember m;
        m.entPath = r.TaggedString();
        m.displayName = r.TaggedString();
        m.rank = r.TaggedString();
        m.tag = r.TaggedString();
        m.hp = r.I32();
        m.maxHp = r.I32();
        m.xp = r.I32();
        m.level = r.I32();
        m.dead = r.U8() != 0;
        m.inSquad = r.U8() != 0;
        m.hand1 = r.TaggedString();
        m.hand2 = r.TaggedString();
        m.armour = r.TaggedString();
        m.ammo = r.I32();
        const int32_t ni = r.I32();
        for (int32_t k = 0; k < ni && !r.Failed(); ++k) {
            RosterItem it;
            it.cls = r.TaggedString();
            it.displayName = r.TaggedString();
            it.itemType = r.TaggedString();
            it.entPath = r.TaggedString();
            it.spritePath = r.TaggedString();
            it.count = r.I32();
            m.inventory.push_back(it);
        }
        s->roster.push_back(m);
    }
    s->pool.clear();
    const int32_t np = r.I32();
    for (int32_t i = 0; i < np; ++i) s->pool.push_back(r.TaggedString());
    s->stock.clear();
    const int32_t ns = r.I32();
    for (int32_t i = 0; i < ns; ++i) {
        const std::string k = r.TaggedString();
        s->stock[k] = r.I32();
    }
    s->reputation = r.I32();
    s->encountersEnabled = r.U8() != 0;
    s->destCore = r.I32();
    s->travelling = r.U8() != 0;
    auto f32 = [&]() {
        const uint32_t u = r.U32();
        float f;
        std::memcpy(&f, &u, 4);
        return static_cast<double>(f);
    };
    s->destX = f32();
    s->destY = f32();
    s->rng = r.U32();
    return !r.Failed();
}

// Reading back the `headerOnly` `<campaign>` block a save carries.
bool ReadCampaignSection(Reader& r, CampaignState* s) {
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<campaign>") return false;
    const uint8_t headerOnly = r.U8();
    if (headerOnly == 0) return false;    // a full campaign is not what a save holds
    s->campaignPath = r.TaggedString();
    const int32_t w = r.I32(), h = r.I32();
    r.Take(static_cast<size_t>(std::max(0, w)) * std::max(0, h) * 4);
    s->currentMission = r.TaggedString();
    r.I32();
    const uint32_t lo = r.U32(), hi = r.U32();
    s->timeMs = static_cast<int64_t>(lo) | (static_cast<int64_t>(hi) << 32);
    r.I32();
    std::string vt;
    int32_t vv = 0;
    r.FormatToken(&vt, &vv);
    if (vt != "varTableHeader") return false;
    std::vector<std::string> names, values;
    int32_t n = r.I32();
    for (int32_t i = 0; i < n; ++i) names.push_back(r.TaggedString());
    n = r.I32();
    for (int32_t i = 0; i < n; ++i) values.push_back(r.TaggedString());
    s->vars.clear();
    for (size_t i = 0; i < names.size() && i < values.size(); ++i) s->vars[names[i]] = values[i];

    auto f32 = [&]() {
        const uint32_t u = r.U32();
        float f;
        std::memcpy(&f, &u, 4);
        return static_cast<double>(f);
    };
    s->viewX = f32();
    s->viewY = f32();
    s->posX = f32();
    s->posY = f32();

    ft::Esh esh;
    std::string e;
    s->cores.clear();
    const int32_t nc = r.I32();
    for (int32_t i = 0; i < nc; ++i) {
        if (!esh.Read(r, &e)) return false;
        CoreMission c;
        esh.GetString("Name", &c.nameKey);
        esh.GetString("MissionFile", &c.misPath);
        esh.GetInt("Loc x", &c.locX);
        esh.GetInt("Loc y", &c.locY);
        esh.GetInt("Radius", &c.radius);
        std::string st;
        if (esh.GetString("State", &st)) {
            c.state = st == "Visited" ? MissionState::Visited
                                      : (st == "UnCovered" ? MissionState::UnCovered
                                                           : MissionState::Covered);
        }
        esh.GetBool("Is Bunker", &c.isBunker);
        s->cores.push_back(c);
    }
    // The remaining sections of the block are the campaign definition, which
    // the loader takes from the `.cam` it has already opened; they are skipped
    // rather than re-read.
    return !r.Failed();
}

}  // namespace

// ===========================================================================
bool CampaignState::Save(const std::string& path, const std::string& label,
                         std::string* errorOut) const {
    Writer w;
    SaveGameHeader h;
    // +0x05 is the save file's own stem, which is what the shipped file holds
    // ("New Save Game" for `New Save Game.sav`).
    std::string stem = path;
    const size_t sl = stem.find_last_of("/\\");
    if (sl != std::string::npos) stem = stem.substr(sl + 1);
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    h.name = stem.empty() ? label : stem;
    h.playerName = playerName;
    h.missionName = missionName;
    h.timestamp = FormatSaveTimestamp(timeMs);
    h.timeMs = timeMs;
    WriteSaveHeader(w, h, thumbW, thumbH, thumb.empty() ? nullptr : thumb.data(), portraits);

    // `<campaign_save>` 1: the embedded file list.  MILESTONE 24: EXACTLY the
    // two entries the shipped container has, under the shipped names.
    // `Campaign::SaveCampaign @006a1600` lists the campaign scratch directory
    // and stores each file under its path there; `Campaign::
    // ReadCampaignSaveHeader @006a0c00` extracts each one back into that
    // directory.  A third entry of this port's own was one more thing for the
    // original to trip over and bought nothing -- see below for where the
    // roster went instead.
    Writer camp;
    WriteCampaignSection(camp, *this);

    struct Blob {
        std::string name;
        const std::vector<uint8_t>* data;
    };
    std::vector<Blob> blobs = {{"user/$$current$$/save.cam", &camp.b}};
    if (!missionBlob.empty()) {
        blobs.push_back({missionBlobName.empty() ? std::string("user/$$current$$/mission.sav")
                                                 : missionBlobName,
                         &missionBlob});
    }

    w.Token("<campaign_save>", 1);
    w.U32(static_cast<uint32_t>(blobs.size()));
    for (const Blob& b : blobs) {
        w.Str(b.name);
        w.U32(static_cast<uint32_t>(b.data->size()));
        w.Bytes(b.data->data(), b.data->size());
    }

    // The roster, AFTER the entry list.  `ReadCampaignSaveHeader` reads
    // `fileCount` entries and stops, so bytes past the last one are never
    // looked at by the original -- which is the only place this port's own
    // state can sit without either becoming a container entry the original
    // will extract or being fed to a reader that is still expecting campaign
    // fields.  (The shipped `<campaign>` 21 runs about 450 bytes longer than
    // this port reconstructs, so appending anything INSIDE that blob would
    // hand the original our bytes where it expects its own.)
    Writer roster;
    WriteRoster(roster, *this);
    w.Bytes(roster.b.data(), roster.b.size());

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        *errorOut = "cannot write " + path;
        return false;
    }
    const bool ok = std::fwrite(w.b.data(), 1, w.b.size(), f) == w.b.size();
    std::fclose(f);
    if (!ok) *errorOut = "short write";
    return ok;
}

bool CampaignState::LoadSave(const std::string& path, std::string* errorOut) {
    std::vector<uint8_t> raw;
    if (!ft::ReadWholeFile(path, &raw, errorOut)) return false;
    Reader r(raw);
    SaveGameHeader h;
    ReadSaveHeader(r, &h);   // optional, exactly as ReadCampaignSaveHeader treats it
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<campaign_save>") {
        *errorOut = "not a save: tag='" + tag + "'";
        return false;
    }
    const uint32_t n = r.U32();
    missionBlob.clear();
    bool gotCampaign = false, gotRoster = false;
    for (uint32_t i = 0; i < n && !r.Failed(); ++i) {
        const std::string name = r.TaggedString();
        const uint32_t size = r.U32();
        if (size > 20000000u) {          // Campaign::ReadCampaignSaveHeader l.18642
            r.Take(size);
            continue;
        }
        const uint8_t* p = r.Take(size);
        if (p == nullptr) break;
        Reader sub(p, size);
        // The stored DIRECTORY is discarded and the entry is recognised by
        // its extension, exactly as `Campaign::ReadCampaignSaveHeader
        // @006a0c00` re-roots each name in the scratch directory
        // (`Campaign::ReadCampaignSaveHeader@006a0c00`).  That is what lets a save written
        // by the shipped game be read here: its campaign is
        // `user/$$current$$/save.cam` and its mission is
        // `user/$$current$$/<base>.sav`.
        std::string base = name;
        const size_t sl = base.find_last_of("/\\");
        if (sl != std::string::npos) base = base.substr(sl + 1);
        std::string ext;
        const size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) ext = base.substr(dot);
        if (ext == ".cam") {
            if (!ReadCampaignSection(sub, this)) {
                *errorOut = "bad campaign section in save";
                return false;
            }
            gotCampaign = true;
        } else if (base == "roster.f2ce") {
            // Saves written before milestone 24 carried it as an entry.
            if (!ReadRoster(sub, this)) {
                *errorOut = "bad roster section in save";
                return false;
            }
            gotRoster = true;
        } else if (ext == ".sav") {
            missionBlob.assign(p, p + size);
            missionBlobName = name;
        }
    }
    // MILESTONE 24: the roster now sits AFTER the entry list, where the
    // original never reads (see `Save`).
    if (!gotRoster && !r.Failed() && r.Pos() < r.Size()) {
        Reader tail(raw.data() + r.Pos(), r.Size() - r.Pos());
        if (ReadRoster(tail, this)) gotRoster = true;
    }
    if (!gotCampaign) {
        *errorOut = "save is missing its campaign section";
        return false;
    }
    if (!gotRoster) {
        // A save written by the shipped game has no `roster.f2ce`: the squad
        // lives inside the mission world, where this port now reads it from.
        // Nothing else in the file is this port's own, so the save still loads.
        roster.clear();
        std::fprintf(stderr, "load: no roster section -- a save from the shipped game\n");
    }
    saveLabel = h.name;
    playerName = h.playerName;
    missionName = h.missionName;
    return true;
}

bool CampaignState::ReadHeaderOnly(const std::string& path, SaveGameHeader* out,
                                   std::string* errorOut) {
    std::vector<uint8_t> raw;
    if (!ft::ReadWholeFile(path, &raw, errorOut)) return false;
    Reader r(raw);
    if (!ReadSaveHeader(r, out)) {
        *errorOut = "no <saveh> block";
        return false;
    }
    return true;
}

}  // namespace game
