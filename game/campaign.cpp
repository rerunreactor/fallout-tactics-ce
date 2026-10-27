// campaign.cpp -- see campaign.h for the provenance of every field.

#include "campaign.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "file.h"
#include "stream.h"
#include "zar.h"

namespace game {
namespace {

using ft::Reader;

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// ---------------------------------------------------------------------------
// The 8-bit Windows BMP the campaign carries as its world map picture.  This
// is `WriteBitmapToDeviceTimed`'s payload; the original hands it to the OS,
// so nothing about the decode is in the dumps -- it is a plain BI_RGB DIB and
// is read as one here.
// ---------------------------------------------------------------------------
bool DecodeBmpGrey(const uint8_t* p, size_t n, WorldTerrain* out) {
    if (n < 54 || p[0] != 'B' || p[1] != 'M') return false;
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8) |
               (static_cast<uint32_t>(p[o + 2]) << 16) | (static_cast<uint32_t>(p[o + 3]) << 24);
    };
    auto u16 = [&](size_t o) {
        return static_cast<uint32_t>(p[o]) | (static_cast<uint32_t>(p[o + 1]) << 8);
    };
    const uint32_t dataOff = u32(10);
    const uint32_t hdrSize = u32(14);
    const int32_t w = static_cast<int32_t>(u32(18));
    const int32_t h = static_cast<int32_t>(u32(22));
    const uint32_t bpp = u16(28);
    const uint32_t comp = u32(30);
    if (w <= 0 || h == 0 || comp != 0 || (bpp != 8 && bpp != 24 && bpp != 32)) return false;
    const bool bottomUp = h > 0;
    const int32_t hh = bottomUp ? h : -h;
    const size_t stride = ((static_cast<size_t>(w) * bpp / 8) + 3) & ~size_t(3);
    if (dataOff + stride * static_cast<size_t>(hh) > n) return false;

    uint32_t pal[256];
    std::memset(pal, 0, sizeof(pal));
    if (bpp == 8) {
        const size_t po = 14 + hdrSize;
        uint32_t entries = u32(46);
        if (entries == 0 || entries > 256) entries = 256;
        for (uint32_t i = 0; i < entries && po + i * 4 + 3 < n; ++i) {
            pal[i] = 0xFF000000u | (static_cast<uint32_t>(p[po + i * 4 + 2]) << 16) |
                     (static_cast<uint32_t>(p[po + i * 4 + 1]) << 8) |
                     static_cast<uint32_t>(p[po + i * 4 + 0]);
        }
    }

    out->w = w;
    out->h = hh;
    out->grey.assign(static_cast<size_t>(w) * hh, 0);
    for (int32_t y = 0; y < hh; ++y) {
        const size_t src = dataOff + stride * static_cast<size_t>(bottomUp ? (hh - 1 - y) : y);
        uint8_t* dst = out->grey.data() + static_cast<size_t>(y) * w;
        for (int32_t x = 0; x < w; ++x) {
            uint32_t c;
            if (bpp == 8) {
                c = pal[p[src + x]];
            } else {
                const size_t o = src + static_cast<size_t>(x) * (bpp / 8);
                c = 0xFF000000u | (static_cast<uint32_t>(p[o + 2]) << 16) |
                    (static_cast<uint32_t>(p[o + 1]) << 8) | static_cast<uint32_t>(p[o]);
            }
            // The shipped bitmap is greyscale; the green channel is the value
            // `WWorldWindow::Update` divides by 255.
            dst[x] = static_cast<uint8_t>((c >> 8) & 0xFF);
        }
    }
    out->ok = true;
    return true;
}

// `<zar>` skip: the same fields common/zar.cpp reads, without decoding.
bool SkipZar(Reader& r, int32_t* wOut, int32_t* hOut) {
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<zar>" || v < 2 || v > 4) return false;
    const int32_t w = r.I32(), h = r.I32();
    if (wOut) *wOut = w;
    if (hOut) *hOut = h;
    if (r.U8() != 0) {
        const uint32_t n = r.U32();
        if (n > 256) return false;
        r.Take(static_cast<size_t>(n) * 4);
        if (v > 3) r.U8();
    }
    const uint32_t ds = r.U32();
    return r.Take(ds) != nullptr && !r.Failed();
}

}  // namespace

