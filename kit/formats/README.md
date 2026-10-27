# Fallout Tactics file formats

These pages describe the data formats of Fallout Tactics: Brotherhood of Steel 1.27, as far as we understand them. Like the rest of `kit/`, they're under the MIT License.

| Page | Files |
|---|---|
| [bos.md](bos.md) | `.bos` archives, and how the game finds files in them |
| [spr-zar.md](spr-zar.md) | `.spr` sprites and ZAR images, with the per-layer palettes and the colour records that tint them |
| [til.md](til.md) | `.til` tile prototypes |
| [mis.md](mis.md) | `.mis` maps: the `<world>` wrapper, `<mapManager>`, regions, the entity file, scripting sections, lights |
| [ent-esh.md](ent-esh.md) | `.ent` prototypes and the `<esh>` tagged-property records, which store every entity |
| [saves.md](saves.md) | `.sav` saves and the `.cam` campaign file: the container, `<saveh>`, `<world>` 70, `<sgd>`, `<campaign>` and the variable table |

## Where the facts come from

Each fact comes from one of three sources:

- **The original's code.** The reader or writer is cited as `Name@address`: a function name from the project's Ghidra work, and its virtual address (image base `0x00400000`). The address is the same in `BOS.exe` and `BOS_HR.exe`, which differ only inside two display-mode functions. Names are descriptive, not the original source names, and some are guesses. The address is what identifies the function.
- **The shipped data.** These facts were found by parsing every shipped file of a kind and checking that each record ends exactly where the next begins.
- **FreeFT**, an earlier open-source reimplementation. We say so when it's the only source.

Anything we haven't established is listed under "Unknown or unverified" at the end of each page.

## Conventions shared by every format

### Byte order

Little-endian throughout.

### Format tokens

Most blocks start with a token: a NUL-terminated tag, then the version as NUL-terminated ASCII decimal. For example, `<sprite>` version 4 is these 11 bytes:

```
3c 73 70 72 69 74 65 3e 00 34 00      "<sprite>\0" "4\0"
```

Most tags are in angle brackets. A few aren't, such as `varTableHeader` and `timerTableHeader`.

Each reader accepts a range of versions. These pages give the range as the original's test, for example "accepted when 3 < v < 5".

### Strings

A string is an `i32` length, then that many bytes, with no terminator.

The length can be tagged. If bit 31 is set, the low 31 bits are a count of 16-bit characters (UTF-16LE), so the string takes twice that many bytes.

- Saves use the wide form for every string.
- Maps and most `.ent` files use the narrow form.
- A few loose `.ent` files in the 1.27 install use the wide form.

The entity-stream string reader, `EntityStreamReadString@006f1ff0`, accepts both forms. Whether every reader in the original does is unverified.

### Units and coordinates

- **World units.** The tile map uses integer world units, four to the metre. A floor tile is 6 × 1 × 6 units (1.5 m on a side).
- **Axes.** `x` and `z` are the ground plane, and `y` is height.
- **Entity positions** are stored as floats in metres. The integer position is `round(metres × 4)`.
- **Projection** (`ComputeIsoScreenOffset@0044de60`), with screen `y` growing downward:

  ```
  screenX =  6 × (x − z)
  screenY = −3 × (x + z) − 7 × y
  ```

- **Directions.** Sprite directions count clockwise on screen from +z. With eight directions:

  | Index | World direction | On screen |
  |---|---|---|
  | 0 | +z | up-left |
  | 2 | +x | up-right |
  | 4 | −z | down-right |
  | 6 | −x | down-left |

  The odd indices fall between these. The angle is `atan2(dx, dz)`, measured from +z towards +x, and is quantised to the sprite's own number of directions.

### Compression

| Where | Method |
|---|---|
| `.bos` archive members | Zip deflate, with no zlib header |
| `<world>` streams in maps and saves | zlib, with a zlib header |
| Version-2 sprite image blocks | gzip |

### Images

Images embedded in other formats are always ZAR; see [spr-zar.md](spr-zar.md).

The game identifies standalone image files by their first bytes, not their extension (`FormatManager::DetectImageFormatFromStream@006d2cb0`):

| First bytes | Format |
|---|---|
| `<zar>` | ZAR |
| `89 50 4E 47 0D 0A 1A 0A` | PNG |
| `FF D8 FF` | JPEG |
| `BM` | BMP |

### In-memory offsets

Some tables give a field's offset in the original's object, such as `Entity+0x9A` or `World+0x2e7`. These help when reading the decompiled code. They aren't file offsets.

## Our readers

| Format | Reader in this repository |
|---|---|
| `.bos` | `common/vfs.cpp` |
| `.spr` | `sprview/spr.cpp` |
| ZAR | `common/zar.cpp` |
| `.til` | `mapview/til.cpp` |
| `.mis` | `mapview/mis.cpp`, `mapview/ent.cpp`, `game/script.cpp` |
| `<esh>` | `mapview/esh.cpp` |
| Saves and `.cam` | `game/campaign_save.cpp`, `game/campaign.cpp`, `mapview/mis.cpp` |
