# Reverse-engineering kit

This kit loads our names and type layouts for Fallout Tactics: Brotherhood of Steel 1.27 into Ghidra, on **your own copy** of the game's executable. It has function names, global names, function signatures and struct layouts, as plain text, plus two Ghidra scripts.

It never holds any of the game: no code, no bytes, no text, no disassembly or decompiler output. Only names, addresses, sizes, signatures and type layouts.

## What you need

- **Your own copy of the game.** Steam ships both `BOS.exe` and the hi-res `BOS_HR.exe`; GOG ships `BOS.exe`. The kit works with either.
- **The unmodified file.** The scripts check the file's SHA-256 and refuse anything else, including a copy you have patched yourself (FTSE, a hi-res patch tool, a no-CD crack).

  | File | SHA-256 |
  |---|---|
  | `BOS.exe` (Steam and GOG, 1.27) | `c5b91fe56ef8de118ce22ccd5e6512f217ca16c8b8758ea14c626229eb5a4e40` |
  | `BOS_HR.exe` (Steam) | `ae7ddade7f3c5c48552b54f72b795b3919477b789bba7c910db46c8e75c812d0` |

  To check yours: `shasum -a 256 BOS.exe` on macOS or Linux, `certutil -hashfile BOS.exe SHA256` on Windows.
- **Ghidra 12.x.** The kit was tested with 12.1.3.

## Using it

1. **Import** the executable into a Ghidra project: *File > Import File*, accept the defaults (format Portable Executable, language `x86:LE:32:default`, compiler `windows`). Leave the image base at `0x400000`.
2. **Analyze** it with the default options.
3. **Run `ApplyFotSymbols`.**
   - In the Script Manager, open *Manage Script Directories* and add this kit's `ghidra` folder. Find `ApplyFotSymbols.java` under the `FOT` category and run it with the program open.
   - Or headless, in one go (import, analyze, apply):

     ```
     analyzeHeadless <projectDir> <ProjectName> -import /path/to/BOS.exe \
       -scriptPath <kit>/ghidra -postScript ApplyFotSymbols.java
     ```

     On a program you have already imported and analyzed, use `-process BOS.exe -noanalysis` instead of `-import`.
4. **Read the counts** the script prints, then save the program.

The script takes well under a minute; the analysis before it takes a few minutes. This is what it did on a fresh analysis of `BOS.exe` (Ghidra 12.1.3, default options):

- 143 types added, 1 already there.
- 9,432 of 9,438 function names applied. For 3,020 of them Ghidra's own analysis had found no function at the address, so the script disassembled there and created one.
- 1,826 of 1,827 signatures applied.
- 31 global data names, 26 of them with a data type.
- Not applied: 2 names whose address is inside a larger function in your analysis, 4 names where Ghidra had already identified a C-library function, and 1 signature that Ghidra rejects.

`BOS_HR.exe` is the same, except two functions that the hi-res build changes (see below).

**What the script changes in your database.** Names and namespaces, signatures, struct and typedef types (in the `/FOT` category), data labels and data types, and functions it creates. The only comment it writes is a `[agent conf=...]` line on AI-made names (see below). Nothing is saved until you save.

**Running it again is safe.** It records what it applied. A second run changes nothing, except for rows that could not be applied the first time and now can. A name you changed yourself afterwards is left alone, a name the kit changed since your last run is updated, and names Ghidra took from the C library are kept. Options:

| Option | Effect |
|---|---|
| `dir=<folder>` | The kit's `symbols` folder. Default: `../symbols` next to the script. |
| `minconf=medium` or `high` | Skip AI-made names below that confidence. Default: apply all. |
| `tags=no` | Do not write the `[agent conf=...]` comment lines. |
| `overwrite=yes` | Replace names and signatures you changed yourself. |
| `types=no`, `functions=no`, `data=no` | Skip a part. |

With `analyzeHeadless`, put options after the script name: `-postScript ApplyFotSymbols.java minconf=medium`.

If it stops with a message instead of running: `Refusing to run: this program's SHA-256 is ...` means the file is not one of the two above (patched, or a different version). `Cannot find symbols/functions.tsv` means the script was copied away from the kit; pass `dir=<path to the kit's symbols folder>`.

## What is in the kit

```
ghidra/
  ApplyFotSymbols.java      loads the symbols into your program
  ExportFotSymbols.java     regenerates the symbol files (for maintainers)
symbols/
  functions.tsv             named functions: address, size, name, signature, source, confidence, verified
  functions-unverified.tsv  named functions that match neither executable
  data.tsv                  named global data
  types.h                   struct and typedef layouts, as C
  verification.txt          where the database differs from each executable
formats/                    notes on the game's file formats
LICENSE
```

The engine's source cites the original as `Name@address`, for example `ComputeWeaponAttackHitChance@00615e80`. Look the name up in `functions.tsv`.

## Reading the files

`functions.tsv` and `data.tsv` are tab-separated, sorted by address, with a header row. Addresses are 8 hex digits, as Ghidra shows them for an image based at `0x400000`.

| Column | Meaning |
|---|---|
| `address`, `size` | Entry point, and the size in bytes of the function in the database we made the names in. A fresh analysis can draw the boundary a little differently. |
| `name` | With its namespace, for example `Light::GetRenderLightLevel`. `Class::virtN` is a placeholder for virtual function number N of a class. |
| `signature` | As Ghidra prints it, when someone set one: calling convention, `this`, parameter types. Functions with custom variable storage have none here. |
| `source` | Where the name came from (next section). |
| `confidence` | `high`, `medium` or `low`, for AI-made names. Blank otherwise. |
| `verified_bos`, `verified_bos_hr` | `yes` when the function's bytes are identical to the same address in that executable (see below). The script applies only `yes` rows. |