// ===========================================================================
// CampaignFile
// ===========================================================================
bool CampaignFile::Load(const std::string& path, std::string* errorOut) {
    std::vector<uint8_t> rawKeep;
    if (!ft::ReadWholeFile(path, &rawKeep, errorOut)) return false;
    const std::vector<uint8_t>& raw = rawKeep;
    Reader r(raw);

    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    if (tag != "<campaign>") {
        *errorOut = "not a campaign file: tag='" + tag + "'";
        return false;
    }
    fileVersion = v;

    // Campaign::SaveCampaignProperties, field for field.
    const uint8_t headerOnly = r.U8();
    if (headerOnly == 0) {
        gridW = r.I32();
        gridH = r.I32();
        tilePx = r.I32();
        if (gridW <= 0 || gridH <= 0 || gridW > 4096 || gridH > 4096) {
            *errorOut = "implausible campaign grid";
            return false;
        }
        tiles.resize(static_cast<size_t>(gridW) * gridH);
        for (int32_t x = 0; x < gridW; ++x) {
            for (int32_t y = 0; y < gridH; ++y) {
                CampaignTile& t = tiles[static_cast<size_t>(x) * gridH + y];
                std::string tt;
                int32_t tv = 0;
                r.FormatToken(&tt, &tv);
                if (tt != "<campaign_tile>") {
                    *errorOut = "expected <campaign_tile> at " + std::to_string(x) + "," +
                                std::to_string(y) + " got '" + tt + "'";
                    return false;
                }
                auto ints = [&](std::vector<int32_t>* out) {
                    const int32_t n = r.I32();
                    if (n < 0 || n > 100000) {
                        r.Fail("bad tile array");
                        return;
                    }
                    out->resize(static_cast<size_t>(n));
                    for (int32_t i = 0; i < n; ++i) (*out)[i] = r.I32();
                };
                ints(&t.forceChance);
                ints(&t.specialChance);
                ints(&t.landscapeChance);
                if (r.U8() != 0) {
                    t.hasArt = true;
                    t.artOffset = r.Pos();
                    if (!SkipZar(r, &t.artW, &t.artH)) {
                        *errorOut = "bad <zar> in campaign tile";
                        return false;
                    }
                    t.artSize = r.Pos() - t.artOffset;
                }
                if (r.Failed()) {
                    *errorOut = "truncated campaign tiles";
                    return false;
                }
            }
        }
        if (r.U8() != 0) {
            // WriteBitmapToDeviceTimed: a bare Windows BMP, its own header
            // giving its length.
            const size_t at = r.Pos();
            if (at + 6 > raw.size()) {
                *errorOut = "truncated world map bitmap";
                return false;
            }
            const uint32_t bmpSize = static_cast<uint32_t>(raw[at + 2]) |
                                     (static_cast<uint32_t>(raw[at + 3]) << 8) |
                                     (static_cast<uint32_t>(raw[at + 4]) << 16) |
                                     (static_cast<uint32_t>(raw[at + 5]) << 24);
            if (raw[at] != 'B' || raw[at + 1] != 'M' || at + bmpSize > raw.size()) {
                *errorOut = "world map bitmap is not a BMP";
                return false;
            }
            DecodeBmpGrey(raw.data() + at, bmpSize, &terrain);
            r.Seek(at + bmpSize);
        }
    } else {
        r.LengthPrefixedString();
    }

    // The second grid header and the per-tile fog word (Campaign+0x1ad, the
    // element at tile+0x00).
    const int32_t w2 = r.I32(), h2 = r.I32();
    for (int32_t x = 0; x < w2; ++x) {
        for (int32_t y = 0; y < h2; ++y) {
            const int32_t v = r.I32();
            if (x < gridW && y < gridH) tiles[static_cast<size_t>(x) * gridH + y].fog = v;
        }
    }

    startMission = r.LengthPrefixedString();
    r.I32();                  // Campaign+0x1c
    r.I32();                  // Campaign+0x48 (int64, low)
    r.I32();                  // Campaign+0x4c (high)
    const int32_t nStringFiles = r.I32();
    (void)nStringFiles;       // always 0 in the shipped file; the list follows only if non-zero

    // varTableHeader 1
    {
        std::string vt;
        int32_t vv = 0;
        r.FormatToken(&vt, &vv);
        if (vt != "varTableHeader") {
            *errorOut = "expected varTableHeader, got '" + vt + "'";
            return false;
        }
        const int32_t nv = r.I32();
        for (int32_t i = 0; i < nv; ++i) varValues.push_back(r.LengthPrefixedString());
        const int32_t nn = r.I32();
        for (int32_t i = 0; i < nn; ++i) varNames.push_back(r.LengthPrefixedString());
    }

    auto f32 = [&]() {
        const uint32_t u = r.U32();
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    viewX = f32();
    viewY = f32();
    squadX = f32();
    squadY = f32();

    ft::Esh esh;
    std::string eerr;

    const int32_t nCores = r.I32();
    for (int32_t i = 0; i < nCores; ++i) {
        if (!esh.Read(r, &eerr)) {
            *errorOut = "core mission " + std::to_string(i) + ": " + eerr;
            return false;
        }
        CoreMission c;
        esh.GetString("Name", &c.nameKey);
        esh.GetString("MissionFile", &c.misPath);
        esh.GetInt("Loc x", &c.locX);
        esh.GetInt("Loc y", &c.locY);
        if (!esh.GetInt("Radius", &c.radius)) c.radius = 37;
        std::string st;
        if (esh.GetString("State", &st)) {
            const std::string l = Lower(st);
            c.state = l == "uncovered" ? MissionState::UnCovered
                                       : (l == "visited" ? MissionState::Visited
                                                         : MissionState::Covered);
        }
        esh.GetBool("Is Bunker", &c.isBunker);
        esh.GetBool("vehicleMission", &c.vehicleMission);
        esh.GetBool("specialMission", &c.specialMission);
        cores.push_back(c);
    }

    const int32_t nSpecials = r.I32();
    for (int32_t i = 0; i < nSpecials; ++i) {
        if (!esh.Read(r, &eerr)) {
            *errorOut = "special mission " + std::to_string(i) + ": " + eerr;
            return false;
        }
        SpecialMission s;
        esh.GetString("Name", &s.nameKey);
        esh.GetString("MissionFile", &s.misPath);
        if (!esh.GetInt("Radius", &s.radius)) s.radius = 20;
        esh.GetBool("Should Morph", &s.morph);
        if (!esh.GetBool("Once Only", &s.onceOnly)) s.onceOnly = true;
        specials.push_back(s);
    }

    const int32_t nForces = r.I32();
    for (int32_t i = 0; i < nForces; ++i) {
        std::string ft_, fv;
        int32_t fver = 0;
        r.FormatToken(&ft_, &fver);
        if (ft_ != "<random_force>") {
            *errorOut = "expected <random_force>, got '" + ft_ + "'";
            return false;
        }
        ForceDef f;
        f.name = r.LengthPrefixedString();
        f.align = r.LengthPrefixedString();
        f.minLevel = r.I32();
        f.maxLevel = r.I32();
        f.enabled = r.U8() != 0;
        const int32_t np = r.I32();
        std::vector<std::string> paths;
        for (int32_t k = 0; k < np; ++k) paths.push_back(r.LengthPrefixedString());
        const int32_t nl = r.I32();
        std::vector<int32_t> lv(static_cast<size_t>(std::max(0, nl)));
        for (int32_t k = 0; k < nl; ++k) lv[k] = r.I32();
        const int32_t nc = r.I32();
        std::vector<int32_t> ch(static_cast<size_t>(std::max(0, nc)));
        for (int32_t k = 0; k < nc; ++k) ch[k] = r.I32();
        f.cdCode = r.LengthPrefixedString();
        for (size_t k = 0; k < paths.size(); ++k) {
            ForceMember m;
            m.entPath = paths[k];
            m.level = k < lv.size() ? lv[k] : 0;
            m.chance = k < ch.size() ? ch[k] : 0;
            f.members.push_back(m);
        }
        forces.push_back(f);
    }

    auto strvec = [&](std::vector<std::string>* out) {
        const int32_t n = r.I32();
        for (int32_t i = 0; i < n; ++i) out->push_back(r.LengthPrefixedString());
    };
    strvec(&landscapes);   // Campaign+0x1fd
    strvec(&prefabs);      // Campaign+0x1bd

    const int32_t nQm = r.I32();
    for (int32_t i = 0; i < nQm; ++i) {
        StockEntry e;
        e.tag = r.LengthPrefixedString();
        e.entPath = r.LengthPrefixedString();
        e.count = r.I32();
        quarterMaster.push_back(e);
    }

    std::vector<std::string> unused151;
    strvec(&unused151);                        // Campaign+0x151, empty in the shipped file
    const int32_t n161 = r.I32();              // Campaign+0x161
    r.Take(static_cast<size_t>(std::max(0, n161)) * 4);

    const int32_t nRp = r.I32();
    for (int32_t i = 0; i < nRp; ++i) {
        RecruitEntry e;
        e.tag = r.LengthPrefixedString();
        e.entPath = r.LengthPrefixedString();
        e.op = r.LengthPrefixedString();
        recruitPool.push_back(e);
    }

    if (r.Failed()) {
        *errorOut = "truncated campaign file";
        return false;
    }
    info = "campaign v" + std::to_string(fileVersion) + ": " + std::to_string(gridW) + "x" +
           std::to_string(gridH) + " tiles of " + std::to_string(tilePx) + "px, " +
           std::to_string(cores.size()) + " cores, " + std::to_string(specials.size()) +
           " specials, " + std::to_string(forces.size()) + " forces, " +
           std::to_string(landscapes.size()) + " landscapes, " + std::to_string(prefabs.size()) +
           " prefabs, " + std::to_string(quarterMaster.size()) + " stock, " +
           std::to_string(recruitPool.size()) + " recruit rules" +
           (terrain.ok ? (", terrain " + std::to_string(terrain.w) + "x" +
                          std::to_string(terrain.h))
                       : "");
    this->raw = std::move(rawKeep);
    return true;
}

float WorldTerrain::Speed(double px, double py) const {
    if (!ok) return 1.0f;
    // `WWorldWindow::Update` l.23104-23116: sample at pos * 0.5, byte / 255.
    const int32_t x = static_cast<int32_t>(px * 0.5), y = static_cast<int32_t>(py * 0.5);
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
    return static_cast<float>(grey[static_cast<size_t>(y) * w + x]) * (1.0f / 255.0f);
}

bool CampaignFile::TileArt(int32_t tx, int32_t ty, std::vector<uint8_t>* rgba) const {
    if (tx < 0 || ty < 0 || tx >= gridW || ty >= gridH) return false;
    const CampaignTile& t = tiles[static_cast<size_t>(tx) * gridH + ty];
    if (!t.hasArt || t.artSize == 0 || t.artOffset + t.artSize > raw.size()) return false;
    ft::Reader r(raw.data() + t.artOffset, t.artSize);
    ft::ZarImage z;
    std::string e;
    if (!z.Read(r, &e)) return false;
    return z.DecodeToRGBA(std::vector<ft::Color32>(), rgba, &e);
}

const CampaignTile* CampaignFile::TileAtPixel(int32_t px, int32_t py) const {
    if (tilePx <= 0) return nullptr;
    const int32_t tx = px / tilePx, ty = py / tilePx;
    if (tx < 0 || ty < 0 || tx >= gridW || ty >= gridH) return nullptr;
    return &tiles[static_cast<size_t>(tx) * gridH + ty];
}

int32_t CampaignFile::CoreAtPixel(int32_t px, int32_t py) const {
    for (size_t i = 0; i < cores.size(); ++i) {
        const CoreMission& c = cores[i];
        const int64_t dx = px - c.locX, dy = py - c.locY;
        if (dx * dx + dy * dy <= static_cast<int64_t>(c.radius) * c.radius) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// ===========================================================================
// CampaignTables -- tables/campaign.txt
// ===========================================================================
namespace {

// The file is `key = { ... }` blocks of quoted strings and `{ ... }` groups,
// with `//` comments.  `Campaign::LoadCampaign*Section` all take the block's
// text through `GetLocalizedString(file, out, "<key>")` and then
// `ParseTokenListIntoVector`, so the tokenising below matches: quoted strings
// are the tokens, and a `{ ... }` group is a token list of its own.
struct TxtBlock {
    std::vector<std::vector<std::string>> groups;   // each group is its quoted strings
};

std::string StripComments(const std::string& s) {
    std::string out;
    bool inStr = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (!inStr && s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            out.push_back('\n');
            continue;
        }
        if (s[i] == '"') inStr = !inStr;
        out.push_back(s[i]);
    }
    return out;
}

}  // namespace

bool CampaignTables::Load(const std::string& path, std::string* errorOut) {
    forces.clear();
    cores.clear();
    specials.clear();
    landscapes.clear();
    prefabs.clear();
    quarterMaster.clear();
    recruitPool.clear();
    std::vector<uint8_t> raw;
    if (!ft::ReadWholeFile(path, &raw, errorOut)) return false;
    const std::string text =
        StripComments(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));

    // Find `key = {` ... matching `}` at depth 0.
    auto block = [&](const std::string& key, std::string* out) {
        const size_t k = text.find("\n" + key);
        if (k == std::string::npos) return false;
        size_t i = text.find('{', k);
        if (i == std::string::npos) return false;
        int depth = 0;
        size_t start = i;
        for (; i < text.size(); ++i) {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') {
                if (--depth == 0) {
                    *out = text.substr(start + 1, i - start - 1);
                    return true;
                }
            }
        }
        return false;
    };

    // Every quoted string in `body`, in order, with the group boundaries the
    // braces make.
    auto groups = [](const std::string& body) {
        std::vector<std::vector<std::string>> out;
        std::vector<std::string> flat;
        std::vector<std::string> cur;
        int depth = 0;
        for (size_t i = 0; i < body.size(); ++i) {
            const char c = body[i];
            if (c == '{') {
                ++depth;
                cur.clear();
            } else if (c == '}') {
                if (depth > 0 && !cur.empty()) out.push_back(cur);
                cur.clear();
                --depth;
            } else if (c == '"') {
                const size_t e = body.find('"', i + 1);
                if (e == std::string::npos) break;
                const std::string tokv = body.substr(i + 1, e - i - 1);
                if (depth > 0) cur.push_back(tokv);
                else flat.push_back(tokv);
                i = e;
            }
        }
        for (const std::string& s : flat) out.push_back({s});
        return out;
    };

    std::string body;
    if (block("forces", &body)) {
        for (const auto& g : groups(body)) {
            if (g.empty()) continue;
            const std::vector<std::string> h = Split(g[0], ';');
            if (h.size() < 6) continue;
            ForceDef f;
            f.name = Trim(h[0]);
            f.align = Trim(h[1]);
            f.minLevel = std::atoi(h[2].c_str());
            f.maxLevel = std::atoi(h[3].c_str());
            f.enabled = Lower(Trim(h[4])) != "false";
            f.cdCode = Trim(h[5]);
            for (size_t i = 1; i < g.size(); ++i) {
                const std::vector<std::string> m = Split(g[i], ';');
                if (m.size() < 3) continue;
                ForceMember mm;
                mm.entPath = Trim(m[0]);
                mm.level = std::atoi(m[1].c_str());
                mm.chance = std::atoi(m[2].c_str());
                f.members.push_back(mm);
            }
            forces.push_back(f);
        }
    }
    // `Campaign::LoadCampaignCoresSection @006a3f80` l.19880-19960: split on
    // ";", require >= 4 tokens (it uses five), token 2 clamped to [5, 250],
    // token 3 the state word, token 4 "isBunker" as != "false".
    if (block("cores", &body)) {
        for (const auto& g : groups(body)) {
            const std::vector<std::string> t = Split(g[0], ';');
            if (t.size() < 5) continue;
            CoreMission c;
            c.misPath = Trim(t[0]);
            c.nameKey = Trim(t[1]);
            c.radius = std::min(250, std::max(5, std::atoi(t[2].c_str())));
            const std::string st = Lower(Trim(t[3]));
            c.state = st == "uncovered" ? MissionState::UnCovered
                                        : (st == "visited" ? MissionState::Visited
                                                           : MissionState::Covered);
            c.isBunker = Lower(Trim(t[4])) != "false";
            cores.push_back(c);
        }
    }
    if (block("specials", &body)) {
        for (const auto& g : groups(body)) {
            const std::vector<std::string> t = Split(g[0], ';');
            if (t.size() < 5) continue;
            SpecialMission s;
            s.misPath = Trim(t[0]);
            s.nameKey = Trim(t[1]);
            s.radius = std::min(250, std::max(5, std::atoi(t[2].c_str())));
            s.morph = Lower(Trim(t[3])) != "false";
            s.onceOnly = Lower(Trim(t[4])) != "false";
            specials.push_back(s);
        }
    }
    if (block("landscapes", &body)) {
        for (const auto& g : groups(body)) landscapes.push_back(Trim(g[0]));
    }
    if (block("prefabs", &body)) {
        for (const auto& g : groups(body)) prefabs.push_back(Trim(g[0]));
    }
    if (block("quarterMaster", &body)) {
        for (const auto& g : groups(body)) {
            const std::vector<std::string> t = Split(g[0], ';');
            if (t.size() < 3) continue;
            StockEntry e;
            e.tag = Trim(t[0]);
            e.count = std::atoi(Trim(t[1]).c_str());
            e.entPath = Trim(t[2]);
            quarterMaster.push_back(e);
        }
    }
    if (block("recruitsPool", &body)) {
        for (const auto& g : groups(body)) {
            const std::vector<std::string> t = Split(g[0], ';');
            if (t.size() < 3) continue;
            RecruitEntry e;
            e.tag = Trim(t[0]);
            e.op = Trim(t[1]);
            e.entPath = Trim(t[2]);
            recruitPool.push_back(e);
        }
    }
    // The `quarterMaster` and `recruitsPool` blocks are brace groups, not
    // quoted strings, so `groups` above sees them as one-element groups only
    // when the braces contain quotes.  They do not, so re-scan them raw.
    auto braceRows = [&](const std::string& key, std::vector<std::string>* rows) {
        std::string b;
        if (!block(key, &b)) return;
        size_t i = 0;
        while (i < b.size()) {
            const size_t o = b.find('{', i);
            if (o == std::string::npos) break;
            const size_t c = b.find('}', o);
            if (c == std::string::npos) break;
            rows->push_back(b.substr(o + 1, c - o - 1));
            i = c + 1;
        }
    };
    if (quarterMaster.empty()) {
        std::vector<std::string> rows;
        braceRows("quarterMaster", &rows);
        for (const std::string& row : rows) {
            const std::vector<std::string> t = Split(row, ';');
            if (t.size() < 3) continue;
            StockEntry e;
            e.tag = Trim(t[0]);
            e.count = std::atoi(Trim(t[1]).c_str());
            e.entPath = Trim(t[2]);
            quarterMaster.push_back(e);
        }
    }
    if (recruitPool.empty()) {
        std::vector<std::string> rows;
        braceRows("recruitsPool", &rows);
        for (const std::string& row : rows) {
            const std::vector<std::string> t = Split(row, ';');
            if (t.size() < 3) continue;
            RecruitEntry e;
            e.tag = Trim(t[0]);
            e.op = Trim(t[1]);
            e.entPath = Trim(t[2]);
            recruitPool.push_back(e);
        }
    }
    return true;
}

