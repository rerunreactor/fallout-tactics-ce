#include "vfs.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <unordered_map>

#include "file.h"

namespace fs = std::filesystem;

namespace ft {
namespace {

// --- little-endian scalar reads out of a byte buffer --------------------------
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

constexpr uint32_t kSigEocd = 0x06054b50;
constexpr uint32_t kSigCentral = 0x02014b50;
constexpr uint32_t kSigLocal = 0x04034b50;
constexpr uint32_t kSigEocd64Locator = 0x07064b50;

constexpr uint16_t kMethodStore = 0;
constexpr uint16_t kMethodDeflate = 8;

struct ZipEntry {
    uint32_t localHeaderOffset = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t method = 0;
};

// One mounted `.bos`. The archive file stays open and members are read by
// offset: a 256 MB `spr-character_0.bos` must not be resident twice, and under
// Emscripten the MEMFS copy is already the only copy we can afford.
struct Archive {
    std::string path;
    std::FILE* fp = nullptr;
    uint64_t size = 0;
    size_t entryCount = 0;

    ~Archive() {
        if (fp != nullptr) std::fclose(fp);
    }
    Archive() = default;
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
};

// A mounted loose directory.
struct DirMount {
    std::string root;
    size_t entryCount = 0;
};

// Where one resource name resolves to. Exactly one of the two is live.
struct Resolved {
    int archive = -1;    // index into g_archives, or -1 for a loose file
    ZipEntry entry;      // valid when archive >= 0
    std::string osPath;  // valid when archive < 0
};

std::vector<std::unique_ptr<Archive>> g_archives;
std::vector<DirMount> g_dirs;
// Keyed by VfsKey(name). Later mounts overwrite earlier ones, which is the
// "loose files and per-CD archives patch the base archives" rule of vfs.h.
std::unordered_map<std::string, Resolved> g_index;
bool g_accessLog = false;
std::vector<std::string> g_accessed;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadAt(std::FILE* fp, uint64_t offset, void* dst, size_t n) {
    if (n == 0) return true;
    if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) return false;
    return std::fread(dst, 1, n, fp) == n;
}

// The one read primitive every archive access goes through. Bounds-checked
// against the size recorded at mount, so a truncated or swapped-out archive
// fails here rather than as a puzzling inflate error.
bool ArchiveReadAt(const Archive& a, uint64_t offset, void* dst, size_t n) {
    if (n == 0) return true;
    if (offset + n > a.size) return false;
    return ReadAt(a.fp, offset, dst, n);
}

// Raw-deflate a member into `out`, which is pre-sized to the uncompressed
// length the central directory promised.
bool Inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>* out, std::string* errorOut) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    // -MAX_WBITS selects raw deflate: a zip member has no zlib header.
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
        *errorOut = "inflateInit2 failed";
        return false;
    }
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = static_cast<uInt>(in.size());
    zs.next_out = out->empty() ? nullptr : out->data();
    zs.avail_out = static_cast<uInt>(out->size());
    const int rc = inflate(&zs, Z_FINISH);
    const uLong produced = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
        *errorOut = "inflate failed (" + std::to_string(rc) + ")";
        return false;
    }
    if (produced != out->size()) {
        *errorOut = "short inflate: " + std::to_string(produced) + " of " +
                    std::to_string(out->size());
        return false;
    }
    return true;
}

// Find the end-of-central-directory record by scanning backwards. The comment
// field is at most 64 KB, so that plus the 22-byte record bounds the search.
bool FindEocd(const Archive& a, uint32_t* cdOffset, uint32_t* cdEntries,
              std::string* errorOut) {
    const uint64_t fileSize = a.size;
    const size_t kMaxTail = 22 + 65535;
    const size_t tail = static_cast<size_t>(std::min<uint64_t>(fileSize, kMaxTail));
    if (tail < 22) {
        *errorOut = "too small to be a zip";
        return false;
    }
    std::vector<uint8_t> buf(tail);
    if (!ArchiveReadAt(a, fileSize - tail, buf.data(), tail)) {
        *errorOut = "cannot read the archive tail";
        return false;
    }
    for (size_t i = buf.size() - 22 + 1; i-- > 0;) {
        if (U32(&buf[i]) != kSigEocd) continue;
        const uint16_t entriesTotal = U16(&buf[i + 10]);
        const uint32_t off = U32(&buf[i + 16]);
        // A zip64 archive puts 0xFFFF/0xFFFFFFFF here and the real values in a
        // separate record. None of the shipped `.bos` need it (the largest is
        // 256 MB with well under 65,535 members), so say so rather than
        // silently indexing garbage.
        if (entriesTotal == 0xFFFF || off == 0xFFFFFFFFu) {
            *errorOut = "zip64 archives are not supported";
            return false;
        }
        // Guard against the locator sitting where we expect the record.
        if (i >= 20 && U32(&buf[i - 20]) == kSigEocd64Locator) {
            *errorOut = "zip64 archives are not supported";
            return false;
        }
        *cdEntries = entriesTotal;
        *cdOffset = off;
        return true;
    }
    *errorOut = "no end-of-central-directory record";
    return false;
}

}  // namespace

