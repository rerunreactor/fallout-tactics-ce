# Architecture

This page covers four things:

- how the original game is organised;
- how this port maps onto it;
- how data gets from your `.bos` archives to the screen;
- the frame loop, and where to start reading.

## The original: an engine and a game

Fallout Tactics 1.27 ships as `BOS.exe`, built with Microsoft Visual C++ 6. GOG's `BOS.exe` is identical to Steam's. Steam also ships a high-resolution `BOS_HR.exe`. It's the same size as `BOS.exe` and differs in only six bytes, all inside two display-mode functions: `IsValidDisplayMode@004b3440` and `RegisterOptionsStringMappings@00650640`. Every address in this project therefore applies to either executable.

Two things inside the executable show how it's organised.

- **The run-time type information is intact.** Several hundred classes keep their names: `World`, `Entity`, `Actor`, `Weapon`, `BaseAI`, `Controller`, `Campaign`, `ImageZAR`, `DirectX7Render` and so on. Virtual tables can be matched to classes, which is how many functions got their names.
- **Source file paths survive** in assertion and error strings: 99 distinct `.cpp` paths under two roots, `phoenix\` and `bos\`.

`phoenix` is a general 2D isometric engine. `bos` (Brotherhood of Steel) is the game built on it.

| Root | Directories | What they cover |
|---|---|---|
| `phoenix` | `display` (DirectDraw 3 and 7 devices, Win32 windows), `render` (a software renderer and a DirectX 7 renderer), `image` (images, palettes, ZAR, quantising), `format` (the image format registry, with BMP and PCX), `font`, `style`, `gui`, `window`, `event`, `time`, `file`, `string`, `map` (tiles, regions, the map file), `sprite`, `sound`, `movie`, `network` (`dp_network.cpp`, presumably DirectPlay, and a null network) | Platform, drawing, resources, the tile map, sprites, sound |
| `bos` | `game/world`, `game/entity` (actors, AI, items, inventory, combat effects, the entity stream), `game/core` (attributes, tables), `game/gui`, `game/net`, `script` (triggers, actions, conditions), `campaign` (the campaign and the world map), `editor`, `tile`, `sprite`, `common`, plus `main.cpp`, `main_menu.cpp` and `std_app.cpp` | Rules and content systems, the campaign, the front end, the level editor |

The level editor is linked into the game executable: `bos\editor\`, `bos\tile\wtile.cpp` and `bos\sprite\wentity_editor.cpp`.

The platform dependencies are:

- DirectDraw and Direct3D 7 for display. The shipped game draws through the DirectX 7 renderer, but the software renderer is also present.
- The Miles Sound System (`mss32.dll`) for sound.
- Bink (`binkw32.dll`) for video.
- The network module for multiplayer.

These are the seams the port replaces:

| Original | Port |
|---|---|
| Window, input, audio device | SDL2 |
| DirectX 7 renderer | our own software blitter, with the original's lighting and tint |
| Miles | our own mixer |
| Bink and networking | nothing yet |

## How the port is organised

The port isn't a file-for-file copy of the original. Each file format got a reader first, since the tools needed those before anything else; the game systems came after.

| Directory | Contents | Nearest original modules |
|---|---|---|
| `common/` | The resource layer, byte-stream reading, the ZAR codec, image sniffing, JPEG, PNG output, MP3 | `phoenix\file`, `phoenix\image`, `phoenix\format` |
| `sprview/` | The `.spr` reader and the `sprview` tool | `phoenix\sprite` |
| `mapview/` | The tile, map and entity-stream readers, lighting, draw order, the projection, and the `mapview` tool | `phoenix\map`, parts of `phoenix\render`, `bos\game\world`, `bos\game\entity\estream.cpp` |
| `engine/` | Drawing helpers shared by `mapview` and `fotgame` | parts of `phoenix\render` and `phoenix\image` |
| `sprtrim/` | The sprite trimmer | none |
| `game/` | The engine itself, `fotgame` | most of `bos\` |
| `difftest/` | Tools that compare the original with the port | none |

We know the original's file boundaries only from the embedded paths, so the third column is approximate.

### common/

- **`vfs.h`, `vfs.cpp`**: the resource layer. It mounts `.bos` archives (plain zip files) and loose folders into one case-insensitive namespace rooted at `core`, and inflates archive members when they're read.
- **`file.h`, `file.cpp`**: `ReadWholeFile`. It reads either a resource handle or a file on disk. If both fail, it tries the tail of the path as a resource name, so code that asks for `<assets>/campaigns/bos.cam` gets the archive member.
- **`stream.h`**: `Reader`, the bounds-checked little-endian reader every loader uses. It reads format tokens and narrow or wide length-prefixed strings. A failed read sets a sticky failure flag, as the original's stream does.
- **`zar.h`, `zar.cpp`**: the run-length ZAR image used by sprites, tiles and interface art (`ImageZAR::ReadFromStream@006f7d50`, `DecodeZarImageTo32Bit@006f9a10`).
- **`image.h`, `image.cpp`**: picks an image decoder from the file's first bytes, as `FormatManager::DetectImageFormatFromStream@006d2cb0` does, and decodes JPEG with stb_image.
- **`png.h`, `png.cpp`**: a small PNG writer for screenshots and tool output. PNG reading, needed for fonts, is in `game/ui.cpp`.
- **`mp3.h`, `mp3.cpp`**: MP3 decoding from memory with minimp3, for music and speech.

### sprview/

- **`spr.h`, `spr.cpp`**: the `.spr` sprite container, with sequences, collections, per-layer palettes and images (`SpriteAnimation::LoadSpriteAnimationsFromStream@0070af30`, `LoadSpranimImageSectionFromStream@00716390`). A collection's images are decoded on first use, and the reader can log which sequences a run used.
- **`main.cpp`**: the `sprview` tool. It prints sprite info, dumps images as PNG, composites layers and runs a self-test.

### mapview/

- **`til.h`, `til.cpp`**: `.til` tile prototypes (`Tile::ReadTileFileHeader@006f04c0`, `ParseTileDataAndLoadImages@006f0740`).
- **`mis.h`, `mis.cpp`**: the `.mis` world file. Covers the zlib wrapper, `<mapManager>` (`ReadMapManagerSection@006ed930`) and its regions, and the save-game form of a world (`World::Save@004ea450`), which it can also write.
- **`esh.h`, `esh.cpp`**: the `<esh>` tagged-property records that hold every entity, prototype and script object, and the `<entity>` block a `.ent` file contains.
- **`ent.h`, `ent.cpp`**: entities and how to draw them. Covers:
  - the `<entity_file>` section (`World::ReadEntityFileRecordsFromStream@004e52d0`);
  - loading prototypes and applying the prototype-override rule;
  - the path indexes;
  - choosing the sprite sequence and direction to draw.
- **`light.h`, `light.cpp`**: the world clock, time-of-day lighting (`ComputeTimeOfDayLightingState@004f2770`) and light entities (`Light::GetRenderLightLevel@0059cc50`).
- **`sort.h`**: the per-frame draw-order sort (`SortEntitiesByDrawOrder@007426b0`, `CompareEntityDrawOrder@007427c0`).
- **`iso.h`**: the world-to-screen projection (`ComputeIsoScreenOffset@0044de60`).
- **`main.cpp`**: the `mapview` tool. It prints map info, renders maps to PNG, dumps tiles, runs a self-test and re-emits a saved world.

### engine/

**`shared.h`, `shared.cpp`** hold what `mapview` and `fotgame` both need:

- the per-layer tint (`ApplyPaletteColorAdjustments@00708e70`);
- the sprite and tile caches;
- building light entities;
- the roof-fade and invisible-tile rules (`ComputeElementScreenBoundsAndHighlight@006e2c60`);
- per-corner light quads;
- the depth key.

### sprtrim/

**`trim.h`, `trim.cpp`, `main.cpp`** rewrite a `.spr` so that it keeps only the collections that a list of sequence names needs.

- A kept collection is copied byte for byte.
- A dropped one keeps its header with a frame count of zero, so no index moves.

We use it to make small test fixtures and smaller downloads for the web build.

### game/

- **`main.cpp`**: the program. It contains:
  - the command line, the window and input;
  - the per-frame `Tick`;
  - the scripted test input;
  - the glue for the campaign shell: entering and leaving missions, saving, the roster;
  - the data dumps that tests and difftest use.
- **`scene.h`, `scene.cpp`**: `Scene`, one loaded mission. It covers:
  - building live entities from the map and their prototypes;
  - the fixed-tick update, animation and movement, doors and vehicles;
  - combat, the AI's thinking, looting and skills;
  - drawing a frame.

  It's the largest file. `Scene::Update` and `Scene::Render` are the entry points.
- **`move.h`, `move.cpp`**: the navigation grid and the A* search (`RunPathfindingSearchStep@005355c0`, with neighbours from `DirSearch::DirSearch@00511370`).
- **`combat.h`, `combat.cpp`**: the combat rules and tables that don't need the scene. They include:
  - the shared random number generator (the MSVC `rand` generator);
  - weapons and their modes, and ammunition variants;
  - action-point costs and criticals;
  - attributes and derived stats, and the experience ladder.
- **`ai.h`, `ai.cpp`**: the AI's data. Natures, demeanours and events come from `tables/aitable.txt`, along with per-actor state. The thinking itself is in `scene.cpp`.
- **`script.h`, `script.cpp`**: mission scripting. Reads zones, players, teams and the hostility matrix, speech nodes, minimap objectives and triggers. Also holds the trigger evaluator and the actions and conditions.
- **`campaign.h`, `campaign.cpp`**: the campaign. Reads the campaign file (`campaigns/bos.cam`, written by `Campaign::SaveCampaignProperties@0069f330`) and `tables/campaign.txt`, and runs the campaign itself: world-map travel, random-encounter rolls, recruits and stock.
- **`campaign_save.cpp`**: the `.sav` container, its header, and the campaign block inside it.
- **`campaign_ui.h`, `campaign_ui.cpp`**: the front end and campaign screens. These are the title menu, the single-player menu, character selection, the briefing and debrief, the world map, recruiting, save and load, and the loading and end screens.
- **`ui.h`, `ui.cpp`, `ui_screens.cpp`**: the interface inside a mission. Covers:
  - fonts (PNG strips) and the style file;
  - the game bar, inventory, character sheet, Pip-Boy, skilldex, menu and loot window;
  - cursors, tooltips and the message list.
- **`audio.h`, `audio.cpp`, `audio_names.h`, `audio_names.cpp`**: sound. The mixer (SDL2 audio) handles distance rolloff, pan, voice limits, volume buses, ambient zones and music. The other two files hold the rules for which sound file an event plays.
- **`hudfont.h`**: a 5×7 debug font for the `--hud` overlay. It isn't the game's font.

## From .bos archives to pixels

1. **Mount.** `--assets` names your `core` folder.
   - `VfsMountAssetRoot` mounts every `*.bos` in it in ascending file-name order, then the folder itself as a loose tree.
   - Later mounts win. Per-CD archives (`*_A.bos` and so on) override the base archives (`*_0.bos`), and loose files override both.
   - Names are folded to lower case with forward slashes.
   - Nothing is extracted. Each archive's directory is read once, and members are inflated when something reads them.
2. **Resolve.** Loaders ask for resources by the names the data itself uses (`tiles/...`, `sprites/...`, `entities/...`), through `PathIndex` or `ReadWholeFile`.
3. **Load the map.**
   - `World::Load` inflates the `.mis`. It reads `<mapManager>` into tile prototypes and placements, and `<entity_file>` into entity records.
   - `MissionScript` reads the scripting sections.
   - The AI reads its entities from the same slot table.
4. **Build the scene.** `Scene::Load`:
   - decodes each tile prototype the map uses, once;
   - resolves every entity's `.ent` prototype and applies the override rule;
   - follows entity references (hands, armour, inventory);
   - loads sprites;
   - builds the light list and the navigation grid.
5. **Decode images when needed.**
   - A sprite collection is decoded the first time it's drawn.
   - Each sprite layer has its own palette.
   - The entity's colour record for that layer is applied to the palette, not to pixels.
6. **Draw a frame.** `Scene::Render`:
   1. collects the visible tiles and entities;
   2. draws floors first, by depth;
   3. sorts objects and entities together with the original's two-phase draw-order sort and draws them, lit and tinted;
   4. draws overlays and the interface on top;
   5. hands the 32-bit RGBA frame to an SDL texture.

Other resources take the same path:

- interface art and fonts (`gui/`);
- text (`locale/`);
- tables (`tables/`);
- sound (`sound/`);
- music (`music/`, which is loose, not archived, in a 1.27 install).

## The frame loop

`main()` does the setup:

1. parses the command line;
2. mounts the resources;
3. reads `bos.cfg`;
4. starts audio;
5. loads a mission (`Scene::Load`) or starts the campaign shell.

It then calls `Tick()` once per frame: in a loop on the desktop, and from `emscripten_set_main_loop` on the web.

`Tick()` does this:

1. Measures the frame time. With `--headless` it's a fixed 33 ms.
2. Handles SDL events. Keys map to the action names in the game's `keys.cfg`. The shipped default bindings are built in; the port doesn't read `keys.cfg` itself.
3. Runs any scripted input for this frame number (`--click`, `--move` and so on).
4. On a campaign screen, runs that screen (`CampaignFrame`) instead.
5. Scrolls the camera.
6. Calls `Scene::Update(deltaMs)`:
   1. Ambient sound zones and light flicker advance.
   2. The elapsed real time is capped at 100 ms and accumulated, and whole **70 ms ticks** run, as in `World::AdvanceWorldSimulationTimeStep@004fbf50`.
   3. Each tick runs, in order:
      - animation, for each entity whose think time has come (`Entity::ProcessAnimation@005e2b00`);
      - movement, doors and vehicles;
      - action-point regeneration, in real-time mode;
      - the AI;
      - queued use and skill orders;
      - the trigger engine.
7. Updates audio and the interface state.
8. Calls `Scene::Render`. The positions of moving entities are interpolated between the last two ticks. The original doesn't interpolate, and moves things only once per tick; `--no-interp` turns interpolation off.
9. Draws selection rings and the interface, writes any screenshot that's due, and presents the frame.

## Where to start reading

1. **`common/stream.h` and `common/vfs.h`**: how bytes are read, and where they come from.
2. **`common/zar.cpp` and `sprview/spr.cpp`**: the smallest complete format, end to end.
3. **`mapview/mis.cpp`, `mapview/esh.cpp`, `mapview/ent.cpp`**: maps and entities. [`kit/formats/`](../kit/formats/README.md) describes the same formats without code.
4. **`game/scene.h`**: `LiveEntity` and the `Scene` interface. Then `Scene::Update` and `Scene::Render` in `game/scene.cpp`.
5. **`Tick()` and `main()` in `game/main.cpp`**.
6. **One system**:
   - combat: `Scene::ComputeHitChance`, `Scene::ApplyHit`, `Scene::ResolveBurst`;
   - scripting: `game/script.cpp`;
   - the AI: `game/ai.h` and `Scene::AiTick`;
   - the campaign: `game/campaign.cpp`.

The comments follow some conventions.

- **`Name@address`** cites the original function. The address is a virtual address, and it's the same in `BOS.exe` and `BOS_HR.exe`. The [kit](../kit/README.md) has the names. Some older comments say the project's Ghidra program is the high-resolution `BOS_HR.exe`. It isn't: it's `BOS.exe` 1.27 with a 66-byte loader patch from an old FTSE version.
- **`RECONSTRUCTION`** marks behaviour worked out from the shipped data, from measurement, or from FreeFT (an earlier open-source Fallout Tactics engine), rather than read from the original's code.
- **`PLACEHOLDER`** marks a value we chose.
- **`PORT ADDITION`** marks a deliberate difference from the original.
- **`MILESTONE n`** says when a change was made. The history is in [status.md](status.md), and the numbering there is approximate for a few comments.
- **`NOTES.md §72`, `l.12345` and `f0348`** cite the maintainers' private notes, which aren't published. [CONTRIBUTING.md](../CONTRIBUTING.md) explains them.

[status.md](status.md) summarises what is reconstructed or chosen.