// ===========================================================================
// CampaignState
// ===========================================================================
namespace {

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

}  // namespace

void CampaignState::Logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log.push_back(buf);
}

bool CampaignState::New(const std::string& assetsDir, std::string* errorOut) {
    const std::string want = JoinPath(assetsDir, "campaigns/bos.cam");
    // A restart re-reads the tables but not the 20 MB `.cam`; the campaign
    // file is immutable apart from the fog words, which New resets below.
    if (campaignPath != want || file.gridW == 0) {
        campaignPath = want;
        if (!file.Load(campaignPath, errorOut)) return false;
    }
    for (CampaignTile& t : file.tiles) t.fog = 0;
    std::string terr;
    tables.Load(JoinPath(assetsDir, "tables/campaign.txt"), &terr);

    // `Campaign::LoadCampaignCoresSection @006a3f80` REPLACES the vector the
    // `.cam` loaded (l.19855-19862, `VectorEraseCoreMissionRange`) with what
    // `tables/campaign.txt` says, having first copied the old records' `+4`
    // and `+8` -- Loc x and Loc y -- into a temporary and written them back by
    // INDEX afterwards (l.19845-19850 and l.20040-20065).  So the text file is
    // the runtime authority for names, radii, states and the bunker flag, and
    // the `.cam` is the authority for the positions.  The four sibling loaders
    // do the same for specials, forces, landscapes and prefabs.
    if (!tables.cores.empty()) {
        cores = tables.cores;
        for (size_t i = 0; i < cores.size(); ++i) {
            const size_t j = std::min(i, file.cores.empty() ? 0 : file.cores.size() - 1);
            if (!file.cores.empty()) {
                cores[i].locX = file.cores[j].locX;
                cores[i].locY = file.cores[j].locY;
            }
        }
    } else {
        cores = file.cores;
    }
    if (!tables.forces.empty()) file.forces = tables.forces;
    if (!tables.specials.empty()) {
        for (size_t i = 0; i < tables.specials.size() && i < file.specials.size(); ++i) {
            tables.specials[i].locX = file.specials[i].locX;
            tables.specials[i].locY = file.specials[i].locY;
        }
        file.specials = tables.specials;
    }
    if (!tables.landscapes.empty()) file.landscapes = tables.landscapes;
    if (!tables.prefabs.empty()) file.prefabs = tables.prefabs;
    if (!tables.quarterMaster.empty()) file.quarterMaster = tables.quarterMaster;
    if (!tables.recruitPool.empty()) file.recruitPool = tables.recruitPool;

    posX = file.squadX;
    posY = file.squadY;
    destX = posX;
    destY = posY;
    viewX = file.viewX;
    viewY = file.viewY;
    currentMission.clear();
    timeMs = 0;
    vars.clear();
    for (size_t i = 0; i < file.varNames.size() && i < file.varValues.size(); ++i) {
        vars[file.varNames[i]] = file.varValues[i];
    }
    roster.clear();
    pool.clear();
    stock.clear();
    // `mission00` is the tag campaign.txt uses for the starting squad; every
    // other tag is a campaign variable a mission sets.
    ApplyTag("mission00");
    for (const std::string& p : pool) {
        RosterMember m;
        m.entPath = p;
        m.inSquad = true;
        roster.push_back(m);
    }
    pool.clear();
    ApplyTag("mission01");
    Logf("new campaign: %s", file.info.c_str());
    return true;
}