std::string VfsKey(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    name = LowerAscii(std::move(name));
    const size_t core = name.find("/core/");
    // Only strip when it really is an absolute developer path -- a drive letter
    // before the segment. `campaigns/missions/core/mission01.mis` must not lose
    // its `core` directory, and it is a real shipped name.
    if (core != std::string::npos && name.find(':') < core) {
        name.erase(0, core + 6);
    }
    while (!name.empty() && name[0] == '/') name.erase(name.begin());
    return name;
}

bool VfsMountArchive(const std::string& path, std::string* errorOut) {
    auto archive = std::make_unique<Archive>();
    archive->path = path;
    std::FILE* fp = std::fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        *errorOut = "cannot open " + path;
        return false;
    }
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    if (sz <= 0) {
        std::fclose(fp);
        *errorOut = "cannot size " + path;
        return false;
    }
    archive->fp = fp;
    archive->size = static_cast<uint64_t>(sz);

    uint32_t cdOffset = 0, cdEntries = 0;
    if (!FindEocd(*archive, &cdOffset, &cdEntries, errorOut)) {
        *errorOut = path + ": " + *errorOut;
        return false;
    }
    // The central directory of `tiles_0.bos` is ~1 MB for 30k entries; reading
    // it in one go is much cheaper than 30k seeks.
    if (cdOffset >= archive->size) {
        *errorOut = path + ": central directory past the end of the file";
        return false;
    }
    const size_t cdLen = static_cast<size_t>(archive->size - cdOffset);
    std::vector<uint8_t> cd(cdLen);
    if (!ArchiveReadAt(*archive, cdOffset, cd.data(), cdLen)) {
        *errorOut = path + ": cannot read the central directory";
        return false;
    }

    const int index = static_cast<int>(g_archives.size());

    size_t at = 0;
    size_t indexed = 0;
    for (uint32_t i = 0; i < cdEntries; ++i) {
        if (at + 46 > cd.size() || U32(&cd[at]) != kSigCentral) break;
        const uint16_t method = U16(&cd[at + 10]);
        const uint32_t compSize = U32(&cd[at + 20]);
        const uint32_t uncompSize = U32(&cd[at + 24]);
        const uint16_t nameLen = U16(&cd[at + 28]);
        const uint16_t extraLen = U16(&cd[at + 30]);
        const uint16_t commentLen = U16(&cd[at + 32]);
        const uint32_t localOffset = U32(&cd[at + 42]);
        if (at + 46 + nameLen > cd.size()) break;
        std::string name(reinterpret_cast<const char*>(&cd[at + 46]), nameLen);
        at += 46 + nameLen + extraLen + commentLen;

        // Directory markers (`gui/`, `sound/Game/`) are zero-length Stored
        // entries with a trailing slash. They carry nothing.
        if (!name.empty() && (name.back() == '/' || name.back() == '\\')) continue;
        if (method != kMethodStore && method != kMethodDeflate) continue;

        ZipEntry e;
        e.localHeaderOffset = localOffset;
        e.compSize = compSize;
        e.uncompSize = uncompSize;
        e.method = method;
        Resolved r;
        r.archive = index;
        r.entry = e;
        g_index[VfsKey(name)] = std::move(r);
        ++indexed;
    }
    archive->entryCount = indexed;
    g_archives.push_back(std::move(archive));
    return true;
}

size_t VfsMountDir(const std::string& root) {
    if (root.empty()) return 0;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return 0;
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string full = it->path().string();
        // MILESTONE 20.  An archive is not a resource.  Indexing `tiles_0.bos`
        // as one shadowed nothing, but it made `VfsSize()` and the
        // `--resource-manifest` count seven files larger than the run's.
        if (LowerAscii(it->path().extension().string()) == ".bos") continue;
        std::string rel = full.substr(root.size());
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(rel.begin());
        Resolved r;
        r.archive = -1;
        r.osPath = full;
        g_index[VfsKey(rel)] = std::move(r);
        ++n;
    }
    g_dirs.push_back(DirMount{root, n});
    return n;
}

