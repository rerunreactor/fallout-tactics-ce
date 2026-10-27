# .bos archives

Nearly all of the game's data is packed in `.bos` files in the installation's `core` folder. A `.bos` file is an ordinary PKZIP archive; any unzip tool can open it.

## Structure

Standard zip: local file headers and data, a central directory, and an end-of-central-directory record.

| Property | Shipped archives |
|---|---|
| Compression | Deflate (method 8) for file members. Stored (method 0) for zero-length folder entries (names ending in `/`). |
| Zip64 | Not used. The largest archive is about 256 MB, and none has 65,535 members. |
| Encryption | None. Every member reads with plain deflate. |

Take each member's data offset from its **local** header, not the central directory: the two can give different name and extra-field lengths.

## Names

Member names are relative to `core`, with forward slashes.

| Top-level folder | Contents |
|---|---|
| `campaigns/` | The campaign file `bos.cam`, and maps under `campaigns/missions/` |
| `entities/` | `.ent` prototypes |
| `gui/` | Interface art, fonts, cursors, style files, backdrops |
| `locale/` | Text tables |
| `sound/` | Sound effects and ambient sounds |
| `sprites/` | `.spr` sprites |
| `tables/` | Game rule tables |
| `tiles/` | `.til` tile prototypes |

One archive also has `keys.cfg` at its root.

The loose files in an installed `core` folder use the same names. An extracted tree and the archives are therefore interchangeable.

## Archive naming

Names follow the pattern `<family>_<suffix>.bos`: for example `tiles_0.bos`, `spr-character_ABD.bos` and `loc-mis_C.bos`.

- **Suffix `0`** marks a base archive.
- **Letter suffixes** (`A` to `F`, and combinations such as `ABCDE`) mark per-CD content sets. Their exact meaning is unverified. The campaign file's random-encounter forces carry a matching "cd code".

## Loading order and overrides

How our engine mounts a `core` folder:

1. every `*.bos` in the folder, in ascending byte-wise name order, which puts `_0` before the letter suffixes;
2. the folder itself, as loose files.

A later mount replaces an earlier one's file of the same name. So per-CD archives override base archives, and loose files override both.

The original also prefers a loose file to an archived one. The 1.27 install ships loose files that rely on this:

- corrected copies of a few sound files;
- about a hundred `.ent` prototypes.

Whether the original orders the archives themselves the same way is unverified.

## Looking names up

- **Case and separators.** Lookups ignore case and treat `\` as `/`. Maps name resources in whatever case their author typed, and the archives don't always agree with them.
- **Absolute paths.** A few shipped references store an absolute path from the original build machine, like `F:/bos/core/entities/...`. Treat everything up to and including `/core/` as part of the name only when a drive letter comes before it. `campaigns/missions/core/...` is a real folder and keeps its `core`.
- **Extension fallback.** When opening an interface image, `ZARProxy::OpenZarStreamWithFallbackExtensions@006d35d0` tries:
  1. the name as given;
  2. the name with `.zar` appended;
  3. the name with `.png` appended.

  It appends; it never replaces an extension.

## Unknown or unverified

- The original's own archive code hasn't been studied. The loading order above is our engine's; only "loose files win" is established for the original.
- What the letter suffixes mean, and how the game chooses between per-CD sets.
- Whether the original accepts any compression method other than store and deflate.