int32_t CampaignState::FindCore(const std::string& key) const {
    for (size_t i = 0; i < cores.size(); ++i) {
        if (cores[i].nameKey == key || cores[i].misPath == key) return static_cast<int32_t>(i);
        const size_t s = cores[i].misPath.find_last_of('/');
        if (s != std::string::npos && Lower(cores[i].misPath.substr(s + 1)) == Lower(key)) {
            return static_cast<int32_t>(i);
        }
    }
    return -1;
}

// `ActionSetMissionState`'s effect: `CoreMission+0x24`, the enum
// `CoreMission::Serialize @0069b220` writes as "Covered"/"UnCovered"/"Visited".
void CampaignState::SetMissionState(const std::string& nameKey, MissionState st) {
    const int32_t i = FindCore(nameKey);
    if (i < 0) {
        Logf("set mission state: no such mission '%s'", nameKey.c_str());
        return;
    }
    cores[i].state = st;
    Logf("mission '%s' -> %s", cores[i].nameKey.c_str(),
         st == MissionState::Visited ? "Visited"
                                     : (st == MissionState::UnCovered ? "UnCovered" : "Covered"));
}

void CampaignState::SetVar(const std::string& k, const std::string& v) {
    vars[k] = v;
}

std::string CampaignState::Var(const std::string& k) const {
    const auto it = vars.find(k);
    return it == vars.end() ? std::string() : it->second;
}

