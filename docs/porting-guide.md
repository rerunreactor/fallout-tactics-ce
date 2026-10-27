# Porting guide

This guide covers how engine work is done here: finding the original code for a behaviour, writing it as clean C++17, citing it, and checking the result. [CONTRIBUTING.md](../CONTRIBUTING.md) covers the legal side and pull requests.

## 1. Get the kit

You need:

- your own copy of the 1.27 executable: `BOS.exe` from Steam or GOG (the two are identical), or `BOS_HR.exe` from Steam;
- Ghidra 12;
- the reverse-engineering kit.

The kit loads the project's names for functions and globals, and its types, into your own Ghidra import of the executable. Its 9,438 function names are byte-verified against both executables, with two exceptions: `IsValidDisplayMode@004b3440` and `RegisterOptionsStringMappings@00650640`. Those two are the only functions where `BOS_HR.exe` differs, so they're verified for `BOS.exe` only.

Some older code comments and notes say the project's Ghidra program is the high-resolution `BOS_HR.exe`. It isn't. The program is `BOS.exe` 1.27 with a 66-byte loader patch from an old FTSE version, which loads `FTSE.DLL`.

See [kit/README.md](../kit/README.md) for setup.

The names come from several sources and carry different confidence. Some come from the community's earlier work, some are derived from run-time type information and log strings, and some are educated guesses. Treat a name as a hint; the code is the evidence.

The file formats are described separately, in [kit/formats/](../kit/formats/README.md).

## 2. Find the function

Start from a behaviour, not from an address.

1. **Find our side.**
   - Search the source for the behaviour, or for the `RECONSTRUCTION` or `PLACEHOLDER` label that marks it. [status.md](status.md) lists the open items.
   - Comments cite the original functions involved as `Name@address`. Those are your entry points in Ghidra.
2. **Find the original's side.**
   - Look the name up in the kit, or go to the address.
   - If there's no citation, look for:
     - **strings**: error messages, property names such as `"Radius (meters)"`, and locale keys;
     - **source paths**: the `phoenix\...` and `bos\...` paths in assertions mark which module you're in;
     - **virtual tables**: run-time type information names the class, and a slot number names the method across subclasses.
   - Follow callers and callees. A property name in a `Serialize` function tells you the field's offset and default.
3. **Read the pseudo-C until you can say what the function does in a sentence or two.** Note its inputs, outputs, constants and side effects.

### The decompiler can mislead you

- **Floating-point constants are often left as bare labels.** Read the bytes at the address yourself, and check the width. A double read as a float can look like 0.0. The to-hit range factor was missed that way: the double at `0x00809580` is 1.5.
- **Strings can be unnamed** when only a pointer refers to them. Search for references to the address. That's how the cone-spread string, and the shotgun's range rule behind it, were found.
- **Float-to-integer conversion truncates.** MSVC 6 calls `__ftol`, which rounds toward zero. Use truncation unless the code rounds explicitly.
- **Arithmetic precision differs.** The original works in x87 registers and stores floats. Keep its types (`float` versus `double`) where results are compared or stored.
- **The random number generator is shared.** Most rolls go through one MSVC `rand()` linear congruential generator. Use the engine's `Rng` and draw in the same order the original does; replays depend on it.
- **Some functions have no body in the export:** only a call site, or a virtual slot that was never resolved. Say so in the code. Don't fill the gap silently.
- **Struct fields are offsets.** Comments may give them as `Entity+0x9A`. That's fine and useful.

### Check your reading against the shipped data

Before porting a rule, check it against the data. For example:

- every map should still parse;
- every record should end exactly where the next begins;
- every actor's stored facing should match the formula.

Several early readings in this project were wrong in ways that only the data showed.

## 3. Write clean C++17 that matches the behaviour

- **Match the behaviour, quirks included.** Keep clamps, truncations, off-by-ones, the order of random draws, and fallbacks. Don't copy the structure: use clear names, small functions and the types already in the engine (`LiveEntity`, `Esh`, `Reader` and so on).
- **Name every constant**, and say where it came from:

  ```cpp
  constexpr float kRangeUnitsPerMetre = 1.5f;  // the double at 0x00809580
  ```

- **Put the code with its system.** Rules that don't need the scene go in `game/combat.cpp`. Per-mission behaviour goes in `game/scene.cpp`. Scripting goes in `game/script.cpp`, and file formats in `mapview/` or `common/`. [architecture.md](architecture.md) has the map.
- **Label anything the original didn't settle.**

  | Label | Meaning |
  |---|---|
  | `RECONSTRUCTION` | Worked out from the data, from measurement, or from FreeFT. Say from what. |
  | `PLACEHOLDER` | A value we chose. Say why, and what would settle it. |
  | `PORT ADDITION` | A deliberate difference from the original, such as render interpolation. Keep it switchable if you can (`--no-interp`). |

- **Keep test hooks separate.** If a check needs a way in, add a dump or a scripted option, as `--hitcalc`, `--entities` and `--trace-*` do. Don't add special cases to the rules.

## 4. Cite the original

Cite every function you port from, in a comment, as `Name@address`:

```cpp
// The to-hit sum, ComputeWeaponAttackHitChance@00615e80.
```

- Addresses are virtual addresses (the image base is `0x00400000`), and they apply to either executable. `BOS.exe` and `BOS_HR.exe` are the same size and differ only inside `IsValidDisplayMode@004b3440` and `RegisterOptionsStringMappings@00650640`. If you port either of those two, say which executable you read.
- Data is cited by address: "the table at `0x008a8a38`", "the float at `0x0082431c`".
- Use the kit's name if it has one. If you rename a function, update the kit too, so the comment and the kit agree.
- Some older comments cite the maintainers' private notes (`NOTES.md §72`, `l.12345`, `f0348`). When you touch one, replace it with evidence anyone can check: a `Name@address`, a format detail, or a measurement you describe.

## 5. Never paste decompiler output

- No pseudo-C, in code or in comments.
- No Ghidra-generated names: `param_1`, `local_2c`, `iVar3`, `FUN_00615e80`, `DAT_...` and the like.
- No listing line numbers.

Describe what the original does in prose. Mentioning a field offset, a constant or a formula is fine:

> The range penalty is four points per range unit past the free range. The free range is the range class times the attacker's Perception, plus two per rank of Sharpshooter.

A block of pseudo-C copied from Ghidra, with its generated names and casts, isn't fine, even shortened.

The same applies to pull request descriptions and issues.

## 6. Check your work

Say in your pull request which of these you ran.

### Build cleanly

- Build with warnings on, as the default build does, and read them. `-Wcomment` once flagged a `//` comment ending in a backslash: it silently swallowed the next line, a field write in the save writer.
- For memory errors, configure a sanitiser build with `-DFOT_SANITIZE=ON`.

### Data tests

The data tests run the tools and the engine over your own copy of the game. Point `FOT_DATA_DIR` at the `core` folder:

```sh
cmake -S . -B build
cmake --build build --parallel
FOT_DATA_DIR="/path/to/Fallout Tactics/core" ctest --test-dir build -j4 -L data
```

| Test | Checks |
|---|---|
| `data.unpack` | Extracts the archives the tools need into `<build>/data-tests/`. Your `core` folder is only read. |
| `data.sprview.selftest` | Every sprite decodes with no errors, and every layer fits its frame rectangle. |
| `data.sprview.dump` | `--info`, `--dump` and `--composite` work on a sample. |
| `data.sprtrim.selftest` | Every sprite repacks and trims, and reloads identically. |
| `data.mapview.selftest` | Every map parses (85 or more), with no errors, and every tile prototype resolves. |
| `data.mapview.render.1`–`4` | Every map renders, split across four tests. |
| `data.fotgame.weapons` | `--weapon-dump` reads every weapon through the archives. |
| `data.fotgame.mission` | The first mission loads with a six-member squad, runs headless, takes a screenshot and mixes its audio. |
| `data.fotgame.campaign` | The campaign's main menu comes up headless. |

Without `FOT_DATA_DIR` the data tests report themselves as skipped, as they are in CI. The `smoke.*` tests need no data. Set `FOT_DATA_KEEP=1` to keep the extracted files and renders.

### Your own runs

- **Headless runs.** `fotgame` runs without a window (`--headless --frames N`), with scripted input (`--select`, `--move`, `--attack`, `--click` and so on) and screenshots (`--shot-at N:file.png`). Compare runs before and after your change.
- **Traces and dumps.** For rules, use `--trace-combat`, `--trace-ai`, `--trace-detect`, `--hitcalc` and `--entities --stats`. A fixed `--seed` makes runs repeatable.

[running.md](running.md) lists every option.

### Against the original: difftest

For combat and character rules, record what the original computes and replay it through `fotgame`. [difftest/README.md](../difftest/README.md) explains how.

A new rule is most convincing when it reproduces records that the old one didn't.

### Against the original: side by side

For behaviour no hook reports, such as movement, AI and the interface, run the original and `fotgame` on the same situation and compare:

- the same mission;
- the same squad (the first pregenerated character is the easiest to reproduce);
- the same clock (`--time`);
- `--no-interp`, so `fotgame` moves things once per tick as the original does.

Screenshots taken at the same moment are good evidence for drawing and layout.

Run the original from a separate copy of the game folder. [difftest/README.md](../difftest/README.md#recording-under-wine) has notes on running it under Wine.

## A worked example

**The to-hit range term.**

1. In a recording, the original's hit chance fell off with distance faster than ours.
2. `ComputeWeaponAttackHitChance@00615e80` multiplies the distance by a constant at `0x00809580`. The decompiler showed it as a float, and as a float it reads 0.0.
3. The eight bytes there are the double 1.5. Every distance comparison in the function uses the scaled value: the range penalty, the close-range bonus, the melee dodge and the fallen-target check.
4. The port now names it `kRangeUnitsPerMetre`, cites the address, and applies it in all four places.
5. The same recording replays to a constant offset, which points at a separate term that is still open.

Each step had evidence that someone else can check:

- the record;
- the function and the constant's address;
- the replay.
