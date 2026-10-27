#include "trim.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "spr.h"

namespace ft {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void PutU32(std::vector<uint8_t>* out, uint32_t v) {
    out->push_back(static_cast<uint8_t>(v & 0xff));
    out->push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    out->push_back(static_cast<uint8_t>((v >> 16) & 0xff));
    out->push_back(static_cast<uint8_t>((v >> 24) & 0xff));
}

void PutI32At(std::vector<uint8_t>* out, size_t at, int32_t v) {
    const uint32_t u = static_cast<uint32_t>(v);
    (*out)[at] = static_cast<uint8_t>(u & 0xff);
    (*out)[at + 1] = static_cast<uint8_t>((u >> 8) & 0xff);
    (*out)[at + 2] = static_cast<uint8_t>((u >> 16) & 0xff);
    (*out)[at + 3] = static_cast<uint8_t>((u >> 24) & 0xff);
}

// A format token is a NUL-terminated tag then a NUL-terminated decimal version
// (stream.h `FormatToken`). `<spranim>` is always version 1 -- the reader's
// guard is `0 < v < 2`.
void PutToken(std::vector<uint8_t>* out, const char* tag, const char* version) {
    out->insert(out->end(), tag, tag + std::strlen(tag) + 1);
    out->insert(out->end(), version, version + std::strlen(version) + 1);
}

void PutLengthPrefixed(std::vector<uint8_t>* out, const std::string& s) {
    PutU32(out, static_cast<uint32_t>(s.size()));
    out->insert(out->end(), s.begin(), s.end());
}

// The `<spranim_img>` block of collection `n` runs from its own offset to the
// next collection offset that is strictly greater -- which is exactly the rule
// `Sprite::LoadCollectionImages` applies, generalised over the (rare) case of
// two collections sharing an offset.
void BlockExtent(const Sprite& spr, size_t n, size_t fileSize, size_t* begin, size_t* end) {
    const int32_t off = spr.collections[n].offset;
    *begin = static_cast<size_t>(off < 0 ? 0 : off);
    size_t best = fileSize;
    for (const Collection& c : spr.collections) {
        if (c.offset > off && static_cast<size_t>(c.offset) < best) {
            best = static_cast<size_t>(c.offset);
        }
    }
    *end = std::max(best, *begin);
}

}  // namespace

bool TrimSprite(const std::vector<uint8_t>& in, const TrimOptions& opts,
                std::vector<uint8_t>* out, TrimStats* stats, std::string* errorOut) {
    Sprite spr;
    if (!spr.Load(in, /*loadImages=*/false, errorOut)) return false;
    if (spr.collectionTableStart == 0 || spr.collectionTableStart > in.size()) {
        *errorOut = "collection table offset not recorded";
        return false;
    }

    // --- which collections survive -------------------------------------------
    const size_t n = spr.collections.size();
    std::vector<bool> referenced(n, false);
    std::vector<bool> keep(n, false);
    for (const Sequence& seq : spr.sequences) {
        if (seq.collectionId < 0 || static_cast<size_t>(seq.collectionId) >= n) continue;
        const size_t c = static_cast<size_t>(seq.collectionId);
        referenced[c] = true;
        if (opts.keepSequences.empty() ||
            opts.keepSequences.count(Lower(seq.name)) != 0) {
            keep[c] = true;
        }
    }
    for (size_t i = 0; i < n; ++i) {
        // A collection with no images was never carrying anything to drop.
        if (spr.collections[i].frameCount == 0 || spr.collections[i].dirCount == 0) {
            keep[i] = false;
            continue;
        }
        if (!referenced[i] && opts.keepUnreferenced) keep[i] = true;
    }

    // --- the header, the sequence table and the collection count, verbatim ----
    out->clear();
    out->reserve(in.size() / 4 + 1024);
    out->insert(out->end(), in.begin(), in.begin() + spr.collectionTableStart);

    // --- the `<spranim>` headers ---------------------------------------------
    // Their sizes are known now; the image-block offsets are not, so each
    // offset field is written as zero and patched once the headers are laid out.
    std::vector<size_t> offsetField(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const Collection& col = spr.collections[i];
        PutToken(out, "<spranim>", "1");
        offsetField[i] = out->size();
        PutU32(out, 0);
        PutLengthPrefixed(out, col.name);
        if (!keep[i]) {
            // A zero frame count ends the record: no direction count, no rects.
            PutU32(out, 0);
            continue;
        }
        PutU32(out, static_cast<uint32_t>(col.frameCount));
        PutU32(out, static_cast<uint32_t>(col.dirCount));
        for (const Rect& r : col.rects) {
            PutU32(out, static_cast<uint32_t>(r.x0));
            PutU32(out, static_cast<uint32_t>(r.y0));
            PutU32(out, static_cast<uint32_t>(r.x1));
            PutU32(out, static_cast<uint32_t>(r.y1));
        }
    }

    // --- the kept image blocks, byte for byte --------------------------------
    // A dropped collection still gets an offset: it is the cursor at its turn,
    // which keeps the table non-decreasing so the "next greater offset" rule
    // still bounds the preceding kept block correctly.
    size_t kept = 0;
    for (size_t i = 0; i < n; ++i) {
        PutI32At(out, offsetField[i], static_cast<int32_t>(out->size()));
        if (!keep[i]) continue;
        size_t begin = 0, end = 0;
        BlockExtent(spr, i, in.size(), &begin, &end);
        if (begin >= in.size() || end > in.size() || end < begin) {
            *errorOut = "collection " + std::to_string(i) + " (" + spr.collections[i].name +
                        "): image block out of range";
            return false;
        }
        out->insert(out->end(), in.begin() + begin, in.begin() + end);
        ++kept;
    }

    if (stats != nullptr) {
        stats->collections = n;
        stats->collectionsKept = kept;
        stats->bytesIn = in.size();
        stats->bytesOut = out->size();
    }
    return true;
}

