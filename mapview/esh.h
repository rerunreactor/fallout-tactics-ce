// esh.h -- the "<esh>" tagged-property block, the entity layer's serialisation
// format.  Used by `.ent` prototype files and by the `<entity_file>` section of
// a `.mis`.
//
// Sources, all in the Ghidra export:
//
//   EntityStreamStruct::InitializeEntityStreamHandle @005f2ea0 (l.17383) -- the
//     writer: it emits the "<esh>" token, a u32 placeholder, calls the virtual
//     Serialize, then seeks back and backpatches the placeholder with the
//     *record counter* at stream+0x3b (l.17441-17449).  So the u32 after the
//     token is a record COUNT, not a byte size.
//   EntityStreamStruct::OpenEmbeddedStreamForEntity @005f2de0 (l.17347) -- the
//     read entry point: mode 2, ParseEshTableSection, then Serialize.
//   EntityStreamWriteInteger @005f6??? mode 3 (l.20206-20264) -- the record
//     header: name (length-prefixed string), u32 typeId, u32 payloadSize, then
//     payloadSize bytes.  EntityStreamWriteBoolean (l.18096-18157) and
//     EntityStreamWriteStringType agree field for field.
//   EntityStreamReadString @006f1ff0 (l.21333) -- i32 length then exactly
//     `length` bytes, no NUL on the wire; the same helper writes property
//     *names*, so one codec covers both.
//   SerializeTaggedPropertyRecord @005f1880 (l.16967) -- nesting: type 0x0b's
//     payload is `i32 innerLen` + a complete "<esh>" document, and
//     payloadSize == innerLen + 4 (l.17233-17240).
//
// Every record carries its own size, so an unknown type is skipped, not fatal
// -- which is what the original does too (l.20136: on a type mismatch it seeks
// to the record's end and substitutes the property's compile-time default).

#ifndef MAPVIEW_ESH_H
#define MAPVIEW_ESH_H

#include <cstdint>
#include <string>
#include <vector>

#include "stream.h"

namespace ft {

// Type ids, from the size-computation switch in EntityStreamWriteBoolean
// (l.18118-18153) and EntityStreamWriteInteger (l.20223-20261).  The names for
// 13 and 14 are this port's: the decompilation only says "sizeof(T)" for them,
// and the shipped data settles the sizes (48 and 16 bytes).
enum EshType : uint32_t {
    kEshBool = 1,         // 1 byte
    kEshFloat = 2,        // 4 bytes
    kEshInt = 3,          // 4 bytes
    kEshString = 4,       // i32 len + len bytes.  Also used for nested <entity> blobs.
    kEshVec3 = 5,         // 12 bytes, 3 floats
    kEshColour = 6,       // 20 bytes, 5 floats
    kEshStringList = 7,   // vector<string>
    kEshString8 = 8,      // string, "resource path" flavour (Sprite, EntPath)
    kEshString9 = 9,      // string, "enum/picker" flavour (Race Type, Item Type)
    kEshString10 = 10,    // string
    kEshNested = 11,      // i32 len + a complete "<esh>" document
    kEshEntityRef = 12,   // 4 bytes; on the wire an i32 length prefix, usually 0
    kEshFrame = 13,       // 48 bytes: 3x3 orientation matrix then the position
    kEshRect = 14,        // 16 bytes, 4 int32
    kEshIntArray = 15,    // u32 count then count*4 bytes
    kEshString16 = 16,    // string
    kEshEntityId = 17,    // u16
    kEshEntityHandle = 18,// u32 + u8
    kEshString19 = 19,    // string
    kEshStringPair = 20,  // two length-prefixed strings
    kEshString21 = 21,    // string
    kEshString25 = 25,    // string
    kEshString26 = 26,    // string
};

// The nine ids the decompilation groups together as "strlen + 4"
// (`EntityStreamWriteBoolean@005f4760`).  Type 4 is in the group but is also the
// carrier for nested "<entity>" blobs, so callers must check the payload.
bool EshTypeIsString(uint32_t type);

struct EshProp {
    std::string name;
    uint32_t type = 0;
    std::vector<uint8_t> payload;
    // Whether the name was stored in the 16-bit form (bit 31 of the length).
    // Kept so a record can be written back exactly as it was read; a world
    // written by `World::Save` uses the wide form throughout, an editor-written
    // `.mis` the narrow one.
    bool nameWide = false;
};

// The tagged length-prefixed string, written the way it was read.
void WriteTaggedString(std::vector<uint8_t>* out, const std::string& s, bool wide);

struct Esh {
    int32_t version = 0;
    std::vector<EshProp> props;

    // The original keys a std::map on the exact bytes and then filters on the
    // type id (l.20112-20129), treating "present but wrong type" the same as
    // absent.  `Find` reproduces the name lookup; the typed accessors below
    // reproduce the type filter.  The case-insensitive retry is this port's:
    // the comparison goes through CompareFOTStringRanges with locale arguments
    // that could not be settled, and nothing in the shipped data needs it.
    const EshProp* Find(const std::string& name) const;

    bool GetString(const std::string& name, std::string* out) const;
    bool GetInt(const std::string& name, int32_t* out) const;
    bool GetFloat(const std::string& name, float* out) const;
    bool GetBool(const std::string& name, bool* out) const;
    bool GetColour(const std::string& name, float out[5]) const;   // type 6
    bool GetVec3(const std::string& name, float out[3]) const;      // type 5
    bool GetFrame(const std::string& name, float out[12]) const;   // type 13
    bool GetRect(const std::string& name, int32_t out[4]) const;   // type 14
    bool GetNested(const std::string& name, Esh* out, std::string* errorOut) const;

    // The setters a save needs.  Each rewrites an EXISTING record's payload in
    // place, keeping its name, type and position, and returns false when the
    // property is absent or of the wrong type -- a save must not invent
    // properties the original's serializers would not expect.
    EshProp* FindMut(const std::string& name);
    bool SetInt(const std::string& name, int32_t v);
    bool SetFloat(const std::string& name, float v);
    bool SetFrame(const std::string& name, const float v[12]);   // type 13
    bool SetNested(const std::string& name, const Esh& inner);   // type 11

    // The same, but appending the record when it is absent, which is what a
    // squad member built out of a `.ent` prototype needs: the prototype carries
    // no `frame`, `EntPath` or `Player Index` -- those are instance properties
    // the map editor writes and a spawned entity has to grow.
    void PutInt(const std::string& name, int32_t v);
    void PutFrame(const std::string& name, const float v[12]);
    void PutString(const std::string& name, const std::string& v, uint32_t type = 4);

    // Reads the "<esh>" token and the record run.
    bool Read(Reader& stream, std::string* errorOut);

    // The exact inverse: `EntityStreamStruct::InitializeEntityStreamHandle
    // @005f2ea0` emits the token, the record count, then each record as
    // name / u32 type / u32 payloadSize / payload.  Re-emitting a block that
    // was read here reproduces its bytes exactly.
    void Write(std::vector<uint8_t>* out) const;
};

// An "<entity>" version-2 block: the class name then one <esh>.  This is what a
// `.ent` file is, and what type-4 properties like `Attached` / `Inventory` /
// `unarmedSlot1` carry as their payload.
struct EntityBlock {
    int32_t version = 0;
    std::string className;
    Esh esh;

    bool Read(Reader& stream, std::string* errorOut);
};

}  // namespace ft

#endif  // MAPVIEW_ESH_H
