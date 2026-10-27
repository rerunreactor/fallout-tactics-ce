// sprtrim -- the `.spr` trimmer's command line.  See trim.h for what a trim is
// and why it is shaped this way.
//
//   sprtrim --keep <manifest> <in.spr> <out.spr>
//   sprtrim --report <file.spr>...              per-collection byte cost
//   sprtrim --selftest <file.spr>...            repack + trim, verified
//
// A manifest is one sequence name per line; `#` starts a comment.  It is what
// `fotgame --sprite-manifest` writes, which is the list of sequences a real run
// actually drew.

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "file.h"
#include "spr.h"
#include "trim.h"

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadManifest(const std::string& path, std::set<std::string>* out) {
    std::vector<uint8_t> bytes;
    std::string err;
    if (!ft::ReadWholeFile(path, &bytes, &err)) {
        std::fprintf(stderr, "sprtrim: %s: %s\n", path.c_str(), err.c_str());
        return false;
    }
    std::string line;
    bytes.push_back('\n');
    for (uint8_t c : bytes) {
        if (c == '\n' || c == '\r') {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.erase(hash);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t b = 0;
            while (b < line.size() && (line[b] == ' ' || line[b] == '\t')) ++b;
            line.erase(0, b);
            if (!line.empty()) out->insert(Lower(line));
            line.clear();
            continue;
        }
        line.push_back(static_cast<char>(c));
    }
    return true;
}