bool VerifyTrim(const std::vector<uint8_t>& original, const std::vector<uint8_t>& trimmed,
                std::string* errorOut) {
    Sprite a, b;
    if (!a.Load(original, false, errorOut)) {
        *errorOut = "original: " + *errorOut;
        return false;
    }
    if (!b.Load(trimmed, false, errorOut)) {
        *errorOut = "trimmed: " + *errorOut;
        return false;
    }
    if (a.version != b.version || a.bboxX != b.bboxX || a.bboxY != b.bboxY ||
        a.bboxZ != b.bboxZ || a.offsetX != b.offsetX || a.offsetY != b.offsetY ||
        a.header != b.header) {
        *errorOut = "sprite header changed";
        return false;
    }
    if (a.sequences.size() != b.sequences.size()) {
        *errorOut = "sequence count changed";
        return false;
    }
    for (size_t i = 0; i < a.sequences.size(); ++i) {
        if (a.sequences[i].name != b.sequences[i].name ||
            a.sequences[i].collectionId != b.sequences[i].collectionId ||
            a.sequences[i].frames.size() != b.sequences[i].frames.size()) {
            *errorOut = "sequence " + std::to_string(i) + " (" + a.sequences[i].name +
                        ") changed";
            return false;
        }
    }
    if (a.collections.size() != b.collections.size()) {
        *errorOut = "collection count changed -- every Sequence::collectionId after the "
                    "first drop would now point at the wrong animation";
        return false;
    }

    for (size_t i = 0; i < b.collections.size(); ++i) {
        const Collection& kb = b.collections[i];
        const Collection& ka = a.collections[i];
        if (kb.name != ka.name) {
            *errorOut = "collection " + std::to_string(i) + ": name changed";
            return false;
        }
        if (kb.frameCount == 0) continue;  // dropped on purpose
        if (kb.frameCount != ka.frameCount || kb.dirCount != ka.dirCount ||
            kb.rects.size() != ka.rects.size()) {
            *errorOut = "collection " + kb.name + ": geometry changed";
            return false;
        }
        for (size_t r = 0; r < kb.rects.size(); ++r) {
            if (kb.rects[r].x0 != ka.rects[r].x0 || kb.rects[r].y0 != ka.rects[r].y0 ||
                kb.rects[r].x1 != ka.rects[r].x1 || kb.rects[r].y1 != ka.rects[r].y1) {
                *errorOut = "collection " + kb.name + ": rect " + std::to_string(r) +
                            " differs";
                return false;
            }
        }
        // The images too, decoded through the collection's own palettes.
        Sprite& ma = a;
        Sprite& mb = b;
        if (!ma.LoadCollectionImages(original, i) || !mb.LoadCollectionImages(trimmed, i)) {
            *errorOut = "collection " + kb.name + ": images would not decode (" +
                        (ma.collections[i].loadError.empty() ? mb.collections[i].loadError
                                                             : ma.collections[i].loadError) +
                        ")";
            return false;
        }
        const Collection& ia = ma.collections[i];
        const Collection& ib = mb.collections[i];
        for (int l = 0; l < kLayerCount; ++l) {
            if (ia.palettes[static_cast<size_t>(l)] != ib.palettes[static_cast<size_t>(l)]) {
                *errorOut = "collection " + kb.name + ": layer " + std::to_string(l) +
                            " palette differs";
                return false;
            }
        }
        if (ia.images.size() != ib.images.size()) {
            *errorOut = "collection " + kb.name + ": image count differs";
            return false;
        }
        for (size_t k = 0; k < ia.images.size(); ++k) {
            const LayerImage& xa = ia.images[k];
            const LayerImage& xb = ib.images[k];
            if (xa.present != xb.present || xa.x != xb.x || xa.y != xb.y) {
                *errorOut = "collection " + kb.name + ": image " + std::to_string(k) +
                            " placement differs";
                return false;
            }
            if (!xa.present) continue;
            const size_t layer = k % kLayerCount;
            const std::vector<Color32>& pal =
                ia.imgType == '1' ? ia.palettes[layer]
                                  : ia.palettes[(k / (static_cast<size_t>(ia.frameCount) *
                                                      static_cast<size_t>(ia.dirCount))) %
                                                kLayerCount];
            std::vector<uint8_t> ra, rb;
            std::string e1, e2;
            ZarImage za = xa.zar, zb = xb.zar;
            if (!za.DecodeToRGBA(pal, &ra, &e1) || !zb.DecodeToRGBA(pal, &rb, &e2)) {
                *errorOut = "collection " + kb.name + ": image " + std::to_string(k) +
                            " will not decode (" + e1 + e2 + ")";
                return false;
            }
            if (ra != rb) {
                *errorOut = "collection " + kb.name + ": image " + std::to_string(k) +
                            " pixels differ";
                return false;
            }
        }
        // Decoded images are the expensive part; do not hold two copies of a
        // 25 MB sprite's worth of them while walking the rest.
        ma.collections[i].images.clear();
        ma.collections[i].imagesLoaded = false;
        mb.collections[i].images.clear();
        mb.collections[i].imagesLoaded = false;
    }
    return true;
}

}  // namespace ft
