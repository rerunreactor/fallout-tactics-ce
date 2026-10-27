#include "esh.h"

#include <cctype>
#include <cstring>

namespace ft {
namespace {

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

float ReadFloatLE(const uint8_t* p) {
    uint32_t bits = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                    (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

int32_t ReadIntLE(const uint8_t* p) {
    return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                                (static_cast<uint32_t>(p[2]) << 16) |
                                (static_cast<uint32_t>(p[3]) << 24));
}

// EntityStreamWriteInteger l.20138: a payload bigger than 16,000,000 bytes is
// refused for every type except the nested-<esh> one.
constexpr uint32_t kPayloadSanityLimit = 0xf42400;

// The length prefix in front of every entity-stream string is a TAGGED count,
// not a plain one: bit 31 means "the characters that follow are 16 bit", and
// the low 31 bits are the CHARACTER count, so a wide string occupies `2 * n`
// bytes.  Six files in the shipped loose-file set --
// `entities/items/Enhancers/{doctorsBag,paramedicsBag,fieldMedicKit,
// firstAidKit}.ent` and `entities/items/Ammo/{ammo2mmEC,ammoFlamer}.ent`, every
// one of which SHADOWS its `entities_0.bos` copy -- carry their `<entity>`
// class name in that form (doctorsBag also carries one property name that way).
// Read with the plain rule the length is negative and the whole file is
// refused, which is why the four medical `SkillConsumable`s could not be
// loaded at all.  Decoded with this rule all six parse to exactly EOF.
constexpr uint32_t kWideStringFlag = 0x80000000u;

// Every shipped wide string is ASCII, so a character outside Latin-1 is
// replaced rather than silently truncated to its low byte.
std::string NarrowUtf16(const uint8_t* p, size_t chars) {
    std::string out;
    out.reserve(chars);
    for (size_t i = 0; i < chars; ++i) {
        const uint32_t c =
            static_cast<uint32_t>(p[i * 2]) | (static_cast<uint32_t>(p[i * 2 + 1]) << 8);
        out.push_back(c < 0x100 ? static_cast<char>(c) : '?');
    }
    return out;
}

// The tagged length-prefixed string, off a stream.  `Reader::LengthPrefixedString`
// lives in common/ and is shared with the sprite and tile loaders, which never
// see a wide string, so the tag is decoded here.
std::string ReadTaggedString(Reader& stream, bool* wideOut = nullptr) {
    const uint32_t raw = stream.U32();
    if (stream.Failed()) return std::string();
    const size_t chars = static_cast<size_t>(raw & ~kWideStringFlag);
    const bool wide = (raw & kWideStringFlag) != 0;
    if (wideOut != nullptr) *wideOut = wide;
    const uint8_t* body = stream.Take(wide ? chars * 2 : chars);
    if (body == nullptr) return std::string();
    if (wide) return NarrowUtf16(body, chars);
    return std::string(reinterpret_cast<const char*>(body), chars);
}

}  // namespace

bool EshTypeIsString(uint32_t type) {
    switch (type) {
        case kEshString:
        case kEshString8:
        case kEshString9:
        case kEshString10:
        case kEshNested:
        case kEshString16:
        case kEshString19:
        case kEshString21:
        case kEshString25:
        case kEshString26:
            return true;
        default:
            return false;
    }
}

const EshProp* Esh::Find(const std::string& name) const {
    for (const EshProp& p : props) {
        if (p.name == name) {
            return &p;
        }
    }
    const std::string want = LowerAscii(name);
    for (const EshProp& p : props) {
        if (LowerAscii(p.name) == want) {
            return &p;
        }
    }
    return nullptr;
}

bool Esh::GetString(const std::string& name, std::string* out) const {
    const EshProp* p = Find(name);
    if (p == nullptr || !EshTypeIsString(p->type) || p->payload.size() < 4) {
        return false;
    }
    const uint32_t raw = static_cast<uint32_t>(ReadIntLE(p->payload.data()));
    const size_t chars = static_cast<size_t>(raw & ~kWideStringFlag);
    const bool wide = (raw & kWideStringFlag) != 0;
    const size_t bytes = wide ? chars * 2 : chars;
    if (bytes + 4 != p->payload.size()) {
        return false;
    }
    if (wide) {
        *out = NarrowUtf16(p->payload.data() + 4, chars);
        return true;
    }
    out->assign(reinterpret_cast<const char*>(p->payload.data() + 4), chars);
    return true;
}

bool Esh::GetInt(const std::string& name, int32_t* out) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshInt || p->payload.size() != 4) {
        return false;
    }
    *out = ReadIntLE(p->payload.data());
    return true;
}

