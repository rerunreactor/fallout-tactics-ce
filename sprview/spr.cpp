#include "spr.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>

namespace ft {
namespace {

// Sprite::Frame::paramCount.  Event ids are negative and sign-extended from the
// low byte of the table entry; the events that carry parameters are
// ev_stop_anim (-2), ev_time_of_display (-3), ev_jump_to_frame (-5) with one
// each, and ev_fire (-43) with three.
int ParamCount(int16_t rawId) {
    const int8_t id = static_cast<int8_t>(rawId & 0xff);
    if (id == -2 || id == -5 || id == -3) {
        return 1;
    }
    if (id == -43) {
        return 3;
    }
    return 0;
}

// The "<spranim_img>" type-2 payload is a gzip member.  windowBits 47 = 15 with
// the +32 "detect zlib or gzip header" flag.
bool GunzipInto(const uint8_t* src, size_t srcSize, size_t expected,
                std::vector<uint8_t>* out, std::string* errorOut) {
    z_stream zs{};
    if (inflateInit2(&zs, 47) != Z_OK) {
        *errorOut = "inflateInit2 failed";
        return false;
    }
    out->assign(expected != 0 ? expected : srcSize * 4, 0);
    zs.next_in = const_cast<Bytef*>(src);
    zs.avail_in = static_cast<uInt>(srcSize);
    zs.next_out = out->data();
    zs.avail_out = static_cast<uInt>(out->size());

    for (;;) {
        const int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            break;
        }
        if (rc != Z_OK) {
            inflateEnd(&zs);
            *errorOut = "inflate failed: " + std::to_string(rc);
            return false;
        }
        if (zs.avail_out == 0) {
            const size_t used = out->size();
            out->resize(used * 2);
            zs.next_out = out->data() + used;
            zs.avail_out = static_cast<uInt>(out->size() - used);
        }
    }
    out->resize(zs.total_out);
    inflateEnd(&zs);
    return true;
}

bool g_usageLog = false;
std::vector<std::string> g_usage;

}  // namespace

void SprSetUsageLog(bool on) { g_usageLog = on; }

std::vector<std::string> SprTakeUsageLog() {
    std::sort(g_usage.begin(), g_usage.end());
    g_usage.erase(std::unique(g_usage.begin(), g_usage.end()), g_usage.end());
    std::vector<std::string> out;
    out.swap(g_usage);
    return out;
}