// campaign.txt's `recruitsPool` and `quarterMaster` are both keyed by a tag,
// and the tag becomes true when the campaign reaches that point.  Applying a
// tag adds every row that carries it.
void CampaignState::ApplyTag(const std::string& tag) {
    const std::string t = Lower(tag);
    for (const RecruitEntry& e : file.recruitPool) {
        if (Lower(e.tag) != t) continue;
        if (Lower(e.op) == "add") {
            if (std::find(pool.begin(), pool.end(), e.entPath) == pool.end()) {
                bool already = false;
                for (const RosterMember& m : roster) already |= (m.entPath == e.entPath);
                if (!already) pool.push_back(e.entPath);
            }
        } else {
            pool.erase(std::remove(pool.begin(), pool.end(), e.entPath), pool.end());
        }
    }
    for (const StockEntry& e : file.quarterMaster) {
        if (Lower(e.tag) == t) stock[e.entPath] += e.count;
    }
    vars["TAG_" + tag] = "TRUE";
}

std::vector<std::string> CampaignState::DeployedPaths() const {
    std::vector<std::string> out;
    for (const RosterMember& m : roster) {
        if (m.inSquad && !m.dead) out.push_back(m.entPath);
    }
    return out;
}

void CampaignState::SetDestination(double px, double py) {
    destX = std::max(0.0, std::min<double>(px, file.PixelW() - 1));
    destY = std::max(0.0, std::min<double>(py, file.PixelH() - 1));
    travelling = true;
    destCore = file.CoreAtPixel(static_cast<int32_t>(destX), static_cast<int32_t>(destY));
    Logf("travel: (%.0f,%.0f) -> (%.0f,%.0f)%s", posX, posY, destX, destY,
         destCore >= 0 ? (" [" + cores[destCore].nameKey + "]").c_str() : "");
}

