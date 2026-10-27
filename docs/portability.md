# Portability

The sources were written and built on one Mac with Apple clang. This file
records what stood in the way of GCC (Linux CI) and MSVC (Windows CI), what
has been changed, and what was checked and left alone.

Status, 2026-10-26: every item under "Fixed" is applied. The fixes are code
only (no comment was edited) and change no output: the CMake build of the
fixed tree produces byte-identical results to the old Makefile build on the
real game data (18,674 files: sprite decodes, map renders, tile dumps, saves,
trimmed sprites, fotgame screenshots and audio). The one deliberate behaviour
change is noted under "Fixed".

## How this was checked

Neither GCC nor MSVC is installed on the build Mac, so the checks are
clang-based and aimed at what the other two compilers and their standard
libraries reject:

- `clang++ -std=c++17 -fsyntax-only -pedantic -Wall -Wextra -Wvla` over every
  `.cpp`: no warnings. That rules out VLAs, designated initializers (a C++20
  feature MSVC rejects in C++17 mode), zero-length arrays, `case` ranges,
  statement expressions and the other GNU extensions.
- The same compile against libc++ with `-D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES`,
  which catches standard headers a file only got by accident through another
  one. libstdc++ 13 (Ubuntu 24.04's GCC) and the MSVC STL pull in a different
  set, so those accidents are exactly what breaks.
- A scan for standard and C library names used without their header being
  included by the file or one of the project headers it includes.
- A search for POSIX-only headers and calls: `dirent.h`, `sys/stat.h`,
  `unistd.h`, `mkdir`, `opendir`, `stat`, `strcasecmp`, `alloca` and so on, and
  for `__attribute__`, `__builtin_*`, `#pragma pack` and packed structs.

Not verified: an actual GCC or MSVC compile. The first CI run is that test.

## Fixed

| Where (now) | Problem | Breaks | Fix |
|---|---|---|---|
| `game/ui.cpp:14`, `:142` (`ScanDir`) | `<dirent.h>`, `<sys/stat.h>`, `opendir`/`readdir`/`stat`/`S_ISDIR` | MSVC | `std::filesystem::directory_iterator` and `std::filesystem::status` (follows links, as `stat` did); unreadable entries are still skipped |
| `game/audio.cpp:23`, `:259` (`Audio::IndexTree`) | the same | MSVC | the same |
| `game/main.cpp:20`, `:1040` (`RefreshSaveSlots`) | `opendir`/`readdir` over the save directory | MSVC | `std::filesystem::directory_iterator` |
| `game/main.cpp:3736` | `::mkdir(path, 0755)` on `<core>/user` and on the save directory | MSVC (its `mkdir` takes one argument) | `std::filesystem::create_directories(g.saveDir, ec)`, which also makes `<core>/user` |
| `game/main.cpp:4298` (`--vehicle-census`) | `opendir`/`readdir` | MSVC | `std::filesystem::directory_iterator` |
| `sprview/main.cpp:5`, `:231`, `:313` (`--composite`, `--dump`) | `<sys/stat.h>`, `::mkdir(outDir, 0755)` | MSVC | `std::filesystem::create_directories(outDir, ec)` |
| `game/audio_names.cpp:8` | `uint32_t` with no `<cstdint>` | GCC 13+: libstdc++'s `<string>` no longer brings it in | `#include <cstdint>` |
| `game/ui_screens.cpp:14` | `std::function` (`:2323`) with no `<functional>` | MSVC STL, possibly libstdc++ | `#include <functional>` |
| `game/audio.cpp:21`, `game/campaign.cpp:9`, `mapview/main.cpp:6` | `std::atoi`, `atof`, `atoll`, `strtoul`, `exit` with no `<cstdlib>` | a standard library that stops including it from `<string>` | `#include <cstdlib>` |
| `game/combat.cpp:6`, `game/main.cpp:15`, `game/scene.cpp:6`, `game/script.cpp:6`, `game/ui_screens.cpp:9`, `sprtrim/main.cpp:12` | `std::tolower`, `toupper`, `isspace`, `isdigit`, `isalnum` with no `<cctype>` | the same | `#include <cctype>` |
| `game/combat.cpp:8` | `std::snprintf` with no `<cstdio>` | the same | `#include <cstdio>` |

Behaviour change: `create_directories` makes every missing level, where
`mkdir` made only the last one. `--save-dir a/b/saves` and
`sprview --dump a/b/out` now work when `a/b` does not exist yet; before, every
save and every PNG failed with "cannot write". With a single-level path the
old and new builds behave identically (checked: same saves, same load-screen
listing, same dumps).

## Handled by the build, no source change

- `M_PI` (`game/audio.cpp:851`) is not defined by MSVC's `<cmath>` unless
  `_USE_MATH_DEFINES` is: CMakeLists.txt defines it on Windows, with
  `_CRT_SECURE_NO_WARNINGS`, `_CRT_NONSTDC_NO_WARNINGS`, `NOMINMAX` and
  `WIN32_LEAN_AND_MEAN` (the set fallout2-ce uses).
- MSVC gets `/utf-8` (the comments carry UTF-8 `§` signs, e.g.
  `common/zar.h:26`) and `/bigobj` (`game/scene.cpp` is 370 KB of source,
  `game/main.cpp` 225 KB).
- On Windows `SDL.h` renames `main` to `SDL_main`; fotgame links SDL2main,
  which supplies the real entry point.
- `std::filesystem` needs macOS 10.15, so that is the deployment target.

## Checked and fine

- `common/image.cpp:37-43`: the `#pragma GCC diagnostic` around stb_image is
  guarded by `__clang__`/`__GNUC__`.
- `game/audio.cpp:37-41`: AudioToolbox is only compiled with
  `FOT_MP3_AUDIOTOOLBOX` on Apple; every platform decodes MP3 with minimp3.
- 64-bit values are cast to `long long` before `%lld`, so the format strings
  are right whether `int64_t` is `long` (Linux) or `long long` (macOS, Windows).
- Emscripten-only code is behind `__EMSCRIPTEN__`.
- No overloads on `int64_t`/`long` or `uint64_t`/`size_t` pairs that would
  collide where those are the same type, and no POSIX integer typedefs
  (`uint`, `u_int32_t`, `ssize_t`, `off_t`).
- MSVC stops at 128 nested blocks (C1061), and every `else if` nests one
  deeper. fotgame's option parser (`game/main.cpp:3292`) is the longest chain
  at 79 links, so there is room for about 45 more options before it has to
  be split; mapview's has 37.

## Left alone

- 2 GB file offsets on Windows. `common/vfs.cpp:88` seeks with
  `static_cast<long>(offset)`, and `common/vfs.cpp:198` and
  `common/file.cpp:18` size files with `std::ftell`, which returns `long`:
  32 bits on Windows. The largest retail archive is 268 MB, so nothing reaches
  the limit. If it ever matters: `_fseeki64`/`_ftelli64` on `_WIN32`,
  `fseeko`/`ftello` elsewhere.
- `std::move`, `std::pair`, `std::swap` and `std::error_code` are used without
  `<utility>`/`<system_error>` in 27 places (e.g. `common/vfs.cpp:177`,
  `game/campaign_ui.h:158`, `mapview/sort.h:261`, `mapview/ent.cpp:65`). Every
  standard library provides them through the container headers and
  `<filesystem>` these files already include, so they were not touched.
- Paths with non-ASCII characters on Windows go through narrow `fopen` and
  `std::filesystem::path::string()`, i.e. the ANSI code page, as before.
- Comments that still mention `make`: `game/main.cpp:6` (`make wasm`),
  `game/audio.cpp:36` (`make native CXXFLAGS+=-DFOT_MP3_AUDIOTOOLBOX=1`; with
  CMake that is `-DCMAKE_CXX_FLAGS=-DFOT_MP3_AUDIOTOOLBOX=1`) and
  `game/audio.cpp:128` (`game/Makefile`). `difftest/compare.py:60-124` still
  runs `game/fotgame` and tells the user to `make native`; the binary is now
  `<build>/bin/fotgame`.