size_t VfsMountAssetRoot(const std::string& dir, std::string* errorOut) {
    if (dir.empty()) return 0;
    std::error_code ec;
    std::vector<std::string> archives;
    if (fs::is_directory(dir, ec)) {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (LowerAscii(e.path().extension().string()) == ".bos") {
                archives.push_back(e.path().string());
            }
        }
    }
    // `*_0.bos` before `*_A.bos`, so a per-CD archive patches the base.
    std::sort(archives.begin(), archives.end());
    size_t mounted = 0;
    for (const std::string& a : archives) {
        std::string err;
        if (VfsMountArchive(a, &err)) {
            ++mounted;
        } else if (errorOut != nullptr) {
            if (!errorOut->empty()) *errorOut += "; ";
            *errorOut += err;
        }
    }
    // Loose files last: they win over every archive.
    VfsMountDir(dir);
    return mounted;
}

bool VfsMounted() { return !g_index.empty(); }

size_t VfsSize() { return g_index.size(); }

bool VfsResolve(const std::string& name, std::string* handleOut) {
    const std::string key = VfsKey(name);
    auto it = g_index.find(key);
    if (it == g_index.end()) return false;
    if (it->second.archive < 0) {
        *handleOut = it->second.osPath;  // a loose file: a plain OS path.
    } else {
        *handleOut = std::string(1, kVfsHandlePrefix) + key;
    }
    return true;
}

bool VfsReadHandle(const std::string& handle, std::vector<uint8_t>* out, std::string* errorOut) {
    if (!IsVfsHandle(handle)) {
        *errorOut = "not a resource handle";
        return false;
    }
    const std::string key = handle.substr(1);
    auto it = g_index.find(key);
    if (it == g_index.end()) {
        *errorOut = "no such resource: " + key;
        return false;
    }
    // Record the *read*, not the resolve: PathIndex and ResourceRoot resolve
    // every entry they index, so a resolve says nothing about what a run used.
    if (g_accessLog) g_accessed.push_back(key);
    const Resolved& r = it->second;
    if (r.archive < 0) return ReadWholeFile(r.osPath, out, errorOut);

    Archive& a = *g_archives[static_cast<size_t>(r.archive)];
    // The central directory's name and extra lengths need not match the local
    // header's, so the data offset has to come from the local header.
    uint8_t local[30];
    if (!ArchiveReadAt(a, r.entry.localHeaderOffset, local, sizeof(local)) ||
        U32(local) != kSigLocal) {
        *errorOut = key + ": bad local header in " + a.path;
        return false;
    }
    const uint64_t dataOffset = static_cast<uint64_t>(r.entry.localHeaderOffset) + 30 +
                                U16(&local[26]) + U16(&local[28]);

    if (r.entry.method == kMethodStore) {
        out->resize(r.entry.uncompSize);
        if (!ArchiveReadAt(a, dataOffset, out->data(), out->size())) {
            *errorOut = key + ": short read in " + a.path;
            return false;
        }
        return true;
    }
    std::vector<uint8_t> comp(r.entry.compSize);
    if (!ArchiveReadAt(a, dataOffset, comp.data(), comp.size())) {
        *errorOut = key + ": short read in " + a.path;
        return false;
    }
    out->resize(r.entry.uncompSize);
    std::string err;
    if (!Inflate(comp, out, &err)) {
        *errorOut = key + " in " + a.path + ": " + err;
        out->clear();
        return false;
    }
    return true;
}

bool VfsRead(const std::string& name, std::vector<uint8_t>* out, std::string* errorOut) {
    std::string handle;
    if (!VfsResolve(name, &handle)) {
        *errorOut = "not in the resource layer: " + name;
        return false;
    }
    return ReadWholeFile(handle, out, errorOut);
}

void VfsEnumerate(const std::string& prefix, const std::string& extFilter,
                  const std::function<void(const std::string&)>& fn) {
    for (const auto& kv : g_index) {
        const std::string& key = kv.first;
        if (!prefix.empty() && key.rfind(prefix, 0) != 0) continue;
        if (!extFilter.empty()) {
            if (key.size() < extFilter.size()) continue;
            if (key.compare(key.size() - extFilter.size(), extFilter.size(), extFilter) != 0) {
                continue;
            }
        }
        fn(key);
    }
}

std::vector<std::string> VfsMountReport() {
    std::vector<std::string> out;
    for (const auto& a : g_archives) {
        out.push_back(a->path + " (" + std::to_string(a->entryCount) + " entries, " +
                      std::to_string(a->size / (1024 * 1024)) + " MB)");
    }
    for (const auto& d : g_dirs) {
        out.push_back(d.root + "/ (" + std::to_string(d.entryCount) + " loose files)");
    }
    return out;
}

void VfsSetAccessLog(bool on) { g_accessLog = on; }

std::vector<std::string> VfsTakeAccessLog() {
    std::sort(g_accessed.begin(), g_accessed.end());
    g_accessed.erase(std::unique(g_accessed.begin(), g_accessed.end()), g_accessed.end());
    std::vector<std::string> out;
    out.swap(g_accessed);
    return out;
}

void VfsReset() {
    g_index.clear();
    g_archives.clear();
    g_dirs.clear();
}

}  // namespace ft
