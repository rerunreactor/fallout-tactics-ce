#include "file.h"

#include <cstdio>

#include "vfs.h"

namespace ft {

namespace {

bool ReadOsFile(const std::string& path, std::vector<uint8_t>* out, std::string* errorOut) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        *errorOut = "cannot open " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0) {
        std::fclose(f);
        *errorOut = "cannot size " + path;
        return false;
    }
    out->resize(static_cast<size_t>(size));
    const size_t got = out->empty() ? 0 : std::fread(out->data(), 1, out->size(), f);
    std::fclose(f);
    if (got != out->size()) {
        *errorOut = "short read on " + path;
        return false;
    }
    return true;
}

}  // namespace

// MILESTONE 15.  Three cases, in order:
//
//   1. a resource handle from `VfsResolve` -- read it out of its `.bos`;
//   2. a real file on disk -- the pre-milestone-14 behaviour, and still what
//      every save, screenshot and `--assets`-relative path takes;
//   3. neither, but the name resolves in the mounted resource layer.
//
// Case 3 is what lets the call sites that build a path by concatenation rather
// than through an index -- `assetsDir + "/campaigns/bos.cam"`, `assetsDir +
// "/bos.cfg"`, the mission path on the command line -- work against archives
// with no change at all: the OS open fails, and the tail of the path is a
// resource name.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>* out, std::string* errorOut) {
    if (IsVfsHandle(path)) {
        return VfsReadHandle(path, out, errorOut);
    }
    if (ReadOsFile(path, out, errorOut)) {
        return true;
    }
    if (VfsMounted()) {
        std::string handle;
        if (VfsResolve(path, &handle) && handle != path) {
            std::string ignored;
            if (ReadWholeFile(handle, out, &ignored)) return true;
        }
        // `--assets <root>` callers pass `<root>/campaigns/bos.cam`; the
        // resource name is the tail. Try progressively shorter suffixes, which
        // is bounded by the number of path separators and only runs on a miss.
        std::string tail = path;
        for (size_t slash = tail.find('/'); slash != std::string::npos;
             slash = tail.find('/')) {
            tail = tail.substr(slash + 1);
            std::string h;
            if (VfsResolve(tail, &h) && h != path) {
                std::string ignored;
                if (ReadWholeFile(h, out, &ignored)) return true;
            }
        }
    }
    *errorOut = "cannot open " + path;
    return false;
}

}  // namespace ft