// ---------------------------------------------------------------------------
// SpriteAnimation::LoadSpriteAnimationsFromStream @ 0070af30
// ---------------------------------------------------------------------------
bool Sprite::Load(const std::vector<uint8_t>& file, bool loadImages, std::string* errorOut) {
    Reader s(file);

    std::string tag;
    s.FormatToken(&tag, &version);
    // `(tag == "<sprite>") && 3 < version && version < 5`, i.e. version == 4.
    if (tag != "<sprite>" || version <= 3 || version >= 5) {
        *errorOut = "not a <sprite> v4 stream (tag='" + tag + "', version=" +
                    std::to_string(version) + ")";
        return false;
    }

    // Three contiguous reads at +0x0c (3 bytes), +0x0f (8 bytes) and +0x17/+0x19
    // (2 + 1 bytes) -- 14 bytes in all.
    // Read in file order; the assignment is (x, y, z) -- see spr.h.
    bboxX = s.U8();
    bboxY = s.U8();
    bboxZ = s.U8();
    offsetX = s.I32();
    offsetY = s.I32();
    header[0] = s.U8();
    header[1] = s.U8();
    header[2] = s.U8();

    const int32_t sequenceCount = s.I32();
    if (s.Failed() || sequenceCount < 0 || static_cast<size_t>(sequenceCount) > file.size()) {
        *errorOut = "bad sequence count";
        return false;
    }
    sequences.resize(static_cast<size_t>(sequenceCount));

    // ReadEntityShortAndIntArrayStreams, called `sequenceCount` times.
    for (Sequence& seq : sequences) {
        const int16_t rawFrameCount = s.I16();
        s.I16();  // unused second short (always 0 in the shipped data)
        if (s.Failed() || rawFrameCount < 0) {
            *errorOut = "bad frame count";
            return false;
        }
        seq.rawFrameCount = rawFrameCount;

        std::vector<int16_t> table(static_cast<size_t>(rawFrameCount));
        for (int16_t& v : table) {
            v = s.I16();
        }
        seq.frameData2.resize(static_cast<size_t>(rawFrameCount));
        for (int32_t& v : seq.frameData2) {
            v = s.I32();
        }
        seq.name = s.LengthPrefixedString();
        seq.collectionId = s.I16();
        if (s.Failed()) {
            *errorOut = "truncated sequence table: " + s.Error();
            return false;
        }

        for (int32_t f = 0; f < rawFrameCount; ++f) {
            Frame frame;
            frame.id = table[static_cast<size_t>(f)];
            const int params = ParamCount(frame.id);
            for (int p = 0; p < params && f + p + 1 < rawFrameCount; ++p) {
                frame.params[static_cast<size_t>(p)] = table[static_cast<size_t>(f + p + 1)];
            }
            f += params;
            seq.frames.push_back(frame);
        }
    }

    const int32_t collectionCount = s.I32();
    if (s.Failed() || collectionCount < 0 || static_cast<size_t>(collectionCount) > file.size()) {
        *errorOut = "bad collection count";
        return false;
    }
    collections.resize(static_cast<size_t>(collectionCount));
    collectionTableStart = s.Pos();

    // ParseSpriteAnimationBlock, called `collectionCount` times.
    for (Collection& col : collections) {
        std::string blockTag;
        int32_t blockVersion = 0;
        s.FormatToken(&blockTag, &blockVersion);
        // `(tag == "<spranim>") && 0 < version && version < 2`.
        if (blockTag != "<spranim>" || blockVersion <= 0 || blockVersion >= 2) {
            *errorOut = "bad <spranim> block (tag='" + blockTag + "')";
            return false;
        }
        col.offset = s.I32();          // +0x20c
        col.name = s.LengthPrefixedString();  // +0x214
        col.frameCount = s.I32();
        if (s.Failed()) {
            *errorOut = "truncated <spranim>: " + s.Error();
            return false;
        }
        if (col.frameCount == 0) {
            // The original returns immediately when the frame count is zero,
            // without reading a direction count.
            continue;
        }
        col.dirCount = s.I32();
        if (s.Failed() || col.frameCount < 0 || col.dirCount < 0) {
            *errorOut = "bad <spranim> counts";
            return false;
        }
        const int64_t rectCount = static_cast<int64_t>(col.frameCount) * col.dirCount;
        if (rectCount < 0 || static_cast<uint64_t>(rectCount) * 16 > file.size()) {
            *errorOut = "implausible <spranim> rect count";
            return false;
        }
        col.rects.resize(static_cast<size_t>(rectCount));
        for (Rect& r : col.rects) {
            r.x0 = s.I32();
            r.y0 = s.I32();
            r.x1 = s.I32();
            r.y1 = s.I32();
            if (r.x1 - r.x0 < 0 || r.y1 - r.y0 < 0) {
                // FormatAndReportError("Sprite with bad screen extents") -- the
                // original reports and carries on.
                std::fprintf(stderr, "warning: %s: sprite with bad screen extents\n",
                             col.name.c_str());
            }
        }
        if (s.Failed()) {
            *errorOut = "truncated <spranim> rects";
            return false;
        }
    }

    if (!loadImages) {
        return true;
    }

    for (size_t n = 0; n < collections.size(); ++n) {
        LoadCollectionImages(file, n);
    }
    return true;
}

