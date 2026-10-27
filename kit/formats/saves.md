# Saves (.sav) and the campaign file (.cam)

A save is a small container of files:

- a header, which the Load Game list shows;
- the campaign's state;
- the mission world in progress.

The campaign definition, `campaigns/bos.cam`, uses the same `<campaign>` block as a save's campaign state, in its full form.

| Role | Function |
|---|---|
| Container writer | `Campaign::SaveCampaign@006a1600` |
| Container reader | `Campaign::ReadCampaignSaveHeader@006a0c00` |
| Accepting a mission entry | `Campaign::MatchesMissionName@006a0a10` |
| `<saveh>` | `SaveGameHeader::SaveGameHeaderProperties@004bfea0` |
| The Load Game list | `WSaveGameItem::EnsureSaveGameHeaderLoaded@004bf560`, which reads only `<saveh>` |
| `<campaign>` writer | `Campaign::SaveCampaignProperties@0069f330` |
| `<campaign>` reader | `Campaign::OpenAndParseCampaignFile@0069d3d0` |
| `<world>` 70 writer | `World::Save@004ea450` |
| `<sgd>` | `World::SaveWorldSectionProperties@004eb470` |
| The variable table | `VarTable::SaveVarTableProperties@00500b50` |
| Campaign sub-records | `SerializeCampaignTileProperties@0069c120`, `SerializeRandomForceProperties@0069baf0`, `CoreMission::Serialize@0069b220` |
| Our code | `game/campaign_save.cpp`, `game/campaign.cpp`, `mapview/mis.cpp` (`World::LoadFromMemory`, `BuildSaveWorld`) |

