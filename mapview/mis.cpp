#include "mis.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <map>

namespace ft {
namespace {

bool Inflate(const uint8_t* src, size_t srcSize, size_t hint, std::vector<uint8_t>* out,
             std::string* errorOut) {
    out->clear();
    out->resize(hint > 0 ? hint : 1 << 20);

    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (inflateInit(&zs) != Z_OK) {
        *errorOut = "inflateInit failed";
        return false;
    }
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(srcSize);

    size_t produced = 0;
    for (;;) {
        if (produced == out->size()) {
            out->resize(out->size() * 2);
        }
        zs.next_out = out->data() + produced;
        zs.avail_out = static_cast<uInt>(out->size() - produced);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        produced = out->size() - zs.avail_out;
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            inflateEnd(&zs);
            *errorOut = "zlib inflate error " + std::to_string(rc);
            return false;
        }
        if (zs.avail_in == 0 && zs.avail_out != 0) {
            inflateEnd(&zs);
            *errorOut = "zlib stream truncated";
            return false;
        }
    }
    inflateEnd(&zs);
    out->resize(produced);
    return true;
}

bool IsTagChar(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

}  // namespace

// World::Load's prologue: ReadFormatToken("<world>", version), then two int32s
// holding the inflated size (written twice), then the zlib stream to EOF.
bool World::Load(const std::string& file, std::string* errorOut) {
    std::vector<uint8_t> raw;
    if (!ReadWholeFile(file, &raw, errorOut)) {
        return false;
    }
    return LoadFromMemory(raw, file, errorOut);
}

bool World::LoadFromMemory(const std::vector<uint8_t>& raw, const std::string& name,
                           std::string* errorOut) {
    path = name;
    rawSize = raw.size();

    Reader outer(raw);
    std::string tag;
    int32_t v = 0;
    outer.FormatToken(&tag, &v);
    if (tag == "<saveh>") {
        // The save-game header `World::Save` puts in front of the world: a
        // flag, five strings, eight `<zar>`s (two pictures and six portraits)
        // and six floats.  `SaveGameHeader::SaveGameHeaderProperties @004bfea0`.
        hasSaveHeader = true;
        outer.U8();
        saveHeaderName = outer.TaggedString();
        for (int i = 0; i < 4; ++i) outer.TaggedString();
        for (int i = 0; i < 8; ++i) {
            std::string zt;
            int32_t zv = 0;
            outer.FormatToken(&zt, &zv);
            if (zt != "<zar>") {
                *errorOut = "bad <zar> in the save header: '" + zt + "'";
                return false;
            }
            outer.I32();
            outer.I32();
            if (outer.U8() != 0) {
                const uint32_t n = outer.U32();
                if (n > 65536) {
                    *errorOut = "implausible palette in the save header";
                    return false;
                }
                outer.Take(n * 4);
                outer.U8();
            }
            const uint32_t bytes = outer.U32();
            outer.Take(bytes);
        }
        outer.Take(24);
        if (outer.Failed()) {
            *errorOut = "truncated <saveh> block";
            return false;
        }
        outer.FormatToken(&tag, &v);
    }
    if (tag != "<world>") {
        *errorOut = "not a world file: tag='" + tag + "'";
        return false;
    }
    version = v;
    const int32_t size1 = outer.I32();
    outer.I32();  // the same value, written twice
    if (outer.Failed()) {
        *errorOut = "truncated <world> header";
        return false;
    }

    std::vector<uint8_t> blob;
    if (!Inflate(raw.data() + outer.Pos(), raw.size() - outer.Pos(),
                 size1 > 0 ? static_cast<size_t>(size1) : 0, &blob, errorOut)) {
        return false;
    }
    inflatedSize = blob.size();

    ScanSections(blob);

    // The save-game form.  `World::Save @004ea450` takes the `param_3 != 0`
    // branch: instead of
    // `MapManager::SaveMapManagerPropertiesWithProgress` it writes the source
    // map's path as one tagged length-prefixed string and then
    // `World::SaveWorldSectionProperties @004eb470`, which is the <sgd> block.
    // Nothing of the tile map is written -- the loader re-reads it from the
    // named `.mis` -- so there is no <mapManager> to parse here.
    if (blob.size() > 8) {
        Reader probe(blob.data(), blob.size());
        const uint32_t raw = probe.U32();
        const size_t chars = static_cast<size_t>(raw & 0x7fffffffu);
        const size_t bytes = (raw & 0x80000000u) != 0 ? chars * 2 : chars;
        if (chars > 0 && chars < 1024 && 4 + bytes + 6 <= blob.size() &&
            std::memcmp(blob.data() + 4 + bytes, "<sgd>", 6) == 0) {
            isSaveForm = true;
            saveMapPathWide = (raw & 0x80000000u) != 0;
            saveMapPath.clear();
            for (size_t i = 0; i < chars; ++i) {
                const uint8_t* q = blob.data() + 4 + i * ((raw & 0x80000000u) != 0 ? 2 : 1);
                saveMapPath.push_back(static_cast<char>(*q));
            }
        }
    }

    ParseEntityFile(blob);
    if (isSaveForm) {
        return true;
    }
    return ParseMapManager(blob, errorOut);
}

// The world stream is a flat run of tagged blocks and this walks every
// "<tag>\0<decimal>\0" pattern in it.  World::LoadWorldSections dispatches the
// top-level ones by name in a fixed order; the scan is a superset of that (it
// also finds the per-record <tile>, <region>, <zar> and <esh> tags), which is
// what makes it useful for --info.
void World::ScanSections(const std::vector<uint8_t>& blob) {
    std::map<std::string, SectionInfo> found;
    for (size_t i = 0; i + 3 < blob.size(); ++i) {
        if (blob[i] != '<') {
            continue;
        }
        size_t j = i + 1;
        while (j < blob.size() && IsTagChar(blob[j])) {
            ++j;
        }
        if (j == i + 1 || j >= blob.size() || blob[j] != '>' || j + 1 >= blob.size() ||
            blob[j + 1] != 0) {
            continue;
        }
        size_t k = j + 2;
        int32_t ver = 0;
        size_t digits = 0;
        while (k < blob.size() && blob[k] >= '0' && blob[k] <= '9') {
            ver = ver * 10 + (blob[k] - '0');
            ++k;
            ++digits;
        }
        if (digits == 0 || k >= blob.size() || blob[k] != 0) {
            continue;
        }
        const std::string name(reinterpret_cast<const char*>(blob.data() + i), j + 1 - i);
        auto it = found.find(name);
        if (it == found.end()) {
            SectionInfo s;
            s.tag = name;
            s.firstVersion = ver;
            s.firstOffset = i;
            s.count = 1;
            found.emplace(name, s);
        } else {
            ++it->second.count;
        }
        i = k;
    }
    sections.clear();
    for (auto& kv : found) {
        sections.push_back(kv.second);
    }
    std::sort(sections.begin(), sections.end(),
              [](const SectionInfo& a, const SectionInfo& b) { return a.firstOffset < b.firstOffset; });
}

// World::ReadEntityFileRecordsFromStream: token "<entity_file>", then an int32
// count and that many strings (the component class names).  The entity records
// themselves follow as "<esh>" blocks; milestone 3 will read them properly, so
// for now they are only counted.
// World::ReadEntityFileRecordsFromStream @004e52d0.  The section is found by
// scanning for its tag rather than by walking the sections in order, exactly as
// ParseMapManager does: World::LoadWorldSections is not in the supplied
// decompilation, so the order of the top-level sections is not established.
void World::ParseEntityFile(const std::vector<uint8_t>& blob) {
    static const char kTag[] = "<entity_file>";
    const size_t tagLen = sizeof(kTag);  // includes the NUL
    for (size_t i = 0; i + tagLen < blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, kTag, tagLen) == 0) {
            Reader r(blob.data(), blob.size());
            r.Seek(i);
            std::string err;
            if (!entityFile.Read(r, &err)) {
                entityFile.error = err;
            } else {
                entityFileOffset = i;
                entityFileEnd = r.Pos();
                worldPrefix.assign(blob.begin(), blob.begin() + static_cast<long>(i));
                worldTail.assign(blob.begin() + static_cast<long>(r.Pos()), blob.end());
            }
            entityClassCount = static_cast<int32_t>(entityFile.classNames.size());
            break;
        }
    }
    for (const SectionInfo& s : sections) {
        if (s.tag == "<esh>") {
            entityRecordCount = s.count;
        }
    }
}