bool Esh::GetFloat(const std::string& name, float* out) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshFloat || p->payload.size() != 4) {
        return false;
    }
    *out = ReadFloatLE(p->payload.data());
    return true;
}

bool Esh::GetBool(const std::string& name, bool* out) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshBool || p->payload.size() != 1) {
        return false;
    }
    *out = p->payload[0] != 0;
    return true;
}

bool Esh::GetColour(const std::string& name, float out[5]) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshColour || p->payload.size() != 20) {
        return false;
    }
    for (int i = 0; i < 5; ++i) {
        out[i] = ReadFloatLE(p->payload.data() + i * 4);
    }
    return true;
}

bool Esh::GetVec3(const std::string& name, float out[3]) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshVec3 || p->payload.size() != 12) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        out[i] = ReadFloatLE(p->payload.data() + i * 4);
    }
    return true;
}

bool Esh::GetFrame(const std::string& name, float out[12]) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshFrame || p->payload.size() != 48) {
        return false;
    }
    for (int i = 0; i < 12; ++i) {
        out[i] = ReadFloatLE(p->payload.data() + i * 4);
    }
    return true;
}

bool Esh::GetRect(const std::string& name, int32_t out[4]) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshRect || p->payload.size() != 16) {
        return false;
    }
    for (int i = 0; i < 4; ++i) {
        out[i] = ReadIntLE(p->payload.data() + i * 4);
    }
    return true;
}

bool Esh::GetNested(const std::string& name, Esh* out, std::string* errorOut) const {
    const EshProp* p = Find(name);
    if (p == nullptr || p->type != kEshNested || p->payload.size() < 4) {
        return false;
    }
    const int32_t innerLength = ReadIntLE(p->payload.data());
    if (innerLength <= 0 || static_cast<size_t>(innerLength) + 4 != p->payload.size()) {
        return false;  // an empty nested block ("esbin" with a zero length)
    }
    Reader inner(p->payload.data() + 4, static_cast<size_t>(innerLength));
    return out->Read(inner, errorOut);
}

bool Esh::Read(Reader& stream, std::string* errorOut) {
    std::string tag;
    if (!stream.FormatToken(&tag, &version)) {
        *errorOut = "truncated <esh> token";
        return false;
    }
    if (tag != "<esh>") {
        *errorOut = "expected <esh>, got " + tag;
        return false;
    }
    // The reader in the original (ParseEshTableSection) is not in the supplied
    // decompilation and the readers that are present do not check the version at
    // all, so neither does this one; every shipped block is version 1.
    const uint32_t count = stream.U32();
    if (stream.Failed()) {
        *errorOut = "truncated <esh> record count";
        return false;
    }
    if (count > 100000) {
        *errorOut = "implausible <esh> record count";
        return false;
    }
    props.clear();
    props.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        EshProp p;
        p.name = ReadTaggedString(stream, &p.nameWide);
        p.type = stream.U32();
        const uint32_t size = stream.U32();
        if (stream.Failed()) {
            *errorOut = "truncated <esh> record header";
            return false;
        }
        if (p.type != kEshNested && size > kPayloadSanityLimit) {
            *errorOut = "oversized <esh> payload for '" + p.name + "'";
            return false;
        }
        const uint8_t* body = stream.Take(size);
        if (body == nullptr) {
            *errorOut = "truncated <esh> payload for '" + p.name + "'";
            return false;
        }
        p.payload.assign(body, body + size);
        props.push_back(std::move(p));
    }
    return true;
}

bool EntityBlock::Read(Reader& stream, std::string* errorOut) {
    std::string tag;
    if (!stream.FormatToken(&tag, &version)) {
        *errorOut = "truncated <entity> token";
        return false;
    }
    if (tag != "<entity>") {
        *errorOut = "expected <entity>, got " + tag;
        return false;
    }
    className = ReadTaggedString(stream);
    if (stream.Failed()) {
        *errorOut = "truncated <entity> class name";
        return false;
    }
    return esh.Read(stream, errorOut);
}