Every string in a save written by the game uses the wide (16-bit) form; see [conventions](README.md#strings). The original's reader also accepts narrow strings.

## The .sav container

Saves live in `core/user/save/`. The file has no outer compression.

```
<saveh> 2                                   the header
<campaign_save> 1
u32 fileCount
fileCount × { string path ; u32 size ; size bytes }
```

A save written in the middle of a mission holds exactly two entries:

| Entry | Contents |
|---|---|
| `user/$$current$$/save.cam` | The campaign state: a `<campaign>` block in its short form. |
| `user/$$current$$/<map>.sav` | The mission world in progress. |

The writer stores the files it finds in the campaign's scratch folder that `MatchesMissionName` accepts. Other saves may therefore hold more entries; this is unverified.

When loading, the reader:

1. drops each entry's folder and extracts the entry into the campaign's scratch folder under its own name;
2. refuses any entry larger than 20,000,001 bytes;
3. reads `fileCount` entries and stops, so bytes after the last entry are ignored;
4. opens the mission whose name the campaign block gives as its current mission.

## `<saveh>` 2

The offsets are in the original's header object.

| Type | Offset | Field |
|---|---|---|
| u8 | `+0x00` | 1 in a save's own header. 0 in the header in front of a mission world (see below). |
| string | `+0x01` | Empty in a save's own header. In front of a mission world, the world's own name (`World+0x2c9`). In a shipped save, that's a path to the mission's speech locale file. |
| string | `+0x05` | The save's file name, without extension. |
| string | `+0x09` | The player character's name. |
| string | `+0x0d` | The mission's display name. |
| string | `+0x11` | Game date and time, as `Mon D YYYY.  HH:MM`: a full stop, then two spaces. |
| `<zar>` | `+0x19` | A picture, 280 × 165. |
| `<zar>` | `+0x1d` | A small picture, 128 × 76. |
| 6 × `<zar>` | `+0x21` | Squad portraits, 25 × 33. |
| 6 × f32 | `+0x39` | Squad members' hit points. |

- The pictures are ordinary ZAR images with their own palettes (see [spr-zar.md](spr-zar.md#zar-images)).
- An unused picture is an empty ZAR: version 4, 0 × 0, no palette, no data.
- The Load Game list also shows the file's size, which it computes when displaying the list.

## The mission world: `<world>` 70

The second entry is a world in its saved form:

```
<saveh> 2          with the first field 0
<world> 70
i32 inflated size
i32 inflated size  (the same value again)
zlib stream        to the end of the entry
```

The inflated stream:

| # | Content |
|---|---|
| 1 | A string: the path of the map this world came from, such as `campaigns/missions/core/<map>.mis`. |
| 2 | `<sgd>` 5: world state. |
| 3 | `<SSG>` 1: clocks. |
| 4 | `<entity_file>` 3: every entity as it is now, the squad included. |
| 5 | The same sections as a map, from `<world_zone>` onward, in the same order. See [mis.md](mis.md#sections). |

A saved world holds no tile map. `World::Save` writes the map's path where a map has `<mph>`, `<zar>` and `<mapManager>`, and loading reads the tiles from that map again.

| Content | Map | Saved world |
|---|---|---|
| `<mph>`, `<zar>`, `<mapManager>` | yes | no |
| Map path, `<sgd>`, `<SSG>` | no | yes |
| `<baseAI>` 0 and `<combatFX>` 1 records | no | yes: run-time state |
| Entities | as placed by the designer | as they are now: positions, hit points, inventories |
| Triggers | all of them | fired triggers are gone, because the game destroys a trigger once it fires |

Entities in a saved world use the same records as a map's (see [ent-esh.md](ent-esh.md)):

- **Worn items** are separate entities, referred to by `Hand1`, `Hand2` and `Armour`.
- **A weapon's loaded rounds** are an `Ammo` entity it refers to; that entity's `Count` is the rounds left.
- **The backpack** is an `Inventory` entity, with `ILink<n>` references to its items.
- **A dead creature** has 0 hit points in `Current Attributes`.

### `<sgd>` 5

The offsets are in the original's `World` object.

| Type | Offset | Meaning |
|---|---|---|
| f32 | `+0x2e7` | World clock in seconds: the accumulated real time of `World::AdvanceWorldSimulationTimeStep@004fbf50`. |
| u32 | `+0x2eb` | Tick count. |
| u32 | `+0x2ef` | Unknown. |
| u32 | `+0x2f3` | Unknown. |
| u32 | `+0x729` | Unknown. |
| f32 | `+0x2f7` | Real time: ticks × 0.07. |
| u32 × 2 | `+0x2fb` | Probably the game time, as a 64-bit count of milliseconds. Written as one 8-byte field. |
| u32 | `+0x427` | Unknown. |
| u8 | `+0x42b` | Unknown. 1 in a shipped save. |
| u16 | `+0x43c` | Unknown. |
| f32 | `+0x441` | Unknown. 1/7 in a shipped save. |
| f32 | `+0x445` | Unknown. |
| u32 × 2 | `+0x44d` | Unknown. |
| u32 count, then records | | A table of pairs. Empty in the saves we've seen; the record layout is unknown. |
| f32 × 3 | `+0x4c` | Unknown. Written as one 12-byte field. |
| u8 | `+0x1f4` | Unknown. |
| u32 count, then strings | | The names of the string tables that follow. |
| u32 count, then that many { u32 count, then strings } | | The string tables. In a shipped save, the mission briefing. |

With both string tables empty, `<sgd>` is 80 bytes after its token.

### `<SSG>` 1

| Type | Meaning |
|---|---|
| f32 | Game time, in seconds. |
| f32 | 0.07 in shipped saves: the tick length. |
| f32 | Real time, in seconds. |
| u32 | Tick count. |
| u32 | A tick count a little ahead of the previous field (2 or 4 ahead in shipped saves). Unknown. |

## The campaign block: `<campaign>`

`bos.cam` uses version 19 in its full form. The 1.27 executable writes version 21, which is what saves contain, in the short form.

The offsets are in the original's `Campaign` object.

| # | Type | Offset | Field |
|---|---|---|---|
| 1 | token | | `<campaign>` 19 or 21. |
| 2 | u8 | | Short form: 0 in `bos.cam`, 1 in saves. |
| 3 | 3 × i32 | | **Full form only.** Grid width, grid height, tile size in pixels. `bos.cam` has 65, 34 and 75. |
| | width × height × `<campaign_tile>` 2 | | **Full form only.** Column by column: `x` outer, `y` inner. See [the world map](#the-world-map). |
| | u8, then a BMP | `+0x1a5` | **Full form only.** If the byte is set, a complete Windows BMP follows. Its own header gives its length. |
| 3′ | string | | **Short form only.** The campaign file's path, `campaigns/bos.cam`. |
| 4 | i32 w, i32 h, then w × h × i32 | `+0x1ad` | Fog of war, one value per tile. 0 means unexplored, which is drawn black. All 0 in `bos.cam`. |
| 5 | string | `+0x10` | The current mission. In `bos.cam`, the starting map's path. In a save, the name of the embedded mission world, `user/$$current$$/<map>.sav`. |
| 6 | i32 | `+0x1c` | Unknown. |
| 7 | i64 | `+0x48` | The campaign clock. Nothing we've read uses it. |
| 8 | i32 | | Number of string files. 0 in the files we've seen; a list may follow when it isn't. |
| 9 | variable table | | The campaign variables. See [The variable table](#the-variable-table). |
| 10 | 4 × f32 | `+0x9d`, `+0xa1`, `+0xa5`, `+0xa9` | The world-map view position (x, y), then the squad marker (x, y), in world-map pixels. |
| 11 | i32 n, then n × `<esh>` | `+0x1cd` | Core missions. |
| 12 | i32 n, then n × `<esh>` | | Special missions. |
| 13 | i32 n, then n × `<random_force>` 2 | | Random-encounter forces. |
| 14 | i32 n, then n strings | `+0x1fd` | Landscapes: the maps random encounters are played on. |
| 15 | i32 n, then n strings | `+0x1bd` | The pregenerated characters' `.ent` paths. |
| 16 | i32 n, then n × { string tag, string `.ent` path, i32 count } | `+0x141` | The quartermaster's stock. |
| 17 | i32 n, then n strings | `+0x151` | Empty. |
| 18 | i32 n, then n × i32 | `+0x161` | Empty. |
| 19 | i32 n, then n × { string tag, string `.ent` path, string operation } | `+0x171` | Recruit pool rules: `add` or `remove`. |
| 20 | 2 × i32 | `+0x185`, `+0x195` | 0. |
| 21 | 4 × i32 | | 0. |
| 22 | 3 × u8 | | 0. |
| 23 | i32 | `+0x58` | Unknown. |
| 24 | string | `+0x69` | Empty. |
| 25 | 6 × { string, i32 } | | No meaning. A shipped save holds empty strings, and uninitialised memory (`0xCDCDCDCD`) in the ints. |
| 26 | f32 | `+0x23` | Travel speed multiplier, 1.0. |
| 27 | u8 | `+0x5c` | Unknown. |
| 28–29 | i32 n, then n strings | `+0xd1` | One entry in a shipped save: the current mission's name key in the locale files. |
| 30 | i32 | `+0x18` | 10. Nothing we've read uses it. |
| 31 | 2 × i32 | `+0xe1`, `+0xf1` | 0. |
| 32 | i32 | `+0x14` | 1. |
| 33 | i32 | `+0x209` | 1 in `bos.cam`, 70 in a shipped save. |
| 34 | 7 × i32 | | 0. |
| 35 | i32 n, then n strings | | Queued movies: their locale keys. |
| 36 | i32 n, then n strings | | Queued movies: their `.bik` files. |

**Version 19 against 21.** Version 21 adds three fields at the very end: an i64 (`+0x50`) and the two movie lists (`+0xb1`, `+0xc1`, rows 35 and 36). We read the i64 as the last 8 bytes of row 34. `bos.cam` stops before all three.

This is inferred. No function we've read tests the version, but parsing `bos.cam` without those fields ends exactly at the end of the file.

In a shipped save, rows 20 to 36 take 598 bytes. That includes four queued movies.

### The variable table

```
varTableHeader 1                bare token, no angle brackets
i32 n, then n strings           names
i32 n, then n strings           values
```

**Names come first.** `VarTable::SaveVarTableProperties@00500b50` writes two parallel lists, and a shipped save shows which is which. Values are strings too. A map's mission variables use the same table; see [mis.md](mis.md#timer-and-variable-tables).

### Core and special missions

Each is an `<esh>` record.

| Mission | Properties |
|---|---|
| Core | `Name` (a locale key), `MissionFile` (type 25), `Loc x` and `Loc y` (ints, in world-map pixels), `Radius` (int: how close the squad must come to arrive), `State` (type 9: `Covered`, `UnCovered` or `Visited`), and the bools `Is Bunker`, `vehicleMission` and `specialMission` |
| Special | `Name`, `MissionFile`, `Radius`, `Should Morph`, `Once Only` |

### `<random_force>` 2

| Type | Field |
|---|---|
| string | Name. |
| string | Alignment. |
| i32 | Minimum size. |
| i32 | Maximum size. The encounter's size is drawn between the two (`GenerateRandomEncounterSpawnList@006adcb0`). |
| u8 | Enabled. |
| i32 n, then n strings | The members' `.ent` paths. |
| i32 n, then n × i32 | The members' levels. |
| i32 n, then n × i32 | The members' chances of appearing. |
| string | CD code. |

### The world map

Each `<campaign_tile>` 2 record is one square of the world map:

| Type | Field |
|---|---|
| i32 n, then n × i32 | Weights for each random force. |
| i32 n, then n × i32 | Weights for each special mission. |
| i32 n, then n × i32 | Weights for each landscape. |
| u8 | Has art. |
| `<zar>` | The square's picture. Present only if it has art. |

- The world map's picture is these squares' pictures, laid out on the grid.
- The bitmap after the tiles is an 8-bit greyscale terrain-speed map, at half the world map's resolution. Travel speed scales by the value under the squad divided by 255 (`WWorldWindow::Update@006ba780` samples at position × 0.5). Dark enough squares are impassable, below a threshold constant that isn't resolved.
- At run time, the game rebuilds the mission, force, landscape, character, quartermaster and recruit lists from `tables/campaign.txt`. It keeps only the positions from the `.cam` (`Campaign::LoadCampaignCoresSection@006a3f80` and its siblings).

## How our writer differs

`fotgame` writes the original's layout, with these differences:

| Part | Difference |
|---|---|
| Container | The same two entries, followed by a `<f2ce_roster>` 1 block of our own after the entry list. The original ignores bytes there. The block holds the roster, the backpacks, the recruit pool, the stock and a few campaign values. |
| `<saveh>` | The same fields. The pictures use our own fixed 3-3-2 colour palette. |
| `<campaign>` 21 | The shipped field list, in the short form, with: <br>• the fog of war written as all zeros; <br>• the unknown fields written as 0 (rows 6, 23 and 28), and row 25 as empty strings and zeros; <br>• the movie lists empty. |
| `<sgd>` 5 | Only the clock, tick count and real time are filled in. The other fields are 0, apart from `+0x42b`, which is 1. The string tables are empty. |
| `<SSG>` 1 | The second float is 0, and the last field repeats the tick count. |
| Entities | The map's records, with updated positions (`frame`), hit points and action points. Worn items and loaded ammunition are added as new slots, which `Hand1`, `Hand2`, `Armour` and `Ammo` refer to. Names keep the narrow form the map used. |
| Missing records | No `<baseAI>` or `<combatFX>` records are written. |
| After `<entity_file>` | Copied unchanged from the map. Trigger progress, mission variables and timers are not saved. |

The original game hasn't yet been shown to load a save written this way. `mapview --resave` re-emits a shipped save's mission world byte for byte, which checks the layout but not our writer's contents.

## Unknown or unverified

- **Unknown fields:** those marked unknown in `<sgd>`, `<SSG>` and `<campaign>`.
- **More entries:** whether some saves hold more than two, and what they are.
- **The string-file list** in the campaign block, when its count isn't 0.
- **Tiles without art:** whether one is followed by anything. Every tile in `bos.cam` has art.
- **The terrain-speed threshold.**
- **The version 19 / 21 split**, which is inferred.