### Where the names came from

| `source` | Rows | What it is |
|---|---|---|
| `ftse` | 1,747 | Names people wrote in FTSE's Ghidra database, the work this kit is built on. The signatures and the struct layouts are theirs as well. |
| `deterministic` | 2,568 | Names a script derived without guessing: class and virtual-function names from the executable's own RTTI and vtables, constructors and destructors, and the class and method names in its log messages. Many are placeholders such as `virt12`. |
| `agent` | 5,123 | Names AI agents proposed after reading the decompiled function. |

**Agent names are guesses.** Each agent rated its own guess from 0.3 to 0.95. We bucket that as `high` (0.85 and up), `medium` (0.5 to 0.8) and `low` (below 0.5). Of the 5,123: 313 high, 2,495 medium, 2,315 low. Treat `high` and `medium` as leads to check, and `low` as a hint only. The script marks every one with a `[agent conf=...]` comment line so you can tell a guess from a fact when you meet one.

A name can also be less specific than you would expect. For example, `StartNextCombatTurn` at `00507610` has no `World::` in front, because that is what the database holds. A better name is a welcome contribution.

### What "verified" means

The names were made in a Ghidra database of an executable that is `BOS.exe` plus a small patch. The patch (66 bytes, in four places in the code and one in the data) looks like the loader hook that older FTSE versions added to the executable. `BOS_HR.exe` is `BOS.exe` with six single-byte changes, in two functions.

So every function was compared byte for byte, at its address, with each of the two executables. The file offset comes from the executable's PE section table. Both files and the database are linked at `0x400000`, so no relocation masking is needed. The ranges that differ are in `symbols/verification.txt`.

- **9,436** of the 9,438 functions are identical in both executables.
- **2** are identical in `BOS.exe` only, because they contain bytes the hi-res build changes: `IsValidDisplayMode` (`004b3440`) and `RegisterOptionsStringMappings` (`00650640`). They are `no` for `BOS_HR.exe`.
- **1** matches neither, because it contains part of the loader patch: `GameApplication::MainInit` (`006c4180`). It is only in `functions-unverified.tsv`.
- **Data:** a global's bytes must match; for uninitialized globals, which have no bytes to compare, a verified function must use them. All 31 are verified for both.

A function being verified says the code at that address is the same. It does not say the name is right: that is what `source` and `confidence` are for.

### What is left out

- Default names (`FUN_...`, `thunk_...`) and the names Ghidra's own analysis makes (`Unwind@...`, `Catch@...`, C-library functions it identifies). Your analysis recreates them.
- 28 signatures that rely on custom variable storage, which a printed prototype cannot express.
- Every comment, string and byte of the game, and everything from a decompiler. `ExportFotSymbols` looks at a function's comment only to read the `[agent conf=...]` tag and to spot the "Library Function" note Ghidra's analysis leaves; none of that text is copied.

## Types

`types.h` is in the style of Ghidra's *Export C header*. Fields are packed exactly as listed (`#pragma pack(1)`) and gaps are written out as undefined bytes, so a struct's size and offsets are exact. Names that are not valid C, such as `Actor::ClassType`, use `__`: `Actor__ClassType`. Types Ghidra makes itself (PE headers, RTTI, exception handling) are not in it.

You can also load it by hand with *File > Parse C Source*, but `ApplyFotSymbols` does more: it keeps types you already have, completes empty placeholder structs with the real layout, and names the function-pointer types of the vtable structs.

## Refreshing the files

`ExportFotSymbols` writes the files in `symbols/` from a Ghidra database. It is for maintainers, and for checking a database of your own against the kit. It needs both executables, because it checks every name against each of them, and it stops if either has the wrong hash.

```
analyzeHeadless <projectDir> <ProjectName> -process <program> -noanalysis -readOnly \
  -scriptPath <kit>/ghidra \
  -postScript ExportFotSymbols.java <kit>/symbols /path/to/BOS.exe /path/to/BOS_HR.exe
```

The output is sorted and has no dates, so `git diff` shows exactly what changed. It also parses the `types.h` it wrote with Ghidra's own C parser and checks every struct's layout against the original.

A name is `ftse` when Ghidra records it as user-defined, `deterministic` when it records it as analysis, and `agent` when it was imported and carries the `[agent conf=...]` tag. Ghidra cannot tell who typed a name, so names you rename by hand in your own database come out as `ftse`.

## Contributing

Better names, signatures and types are welcome. That is much of the point of publishing the kit.

- **Change the rows.** Edit `functions.tsv`, `data.tsv` or `types.h` directly. Keep the files sorted by address and tab-separated. Give a new or changed name `community` in the `source` column and leave `confidence` blank.
- **Give your evidence.** Say how you know: what the function does, what calls it, which executables you checked. A pull request that changes a name on a guess is no better than the guess it replaces.
- **Keep the kit clean.** No decompiler output, no disassembly, no bytes, no strings from the game, and no comments copied from a database. Names, addresses, signatures and type layouts only. Never commit a Ghidra project, a `.gar` archive or an executable: they contain the game.
- **Check the verified columns.** If you add a row for an address, run `ExportFotSymbols` or compare the bytes yourself, so `verified_bos` and `verified_bos_hr` are true.

See the repository's [CONTRIBUTING](../CONTRIBUTING.md) for the contributor agreement and the rest of the rules.

## License

MIT, in `LICENSE`. The names and type layouts began as the Ghidra database published with [FTSE](https://github.com/melindil/FTSE) (MIT, by melindil), so `LICENSE` carries FTSE's notice as well.
