// main.cpp -- sprview: Fallout Tactics .spr sprite viewer / dumper.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "png.h"
#include "spr.h"
#include "zar.h"

namespace {

using namespace ft;

std::string Sanitize(const std::string& in) {
    std::string out;
    for (char c : in) {
        out.push_back((c == ' ' || c == '/' || c == '\\' || c == ':' || c == '*') ? '_' : c);
    }
    if (out.empty()) {
        out = "unnamed";
    }
    return out;
}

const char* EventName(int16_t rawId) {
    switch (static_cast<int8_t>(rawId & 0xff)) {
        case -2: return "stop_anim";
        case -3: return "time_of_display";
        case -4: return "repeat_all";
        case -5: return "jump_to_frame";
        case -6: return "overlay";
        case -40: return "step_left";
        case -41: return "step_right";
        case -42: return "hit";
        case -43: return "fire";
        case -44: return "sound";
        case -45: return "pickup";
        default: return "event";
    }
}

int Info(const std::string& path) {
    std::vector<uint8_t> file;
    std::string err;
    if (!ReadWholeFile(path, &file, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Sprite sprite;
    if (!sprite.Load(file, /*loadImages=*/true, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }

    std::printf("file            %s (%zu bytes)\n", path.c_str(), file.size());
    std::printf("tag             <sprite> version %d\n", sprite.version);
    std::printf("bbox (x,y,z)    %u, %u, %u\n", sprite.bboxX, sprite.bboxY, sprite.bboxZ);
    std::printf("offset          %d, %d\n", sprite.offsetX, sprite.offsetY);
    std::printf("header bytes    %02x %02x %02x\n", sprite.header[0], sprite.header[1],
                sprite.header[2]);
    std::printf("sequences       %zu\n", sprite.sequences.size());
    std::printf("collections     %zu\n\n", sprite.collections.size());

    std::printf("--- sequences ---\n");
    for (size_t i = 0; i < sprite.sequences.size(); ++i) {
        const Sequence& seq = sprite.sequences[i];
        std::printf("[%3zu] %-32s frames=%-4zu (table=%d) collection=%d\n", i, seq.name.c_str(),
                    seq.frames.size(), seq.rawFrameCount, seq.collectionId);
        for (size_t f = 0; f < seq.frames.size(); ++f) {
            const Frame& fr = seq.frames[f];
            if (fr.id >= 0) {
                std::printf("        frame %3zu: id=%d\n", f, fr.id);
            } else {
                std::printf("        frame %3zu: %s(%d) params=%d,%d,%d,%d\n", f,
                            EventName(fr.id), fr.id, fr.params[0], fr.params[1], fr.params[2],
                            fr.params[3]);
            }
        }
    }

    std::printf("\n--- collections ---\n");
    for (size_t i = 0; i < sprite.collections.size(); ++i) {
        const Collection& col = sprite.collections[i];
        size_t present = 0;
        for (const LayerImage& li : col.images) {
            present += li.present ? 1 : 0;
        }
        std::printf("[%3zu] %-28s off=%-9d type='%c' frames=%-4d dirs=%-3d images=%zu/%zu\n", i,
                    col.name.c_str(), col.offset, col.imgType ? col.imgType : '?', col.frameCount,
                    col.dirCount, present, col.images.size());
        std::printf("        layer palettes: ");
        for (int l = 0; l < kLayerCount; ++l) {
            std::printf("%zu%s", col.palettes[static_cast<size_t>(l)].size(),
                        l == kLayerCount - 1 ? "\n" : ", ");
        }
        if (!col.loadError.empty()) {
            std::printf("        LOAD ERROR: %s\n", col.loadError.c_str());
        }
        if (!col.rects.empty()) {
            const Rect& r = col.rects[0];
            std::printf("        rect[0] = (%d,%d)-(%d,%d)  %dx%d\n", r.x0, r.y0, r.x1, r.y1,
                        r.Width(), r.Height());
        }
    }
    return 0;
}

struct DumpStats {
    size_t images = 0;
    size_t rectExact = 0;
    size_t rectSlack = 0;
    size_t decoded = 0;
    size_t errors = 0;
    size_t rectMismatch = 0;
    size_t written = 0;
};

// Decodes every image of a collection, optionally writing PNGs.
void ProcessCollection(const Collection& col, const std::string* outDir, DumpStats* stats,
                       size_t* minNonzero, size_t* maxNonzero, double* sumNonzero,
                       size_t* sampleCount, std::string* firstPng) {
    for (int32_t frame = 0; frame < col.frameCount; ++frame) {
        for (int32_t dir = 0; dir < col.dirCount; ++dir) {
            const size_t rectIndex = static_cast<size_t>(frame) * col.dirCount + dir;
            const Rect* rect = rectIndex < col.rects.size() ? &col.rects[rectIndex] : nullptr;
            // The union of the frame's present layers, to compare against the
            // collection rect (GenerateSpriteAnimationZars @ 00712ae0 builds the
            // rect exactly this way).
            int32_t unionW = 0, unionH = 0;
            bool anyLayer = false;

            for (int32_t layer = 0; layer < kLayerCount; ++layer) {
                const int32_t index = col.LayerIndex(frame, dir, layer);
                if (index < 0 || static_cast<size_t>(index) >= col.images.size()) {
                    ++stats->errors;
                    continue;
                }
                const LayerImage& li = col.images[static_cast<size_t>(index)];
                if (!li.present) {
                    continue;
                }
                ++stats->images;

                std::vector<uint8_t> rgba;
                std::string err;
                ZarImage& zar = const_cast<ZarImage&>(li.zar);
                if (!zar.DecodeToRGBA(col.palettes[static_cast<size_t>(layer)], &rgba, &err)) {
                    ++stats->errors;
                    std::fprintf(stderr, "  decode error %s f%d d%d l%d: %s\n", col.name.c_str(),
                                 frame, dir, layer, err.c_str());
                    continue;
                }
                ++stats->decoded;

                // The frame's rect is the union of its four layers, each placed
                // at (x, y) relative to the rect origin (GenerateSpriteAnimationZars
                // @ 00712ae0).  So every layer must fit inside it.
                if (rect != nullptr &&
                    (li.x + zar.Width() > rect->Width() || li.y + zar.Height() > rect->Height() ||
                     li.x < 0 || li.y < 0)) {
                    ++stats->rectMismatch;
                }

                anyLayer = true;
                if (li.x + zar.Width() > unionW) {
                    unionW = li.x + zar.Width();
                }
                if (li.y + zar.Height() > unionH) {
                    unionH = li.y + zar.Height();
                }

                size_t nonzero = 0;
                for (size_t p = 3; p < rgba.size(); p += 4) {
                    nonzero += rgba[p] != 0 ? 1 : 0;
                }
                if (nonzero < *minNonzero) {
                    *minNonzero = nonzero;
                }
                if (nonzero > *maxNonzero) {
                    *maxNonzero = nonzero;
                }
                *sumNonzero += static_cast<double>(nonzero);
                ++*sampleCount;

                if (outDir != nullptr) {
                    char name[512];
                    std::snprintf(name, sizeof(name), "%s/%s_%d_%d_%d.png", outDir->c_str(),
                                  Sanitize(col.name).c_str(), dir, frame, layer);
                    if (WritePng(name, zar.Width(), zar.Height(), rgba, &err)) {
                        ++stats->written;
                        if (firstPng != nullptr && firstPng->empty()) {
                            *firstPng = name;
                        }
                    } else {
                        std::fprintf(stderr, "  png error %s: %s\n", name, err.c_str());
                        ++stats->errors;
                    }
                }
            }
            if (anyLayer && rect != nullptr) {
                if (unionW == rect->Width() && unionH == rect->Height()) {
                    ++stats->rectExact;
                } else {
                    ++stats->rectSlack;
                }
            }
        }
    }
}

// Verification aid: composite a frame's four layers into its collection rect,
// which is exactly what the engine's blit loop does (each layer is drawn at
// (rect.x0 + li.x, rect.y0 + li.y) -- see GenerateSpriteAnimationZars @ 00712ae0,
// which derives li.x / li.y by subtracting the union rect's origin).
int Composite(const std::string& path, const std::string& outDir) {
    std::vector<uint8_t> file;
    std::string err;
    if (!ReadWholeFile(path, &file, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Sprite sprite;
    if (!sprite.Load(file, /*loadImages=*/true, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    size_t written = 0;

    for (const Collection& col : sprite.collections) {
        for (int32_t frame = 0; frame < col.frameCount; ++frame) {
            for (int32_t dir = 0; dir < col.dirCount; ++dir) {
                const size_t rectIndex = static_cast<size_t>(frame) * col.dirCount + dir;
                if (rectIndex >= col.rects.size()) {
                    continue;
                }
                const Rect& rect = col.rects[rectIndex];
                if (rect.Width() <= 0 || rect.Height() <= 0) {
                    continue;
                }
                std::vector<uint8_t> canvas(
                    static_cast<size_t>(rect.Width()) * rect.Height() * 4, 0);

                for (int32_t layer = 0; layer < kLayerCount; ++layer) {
                    const int32_t index = col.LayerIndex(frame, dir, layer);
                    if (index < 0 || static_cast<size_t>(index) >= col.images.size()) {
                        continue;
                    }
                    const LayerImage& li = col.images[static_cast<size_t>(index)];
                    if (!li.present) {
                        continue;
                    }
                    ZarImage& zar = const_cast<ZarImage&>(li.zar);
                    std::vector<uint8_t> rgba;
                    if (!zar.DecodeToRGBA(col.palettes[static_cast<size_t>(layer)], &rgba, &err)) {
                        continue;
                    }
                    for (int32_t y = 0; y < zar.Height(); ++y) {
                        const int32_t dy = li.y + y;
                        if (dy < 0 || dy >= rect.Height()) {
                            continue;
                        }
                        for (int32_t x = 0; x < zar.Width(); ++x) {
                            const int32_t dx = li.x + x;
                            if (dx < 0 || dx >= rect.Width()) {
                                continue;
                            }
                            const uint8_t* src =
                                rgba.data() + (static_cast<size_t>(y) * zar.Width() + x) * 4;
                            if (src[3] == 0) {
                                continue;
                            }
                            uint8_t* dst =
                                canvas.data() + (static_cast<size_t>(dy) * rect.Width() + dx) * 4;
                            const uint32_t a = src[3];
                            for (int c = 0; c < 3; ++c) {
                                dst[c] = static_cast<uint8_t>((src[c] * a + dst[c] * (255 - a)) / 255);
                            }
                            dst[3] = static_cast<uint8_t>(a + (dst[3] * (255 - a)) / 255);
                        }
                    }
                }
                char name[512];
                std::snprintf(name, sizeof(name), "%s/%s_%d_%d.png", outDir.c_str(),
                              Sanitize(col.name).c_str(), dir, frame);
                if (WritePng(name, rect.Width(), rect.Height(), canvas, &err)) {
                    ++written;
                }
            }
        }
    }
    std::printf("composited: %zu\n", written);
    return 0;
}

int Dump(const std::string& path, const std::string& outDir) {
    std::vector<uint8_t> file;
    std::string err;
    if (!ReadWholeFile(path, &file, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    Sprite sprite;
    if (!sprite.Load(file, /*loadImages=*/true, &err)) {
        std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str());
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    DumpStats stats;
    size_t minNonzero = static_cast<size_t>(-1), maxNonzero = 0, samples = 0;
    double sum = 0.0;
    std::string firstPng;
    for (const Collection& col : sprite.collections) {
        if (!col.loadError.empty()) {
            std::fprintf(stderr, "collection %s: %s\n", col.name.c_str(), col.loadError.c_str());
        }
        ProcessCollection(col, &outDir, &stats, &minNonzero, &maxNonzero, &sum, &samples,
                          &firstPng);
    }
    std::printf("images: %zu  decoded: %zu  png written: %zu  errors: %zu  rect mismatches: %zu\n",
                stats.images, stats.decoded, stats.written, stats.errors, stats.rectMismatch);
    if (!firstPng.empty()) {
        std::printf("first png: %s\n", firstPng.c_str());
    }
    return stats.errors == 0 ? 0 : 1;
}

int SelfTest(const std::vector<std::string>& paths) {
    size_t totalImages = 0, totalDecoded = 0, totalErrors = 0, totalMismatch = 0, files = 0;
    for (const std::string& path : paths) {
        std::vector<uint8_t> file;
        std::string err;
        if (!ReadWholeFile(path, &file, &err)) {
            std::printf("%-44s LOAD FAIL: %s\n", path.c_str(), err.c_str());
            ++totalErrors;
            continue;
        }
        Sprite sprite;
        if (!sprite.Load(file, /*loadImages=*/true, &err)) {
            std::printf("%-44s PARSE FAIL: %s\n", path.c_str(), err.c_str());
            ++totalErrors;
            continue;
        }
        ++files;

        DumpStats stats;
        size_t minNonzero = static_cast<size_t>(-1), maxNonzero = 0, samples = 0;
        double sum = 0.0;
        for (const Collection& col : sprite.collections) {
            if (!col.loadError.empty()) {
                std::printf("%-44s collection '%s': %s\n", path.c_str(), col.name.c_str(),
                            col.loadError.c_str());
                ++stats.errors;
            }
            ProcessCollection(col, nullptr, &stats, &minNonzero, &maxNonzero, &sum, &samples,
                              nullptr);
        }
        const size_t slash = path.find_last_of('/');
        const std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
        std::printf("%-26s seq=%-4zu col=%-4zu img=%-6zu dec=%-6zu err=%-3zu rectbad=%-3zu "
                    "rect exact/slack=%zu/%zu nonzero min/mean/max=%zu/%.0f/%zu\n",
                    base.c_str(), sprite.sequences.size(), sprite.collections.size(), stats.images,
                    stats.decoded, stats.errors, stats.rectMismatch, stats.rectExact, stats.rectSlack,
                    samples ? minNonzero : 0, samples ? sum / static_cast<double>(samples) : 0.0,
                    maxNonzero);
        totalImages += stats.images;
        totalDecoded += stats.decoded;
        totalErrors += stats.errors;
        totalMismatch += stats.rectMismatch;
    }
    std::printf("\nTOTAL sprites=%zu images=%zu decoded=%zu errors=%zu rect-mismatches=%zu\n",
                files, totalImages, totalDecoded, totalErrors, totalMismatch);
    return totalErrors == 0 ? 0 : 1;
}

void Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  sprview <file.spr> --info\n"
                 "  sprview <file.spr> --dump <outdir>\n"
                 "  sprview <file.spr> --composite <outdir>   (layers merged, verification aid)\n"
                 "  sprview --selftest <file.spr>...\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        Usage();
        return 2;
    }
    if (std::strcmp(argv[1], "--selftest") == 0) {
        std::vector<std::string> paths(argv + 2, argv + argc);
        if (paths.empty()) {
            Usage();
            return 2;
        }
        return SelfTest(paths);
    }
    if (argc >= 3 && std::strcmp(argv[2], "--info") == 0) {
        return Info(argv[1]);
    }
    if (argc >= 4 && std::strcmp(argv[2], "--dump") == 0) {
        return Dump(argv[1], argv[3]);
    }
    if (argc >= 4 && std::strcmp(argv[2], "--composite") == 0) {
        return Composite(argv[1], argv[3]);
    }
    Usage();
    return 2;
}
