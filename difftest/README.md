# Differential testing against the original

The port is written from a reading of the original executable, so every rule in it is a claim about what the original does. This kit tests those claims against the original:

1. You play the original game with a logging script attached. The script records what the game's own code computes: to-hit chances, critical chances and rolls, damage rolls, the targets each attack considered, and creatures' stat blocks.
2. `compare.py` replays the same situations through `fotgame` and prints the two side by side.

| File | Purpose |
|---|---|
| `ftse/fotlog.lua` | The logger. It runs inside the original game through FTSE. |
| `compare.py` | Reads a session log and compares it with `fotgame`. |

## What you need

- **Your own Fallout Tactics 1.27.** Run `BOS.exe`; that's the executable our recordings used.
- **A separate copy of the game folder to instrument.** FTSE replaces `BinkW32.dll`, so keep your main installation untouched.
- **FTSE 0.60a**, the Fallout Tactics Script Extender, from its GitHub releases. It's MIT-licensed.
  - It installs as a proxy `BinkW32.dll` that forwards to the original, renamed `realBinkw32.dll`.
  - It patches the loaded game in memory, never the file on disk, and runs Lua 5.3 scripts from `core/FTSE/`.
  - It needs no injector, which is why it also works under Wine.
- **For comparing:** Python 3, and a `fotgame` build. See [Comparing with fotgame](#comparing-with-fotgame).

## Recording on Windows

1. **Copy your game folder** somewhere else, for example `C:\fot-difftest\`.
2. **Unzip FTSE 0.60a into the copy.** This adds `BinkW32.dll`, `realBinkw32.dll` and `core\FTSE\`.
3. **Install the logger.** Copy `difftest/ftse/fotlog.lua` over `core\FTSE\ftse.lua`.
   - FTSE's own `init.lua` loads `ftse.lua`, then registers whatever hook functions are defined.
   - Our script defines every attack-arc hook to hand control back to the game. That replaces FTSE's own Lua burst fix, so bursts resolve as the unmodified game resolves them.
4. **Choose FTSE's patches.** `core\FTSE\FTSE_config.json` turns FTSE's built-in patches on and off.
   - Its defaults apply community fixes, some of which change combat rules: the melee and unarmed critical tables, perk fixes and others.
   - For a clean 1.27 baseline, set `"apply": "false"` on every patch.
   - Whichever you choose, note it with the log. Check `FTSE.log` to see what was applied.
5. **Create a `logs` folder** in the game folder. The logger can't create folders itself.
6. **Optional: skip the movies.** Move the files out of `core\movie\` and leave the folder empty. The game skips missing movies and goes straight to the menu and into missions, which saves a few minutes a session.
7. **Run `BOS.exe`.**
   - With FTSE's default patches, the main menu shows two extra buttons.
   - `FTSE.log` lists the patches applied, the hooks installed and any Lua errors.
   - Records go to `logs\session-YYYYMMDD-HHMMSS.log`, a new file per run.

To choose the log file yourself, put a path on the first line of `core\FTSE\logpath.txt`.

## Recording under Wine

This has been tested on macOS. Linux should work the same way, but we haven't tried it.

- **Use a dedicated Wine prefix and an instrumented copy of the game folder**, set up as for Windows.
- **On macOS, the game's DirectX 7 path gave a black, full-screen window.** Use [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) to fix it:
  - put its `ddraw.dll` and a `ddraw.ini` (windowed, OpenGL renderer) in the game folder;
  - run with the DLL override, from the game folder:

  ```sh
  WINEPREFIX=/path/to/prefix WINEDLLOVERRIDES="ddraw=n,b" wine BOS.exe
  ```

- **FTSE loads and its hooks fire under Wine.**
- **Create `logs` yourself before starting.** The logger never shells out, because `os.execute` blocks forever in a prefix with no shell. If you use `logpath.txt`, give it a Wine path such as `Z:\path\to\session.log`.
- **Expect crashes.** We saw intermittent crashes under Wine, some of them without FTSE loaded. For long sessions, Windows, including a virtual machine, has been more reliable.

## What to play

Records are only comparable when `fotgame` can rebuild the same situation:

- start a **new campaign**;
- take the **first pregenerated character** on the character selection screen (`compare.py` replays it as `entities/Special/prefab/prefab1.ent`);
- play the **first mission**, the map `compare.py` loads by default.

Then, in about twenty minutes:

- **To-hit across ranges.** Hover over enemies at about 5, 10, 20 and 30 tiles, for a second or two each. The game recomputes the chance every frame while the cursor rests on a target, so hovering alone produces records.
- **Stances.** Repeat while standing, crouched and prone, against standing and crouched or prone targets.
- **Cover and point blank.** Hover through a fence or a corner, and at under 6 m in the open.
- **Fire.** Fire at least ten single shots, then bursts with an SMG, including at groups of two or three. The damage rolls come from firing.
- **Get shot.** Let enemies shoot at you while you stand, crouch and lie prone.
- **Criticals.** High-chance shots at close range produce them.
- **Creatures.** Stay near creatures for ten seconds or so. Stat blocks are logged on the game's ten-second tick.
- **Skills and loot.** Loot a container and a corpse, use first aid, and pick a lock.

The most useful recordings now:

- **Bursts with a weapon whose burst mode has a damage multiplier**, at several ranges. This settles the [burst question](#what-the-recordings-found).
- **Thrown grenades**, for radial damage.

## The log format

One record per line. Lines starting with `--` are comments: the header, and `-- lua error (handled): ...` when a hook failed and the error was caught.

```
TAG tick=<game ms> n=<record number> name=value name=value ...
```

- `tick` is the game clock in milliseconds. It's 0 until a mission is running.
- `n` counts records in the session.
- Spaces inside values are replaced with underscores, and a missing value is written as `-`.
- Distances and positions are in metres.

| Tag | When | Fields |
|---|---|---|
| `DIFF` | Once, at start-up | `difficulty`, `opponent_level_offset` and `friendly_fire`, read from `core/bos.cfg`; `source`, the file they came from. |
| `HIT` | Every to-hit calculation: the cursor's percentage, sentry checks and real attacks | `attacker`, `target`, `weapon`; `mode`, `mode_id`, `mode_shots` (the mode's ammo usage, which is the burst length); `chance` (clamped to 0–95); `raw_chance` (before the clamp); `location` (the aimed body part); `inelig` (ineligibility flags, 0 when the shot is legal); `gap_m`; positions `ax,ay,az` and `tx,ty,tz`; stances `astance` and `tstance`. |
| `CRIT` | Every critical-chance calculation | `kind` (`ranged` or `melee`), `attacker`, `target`, `weapon`, `location`, `chance`, positions, stances. |
| `CRITFX` | Every critical-effect roll | `kind`, `attacker`, `target`, `weapon`, `location`, `roll` (uncapped). |
| `DMG` | Every damage calculation | `attacker`, `target`, `weapon`; `mode`, `mode_id`, `mode_shots`, `spread`, `mode_dmgmult`, `mode_range`; `location`; `damage` (final); `shots`; `rolls` (each shot's roll before armour, separated by `\|`); `crit` (the critical flags in force, or `none`); `gap_m`; positions, stances. |
| `ATTACK` | Every attack arc: straight, burst, spray, cone, area, radial, projectile | `kind`, `attacker`, `weapon`, `shots`; `candidates`, a comma-separated list of `name:hit_chance:distance:angle:intended`. |
| `ENT` | A creature's first ten-second tick, every change of its hit points, and every hit | `why` (`spawn`, `hp_change`, `death` or `after_damage`); `name`, `internal`, `class`, `race`, `id`, `tag`; `hp`; `maxhp`, `maxhp_perm`, `maxhp_derived`; `ac`, `ac_perm`, `ac_derived`, `bonus_ac`; `ap`, `maxap`, `maxap_perm`; `armour`, `hand_r`, `hand_l`; `melee_dmg`, `crit_chance`, `heal_rate`; `special` (seven values); `skills` (eighteen); `x`, `z`. |
| `MOVE` | On the ten-second tick, for entities that have moved | `actor`, `x`, `y`, `z`, `from_x`, `from_z`. |
| `VAR` | Every mission or campaign variable write | `key`, `value`, `campaign` (`true` or `false`). |

### Pitfalls

- **Hovering logs a `HIT` record every frame**, so expect tens of thousands. Group them.
- **A record with `inelig` other than 0 isn't about the to-hit formula.** It's the number the game produced for a shot it had already ruled out. In one session, 51,178 of 160,213 `HIT` records were like that.
- **Not every `HIT` record is a weapon.** A first aid kit in hand produces them too, as previews of a skill check. `compare.py` reports those separately.
- **A target named `entity` is the null entity.** Its position is the map origin.
- **`gap_m` is the distance between centres.** The to-hit formula uses the gap between bounding boxes, which for two people is about 1.5 m less. The cone damage rule does use the centre distance.
- **Names differ between the two sides.** The logger writes display names. The `ENT` records supply each one's internal name, which is what `fotgame` knows.
- **Stances are read through an experimental FTSE call** (`CallVtable` slot 236), so they're sometimes missing.
- **FTSE has no hooks for movement, sight or detection.** `MOVE` is a coarse track sampled every ten game seconds.

## Comparing with fotgame

```sh
FOT_ASSETS="/path/to/Fallout Tactics/core" python3 difftest/compare.py session-20260101-120000.log
```

### Setup

- **`compare.py` runs `game/fotgame`**, relative to the repository, with `game/` as the working directory. With a CMake build, link or copy the binary there:

  ```sh
  ln -s ../build/bin/fotgame game/fotgame
  ```

- **Give `FOT_ASSETS` as an absolute path.** The default, `core`, is resolved both from `game/` and from your own working directory.
- **The weapon-name map reads `locale/game/items.txt` directly from `FOT_ASSETS`.** A 1.27 install has it as a loose file. If yours doesn't, extract it from `locale_0.bos` into the same place.

### Options

| Option | Default | Meaning |
|---|---|---|
| `--mission <path>` | `campaigns/missions/core/mission01.mis` | The map to replay against. |
| `--squad <ent,...>` | the first pregenerated character and two recruits | The squad `fotgame` places. It has to include every attacker you want replayed. |
| `--difficulty <d>` | `auto` | Read it from the session's `DIFF` record. Otherwise `0`–`4`, `easy`, `normal`, `hard`, or a multiplier. |
| `--limit N` | 24 | How many `HIT` groups to replay. |
| `--min-count N` | 3 | The smallest group worth replaying. |

### Output

It prints five tables, then the worst differences.

| Table | What it compares |
|---|---|
| `ENT` | Each creature's hit points, armour class and action points: the original's against `fotgame --entities --stats`. Joined on the internal name and the race. |
| `HIT` | Records grouped by attacker, weapon, target and range. Each group is replayed through `fotgame --hitcalc`, which places the target at that range and prints every term of our sum, so you can see which term differs. A group whose replay lands more than 0.75 m off the asked-for range isn't counted. |
| `SKILL previews` | The non-weapon `HIT` records. |
| `DMG` | Each record's per-shot rolls against our damage band for the same weapon, mode and range. |
| `CRIT` | Each critical chance against our rule: a tenth of the hit chance, plus the weapon's, the mode's and the attacker's own critical chance. Needs no replay, because the paired `HIT` and `ENT` records carry every input. Burst records can't be paired, because each burst shot uses its own chance. |

## What the recordings found

Three sessions were analysed, all on the first mission. They were recorded on Windows with FTSE's default patch set, as far as we know.

### Fixed in the port

| Finding | Detail |
|---|---|
| **The to-hit distance is in range units** | The original multiplies the metre gap by 1.5 (the double at `0x00809580`) before every distance test: the range penalty, the close-range bonus, the melee dodge and the fallen-target check. Past the free range, that makes the penalty 6 points per metre, not 4. |
| **Melee dodge** | Depends on the target's equipped weapon, not the attacker's. |
| **Armour** | Comes from the instance's `Armour` reference, which resolves to the real item, unless the instance sets `Armour_override`. The prototype's embedded copy can be stale. |
| **Large and Huge bodies get no armour** | Matches the recordings. Our explanation: no armoured sprite exists for those bodies, so the armour is never put on. |
| **`Modifiers`** | A second attribute block, added on top of `Attributes`. |
| **Equipping armour** | Must update armour class, damage threshold and resistance, and the skill changes the armour carries. |
| **Damage** | Rolls, then the ammunition multiplier, then threshold, then resistance. Reproduces every checked shot's final damage exactly. |
| **Shotgun damage scales with range** | `clamp(2.5 − 2 × centre distance / weapon range, 0.2, 2.5)` multiplies the damage band. It was measured from 28 rolls, then found in `ApplyAreaDamageAttackWithFactionCheck@00615100`, with the clamp in `CombatAI_EvaluateAttackApproachAndDamage@00612170`. All 28 rolls fall inside our band. |
| **Critical chance** | The rule matches every single-shot record that pairs cleanly with its `HIT` record: 13 of 13 in one session. |
| **Levelled map actors** | Get experience for their stored `Level` and the per-level hit point grant. The hit points of the five levelled actors in the first mission now match exactly. Skill differences remain from the random part of the original's skill spending. |
| **The difficulty table** | Recovered from `GetDifficultyModifierForAttackerTarget@0061b990`; the logger now records the setting. |

### Still open

- **A constant +2 on every attack by the player's squad** in the first session. No difficulty setting produces it.
- **The burst damage multiplier.** Two sessions disagree about whether burst rolls include the mode's damage multiplier. The logger now records the mode; a session with burst fire will settle it.
- **To-hit residuals of several points** in grouped comparisons. Settling them needs per-record replay, which needs positions. The logger records positions now, but no analysed session has them yet.
- **Critical-chance records that contradict our rule:** 3 of 19, 12 of 64 and 13 of 36 across the three sessions. They include burst shots, which can't be paired with their `HIT` record this way. Per-record replay would settle these too.
- **One recruit's maximum hit points:** 33 in the original, 36 in ours.
- **Six damage records outside our bands:** a grenade, an SMG burst, and some ammunition variants.
- **Radial (grenade) damage** hasn't been examined: `ApplyRadialWeaponAttack@006157b0`.

## Known issues

- **`compare.py` assumes the old build layout and two working directories.** See [Setup](#setup).
- **Wine instability.** See [Recording under Wine](#recording-under-wine).

## Changing the logger

- **Read FTSE's documentation of its hooks and accessors** in the FTSE repository.
- **Keep every hook a pass-through:** log, then return exactly what the engine passed in. An attack-arc hook must return `nil`.
- **Don't let an error escape.** FTSE unloads the whole script on a Lua error, so wrap every hook body as `fotlog.lua` does.
- **Don't call `world:GetTime()` before a mission is running.** It can take the whole process down.
- **Never call `os.execute`.**
- **If you add a field, teach `compare.py` to read it, and keep old logs readable.** `record_gap()` is an example: it falls back to `gap_m` when a log has no positions.
