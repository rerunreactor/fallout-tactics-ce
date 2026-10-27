// mis.h -- the .mis mission/world container.
//
//   World::Load                     @   -- outer file
//   ReadMapManagerSection           @ 006ed930  (phoenix/map/map_file.cpp)
//   MapRegion::LoadRegionFromStream @ (phoenix/map/map_region.cpp)
//   World::ReadEntityFileRecordsFromStream
//
// A .mis is "<world>" <version> <uncompressedSize> <uncompressedSize> followed
// by a raw zlib stream.  The inflated blob is a run of tagged sections, of
// which only <mapManager> is needed to draw the map.

#ifndef MAPVIEW_MIS_H
#define MAPVIEW_MIS_H

#include <cstdint>
#include <string>
#include <vector>

#include "ent.h"
#include "til.h"

namespace ft {

struct SectionInfo {
    std::string tag;
    int32_t firstVersion = 0;
    size_t firstOffset = 0;
    size_t count = 0;
};

struct TileProto {
    std::string name;         // "tiles/<dir>/<file>.til" as stored
    TileHeader header;        // the embedded version-10 <tile> record
    size_t instanceCount = 0;
};

// One 56-byte record of a <region> version 8 block.
struct TileInstance {
    int16_t protoId = 0;    // +0x00; 0 means "no tile"
    uint16_t flags = 0;     // +0x02
    int32_t min[3] = {0, 0, 0};  // +0x04  world (x, y, z), y is elevation
    int32_t max[3] = {0, 0, 0};  // +0x10
    int32_t rect[4] = {0, 0, 0, 0};  // +0x1c  cached screen rect (see NOTES)
    int32_t extra[3] = {0, 0, 0};    // +0x2c
    int32_t region = 0;
};

struct World {
    std::string path;
    int32_t version = 0;
    size_t rawSize = 0, inflatedSize = 0;

    std::vector<SectionInfo> sections;

    // A world written by `World::Save` (the save-game form, version 70) rather
    // than by the editor: it carries no map geometry at all.  In place of
    // <mapManager>/<tile>/<region> it opens with the length-prefixed path of the
    // `.mis` the map came from, then a <sgd> world-state block and a 20-byte
    // <SSG> block, and only then the ordinary <entity_file>.  See NOTES 116.
    bool isSaveForm = false;
    std::string saveMapPath;
    bool saveMapPathWide = false;
    // `World::Save` writes a `<saveh>` 2 block in front of the `<world>` token
    // when it is given a save-game header; for a
    // mission blob every field of it is empty except this one, which
    // `World::Save` l.18452 copies from World+0x2c9.  In the shipped save it is
    // `locale/missions/mission01/MIS_01_Speech.txt`.
    bool hasSaveHeader = false;
    std::string saveHeaderName;

    // The inflated stream, split at the <entity_file> section: everything
    // before it and everything after it, kept verbatim.  Only the entity
    // records carry runtime state that a save has to rewrite, so a save can be
    // produced by re-emitting `entityFile` between these two spans -- which is
    // exactly what `World::Save` does, and is what makes a byte-exact
    // re-emission of a real save possible.
    std::vector<uint8_t> worldPrefix, worldTail;
    size_t entityFileOffset = 0, entityFileEnd = 0;

    // <mapManager> version 50 preamble.
    int32_t mapManagerVersion = 0;
    float vec1[3] = {0, 0, 0}, vec2[3] = {0, 0, 0}, vec3[3] = {0, 0, 0};
    int32_t cullBox[6] = {0, 0, 0, 0, 0, 0};

    std::vector<TileProto> protos;   // protos[0] is the empty "no tile" slot
    int32_t declaredInstances = 0;
    int32_t worldBox[6] = {0, 0, 0, 0, 0, 0};  // min xyz, max xyz
    int32_t regionCount = 0;
    std::vector<TileInstance> instances;

    // <entity_file>
    int32_t entityClassCount = 0;
    size_t entityRecordCount = 0;  // number of <esh> blocks, nested ones included
    EntityFile entityFile;

    bool Load(const std::string& file, std::string* errorOut);
    // The same, on a file image already in memory -- which is what a `.sav`
    // container hands over.
    bool LoadFromMemory(const std::vector<uint8_t>& raw, const std::string& name,
                        std::string* errorOut);

    // `World::Save @004ea450` with a save-game header: "<world>" 70, the
    // inflated size twice, then the zlib stream.  `prefix` replaces
    // `worldPrefix`; pass an empty vector to reuse the one that was read.
    bool WriteSaveWorld(const std::string& outPath, const std::vector<uint8_t>& prefix,
                        std::string* errorOut) const;
    bool BuildSaveWorld(std::vector<uint8_t>* out, const std::vector<uint8_t>& prefix,
                        std::string* errorOut) const;

    // The save form's prefix: the source map's path as one tagged string, then
    // a <sgd> 5 block and a <SSG> 1 block.  `World::SaveWorldSectionProperties
    // @004eb470` is the <sgd> writer; see NOTES.
    static std::vector<uint8_t> BuildSavePrefix(const std::string& mapPath, float gameTimeSec,
                                                int32_t tick, float realTimeSec,
                                                const std::vector<std::string>& stringTable,
                                                const std::vector<std::vector<std::string>>& groups);

private:
    bool ParseMapManager(const std::vector<uint8_t>& blob, std::string* errorOut);
    void ScanSections(const std::vector<uint8_t>& blob);
    void ParseEntityFile(const std::vector<uint8_t>& blob);
};

}  // namespace ft

#endif  // MAPVIEW_MIS_H
