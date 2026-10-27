# Fallout Tactics CE

A reimplementation of the engine of *Fallout Tactics: Brotherhood of Steel* (2001), in C++17 with SDL2. It runs on Windows, macOS and Linux, and in the browser.

You need your own copy of the game. This repository contains none of the game's files.

> This project is not affiliated with or endorsed by Bethesda Softworks, ZeniMax Media, Interplay Entertainment or Micro Forté. Fallout is a trademark of ZeniMax Media Inc.

Maintained by [RerunReactor](https://rerunreactor.com).

## Status

`fotgame` loads your 1.27 game data, reading its `.bos` archives in place, and plays the campaign's missions with rules ported from the original executable. The first mission plays from start to debrief.

- **Works:**
  - drawing, with the original's projection, draw order, colour tints and lighting;
  - the 70 ms simulation tick, animation and A* movement;
  - combat: to-hit, criticals, bursts, damage, armour and action points;
  - mission scripting, the AI and all eight skills;
  - the campaign's menus, world map, bunker and quartermaster;
  - saves in the original's format (the original's saves load);
  - positional sound and music.
- **Missing:** character creation, multiplayer, movies, barter, targeted shots, injuries, grenade area damage, squad levelling and some front-end screens.
- **Not yet verified:** whether the original game loads our saves.

The full picture, including what's approximated and a list of good first issues, is in [docs/status.md](docs/status.md).

## What you need

- **The game:** *Fallout Tactics: Brotherhood of Steel* 1.27, from Steam or GOG. You need its `core` folder.
- **To build:** a C++17 compiler and CMake 3.22 or newer.
  - SDL2 and zlib are fetched automatically.
  - On Linux you can use the system packages instead, with `-DFOT_VENDORED=OFF`.

## Building

```sh
cmake -S . -B build
cmake --build build --parallel
```

The programs end up in `build/bin/`: `fotgame`, and the tools `mapview`, `sprview` and `sprtrim`. With Visual Studio they go in a folder for each configuration below that, such as `build/bin/Release/`.

The data tests run only when `FOT_DATA_DIR` points at your game's `core` folder. Without it they're skipped:

```sh
FOT_DATA_DIR="/path/to/Fallout Tactics/core" ctest --test-dir build -j4
```

To check memory errors, build with AddressSanitizer and UndefinedBehaviorSanitizer: `-DFOT_SANITIZE=ON` (GCC and Clang).

The browser build needs Emscripten 4.0.4:

```sh
emcmake cmake -S . -B build-web
cmake --build build-web --parallel
```

That gives `build-web/bin/fotgame.js.js` and `fotgame.js.wasm`: a module for a web page to load, with the player's own game files mounted into its filesystem. There's no such page in this repository yet.

## Running

```sh
build/bin/fotgame --campaign --assets "/path/to/Fallout Tactics/core" --save-dir ~/fot-saves
```

This opens the title menu. [docs/running.md](docs/running.md) covers the options, the controls, single missions and the tools.

Until the original has been shown to load our saves, keep ours away from your installation with `--save-dir`.

## What's in the repository

| Folder | What it holds |
|---|---|
| `game/` | `fotgame`, the engine. |
| `common/`, `engine/` | Shared code: archives, images and rendering helpers. |
| `mapview/`, `sprview/`, `sprtrim/` | A map renderer, a sprite viewer and a sprite trimmer. Their map and sprite readers are shared with the engine. |
| `difftest/` | Tools that compare the original game with this port. |
| `kit/` | The reverse-engineering kit: names and types to load into Ghidra, and notes on the file formats. MIT-licensed. |
| `docs/` | Architecture, status, the porting guide and portability notes. |
| `third_party/` | stb_image and minimp3. |

## Contributing

Contributions are welcome; see [CONTRIBUTING.md](CONTRIBUTING.md). If you're new to the code, start with [docs/architecture.md](docs/architecture.md), then the [porting guide](docs/porting-guide.md). On your first pull request you'll be asked to sign a short [contributor agreement](CLA.md).

## License

- **The engine and tools** are under the [Sustainable Use License 1.0](LICENSE.md), with one added condition.
  - You may use, change and share them, free of charge and for non-commercial purposes.
  - You may not sell them.
  - If you hand out a changed version, you must publish its source code under the same terms.
- **`kit/`** is under the MIT License.
- **`third_party/`** keeps its own licenses.

## Credits

- **[FTSE](https://github.com/melindil/FTSE)** by melindil. Its Ghidra database, with names, signatures and struct layouts, is where our reverse engineering started.
- **[fallout2-ce](https://github.com/alexbatalov/fallout2-ce)** and [fallout1-ce](https://github.com/alexbatalov/fallout1-ce) by Alexander Batalov: the model for this project.
- **[FreeFT](https://github.com/nadult/FreeFT)** by nadult, for early hints on the sprite format.
- **Libraries:** [SDL2](https://www.libsdl.org/), [zlib](https://zlib.net/), [stb_image](https://github.com/nothings/stb) and [minimp3](https://github.com/lieff/minimp3).
