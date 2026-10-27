# Maps (.mis)

A `.mis` file is a mission map, which the game calls a world. It holds:

- the tile placements;
- every entity: actors, items, containers, doors, lights, AI controllers, waypoints and more;
- the mission's scripting: zones, players and teams, speech, triggers;
- ambient sound zones.

Maps live under `campaigns/missions/` in the archives, and in the loose `missions/` folder.

| Role | Function |
|---|---|
| Wrapper | `World::Load` (address not recorded) |
| `<mapManager>` reader | `ReadMapManagerSection@006ed930`, source file `phoenix\map\map_file.cpp` |
| `<region>` reader | `MapRegion::LoadRegionFromStream@00740350`, source file `phoenix\map\map_region.cpp` |
| `<entity_file>` reader | `World::ReadEntityFileRecordsFromStream@004e52d0`, source file `bos\game\world\world-entities.cpp` |
| Writer (save form) | `World::Save@004ea450` |
| Our readers | `mapview/mis.cpp`, `mapview/ent.cpp`, `game/script.cpp` |

A saved mission uses the same container with different contents. See [saves.md](saves.md#the-mission-world-world-70).

## The wrapper

| Type | Field | Notes |
|---|---|---|
| token | `<world>` | Version 68 or 69 in shipped maps, 70 in saves. What 69 changes is unknown, and our reader doesn't check the version. |
| i32 | inflated size | |
| i32 | inflated size | The same value again. |
| bytes | zlib stream | Runs to the end of the file. It has a zlib header. |

## Sections

The inflated stream is a sequence of tagged sections. In a map they come in this order:

| # | Section | Contents |
|---|---|---|
| 1 | `<mph>` 8 | A player and team header. Not documented here. |
| 2 | `<zar>` | An image. What it's for is unknown. |
| 3 | `<mapManager>` 50 | The tile map: prototypes, then regions of placements. |
| 4 | `<entity_file>` 3 | Every entity. |
| 5 | `<world_zone>` 2 records | Named boxes. |
| 6 | `<Player>` 8 records | One per player slot, each with a nested `<PInfoH>` 1. |
| 7 | `<Team>` 2 records | Always nine. |
| 8 | `timerTableHeader` 2 | Mission timers. |
| 9 | `varTableHeader` 1 | Mission variables. |
| 10 | alignment matrix | 81 bytes, untagged. |
| 11 | `<speechList>` 1 | `<speech_node>` 3 records. |
| 12 | `<mnob>` 2 records | Minimap objective markers. |
| 13 | `<TriggerManager>` 1 | `<Trigger>` 4 records. |
| 14 | `<ambientSound>` 3 records | Ambient sound zones. |
| 15 | music block | Untagged. |
| 16 | more data | Not documented. |

Inside these, other tags appear: `<tile>`, `<region>`, `<esh>`, `<attribs>`, `<actor>`, `<cdelta>`, `<controller>`, `<gameEvent>`, `<combatMessage>` and `<vehicle>`.

The dispatcher that reads the sections, `World::LoadWorldSections@004eb870`, hasn't been studied. Our reader therefore finds each section by searching for its tag rather than walking the sequence. Runs of records are preceded by counts, but where each count sits is only partly documented here.

## `<mapManager>` 50

Accepted when `0 < v < 51`. Versions below 50 take an older path, which isn't documented. Every shipped map is version 50.

| Type | Field | Notes |
|---|---|---|
| f32 × 3 | ambient | Part of the stored light rig (`World+0x5D`). |
| f32 × 3 | diffuse | Part of the stored light rig (`World+0x69`). |
| f32 × 3 | light direction | Read and discarded. |
| i32 × 6 | cull box | Roughly the built area of the map. |
| u32 | `protoCount` | |
| `protoCount` × string | prototype names | `tiles/<path>.til`. Name 0 is empty: slot 0 means "no tile". |
| (`protoCount` − 1) × (`<tile>` 10 token + 23 bytes) | prototype headers | Copies of each `.til` header, for slots 1 onwards. See [til.md](til.md#header-tile). |
| u32 | total placements | Used only for the progress bar. |
| i32 × 6 | world box | Minimum x, y, z, then maximum x, y, z. |
| u32 | `regionCount` | |
| `regionCount` × `<region>` | regions | |

**The light rig** is the map editor's snapshot of the time-of-day lighting when the map was saved. The game recomputes it from the world clock every frame and ignores the stored values; the world clock always starts at 06:29 on the first day. The defaults (`World::InitMapLightManagerPart@006e68a0`) are ambient (0.5, 0.5, 0.5), diffuse (1, 1, 1), and the direction (750, −4000, 750), normalised.

**Regions** cover the world box in cells 256 world units on a side. In shipped maps the region count equals the number of such cells, and every map is one cell high.

## `<region>` 8

Accepted when `0 < v < 9`. Versions below 8 take an older path, which isn't documented. Every shipped region is version 8.

The region is a u32 count, then that many 56-byte placement records.

| Offset | Type | Field | Notes |
|---|---|---|---|
| `0x00` | i16 | `protoId` | Index into the prototype table. 0 means no tile. |
| `0x02` | u16 | flags | See below. |
| `0x04` | i32 × 3 | `min` | World box minimum: x, y (height), z. |
| `0x10` | i32 × 3 | `max` | World box maximum. |
| `0x1c` | i32 × 4 | screen rectangle | A cached copy of the prototype's: `(−offsetX, −offsetY, sizeX − offsetX, sizeY − offsetY)`. |
| `0x2c` | i32 × 2 | unknown | 0 in every shipped record. |
| `0x34` | i32 | draw priority | Read first by the draw-order comparison (`CompareEntityDrawOrder@007427c0`). 0 in every shipped record. |

**Flags.**

- Bit `0x0080` means "don't draw". The game tests this bit in the placement, not in the prototype (`ComputeElementScreenBoundsAndHighlight@006e2c60`). All shipped placements that set it use the Invisible Tile prototype.
- Other values seen: 1, 9, `0x100`, `0x201`, `0x300`, `0x800`, `0x808`. Their meanings are unknown.

A few placements have their x and z extents swapped relative to their prototype. Draw them as stored.

**Draw order.** The game keeps placements and entities in a list, sorted descending by a depth key, and repairs the order every frame with a pairwise test (`SortEntitiesByDrawOrder@007426b0`). The test looks at:

1. the draw priority above;
2. height;
3. the screen rectangles;
4. the world boxes.

The function that computes the depth key isn't recovered. We use `7 × (x + z) − 6 × y`, evaluated at the box corners.

## `<entity_file>` 3

| Type | Field | Notes |
|---|---|---|
| u32 | class count | |
| class count × string | class names | For example `Actor`, `Container`, `Light`, `BaseAI`, `WayPoint`. |
| u32, then that many strings | discarded | Only before version 2. |
| u16 | `slotCount` | Including slot 0, which is reserved. |
| u16 | live count | Only after version 2. The number of occupied slots, used for the progress bar. |
| u16 | unknown | Stored by the game at `World+0x74f`. Its use is unknown. |

Then, for each slot from 1 to `slotCount` − 1:

| Type | Field | Notes |
|---|---|---|
| u16 | id A | Meaning unknown. |
| u16 | sequence number | References to this slot must match it. |
| u16 | class index | `0xFFFF` means the slot is empty. |
| `<esh>` | properties | Only when the slot is occupied. See [ent-esh.md](ent-esh.md). |

This framing comes from the data: the function that reads each record wasn't available. It holds for every shipped map. The number of occupied slots equals the live count, and the last slot ends exactly where `<world_zone>` begins.

Versions above 3 take a different path in the original, which isn't documented.

## Entities in a map

Each slot's `<esh>` record holds the instance's own properties (`Entity::ClassType::Serialize@005e3450`).

### Common properties

| Property | Type | Notes |
|---|---|---|
| `frame` | 13 | A 3×3 orientation matrix (rows), then the position (x, y, z) in metres. |
| `EntPath` | string | The prototype `.ent` this instance is based on. |
| `Sprite` | string | Sprite path. The prototype's value wins. |
| `Display Name` | string | The prototype's value wins. |
| `Tag Name` | string | Scripts refer to units by this. |
| `Player Index` | int | Which player slot owns the entity. |
| `desiredTeam` | int | |
| `Level` | int | Experience level for map actors. Default 0. |
| `animationRotation` | int | Facing; see below. |
| `Base`, `Hair`, `Skin`, `Team` | 6 | Colour records. See [spr-zar.md](spr-zar.md#tint-the-colour-records). |

The integer world position is `round(position × 4)`.

### Facing

`animationRotation` is the direction index into the sprite collection being drawn. The original stores it as a signed byte.

- Like every property, it's written only when it differs from its default, which is 0. A missing `animationRotation` therefore means direction 0.
- The matrix's third row (floats 6–8 of `frame`) is the forward axis. Its angle is `atan2(f6, f8)`, quantised to the sprite's number of directions. Lights use the same row as the axis of their arc.
- We checked this against the 72 maps in `Mis-Main_0.bos`:
  - For every entity whose matrix isn't the identity, the stored `animationRotation` matches the third row's angle. We tried 4, 8, 16 and 32 directions.
  - With eight directions, the first row (floats 0 and 2) matches none of them.
  - Some actors have an identity matrix and a non-zero `animationRotation`. Their facing comes from the property alone.

### Prototype override

When an entity loads, the game reads the map's record first, then reads the `.ent` prototype over it. Each property is registered with a flag mask, which decides who wins:

| Mask | Who wins | Examples |
|---|---|---|
| bit 0 set | The instance | `Player Index`, `Tag Name`, `animationRotation`, `desiredTeam`, `Level` |
| bit 0 clear | The prototype | `Sprite`, `Display Name` |
| — | Always the instance, explicitly protected | `frame`, `EntPath` |

If the prototype omits a property that it wins, the property returns to its default. This is inferred, because the override loader wasn't available, but the evidence is strong.

### References between entities

Map entities never embed their parts. An actor's weapons, armour and inventory, and a container's contents, are other slots in the same table, referred to by type-12 properties: `{u16 slot, u16 sequence number}`. Slot 0 is "nothing".

| Owner | References |
|---|---|
| Actor | `Hand1`, `Hand2`, `Armour`, `Inventory`, `unarmedSlot1`, `unarmedSlot2`, `Attached` (its AI), `Trap`, `Vehicle` |
| Container | `Inventory` |
| Inventory | `ILink0`, `ILink1`, … (the items), with `IListSz` (int) holding the count |
| Weapon | `Ammo` (the loaded rounds; their `Count` is the number left) |

An actor can also carry `<slot>_override` booleans:

- the referenced armour is worn only when `Armour_override` is clear;
- `Hand1_override` plays the same role for the weapon.

### Spawn points

When the map loads, the game removes the actors standing in for the player's squad and puts the squad at a `SpawnPoint` (`World::ValidateSpawnPointsAndAssignTeamColors@004f53d0`, `World::FindRandomEntityByPlayerAndType@004f7350`).

`SpawnPoint` has these properties:

- `spawnCounter`;
- `desiredTeam`;
- an unnamed five-way choice, by default "Generic";
- `Main Dude Only`.

How the squad is arranged around the point isn't recovered.

The `WayPoint`, `Light` and `SpawnPoint` classes are editor markers, and the game doesn't draw them. We chose these three from the data; how the game decides isn't recovered.

## Lights

`Light` entities have these properties (`Light::Serialize@0059c1c0`). "Wins" is the override rule above.

| Property | Type | Default | Wins | Meaning |
|---|---|---|---|---|
| `On` | bool | true | prototype | |
| `Night Only` | bool | false | instance | |
| `Radius (meters)` | float | 2.0 | instance | |
| `Rate of change (ms)` | int | 0 | instance | Flicker step time. |
| `Color1` | type 5 (3 floats) | white | instance | Not clamped. One shipped light has a negative component. |
| `Intensity string (a-z)` | string | "" | instance | The flicker pattern. |
| `Repeating light intenstity` | bool | true | prototype | The spelling is the game's. |
| `Use Color2`, `Color2` | bool, 3 floats | false, white | prototype | |
| `Use Arc`, `Arc (degrees)` | bool, int | false, 360 | instance | |
| `High Bound`, `Low Bound` | float | 0, 0 | instance | Height limits relative to the light, applied when above 0. |
| `Use Fog`, `Fog Alpha` | bool, float | false, 0 | instance | |

How a light reaches a point at squared distance `d²`, where `r` is the radius (`Light::GetRenderLightLevel@0059cc50`):

| Step | Rule |
|---|---|
| Range | No light if `d² > r²`. |
| Arc | With `Use Arc`, the point must be within half the arc of the light's axis. |
| Bounds | The height bounds apply when they're above 0. |
| Falloff | `t = 1 − d² / r²`. |
| Flicker | Each character of the intensity string scales the light by `1 − (char − 'a') × 0.04`: `a` is full brightness and `z` is off. The light steps one character per `Rate of change (ms)` (`Light::DoEngineThink@0059cb40`). |
| Colour | `lerp(Color2, Color1, t) × flicker × t`. |

A light is lit when `On` is set, or when `Night Only` is set and it's night or close to sunrise or sunset. No shipped light overrides `On`'s default of true, so `Night Only` changes nothing in the shipped game.

## Scripting sections

### `<world_zone>` 2

Written by `WorldZone::SaveWorldZoneProperties@00500130`.

| Type | Field | Notes |
|---|---|---|
| f32 × 3 | colour | Probably for the editor. |
| string | name | |
| f32 × 6 | box | Minimum x, y, z, then maximum x, y, z, in metres. |
| u8 | unknown | 1 on the few zones with a colour other than white. |

### `<Player>` 8

Written by `Player::SavePlayerSectionProperties@004df310`.

- **What we use:** a name string, then an i32 side: 2 for the player's squad, 1 for AI, 0 for scenery.
- **The rest, per the writer:** 12 bytes, an i32, two u16, five `<Group Header>` 3 records, an i32, a u8, 12 bytes and three counted lists; then a nested `<PInfoH>` 1; then four u16 and an i32.
- **The team index** is the u16 at offset 13 of the `<PInfoH>` payload, after its token. We found it from the data; the `<PInfoH>` writer wasn't available.

### `<Team>` 2

Nine records, always.

| Type | Field | Notes |
|---|---|---|
| string | name | |
| 5 bytes | unknown | We read the first byte as an "enabled" flag. |

### The alignment matrix

81 signed bytes, 9 × 9. It sits immediately before `<speechList>`, after the timer and variable tables.

- **Indexing.** Row `a`, column `b` is how team `a` regards team `b` (`ActionSetTeamAlignment::Vtable15@006833b0` writes `a × 9 + b`). Negative means hostile.
- **Defaults.** Before the map overwrites it, the game sets 10 on the diagonal and −10 at `[0][0]` (`World::InitWorldTeamsAndDefaults@00505200`).

`GetFactionRelationValue@00505880` decides how two entities regard each other:

| Case | Result |
|---|---|
| Either belongs to player 0 (scenery) | 0 |
| Both belong to the same player | 10 |
| Both players have a team | the matrix value |
| Otherwise | −10 |

### Timer and variable tables

`timerTableHeader` 2 and `varTableHeader` 1 are bare tokens, without angle brackets.

- The variable table is a list of names followed by a list of values, both lists of strings, as in saves (see [saves.md](saves.md#the-variable-table)).
- The timer table's layout isn't documented.

### `<speech_node>` 3

Written by `SpeechNode::SaveSpeechNodeProperties@004ff230`.

| Type | Field | Notes |
|---|---|---|
| u8 | flag | Meaning unknown. |
| string | name | |
| 6 × (string, string) | lines | Each pair is a text key from the mission's locale file and an event name. |
| string | unknown | |

### `<mnob>` 2

A minimap objective marker.

| Type | Field | Notes |
|---|---|---|
| i32, i32 | position | |
| string | name | |
| i32 × 3 | unknown | |

### `<TriggerManager>` 1 and `<Trigger>` 4

`<TriggerManager>` 1 is a u32 count, then that many `<Trigger>` records.

The trigger is read by `Trigger::ReadTriggerFromStream@00679830` and written by `Trigger::SavePropertiesAndCheckDirty@00679a60`.

| Type | Field | Notes |
|---|---|---|
| string | name | |
| f32 | delay | Seconds between checks of its conditions. 1.0 in every shipped trigger. |
| f32 | next check | |
| u8 | armed | |
| u8 | repeat | Only after version 1. |
| string | unknown | Version 3 only. |
| u32, then that many objects | actions | |
| u32, then that many objects | conditions | |

Actions come first.

Each action or condition is a class name string, such as `Set Variable` or `Quantity - Team`, followed by an `<esh>` record of its properties. The game creates the class by name (`FindTriggerByName@00678a80`).

Some property values have formats of their own:

- **Qualified number** (5 bytes), used by quantity and timer checks: i8 mode (0 is `<`, 1 is `>`, 2 is `==`), then i16 `a` and i16 `b`. The value compared against is drawn at random from `[min(a, b), max(a, b)]` (`QualifiedNumber::EvaluateAgainstRandomValue@005ec730`). In every shipped record `a = b`. Its property type id isn't recorded here.
- **String pair** (type 20): for example, a quantity kind and an attribute name.
- **Player reference** (type 17, 2 bytes): the second byte is the player index.

**How triggers run** (`Trigger::TriggerConditionUpdateTimer@00679050`):

1. A trigger that isn't event-driven checks its conditions every `delay` seconds.
2. When they all hold, it arms and runs its actions in order. A blocking action that hasn't finished holds up the rest.
3. Without `repeat`, it fires once.

Which conditions are event-driven is set by their classes (`Trigger::TriggerComputeSpecialConditionMask@00679520`): skill use, speech, and a right click on a squad tab.

### `<ambientSound>` 3

Read by `LoadAmbientSoundSection@004dd0b0`. The field order was read from the data. It matches, one for one, the key list of the editor's zone configuration (`ParseAmbientSoundZoneConfig@004daf40`).

| Type | Field | Notes |
|---|---|---|
| string | name | |
| u32 | id | 5000 plus the zone's index. |
| u8 | `_random` | |
| u32, then that many strings | `_numOfFiles`, files | |
| u8 | `_useRadius` | |
| f32 × 3 | `_position` | |
| i32, i32 | `_minDist`, `_maxDist` | Metres. |
| i32 | `_volume` | |
| f32, f32 | `_volumeVar`, `_pitchVar` | |
| u8 | `_looping` | |
| i32, i32 | `_minTime`, `_maxTime` | Seconds between plays. |
| i32 | `_timeOfDay` | Only 0 and 4 occur. We read 4 as night. |
| 17 bytes | unknown | 0 in every shipped record. |
| u8 | `_notTimeOfDay` | |
| string | `_zone` | Names a `<world_zone>`. |
| u32, then that many strings | `_overrideZone` | |

**The music block** comes right after the last `<ambientSound>`, with no tag:

| Type | Field | Notes |
|---|---|---|
| u32 | unknown | 0 in every mission. |
| string | music | The mission's ambient music. |
| u32, then that many strings | overlays | |

## Unknown or unverified

- **The wrapper and header sections.** What `<world>` 69 changes. The layouts of `<mph>`, the `<zar>` after it, and `<PInfoH>` beyond the team index.
- **Old versions.** `<mapManager>` before version 50; `<region>` before version 8; `<entity_file>` after version 3.
- **The tile map.** Region flag bits other than `0x0080`; the two unknown ints in each placement; the cull box's exact meaning.
- **The entity file.** Slot id A, and the unknown u16 in the header.
- **Draw order.** The depth key's exact formula.
- **Scripting.** The timer table; the trailing fields of `<speech_node>`, `<mnob>` and `<Team>`; the 17 unknown bytes in `<ambientSound>`; the music block's first field.
- **Other records.** The layouts of `<actor>`, `<cdelta>`, `<controller>`, `<gameEvent>`, `<combatMessage>` and `<vehicle>`.
- **Whatever follows the music block.**