// The MSVC LCG `WWorldWindow` uses for every campaign roll
// (`TrySpawnRandomEncounter@006bad60`, `WWorldWindow::WWorldWindow__ProcessWorldMapTick@006bcaa0`).
int32_t CampaignState::Rand() {
    rng = rng * 0x343fdu + 0x269ec3u;
    return static_cast<int32_t>((rng >> 16) & 0x7fff);
}

// `RevealFogOfWarAroundTile` (l.23129).  Its body is in no dump; what is
// recoverable is that it takes the squad's tile and that `RenderWorldView`
// blacks out a tile whose `fog` is 0 (l.23925 / l.24116).  The radius of one
// tile is this port's.
void CampaignState::RevealFog(double px, double py) {
    if (file.tilePx <= 0) return;
    const int32_t tx = static_cast<int32_t>(px) / file.tilePx;
    const int32_t ty = static_cast<int32_t>(py) / file.tilePx;
    for (int32_t dy = -1; dy <= 1; ++dy) {
        for (int32_t dx = -1; dx <= 1; ++dx) {
            const int32_t x = tx + dx, y = ty + dy;
            if (x < 0 || y < 0 || x >= file.gridW || y >= file.gridH) continue;
            CampaignTile& t = file.tiles[static_cast<size_t>(x) * file.gridH + y];
            if (t.fog == 0) t.fog = 1;
        }
    }
}

