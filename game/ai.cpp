// ai.cpp -- the `tables/aitable.txt` reader and the two small name tables.
//
// The file is the game's own, shipped uncompressed under `tables/`, and the
// engine reads it at startup: `BaseAI::BuildDemeanourNatureFunctionTable
// @0051e8a0` looks the actor's `Nature` up in it by name
// ("Failed to find nature '%s' in AI"), walks the ten demeanours in the file's
// own order, and refuses to build anything at all when the demeanour list is
// not exactly ten long ("Only %d of %d listed in AI Table").
//
// The syntax is `name = { ... }` with `//` comments and `"quoted"` items; the
// two matrices are braces of braces.  One shipped row is missing a comma
// (`0        0,` in demeanours_versus_natures), so the parser splits on
// whitespace and commas alike rather than on commas only.

#include "ai.h"

#include "file.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace game {

namespace {

const char* const kDemeanourNames[kDemeanourCount] = {
    "Attack",         "MoveFlee",       "MoveCover",        "MoveWander",
    "MiscAssistHeal", "MiscEatCorpse",  "MiscLayTrap",      "MiscDisarmTrap",
    "MiscTriggerAlarm", "MiscInvestigate",
};

const char* const kEventNames[kAiEventCount] = {
    "tookDamage", "seeTarget", "seeCorpse", "seeTrap", "hearCombat",
    "caughtThief", "overridePassify", "overrideFlee", "overrideAttack",
};

// Strip `//` comments and fold the whole file to one token stream.
std::string StripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    size_t i = 0;
    while (i < src.size()) {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            while (i < src.size() && src[i] != '\n') ++i;
        } else {
            out.push_back(src[i++]);
        }
    }
    return out;
}

// The body of `name = { ... }`, brace-balanced, or "" when absent.
std::string Block(const std::string& s, const std::string& name) {
    size_t p = s.find(name);
    while (p != std::string::npos) {
        // The name must not be a suffix of a longer identifier.
        const bool leftOk = p == 0 || (!std::isalnum(static_cast<unsigned char>(s[p - 1])) &&
                                       s[p - 1] != '_');
        size_t q = p + name.size();
        while (q < s.size() && std::isspace(static_cast<unsigned char>(s[q]))) ++q;
        if (leftOk && q < s.size() && s[q] == '=') {
            ++q;
            while (q < s.size() && s[q] != '{') ++q;
            if (q >= s.size()) return std::string();
            int depth = 0;
            const size_t start = q + 1;
            for (; q < s.size(); ++q) {
                if (s[q] == '{') ++depth;
                else if (s[q] == '}' && --depth == 0) return s.substr(start, q - start);
            }
            return std::string();
        }
        p = s.find(name, p + 1);
    }
    return std::string();
}

std::vector<std::string> Quoted(const std::string& body) {
    std::vector<std::string> out;
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string::npos) {
        const size_t j = body.find('"', i + 1);
        if (j == std::string::npos) break;
        out.push_back(body.substr(i + 1, j - i - 1));
        i = j + 1;
    }
    return out;
}

// Every integer in the text, in order.  Commas and whitespace both separate,
// which is what makes the shipped missing comma harmless.
std::vector<int32_t> Ints(const std::string& body) {
    std::vector<int32_t> out;
    const char* p = body.c_str();
    while (*p != '\0') {
        if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p))) {
            char* end = nullptr;
            out.push_back(static_cast<int32_t>(std::strtol(p, &end, 10)));
            p = end;
        } else {
            ++p;
        }
    }
    return out;
}

}  // namespace

const char* DemeanourName(int d) {
    return (d >= 0 && d < kDemeanourCount) ? kDemeanourNames[d] : "?";
}
const char* AiEventName(int e) {
    return (e >= 0 && e < kAiEventCount) ? kEventNames[e] : "?";
}

int32_t AiTable::NatureIndex(const std::string& name) const {
    for (size_t i = 0; i < natures.size(); ++i) {
        if (natures[i] == name) return static_cast<int32_t>(i);
    }
    return -1;
}

bool AiTable::Load(const std::string& path, std::string* errorOut) {
    // MILESTONE 15.  This was its own `std::ifstream`, which is the one asset
    // read in the tree that did not go through `ft::ReadWholeFile` -- so it was
    // also the one that could not see inside a mounted `.bos`, and booting from
    // the retail archives lost every nature and demeanour with only a warning
    // on stderr.  `tables/aitable.txt` ships inside `entities_0.bos`.
    std::vector<uint8_t> raw;
    std::string err;
    if (!ft::ReadWholeFile(path, &raw, &err)) {
        if (errorOut != nullptr) *errorOut = err;
        return false;
    }
    const std::string src = StripComments(std::string(raw.begin(), raw.end()));

    natures = Quoted(Block(src, "natures"));
    events = Quoted(Block(src, "events"));
    demeanours = Quoted(Block(src, "demeanours"));
    if (demeanours.size() != kDemeanourCount) {
        // The binary's own check, verbatim.
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Only %d of %d listed in AI Table",
                      static_cast<int>(demeanours.size()), kDemeanourCount);
        if (errorOut != nullptr) *errorOut = buf;
        return false;
    }
    const std::vector<int32_t> st = Ints(Block(src, "starting_demeanours"));
    if (st.size() < kDemeanourCount) {
        if (errorOut != nullptr) *errorOut = "starting_demeanours is short";
        return false;
    }
    for (int i = 0; i < kDemeanourCount; ++i) starting[i] = st[static_cast<size_t>(i)];

    const std::vector<int32_t> vn = Ints(Block(src, "demeanours_versus_natures"));
    const std::vector<int32_t> ve = Ints(Block(src, "demeanours_versus_events"));
    if (vn.size() < natures.size() * kDemeanourCount ||
        ve.size() < events.size() * kDemeanourCount) {
        if (errorOut != nullptr) *errorOut = "a demeanour matrix is short";
        return false;
    }
    vsNature.assign(natures.size(), {});
    for (size_t n = 0; n < natures.size(); ++n) {
        for (int d = 0; d < kDemeanourCount; ++d) {
            vsNature[n][static_cast<size_t>(d)] = vn[n * kDemeanourCount + static_cast<size_t>(d)];
        }
    }
    vsEvent.assign(events.size(), {});
    for (size_t e = 0; e < events.size(); ++e) {
        for (int d = 0; d < kDemeanourCount; ++d) {
            vsEvent[e][static_cast<size_t>(d)] = ve[e * kDemeanourCount + static_cast<size_t>(d)];
        }
    }
    return true;
}

}  // namespace game