// ---------------------------------------------------------------------------
// ReadMapManagerSection @ 006ed930, version 50 branch (the only one the
// shipped maps use; 0 < v < 0x33 is accepted, and v < 0x32 delegates to
// DeserializeMapFromStream, which is not ported).
//
//     readBytes(this+0x5d, 12)     3 floats
//     readBytes(this+0x69, 12)     3 floats
//     readBytes(tmp,       12)     3 floats
//     readBytes(cullRect,  24)     6 int32   -> SetCullBoxFromIntRect
//     protoCount = readUInt32()
//     names.resize(protoCount); for i: names[i] = readString()
//     tiles.resize(protoCount); tiles[0] = new Tile()          // no header read
//     for i in [1, protoCount): tiles[i] = new Tile(); tiles[i]->ReadTileFileHeader()
//     ... (a Sprite is constructed and pushed on a deque -- no stream reads) ...
//     totalInstances = readUInt32()                            // progress only
//     readBytes(this+0x1a6, 24)    6 int32 world box -> InitRenderClipRegion
//     regionCount = readUInt32()
//     for i in [0, regionCount): MapRegion::LoadRegionFromStream()
//
// Note names[0] is the empty string and tiles[0] has no <tile> record: slot 0
// is the "no tile" prototype an instance refers to with protoId == 0.
// ---------------------------------------------------------------------------
bool World::ParseMapManager(const std::vector<uint8_t>& blob, std::string* errorOut) {
    static const char kTag[] = "<mapManager>";
    size_t at = std::string::npos;
    for (size_t i = 0; i + sizeof(kTag) < blob.size(); ++i) {
        if (std::memcmp(blob.data() + i, kTag, sizeof(kTag)) == 0) {
            at = i;
            break;
        }
    }
    if (at == std::string::npos) {
        *errorOut = "no <mapManager> section";
        return false;
    }

    Reader r(blob.data(), blob.size());
    r.Seek(at);
    std::string tag;
    int32_t v = 0;
    r.FormatToken(&tag, &v);
    mapManagerVersion = v;
    if (v <= 0 || v >= 0x33) {
        *errorOut = "Unknown tile file type: <mapManager> version " + std::to_string(v);
        return false;
    }
    if (v < 0x32) {
        *errorOut = "<mapManager> version " + std::to_string(v) +
                    " uses the pre-50 DeserializeMapFromStream path, not ported";
        return false;
    }

    for (int i = 0; i < 3; ++i) {
        uint32_t bits = r.U32();
        std::memcpy(&vec1[i], &bits, 4);
    }
    for (int i = 0; i < 3; ++i) {
        uint32_t bits = r.U32();
        std::memcpy(&vec2[i], &bits, 4);
    }
    for (int i = 0; i < 3; ++i) {
        uint32_t bits = r.U32();
        std::memcpy(&vec3[i], &bits, 4);
    }
    for (int i = 0; i < 6; ++i) {
        cullBox[i] = r.I32();
    }

    const int32_t protoCount = r.I32();
    if (r.Failed() || protoCount < 1 || protoCount > 100000) {
        *errorOut = "bad prototype count";
        return false;
    }
    protos.resize(static_cast<size_t>(protoCount));
    for (int32_t i = 0; i < protoCount; ++i) {
        protos[static_cast<size_t>(i)].name = r.LengthPrefixedString();
        if (r.Failed()) {
            *errorOut = "truncated prototype name table";
            return false;
        }
    }
    // Slot 0 gets a default-constructed Tile and no <tile> record.
    for (int32_t i = 1; i < protoCount; ++i) {
        std::string t;
        int32_t tv = 0;
        r.FormatToken(&t, &tv);
        if (t != "<tile>" || tv != 10) {
            *errorOut = "prototype " + std::to_string(i) + ": expected <tile> 10, got '" + t +
                        "' " + std::to_string(tv);
            return false;
        }
        if (!protos[static_cast<size_t>(i)].header.ReadV10Payload(r)) {
            *errorOut = "truncated prototype record";
            return false;
        }
    }

    declaredInstances = r.I32();
    for (int i = 0; i < 6; ++i) {
        worldBox[i] = r.I32();
    }
    regionCount = r.I32();
    if (r.Failed() || regionCount < 0 || regionCount > 1000000) {
        *errorOut = "bad region count";
        return false;
    }

    instances.clear();
    instances.reserve(static_cast<size_t>(declaredInstances > 0 ? declaredInstances : 0));
    for (int32_t region = 0; region < regionCount; ++region) {
        std::string t;
        int32_t rv = 0;
        r.FormatToken(&t, &rv);
        if (t != "<region>" || rv <= 0 || rv >= 9) {
            *errorOut = "region " + std::to_string(region) + ": expected <region> 1..8, got '" + t +
                        "' " + std::to_string(rv);
            return false;
        }
        if (rv < 8) {
            *errorOut = "region " + std::to_string(region) + ": version " + std::to_string(rv) +
                        " uses LoadTileLightSourceListFromStream, not ported";
            return false;
        }
        const int32_t n = r.I32();
        if (r.Failed() || n < 0) {
            *errorOut = "bad region element count";
            return false;
        }
        for (int32_t i = 0; i < n; ++i) {
            TileInstance inst;
            inst.region = region;
            inst.protoId = r.I16();
            inst.flags = static_cast<uint16_t>(r.I16());
            for (int k = 0; k < 3; ++k) {
                inst.min[k] = r.I32();
            }
            for (int k = 0; k < 3; ++k) {
                inst.max[k] = r.I32();
            }
            for (int k = 0; k < 4; ++k) {
                inst.rect[k] = r.I32();
            }
            for (int k = 0; k < 3; ++k) {
                inst.extra[k] = r.I32();
            }
            if (r.Failed()) {
                *errorOut = "truncated region " + std::to_string(region);
                return false;
            }
            if (inst.protoId < 0 || inst.protoId >= protoCount) {
                *errorOut = "region " + std::to_string(region) + ": prototype id " +
                            std::to_string(inst.protoId) + " out of range";
                return false;
            }
            protos[static_cast<size_t>(inst.protoId)].instanceCount++;
            instances.push_back(inst);
        }
    }
    return true;
}