// One collection's "<spranim_img>" payload.  Split out of Sprite::Load so a
// caller that only needs one animation -- mapview draws a single idle frame per
// entity -- does not have to decode all 104 collections of a 20 MB character
// sprite.  The body is unchanged.
bool Sprite::LoadCollectionImages(const std::vector<uint8_t>& file, size_t n) {
    {
        if (n >= collections.size()) {
            return false;
        }
        Collection& col = collections[n];
        if (g_usageLog) {
            for (const Sequence& seq : sequences) {
                if (static_cast<size_t>(seq.collectionId) == n) g_usage.push_back(seq.name);
            }
        }
        if (col.imagesLoaded) {
            return true;
        }
        if (col.frameCount == 0 || col.dirCount == 0) {
            return false;
        }
        // The block extends to the next collection's offset, or to EOF.
        const size_t blockEnd =
            (n + 1 < collections.size() && collections[n + 1].offset > col.offset)
                ? static_cast<size_t>(collections[n + 1].offset)
                : file.size();
        if (col.offset < 0 || static_cast<size_t>(col.offset) >= blockEnd) {
            col.loadError = "collection offset out of range";
            return false;
        }

        Reader b(file);
        b.Seek(static_cast<size_t>(col.offset));
        std::string blockTag;
        int32_t imgVersion = 0;
        b.FormatToken(&blockTag, &imgVersion);
        if (blockTag != "<spranim_img>") {
            col.loadError = "bad <spranim_img> tag '" + blockTag + "'";
            return false;
        }
        // The token is "<spranim_img>\0" (14 bytes) plus "1\0" / "2\0", so the
        // 16-byte header FreeFT subtracts is tag + version + the two int32s that
        // the version-2 path reads next.  We track the cursor instead.
        col.imgType = imgVersion + '0';
        if (col.imgType != '1' && col.imgType != '2') {
            col.loadError = "unknown spranim_img type " + std::to_string(imgVersion);
            return false;
        }

        std::vector<uint8_t> payload;
        if (col.imgType == '1') {
            const size_t size = blockEnd - b.Pos();
            const uint8_t* p = b.Take(size);
            if (p == nullptr) {
                col.loadError = "truncated plain image block";
                return false;
            }
            payload.assign(p, p + size);
        } else {
            const uint32_t plainSize = b.U32();
            const size_t size = blockEnd - b.Pos();
            const uint8_t* p = b.Take(size);
            std::string gzErr;
            if (p == nullptr || !GunzipInto(p, size, plainSize, &payload, &gzErr)) {
                col.loadError = "gzip: " + gzErr;
                return false;
            }
            if (payload.size() != plainSize) {
                col.loadError = "gzip size mismatch (" + std::to_string(payload.size()) +
                                " vs " + std::to_string(plainSize) + ")";
                return false;
            }
        }

        Reader img(payload);
        bool bad = false;
        for (int l = 0; l < kLayerCount; ++l) {
            const int32_t paletteSize = img.I32();
            if (img.Failed() || paletteSize < 0 || paletteSize > 256) {
                col.loadError = "bad layer palette size";
                bad = true;
                break;
            }
            col.palettes[static_cast<size_t>(l)].resize(static_cast<size_t>(paletteSize));
            for (Color32& c : col.palettes[static_cast<size_t>(l)]) {
                c = img.U32();
            }
        }
        if (bad) {
            return false;
        }

        const size_t imageCount =
            static_cast<size_t>(col.frameCount) * col.dirCount * kLayerCount;
        col.images.resize(imageCount);
        for (size_t k = 0; k < imageCount; ++k) {
            const uint8_t entryType = img.U8();
            if (img.Failed()) {
                col.loadError = "truncated image list at " + std::to_string(k);
                break;
            }
            if (entryType == 0) {
                continue;  // empty image
            }
            if (entryType == '<') {
                break;     // early terminator
            }
            if (entryType != 1) {
                col.loadError = "unknown image entry type " + std::to_string(entryType) +
                                " at " + std::to_string(k);
                break;
            }
            LayerImage& li = col.images[k];
            li.x = img.I32();
            li.y = img.I32();
            std::string zarErr;
            if (!li.zar.Read(img, &zarErr)) {
                col.loadError = "image " + std::to_string(k) + ": " + zarErr;
                break;
            }
            li.present = true;
        }
        col.imagesLoaded = col.loadError.empty();
    }
    return true;
}

}  // namespace ft