EncounterRoll CampaignState::Step(double deltaMs) {
    EncounterRoll out;
    if (!travelling) return out;

    // `WWorldWindow::Update` l.23067-23127.
    const double dt = std::min(deltaMs * 0.001, kMaxFrameSeconds);
    const double dx = destX - posX, dy = destY - posY;
    const double len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0) {
        travelling = false;
        return out;
    }
    const float terrain = file.terrain.Speed(posX, posY);
    if (terrain < kImpassable) {
        travelling = false;
        Logf("travel stopped: impassable terrain at (%.0f,%.0f)", posX, posY);
        return out;
    }
    const double stepLen = dt * kBaseSpeed * speed * terrain * kSpeedFactorK;
    const double nx = posX + dx / len * stepLen;
    const double ny = posY + dy / len * stepLen;
    // l.23131-23140: the per-axis arrival test.
    if (std::fabs(destX - nx) < stepLen && std::fabs(destY - ny) < stepLen) {
        posX = destX;
        posY = destY;
        travelling = false;
    } else {
        posX = nx;
        posY = ny;
    }
    travelled += stepLen;
    // Campaign time.  `Campaign+0x48` is the int64 the save carries; nothing in
    // the dumps reads it, so the mapping from travel to the clock is this
    // port's: one in-game minute per world-map pixel, which puts a bunker-to-
    // mission leg at a few hours and the whole map at a few days.
    timeMs += static_cast<int64_t>(stepLen * 60000.0);
    RevealFog(posX, posY);

    // `TrySpawnRandomEncounter @006bad60`, called once per travel frame with
    // the terrain-modified step as its argument (l.23103).
    if (!encountersEnabled) return out;
    encounterClock += static_cast<int64_t>(deltaMs);
    if (encounterClock < kEncounterTickMs) return out;
    encounterClock = 0;

    const CampaignTile* tile =
        file.TileAtPixel(static_cast<int32_t>(posX), static_cast<int32_t>(posY));
    if (tile == nullptr) return out;

    // (a) The unique/special roll, l.23216-23300.  The original compares an
    // integer `rand() % max(total, 100)` against a float accumulator scaled by
    // the unresolved `_DAT_0082dae8`; with that constant missing the two sides
    // are in different units, so the accumulator is normalised to a
    // probability here and rolled the same way the force roll is.  That is a
    // reconstruction, and it is the only way to keep the special rate sane.
    {
        int32_t total = 0;
        for (int32_t c : tile->specialChance) total += c;
        if (total > 0) {
            const double k = alwaysEncounter ? 1.0 : kEncounterScale * stepLen;
            const double roll = Rand() * (1.0 / 32767.0);
            double acc = 0;
            for (size_t i = 0; i < tile->specialChance.size() && i < file.specials.size(); ++i) {
                acc += (tile->specialChance[i] / 100.0) * k;
                if (roll < acc && !file.specials[i].used) {
                    out.fired = true;
                    out.special = static_cast<int32_t>(i);
                    out.forceName = file.specials[i].nameKey;
                    out.mapPath = file.specials[i].misPath;
                    out.roll = static_cast<int32_t>(roll * 32767);
                    out.threshold = static_cast<int32_t>(acc * 32767);
                    file.specials[i].used = file.specials[i].onceOnly;
                    travelling = false;
                    Logf("special encounter: %s", out.mapPath.c_str());
                    return out;
                }
            }
        }
    }

    // (b) The random-force roll, l.23330-23503: only enabled forces count, the
    // total and the maximum are clamped to [5, 50], and up to five forces are
    // drawn.
    int32_t total = 0, maxw = 0;
    for (size_t i = 0; i < tile->forceChance.size() && i < file.forces.size(); ++i) {
        if (!file.forces[i].enabled || tile->forceChance[i] <= 0) continue;
        total += tile->forceChance[i];
        maxw = std::max(maxw, tile->forceChance[i]);
    }
    if (total <= 0) return out;
    total = std::min(50, std::max(5, total));
    maxw = std::min(50, std::max(5, maxw));

    const double k = kEncounterScale * stepLen;
    for (int n = 0; n < 5; ++n) {
        const double chance = (n == 0) ? (maxw / 100.0) * k * 100.0 : total * k;
        out.threshold = static_cast<int32_t>(chance * 32767);
        out.roll = Rand();
        if (out.roll * (1.0 / 32767.0) >= chance) break;
        // Pick one enabled force by weight.
        int32_t pool2 = 0;
        for (size_t i = 0; i < tile->forceChance.size() && i < file.forces.size(); ++i) {
            if (!file.forces[i].enabled || tile->forceChance[i] <= 0) continue;
            pool2 += tile->forceChance[i];
        }
        if (pool2 <= 0) break;
        int32_t pick = Rand() % pool2;
        for (size_t i = 0; i < tile->forceChance.size() && i < file.forces.size(); ++i) {
            const ForceDef& f = file.forces[i];
            if (!f.enabled || tile->forceChance[i] <= 0) continue;
            pick -= tile->forceChance[i];
            if (pick >= 0) continue;
            out.fired = true;
            out.forceName = out.forceName.empty() ? f.name : out.forceName + "+" + f.name;
            // `GenerateRandomEncounterSpawnList @006adcb0` l.21519-21580: a
            // count in [min, max], each member drawn by its `appearance
            // chance` weight, its level nudged toward the campaign level.
            const int32_t lo = std::max(0, f.minLevel), hi = std::max(1, f.maxLevel);
            const int32_t count = lo + (hi > lo ? Rand() % (hi - lo + 1) : 0);
            int32_t wsum = 0;
            for (const ForceMember& m : f.members) wsum += std::max(0, m.chance);
            for (int32_t c = 0; c < count && wsum > 0; ++c) {
                int32_t p = Rand() % wsum;
                for (const ForceMember& m : f.members) {
                    p -= std::max(0, m.chance);
                    if (p < 0) {
                        out.spawn.push_back(m.entPath);
                        out.spawnLevel.push_back(std::max(1, m.level));
                        break;
                    }
                }
            }
            break;
        }
    }
    if (!out.fired) return out;

    // `PickWeightedRandomEntryAtPosition` l.24271: the map comes from the same
    // tile's third weight vector over the `landscapes` list.
    int32_t lpool = 0;
    for (size_t i = 0; i < tile->landscapeChance.size() && i < file.landscapes.size(); ++i) {
        lpool += std::max(0, tile->landscapeChance[i]);
    }
    if (lpool > 0) {
        int32_t p = Rand() % lpool;
        for (size_t i = 0; i < tile->landscapeChance.size() && i < file.landscapes.size(); ++i) {
            p -= std::max(0, tile->landscapeChance[i]);
            if (p < 0) {
                out.mapPath = file.landscapes[i];
                break;
            }
        }
    }
    travelling = false;
    Logf("encounter: %s on %s (%zu units)", out.forceName.c_str(), out.mapPath.c_str(),
         out.spawn.size());
    return out;
}

int32_t CampaignState::SquadLevel() const {
    int32_t n = 0, sum = 0;
    for (const RosterMember& m : roster) {
        if (!m.inSquad || m.dead) continue;
        sum += m.level;
        ++n;
    }
    return n ? std::max(1, sum / n) : 1;
}

}  // namespace game