// ---------------------------------------------------------------------------
// Writing: `World::Save @004ea450`, the save-game branch.
// ---------------------------------------------------------------------------
namespace {

void PutU8(std::vector<uint8_t>* o, uint8_t v) { o->push_back(v); }
void PutU16(std::vector<uint8_t>* o, uint16_t v) {
    o->push_back(static_cast<uint8_t>(v & 0xFF));
    o->push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}
void PutU32(std::vector<uint8_t>* o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o->push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void PutF32(std::vector<uint8_t>* o, float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    PutU32(o, u);
}
void PutToken(std::vector<uint8_t>* o, const char* tag, int32_t version) {
    for (const char* p = tag; *p != 0; ++p) o->push_back(static_cast<uint8_t>(*p));
    o->push_back(0);
    const std::string v = std::to_string(version);
    for (char c : v) o->push_back(static_cast<uint8_t>(c));
    o->push_back(0);
}

bool Deflate(const std::vector<uint8_t>& in, std::vector<uint8_t>* out, std::string* errorOut) {
    uLongf bound = compressBound(static_cast<uLong>(in.size()));
    out->resize(bound);
    const int rc = compress2(out->data(), &bound, in.data(), static_cast<uLong>(in.size()), 9);
    if (rc != Z_OK) {
        *errorOut = "zlib compress error " + std::to_string(rc);
        return false;
    }
    out->resize(bound);
    return true;
}

}  // namespace

// `World::SaveWorldSectionProperties @004eb470`, in
// its own field order.  The offsets in the comments are World's; the sizes are
// the second argument to each CheckPropertySetAndFlagDirty call, and every one
// of them is confirmed against the shipped save byte for byte (NOTES).
std::vector<uint8_t> World::BuildSavePrefix(const std::string& mapPath, float gameTimeSec,
                                            int32_t tick, float realTimeSec,
                                            const std::vector<std::string>& stringTable,
                                            const std::vector<std::vector<std::string>>& groups) {
    std::vector<uint8_t> o;
    WriteTaggedString(&o, mapPath, true);

    PutToken(&o, "<sgd>", 5);
    PutF32(&o, gameTimeSec);        // +0x2e7  the world clock, seconds
    PutU32(&o, static_cast<uint32_t>(tick));  // +0x2eb
    PutU32(&o, 0);                  // +0x2ef
    PutU32(&o, 0);                  // +0x2f3
    PutU32(&o, 0);                  // +0x729
    PutF32(&o, realTimeSec);        // +0x2f7
    PutU32(&o, 0);                  // +0x2fb  (one 8-byte write with the next)
    PutU32(&o, 0);                  // +0x2ff
    PutU32(&o, 0);                  // +0x427
    PutU8(&o, 1);                   // +0x42b
    PutU16(&o, 0);                  // +0x43c
    PutF32(&o, 0.0f);               // +0x441
    PutF32(&o, 0.0f);               // +0x445
    PutU32(&o, 0);                  // +0x44d  (one 8-byte write with the next)
    PutU32(&o, 0);                  // +0x451
    PutU32(&o, 0);                  // the generic-pair table, always empty here
    // +0x4c, +0x50, +0x54: one 12-byte write.  MILESTONE 25: the second of
    // these three was SILENTLY COMMENTED OUT for two milestones -- the line
    // above it ended its `//` comment with a backslash, which splices the next
    // source line into the comment.  The `<sgd>` this wrote was 76 bytes where
    // the shipped one is 80, so every reader of our save desynchronised by four
    // bytes from here on.  -Wcomment had been warning about it all along.
    PutF32(&o, 0.0f);
    PutF32(&o, 0.0f);
    PutF32(&o, 0.0f);
    PutU8(&o, 0);                   // +0x1f4
    PutU32(&o, static_cast<uint32_t>(stringTable.size()));
    for (const std::string& s : stringTable) WriteTaggedString(&o, s, true);
    PutU32(&o, static_cast<uint32_t>(groups.size()));
    for (const std::vector<std::string>& g : groups) {
        PutU32(&o, static_cast<uint32_t>(g.size()));
        for (const std::string& s : g) WriteTaggedString(&o, s, true);
    }

    // `SerializeSSGProperties`: five words, the first three of which repeat the
    // <sgd> clock.
    PutToken(&o, "<SSG>", 1);
    PutF32(&o, gameTimeSec);
    PutF32(&o, 0.0f);
    PutF32(&o, realTimeSec);
    PutU32(&o, static_cast<uint32_t>(tick));
    PutU32(&o, static_cast<uint32_t>(tick));
    return o;
}

bool World::WriteSaveWorld(const std::string& outPath, const std::vector<uint8_t>& prefix,
                           std::string* errorOut) const {
    std::vector<uint8_t> file;
    if (!BuildSaveWorld(&file, prefix, errorOut)) return false;
    FILE* f = std::fopen(outPath.c_str(), "wb");
    if (f == nullptr) {
        *errorOut = "cannot open " + outPath + " for writing";
        return false;
    }
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    if (!ok) *errorOut = "short write to " + outPath;
    return ok;
}

bool World::BuildSaveWorld(std::vector<uint8_t>* out, const std::vector<uint8_t>& prefix,
                           std::string* errorOut) const {
    if (entityFileEnd == 0) {
        *errorOut = "no <entity_file> section was parsed; nothing to write";
        return false;
    }
    std::vector<uint8_t> blob = prefix.empty() ? worldPrefix : prefix;
    entityFile.Write(&blob);
    blob.insert(blob.end(), worldTail.begin(), worldTail.end());

    std::vector<uint8_t> z;
    if (!Deflate(blob, &z, errorOut)) return false;

    out->clear();
    // The `<saveh>` 2 block in front of the world, as `World::Save` writes it
    // for a mission blob: not a campaign save, the world's own name, four empty
    // strings, eight empty `<zar>`s and six zero floats.
    PutToken(out, "<saveh>", 2);
    PutU8(out, 0);
    WriteTaggedString(out, saveHeaderName, true);
    for (int i = 0; i < 4; ++i) WriteTaggedString(out, std::string(), true);
    for (int i = 0; i < 8; ++i) {
        PutToken(out, "<zar>", 4);
        PutU32(out, 0);
        PutU32(out, 0);
        PutU8(out, 0);
        PutU32(out, 0);
    }
    for (int i = 0; i < 6; ++i) PutF32(out, 0.0f);
    PutToken(out, "<world>", 70);
    PutU32(out, static_cast<uint32_t>(blob.size()));
    PutU32(out, static_cast<uint32_t>(blob.size()));
    out->insert(out->end(), z.begin(), z.end());
    return true;
}

}  // namespace ft