namespace {
void PutU32(std::vector<uint8_t>* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void PutToken(std::vector<uint8_t>* out, const char* tag, int32_t version) {
    const char* p = tag;
    while (*p != 0) out->push_back(static_cast<uint8_t>(*p++));
    out->push_back(0);
    const std::string v = std::to_string(version);
    for (char c : v) out->push_back(static_cast<uint8_t>(c));
    out->push_back(0);
}
}  // namespace

void WriteTaggedString(std::vector<uint8_t>* out, const std::string& s, bool wide) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    PutU32(out, wide ? (n | kWideStringFlag) : n);
    for (char c : s) {
        out->push_back(static_cast<uint8_t>(c));
        if (wide) out->push_back(0);
    }
}

void Esh::Write(std::vector<uint8_t>* out) const {
    PutToken(out, "<esh>", version);
    PutU32(out, static_cast<uint32_t>(props.size()));
    for (const EshProp& p : props) {
        WriteTaggedString(out, p.name, p.nameWide);
        PutU32(out, p.type);
        PutU32(out, static_cast<uint32_t>(p.payload.size()));
        out->insert(out->end(), p.payload.begin(), p.payload.end());
    }
}


EshProp* Esh::FindMut(const std::string& name) {
    return const_cast<EshProp*>(Find(name));
}

bool Esh::SetInt(const std::string& name, int32_t v) {
    EshProp* p = FindMut(name);
    if (p == nullptr || p->type != kEshInt || p->payload.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        p->payload[static_cast<size_t>(i)] = static_cast<uint8_t>((static_cast<uint32_t>(v) >> (8 * i)) & 0xFF);
    }
    return true;
}

bool Esh::SetFloat(const std::string& name, float v) {
    EshProp* p = FindMut(name);
    if (p == nullptr || p->type != kEshFloat || p->payload.size() != 4) return false;
    uint32_t u;
    std::memcpy(&u, &v, 4);
    for (int i = 0; i < 4; ++i) {
        p->payload[static_cast<size_t>(i)] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
    }
    return true;
}

bool Esh::SetFrame(const std::string& name, const float v[12]) {
    EshProp* p = FindMut(name);
    if (p == nullptr || p->type != kEshFrame || p->payload.size() != 48) return false;
    for (int k = 0; k < 12; ++k) {
        uint32_t u;
        std::memcpy(&u, &v[k], 4);
        for (int i = 0; i < 4; ++i) {
            p->payload[static_cast<size_t>(k * 4 + i)] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
        }
    }
    return true;
}

bool Esh::SetNested(const std::string& name, const Esh& inner) {
    EshProp* p = FindMut(name);
    if (p == nullptr || p->type != kEshNested) return false;
    std::vector<uint8_t> body;
    inner.Write(&body);
    // SerializeTaggedPropertyRecord l.17233-17240: the payload is `i32 innerLen`
    // followed by the document, and payloadSize == innerLen + 4.
    std::vector<uint8_t> out;
    const uint32_t n = static_cast<uint32_t>(body.size());
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    p->payload = std::move(out);
    return true;
}


void Esh::PutInt(const std::string& name, int32_t v) {
    if (SetInt(name, v)) return;
    EshProp p;
    p.name = name;
    p.type = kEshInt;
    p.payload.assign(4, 0);
    props.push_back(std::move(p));
    SetInt(name, v);
}

void Esh::PutFrame(const std::string& name, const float v[12]) {
    if (SetFrame(name, v)) return;
    EshProp p;
    p.name = name;
    p.type = kEshFrame;
    p.payload.assign(48, 0);
    props.push_back(std::move(p));
    SetFrame(name, v);
}

void Esh::PutString(const std::string& name, const std::string& v, uint32_t type) {
    EshProp* p = FindMut(name);
    if (p == nullptr) {
        EshProp np;
        np.name = name;
        np.type = type;
        props.push_back(std::move(np));
        p = &props.back();
    }
    p->type = type;
    p->payload.clear();
    const uint32_t n = static_cast<uint32_t>(v.size());
    for (int i = 0; i < 4; ++i) p->payload.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xFF));
    p->payload.insert(p->payload.end(), v.begin(), v.end());
}

}  // namespace ft
