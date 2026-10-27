# Running fotgame and the tools

Everything here runs against your own copy of Fallout Tactics: Brotherhood of Steel 1.27. This repository contains no game files.

## What you need

- **The game, version 1.27, from Steam or GOG.** We develop against the Steam release. GOG's `BOS.exe` is identical to Steam's.
- **The `core` folder of your installation.** The examples on this page call it `/path/to/Fallout Tactics/core`. It holds:
  - about 40 `.bos` archives (`tiles_0.bos`, `entities_0.bos`, `spr-sprites_0.bos`, `Mis-Main_0.bos` and the rest);
  - loose folders, including `campaigns`, `entities`, `gui`, `locale`, `missions`, `music` and `sound`;
  - the game's settings files, `bos.cfg` and `keys.cfg`.
- **A build.** See the [README](../README.md). With CMake, the programs end up in `build/bin/`: `fotgame`, `mapview`, `sprview` and `sprtrim`.

`fotgame` reads the `.bos` archives in place. The three tools read loose files, so they need the archives extracted first; see [Extracting the archives](#extracting-the-archives).

Nothing here modifies your installation, with one exception: **saves**. By default `fotgame` writes them to `<core>/user/save`, the folder the original uses. Until the original has been shown to load our saves (see [status](status.md)), keep them apart with `--save-dir`.

## fotgame

### The campaign

```sh
fotgame --campaign --assets "/path/to/Fallout Tactics/core" --save-dir ~/fot-saves
```

This opens the title menu. To start playing:

1. Choose single player, then a new game.
2. Take one of the pregenerated characters on the character selection screen.
3. Travel on the world map, and click a location to enter its mission.

Movies don't play, because Bink isn't supported.

Saving and loading from the in-mission menu need `--campaign`. Without it, the menu says so.

### One mission

```sh
fotgame campaigns/missions/core/mission01.mis \
    --assets "/path/to/Fallout Tactics/core" \
    --campaign-start \
    --squad entities/recruits/hm01.ent,entities/recruits/hm02.ent,entities/recruits/hf01.ent
```

The mission argument is either a file on disk or a resource name inside the archives, as here.

`--campaign-start` sets the mission up the way the campaign would:

- it removes the map's placeholder squad;
- it places the `--squad` members at the map's spawn point;
- it has the quartermaster issue their starting equipment.

Without `--campaign-start`, the map loads as stored, placeholder actors included.

If you run `fotgame` from the folder that contains `core`, you can leave out `--assets`.

### Controls

These are the game's own bindings, taken from its `help.txt` and `keys.cfg`. `fotgame` builds them in rather than reading `keys.cfg`, and implements only part of that file.

| Input | Action |
|---|---|
| Left click on ground | Move the selection. Shift + click runs. |
| Left click on a squad member | Select only them. Shift + click adds or removes them. |
| Left drag (more than 15 px) | Band-select |
| Left click on an enemy | Fire the active weapon |
| Right click | Fire. Ctrl + right click forces fire. |
| Left click on a person | Talk, if they have something to say |
| Left click on an object | Use it: open a container or corpse, pick up an item, work a door |
| F1–F6, Shift + F1–F6 | Select member 1–6; add member 1–6 |
| F12 | Select the whole squad |
| F7–F11, Ctrl + F7–F11 | Recall or store a selection group |
| Space | Next member, and centre the view on them |
| Arrow keys, screen edge | Scroll |
| I, S, C, P, M | Inventory, skilldex, character, Pip-Boy, map |
| Q, A, Z | Stand, crouch, prone |
| W, R, Y, U | Stop, reload, weapon mode, swap hands |
| Backspace | End turn |
| `,` `.` `/` | Sentry off, scout, on |
| 1–8 | Skills: sneak, lockpick, steal, traps, first aid, doctor, science, repair |
| Escape | Menu |
| Alt + X | Quit |

Two bindings are ours, not the game's:

- double-clicking a squad tab centres the view on that member;
- right-clicking cancels an armed skill cursor.

`bos.cfg`'s `game.runMode` decides whether a plain click walks or runs (0 always walk, 1 smart run, 2 always run). Shift inverts it.

These `keys.cfg` bindings aren't implemented yet:

- Return: turn-based on or off;
- Ctrl + Space: end the squad's turn;
- T: targeted shot;
- F: formation;
- O, H and Tab: options, help and scores;
- the save, load, quick-save and quick-load keys;
- Pause;
- the minimap keys, the rotate keys and the use-hand keys.

For now, turn-based play is available only through `--turnbased`.

### Options

The tables below list every option `fotgame` parses. Running `fotgame` with no arguments prints a shorter summary.

Some values need explaining:

- **Positions** are world units, four to the metre. `y` is height.
- **Entity indices** (`i`) are the scene's own indices, not slot numbers in the map file. At start-up `fotgame` prints the spawn point and the squad members' indices: `squad 3 placed [i] ...`.
- **Scripted options** with an `N:` prefix act at frame `N`. They can be repeated.

#### Data and saves

| Option | Meaning |
|---|---|
| `<mission>` | A `.mis` path on disk, or a resource name such as `campaigns/missions/core/mission01.mis`. Not needed with `--campaign` or the data dumps. |
| `--assets <dir>` | Your `core` folder. Default: `core` in the working directory. |
| `--tiles <dir>`, `--sprites <dir>`, `--ents <dir>` | Separate roots for extracted trees. Rarely needed. |
| `--archive <file.bos>` | Mount one more archive after the others. |
| `--gui <dir>` | Root of `gui/` and `locale/`. Default: the assets root. |
| `--resources-verbose` | List every mount at start-up. |
| `--save-dir <dir>` | Where saves go. Default: `<assets>/user/save`. |

#### Starting play

| Option | Meaning |
|---|---|
| `--campaign` | Start at the title menu with the campaign shell. |
| `--campaign-start` | Remove the map's placeholder squad and place the squad at the spawn point. |
| `--squad <ent,ent,...>` | The squad's `.ent` prototypes, for example `entities/recruits/hm01.ent`. |
| `--issue`, `--no-issue` | Turn the quartermaster's starting issue on or off. Default: on with `--campaign-start`. |

#### Settings

| Option | Meaning |
|---|---|
| `--run-mode 0\|1\|2` | Always walk, smart run, always run. Default: `bos.cfg`. |
| `--difficulty <d>` | Combat difficulty: `0`–`4`, or `easy`, `normal`, `hard` for 0–2. Any other number is used as the multiplier itself. Default: `normal`. `bos.cfg`'s difficulty isn't read. |
| `--seed <S>` | Seed of the combat random number generator. Default: 1. |
| `--no-ai` | Enemies don't act. |
| `--no-script` | Don't run the mission's triggers. |
| `--no-sound` | Don't open the audio device. |
| `--audio-volumes m,mu,sfx,sp` | Master, music, effects and speech volume, 0–100. Default: `bos.cfg`. |

#### View and drawing

| Option | Meaning |
|---|---|
| `--size w,h` | Window size. Default: `1024,768`. |
| `--centre x,z` or `x,y,z` | Start the camera on a world point. `--center` also works. |
| `--time HH:MM` | Set the world clock, which drives the lighting. The default world time is 06:29 on the first day. |
| `--lights on\|off` | Light entities on or off. |
| `--light-base clock\|map\|white` | Base lighting: from the world clock (the default, as the game does), from the map's stored light rig (an editor snapshot), or plain white. |
| `--inside x,z` or `x,y,z`, `--inside-k K` | The roof-fade reference point, and its radius scale. |
| `--markers` | Draw editor-only markers: waypoints, lights, spawn points. |
| `--no-interp` | Draw entities where the last tick left them, as the original does. |
| `--no-ui` | Don't draw the interface. |
| `--hud` | Draw a debug combat console. This isn't the game's interface. |
| `--path` | Draw the selected unit's path. |
| `--follow` | Keep the selection centred. |
| `--fps` | Print the frame rate once a second. |

#### Running without a window

| Option | Meaning |
|---|---|
| `--headless` | No window. Each frame advances 33 ms. |
| `--frames N` | Run N frames, then exit. Implies `--headless`. |
| `--bench N` | Scroll for N frames, then report the frame rate. Implies `--headless`. |
| `--screenshot <file.png>` | Write the last frame when `--frames` or `--bench` ends the run. |
| `--shot-at N:<file.png>` | Write frame N. |
| `--shot-every T:<prefix>` | Write a PNG every T ticks. |

#### Scripted input

| Option | Meaning |
|---|---|
| `--click N:x,y[,shift][,ctrl][,right]` | A mouse click through the normal input path, in screen pixels. |
| `--band N:x0,y0,x1,y1` | Band-box selection. |
| `--scroll N:dx,dy` | Scroll the view. |
| `--select N:i` | Select entity i. |
| `--move N:x,z`, `--run N:x,z` | Order the selection to a point. |
| `--posture N:Stand\|Crouch\|Prone` | Change the selection's stance. |
| `--posture-at N:i,Stand\|Crouch\|Prone` | Change entity i's stance. |
| `--attack N:i` | The selection attacks entity i. |
| `--mode N:<mode>` | Pick the active weapon mode by name, such as `single` or `burst`. |
| `--turnbased N:on\|off`, `--endturn N:1` | Switch turn-based mode; end the active turn. |
| `--seed-at N:S` | Reseed the combat random number generator. |
| `--use N:i` | The selection uses entity i. |
| `--talk N:i` | Talk to entity i. |
| `--skill N:<name>[,i]` | Use a skill, on entity i if given. |
| `--loot-take N:i`, `--loot-put N:i` | Move a row between the two halves of the loot window. |
| `--loot-all N:1`, `--loot-close N:1` | The loot window's take-all button; close the window. |
| `--sentry N:i,off\|scout\|on` | A squad member's sentry mode. |
| `--vehicle N:<verb>:<args>` | `mount:actor,vehicle`, `dismount:actor`, `drive:vehicle,x,z` or `fire:vehicle,target`. |
| `--ui-screen N:<name>` | Open `inventory`, `character`, `pipboy`, `skilldex` or `menu`, or `none` to close. |
| `--ui-action N:<action>` | Act as if the key bound to a `keys.cfg` action had been pressed. |
| `--ui-hover N:x,y`, `--ui-click N:x,y` | Move the pointer; click. |
| `--ui-press N:x,y`, `--ui-release N:x,y`, `--ui-drag N:x0,y0,x1,y1` | Press and release separately; drag an item. |
| `--cam N:<verb>[:<arg>]` | A campaign shell action. Needs `--campaign`. The verbs are listed below. |

These are for tests only:

- `--place N:i,x,z`
- `--hittest N:target,count`
- `--give N:i,<ent path>[,count]`
- `--hurt N:i,<hp>`
- `--teleport N:<tag>,x,z`
- `--kill N:<tag>`
- `--speech N:<tag>=<node>`
- `--setvar N:<name>=<value>`
- `--cursor N:i` or `--cursor N:x,y`
- `--rand-move <player>|all`, which forces random wandering on.

The `--cam` verbs:

| Verb | Action |
|---|---|
| `newgame` | Start a new game. |
| `screen:<name>` | Open a screen: `title`, `menu`, `tutorials`, `custom`, `charselect`, `briefing`, `map`, `recruit`, `save`, `load`, `loading` or `endscreen`. |
| `char:<i>` | Show pregenerated character i. |
| `take:<i>`, `drop:<i>` | Move a recruit between the pool and the roster. |
| `quartermaster` | Run the quartermaster's issue. |
| `travel:<location key>` or `travel:x,y` | Travel on the world map. |
| `encounters:on\|off` | Turn random encounters on or off. |
| `enter[:<key>]`, `exit` | Enter a mission (the one under the squad if no key is given); leave the mission. |
| `brief:<mission>`, `debrief:<key,key,...>` | Open a briefing or a debrief. |
| `pip:<0-4>`, `pipscroll:<n>` | Pick a Pip-Boy tab; scroll it. |
| `save:<path>`, `load:<path>` | Save or load. |
| `click:x,y` | Click on a campaign screen. |
| `loading:<1-7>,<percent>`, `endscreen[:<path>]` | Show a loading screen or an end screen. |
| `dump` | Print the campaign state. |

#### Dumps and traces

These print and exit, unless the table says otherwise.

| Option | Output |
|---|---|
| `--triggers` | The mission script. |
| `--ai-dump` | The parsed AI layer. |
| `--entity-dump [substr]` | The raw entity slot table, with every property. |
| `--entities [--stats]` | One `ENT` line per combatant, in the format of the difftest logger. |
| `--hitcalc attacker,target,weaponEnt,gap[,mode]` | Every term of the to-hit sum, and the damage band, for one setup. Repeatable. `difftest/compare.py` uses it. |
| `--weapon-dump [substr]` | Every weapon `.ent`, with its damage and modes. Needs only `--assets`. |
| `--ent-props <path>` | Every property of one `.ent`. Needs only `--assets`. |
| `--image-dump <rel>[:<out.png>]` | Loads one image and reports which decoder handled it. |
| `--campaign-dump` | The campaign and its mission graph. |
| `--vehicle-census` | Every vehicle in the core maps. |
| `--manifest <file>` | Every asset file the mission load opened. |
| `--nav-png <file.png>` | The navigation grid as an image. The run continues. |
| `--farthest` | The reachable cell farthest from the spawn. The run continues. |
| `--resource-manifest <file>` | Every resource the run read, written at exit. The run continues. |
| `--sprite-manifest <file>` | Every sprite sequence the run drew, written at exit; `sprtrim` takes this list. The run continues. |
| `--loot-dump` | Every container, corpse and ground item, at the end of the run. |
| `--trace`, `--trace-move`, `--trace-every N` | Animation and movement state. The run continues, as it does for every option below. |
| `--trace-combat`, `--trace-ai`, `--trace-detect` | Combat, AI and detection state. |
| `--trace-screen i[,j...]` | The screen position of the given entities on every frame. |
| `--script-log`, `--audio-log` | The trigger engine's log; one line per sound event. |
| `--audio-dump <file.wav>` | Mix the run's audio offline and write it. |

The environment variable `FOTGAME_ANIM_TILES=1` lists the animated tile prototypes when a map loads.

### Examples

A five-second headless run that saves a screenshot, as the data tests do:

```sh
fotgame campaigns/missions/core/mission01.mis --assets "/path/to/Fallout Tactics/core" \
    --campaign-start --squad entities/recruits/hm01.ent,entities/recruits/hf01.ent \
    --headless --frames 150 --no-sound --save-dir /tmp/fot-saves --screenshot mission01.png
```

Order a squad member to walk, then photograph it mid-walk:

```sh
fotgame campaigns/missions/core/mission01.mis --assets "/path/to/Fallout Tactics/core" \
    --campaign-start --squad entities/recruits/hm01.ent \
    --headless --frames 120 --select 5:<i> --move 10:<x>,<z> --shot-at 60:walk.png
```

Take `<i>` from the `squad ... placed [i]` line printed at start-up. Pick `<x>,<z>` near the spawn point printed on the same line; positions are in world units.

## Extracting the archives

`mapview`, `sprview` and `sprtrim` read loose files. A `.bos` file is an ordinary zip archive, so any unzip tool can extract it.

Extract the archives in ascending file-name order into one folder, letting later files overwrite earlier ones. That reproduces the engine's mount order, in which per-CD archives patch the base ones.

```sh
mkdir -p extracted
for f in $(cd "/path/to/Fallout Tactics/core" && LC_ALL=C ls *.bos); do
    unzip -o -q "/path/to/Fallout Tactics/core/$f" -d extracted
done
```

The engine also lets loose files in `core` override archive members. The 1.27 install ships a few: some corrected sound files, and about a hundred `.ent` files. To match the engine exactly, copy the loose folders in `core` over `extracted/` afterwards.

The data tests do this extraction themselves, into `<build>/data-tests/unpacked`. Set `FOT_DATA_KEEP=1` to keep the result.

## mapview

`mapview` parses maps, renders them to PNG, dumps tiles and checks the whole map set.

```sh
# Map summary: sections, tile prototypes, regions, entities
mapview extracted/campaigns/missions/core/mission01.mis --info

# List entities, with their prototypes and sprites resolved
mapview extracted/campaigns/missions/core/mission01.mis --entities \
    --ents extracted/entities --sprites extracted/sprites

# Render the whole map at 1/4 scale
mapview extracted/campaigns/missions/core/mission01.mis \
    --tiles extracted/tiles --sprites extracted/sprites --ents extracted/entities \
    --render mission01.png --scale 4

# Dump one tile's frames as PNG
mapview --til "extracted/tiles/<some tile>.til" --dump tile-out

# Check every map under a folder
mapview --selftest --tiles extracted/tiles --sprites extracted/sprites \
    --ents extracted/entities extracted/campaigns
```

| Option | Meaning |
|---|---|
| `--info` | Print the map summary. |
| `--entities` | List entities. `--ents` and `--sprites` resolve their prototypes and sprites. |
| `--instances [--region x0,z0,x1,z1] [--name <substr>]` | List tile placements. Use it together with `--info`. |
| `--render <out.png>` | Render the map. Needs `--tiles`; entities need `--sprites` and `--ents` too. |
| `--scale N` | Shrink the render by N. |
| `--region x0,z0,x1,z1` | Draw only placements whose minimum corner falls inside this ground rectangle, in world units. |
| `--crop x,y,w,h` | Crop the output, in pixels of the unscaled render. |
| `--centre x,z` or `x,y,z`, `--size w,h` | Crop a window around a world point. The default size is `800,600`. Give the height (`x,y,z`), or the crop lands 7×y pixels off. |
| `--bg AARRGGBB` | Background colour. |
| `--max-elev N` | Skip tiles above this height. |
| `--no-entities`, `--markers`, `--show-invisible` | Leave out entities; draw editor markers; draw the invisible tiles the game hides. |
| `--campaign-start`, `--squad <ent,...>` | Place a squad at the spawn point, as `fotgame` does. Bare names are looked up under `entities/recruits/`. |
| `--tint on\|off` | Apply the entity colour records, or don't. |
| `--ambient L` or `r,g,b` or `map` | Override the ambient light: one multiplier `L`, or a colour with 0–255 components. `map` uses the map's stored rig, which the shipped game doesn't. |
| `--lights on\|off`, `--light-base clock\|map\|white`, `--time HH:MM`, `--tick N` | Lighting, as in `fotgame`. `--tick` advances the lights' flicker by N steps. |
| `--order depth\|screeny`, `--layer-order s0,s1,s2,s3` | Experiments with draw order and sprite layer order. |
| `--inside x,z` or `x,y,z`, `--inside-k K` | The roof-fade reference point and radius scale. |
| `--probe x,y,z` | Log every draw that touches the pixel at that world point. |
| `--quiet` | Less output. |
| `--til <file.til> [--info] [--dump <dir>]` | Inspect one tile prototype. |
| `--selftest <dir or file>...` | Parse every map found and report errors and unresolved references. |
| `--resave <out>` | Re-emit a saved mission world: the `<mission>.sav` entry inside a save container. The output must match the input byte for byte, which is how the save layout was proved. |

## sprview

```sh
sprview extracted/sprites/critters/<name>.spr --info            # sequences, collections, frame events
sprview extracted/sprites/critters/<name>.spr --dump out/       # every layer of every frame, as PNG
sprview extracted/sprites/critters/<name>.spr --composite out/  # layers merged into whole frames
sprview --selftest extracted/sprites/*/*.spr                    # decode everything and check frame rectangles
```

`--dump` writes one PNG per layer, named `<collection>_<dir>_<frame>_<layer>.png`. `--composite` writes one per frame, named `<collection>_<dir>_<frame>.png`.

## sprtrim

`sprtrim` makes a smaller `.spr` that keeps only the animations a run needs. The keep-list is a file of sequence names, one per line; `#` starts a comment. `fotgame --sprite-manifest` writes such a list.

```sh
fotgame campaigns/missions/core/mission01.mis --assets "/path/to/Fallout Tactics/core" \
    --campaign-start --squad entities/recruits/hm01.ent --headless --frames 600 \
    --sprite-manifest used.txt
sprtrim --keep used.txt extracted/sprites/characters/<name>.spr trimmed.spr
sprtrim --report trimmed.spr                # what each collection costs
sprtrim --selftest extracted/sprites/*/*.spr
```

| Option | Meaning |
|---|---|
| `--keep <file>` | The sequences to keep. Without it, the output is a plain repack of the input. |
| `--drop-unreferenced` | Also drop collections no sequence points at. They're kept by default, because a few are used directly. |
| `--report <file.spr>...` | Byte cost of each collection, and the sequences that use it. |
| `--selftest <file.spr>...` | Repack and trim every file, then check that the results reload identically. |

A kept collection is copied byte for byte. `sprtrim` checks every output against its input before writing it.

## Tests

`ctest --test-dir build` runs smoke tests that need no game data. To run the data tests as well, point `FOT_DATA_DIR` at your `core` folder:

```sh
FOT_DATA_DIR="/path/to/Fallout Tactics/core" ctest --test-dir build -j4 -L data
```

The [porting guide](porting-guide.md#6-check-your-work) lists the tests.

## The web build

`emcmake cmake` builds `fotgame` for the browser. That build expects a host page to mount the player's `core` folder into the in-browser file system and call `main` with arguments. This repository doesn't include such a page.
