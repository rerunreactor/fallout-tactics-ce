// vfs.h -- the resource layer: `.bos` archives and loose directories, merged
// into one flat, case-insensitive namespace.
//
// MILESTONE 15.  Until now every asset consumer took a *directory* root and
// walked it with `fs::recursive_directory_iterator` (mapview/ent.cpp's
// PathIndex, game/ui.cpp's ScanDir, game/audio.cpp's music scan).  That assumed
// the shipped `core/*.bos` archives had already been unpacked to disk, which is
// fine for a workstation with 1.5 GB spare and impossible in a browser where
// the player brings their own retail install.
//
// A `.bos` is a plain PKZIP archive.  Contrary to what the milestone brief
// assumed, the shipped ones are NOT store-only -- `tiles_0.bos`,
// `entities_0.bos` and `locale_0.bos` are all `Defl:N`, and only the directory
// markers in `gui_0.bos`/`sound_0.bos` are `Stored`.  So entries cannot be read
// in place; this reads the central directory once at mount time and inflates a
// member on demand.  zlib is already a dependency of both builds (`-lz`
// natively, `-sUSE_ZLIB=1` under Emscripten), so raw inflate is free.
//
// The namespace a mount contributes is rooted at the engine's `core`
// directory: archive members are named `tiles/...`, `sprites/...`,
// `entities/...`, `gui/...`, `locale/...`, `campaigns/...`, `sound/...`, and a
// loose directory mount contributes exactly the same names relative to itself.
// That is what makes an unpacked tree and the archives interchangeable, and it
// is why `--assets <dir>` keeps working unchanged for a directory that happens
// to hold `.bos` files instead of subdirectories.
//
// Lookup is case-insensitive with `\` folded to `/`, because the `.mis` files
// name their resources in the level designer's Windows casing and the archives
// do not agree with them.

#ifndef FT_VFS_H
#define FT_VFS_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ft {

// A resolved resource handle. `ReadWholeFile` understands both a plain OS path
// and a handle returned here, so every existing `Resolve()`/`Find()` caller
// keeps working: they hand us back whatever string we gave them.
//
// Archive handles are prefixed with this byte, which cannot occur in a path.
constexpr char kVfsHandlePrefix = '\x01';

inline bool IsVfsHandle(const std::string& s) {
    return !s.empty() && s[0] == kVfsHandlePrefix;
}

// Fold a resource name to the lookup key: lowercase, forward slashes, no
// leading slash, and with any `.../core/` prefix removed.
//
// A handful of shipped maps store an absolute path from the developer's build
// machine -- `F:/bos/core/entities/Actors/Mutants/sm_03_Melee.ent` and two
// others. The engine's resource layer is rooted at `core`, so anything up to
// and including a `/core/` segment is not part of the resource name. This is
// the rule mapview/ent.cpp's PathIndex::Find used to apply on its own.
std::string VfsKey(std::string name);

// Mount a `.bos` archive. Returns false and sets `errorOut` if it is not a
// readable zip.
bool VfsMountArchive(const std::string& path, std::string* errorOut);

// Mount a loose directory, walked recursively. Missing directories are not an
// error (the archives may be supplying that part of the tree); the return is
// the number of files indexed.
size_t VfsMountDir(const std::string& root);

// Mount an asset root the way `--assets` means it: every `*.bos` directly
// inside `dir` in ascending name order, and then `dir` itself as a loose tree.
//
// Order matters and is deliberate. Later mounts win, so:
//   1. `*_0.bos`   the base archives
//   2. `*_A.bos` .. the per-CD archives, which patch the base
//   3. loose files, which are what a patch or a mod drops next to them and
//      which the retail engine also prefers.
// Ascending name order gets (1) before (2) for free, because `0` < `A`.
//
// Returns the number of archives mounted; `*errorOut` collects the names of any
// that failed, but a bad archive is not fatal.
size_t VfsMountAssetRoot(const std::string& dir, std::string* errorOut);

// Is anything mounted? Consumers use this to decide whether to fall back to
// their old plain-directory behaviour.
bool VfsMounted();

// Resolve a resource name to a handle `ReadWholeFile` accepts. Returns false if
// nothing is mounted under that name.
bool VfsResolve(const std::string& name, std::string* handleOut);

// Read a resource by name. Equivalent to VfsResolve + ReadWholeFile.
bool VfsRead(const std::string& name, std::vector<uint8_t>* out, std::string* errorOut);

// Read by handle. `ReadWholeFile` calls this for handles; direct callers should
// not need it.
bool VfsReadHandle(const std::string& handle, std::vector<uint8_t>* out, std::string* errorOut);

// Enumerate. `fn` is called with the canonical key (lowercased, '/'-separated)
// of every mounted resource. `extFilter`, if non-empty, is a lowercase
// extension including the dot; `prefix`, if non-empty, is a lowercase key
// prefix.
void VfsEnumerate(const std::string& prefix, const std::string& extFilter,
                  const std::function<void(const std::string& key)>& fn);

// How many distinct resources are mounted.
size_t VfsSize();

// One line naming each mount and its entry count, for the boot banner.
std::vector<std::string> VfsMountReport();

// Record every resource name that resolves, so a fixture builder can be told
// exactly which files a run touched. `VfsTakeAccessLog` returns them sorted and
// clears the recording. This is how `scripts/make-fot-fixture.py` decides what
// goes in the small archives.
void VfsSetAccessLog(bool on);
std::vector<std::string> VfsTakeAccessLog();

// Drop everything. Only the tests use this.
void VfsReset();

}  // namespace ft

#endif  // FT_VFS_H