// What each collection costs, so a reader can see where a 25 MB sprite goes.
int Report(const std::vector<std::string>& files) {
    for (const std::string& path : files) {
        std::vector<uint8_t> bytes;
        std::string err;
        if (!ft::ReadWholeFile(path, &bytes, &err)) {
            std::fprintf(stderr, "sprtrim: %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        ft::Sprite spr;
        if (!spr.Load(bytes, false, &err)) {
            std::fprintf(stderr, "sprtrim: %s: %s\n", path.c_str(), err.c_str());
            return 1;
        }
        std::printf("%s  %zu bytes, %zu sequences, %zu collections\n", path.c_str(),
                    bytes.size(), spr.sequences.size(), spr.collections.size());
        for (size_t i = 0; i < spr.collections.size(); ++i) {
            const ft::Collection& c = spr.collections[i];
            size_t end = bytes.size();
            for (const ft::Collection& o : spr.collections) {
                if (o.offset > c.offset && static_cast<size_t>(o.offset) < end) {
                    end = static_cast<size_t>(o.offset);
                }
            }
            const size_t size = c.offset >= 0 && static_cast<size_t>(c.offset) <= end
                                    ? end - static_cast<size_t>(c.offset)
                                    : 0;
            std::string seqs;
            for (const ft::Sequence& s : spr.sequences) {
                if (static_cast<size_t>(s.collectionId) != i) continue;
                if (!seqs.empty()) seqs += ", ";
                seqs += s.name;
            }
            std::printf("  [%3zu] %-32s %9zu bytes  <- %s\n", i, c.name.c_str(), size,
                        seqs.empty() ? "(no sequence)" : seqs.c_str());
        }
    }
    return 0;
}

// A repack with an empty keep-set must be a no-op in content, and a trim to a
// small keep-set must still verify. Both are checked on every file given.
int Selftest(const std::vector<std::string>& files) {
    int bad = 0;
    uint64_t totalIn = 0, totalOut = 0;
    // A stance/weapon set a Brahmin Wood-shaped mission actually draws.
    const std::set<std::string> sample = {"stand", "walk", "run", "crouch", "prone",
                                          "death1", "idle"};
    for (const std::string& path : files) {
        std::vector<uint8_t> bytes;
        std::string err;
        if (!ft::ReadWholeFile(path, &bytes, &err)) {
            std::fprintf(stderr, "  %s: %s\n", path.c_str(), err.c_str());
            ++bad;
            continue;
        }
        ft::TrimOptions all;  // empty keep-set == keep everything
        ft::TrimStats s1;
        std::vector<uint8_t> repacked;
        if (!ft::TrimSprite(bytes, all, &repacked, &s1, &err)) {
            std::fprintf(stderr, "  %s: repack failed: %s\n", path.c_str(), err.c_str());
            ++bad;
            continue;
        }
        if (!ft::VerifyTrim(bytes, repacked, &err)) {
            std::fprintf(stderr, "  %s: repack does not verify: %s\n", path.c_str(),
                         err.c_str());
            ++bad;
            continue;
        }
        ft::TrimOptions cut;
        cut.keepSequences = sample;
        cut.keepUnreferenced = false;
        ft::TrimStats s2;
        std::vector<uint8_t> trimmed;
        if (!ft::TrimSprite(bytes, cut, &trimmed, &s2, &err)) {
            std::fprintf(stderr, "  %s: trim failed: %s\n", path.c_str(), err.c_str());
            ++bad;
            continue;
        }
        if (!ft::VerifyTrim(bytes, trimmed, &err)) {
            std::fprintf(stderr, "  %s: trim does not verify: %s\n", path.c_str(),
                         err.c_str());
            ++bad;
            continue;
        }
        totalIn += s2.bytesIn;
        totalOut += s2.bytesOut;
        std::printf("  ok %-56s %2zu/%2zu collections  %9llu -> %9llu\n", path.c_str(),
                    s2.collectionsKept, s2.collections,
                    (unsigned long long)s2.bytesIn, (unsigned long long)s2.bytesOut);
    }
    std::printf("sprtrim selftest: %zu files, %d bad; sample trim %llu -> %llu bytes (%.1f%%)\n",
                files.size(), bad, (unsigned long long)totalIn, (unsigned long long)totalOut,
                totalIn ? 100.0 * (double)totalOut / (double)totalIn : 0.0);
    return bad == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    std::string manifest;
    bool report = false, selftest = false, keepUnreferenced = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--keep" && i + 1 < argc) {
            manifest = argv[++i];
        } else if (a == "--drop-unreferenced") {
            keepUnreferenced = false;
        } else if (a == "--report") {
            report = true;
        } else if (a == "--selftest") {
            selftest = true;
        } else if (a == "--help" || a == "-h") {
            std::printf(
                "usage:\n"
                "  sprtrim --keep <manifest> [--drop-unreferenced] <in.spr> <out.spr>\n"
                "  sprtrim --report <file.spr>...\n"
                "  sprtrim --selftest <file.spr>...\n");
            return 0;
        } else {
            files.push_back(a);
        }
    }
    if (report) return Report(files);
    if (selftest) return Selftest(files);
    if (files.size() != 2) {
        std::fprintf(stderr, "sprtrim: need <in.spr> <out.spr> (see --help)\n");
        return 2;
    }

    ft::TrimOptions opts;
    opts.keepUnreferenced = keepUnreferenced;
    if (!manifest.empty() && !ReadManifest(manifest, &opts.keepSequences)) return 1;

    std::vector<uint8_t> bytes;
    std::string err;
    if (!ft::ReadWholeFile(files[0], &bytes, &err)) {
        std::fprintf(stderr, "sprtrim: %s: %s\n", files[0].c_str(), err.c_str());
        return 1;
    }
    std::vector<uint8_t> out;
    ft::TrimStats stats;
    if (!ft::TrimSprite(bytes, opts, &out, &stats, &err)) {
        std::fprintf(stderr, "sprtrim: %s: %s\n", files[0].c_str(), err.c_str());
        return 1;
    }
    if (!ft::VerifyTrim(bytes, out, &err)) {
        std::fprintf(stderr, "sprtrim: %s: output does not verify: %s\n", files[0].c_str(),
                     err.c_str());
        return 1;
    }
    std::FILE* fp = std::fopen(files[1].c_str(), "wb");
    if (fp == nullptr || std::fwrite(out.data(), 1, out.size(), fp) != out.size()) {
        if (fp != nullptr) std::fclose(fp);
        std::fprintf(stderr, "sprtrim: cannot write %s\n", files[1].c_str());
        return 1;
    }
    std::fclose(fp);
    std::printf("%s -> %s  %zu/%zu collections  %llu -> %llu bytes\n", files[0].c_str(),
                files[1].c_str(), stats.collectionsKept, stats.collections,
                (unsigned long long)stats.bytesIn, (unsigned long long)stats.bytesOut);
    return 0;
}
