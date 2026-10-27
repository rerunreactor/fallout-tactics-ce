# Entity prototypes (.ent) and `<esh>` records

The game stores entities as tagged-property records, `<esh>`. Each property carries its own name, type and size, so a reader can skip what it doesn't understand. The same format is used for:

- `.ent` prototype files under `entities/`;
- every entity in a map's or a save's `<entity_file>` section (see [mis.md](mis.md#entity_file-3));
- the actions and conditions of mission triggers;
- the core and special missions in the campaign file.

| Role | Function |
|---|---|
| Writer | `EntityStreamStruct::InitializeEntityStreamHandle@005f2ea0` |
| Reader entry | `EntityStreamStruct::OpenEmbeddedStreamForEntity@005f2de0` |
| Property records | `EntityStreamWriteInteger@005f7890` and its siblings |
| Strings | `EntityStreamReadString@006f1ff0` |
| Nested blocks | `SerializeTaggedPropertyRecord@005f1880` |
| Our reader | `mapview/esh.cpp`; attributes in `game/combat.cpp` |

## .ent files

```
<entity> 2         token
string             class name: Actor, Weapon, Ammo, Armour, Container, Light, ...
<esh> 1            the prototype's properties
```

- Most `.ent` files use narrow strings. A few loose files in the 1.27 install store the class name, and one property name, in the wide form; see [string encoding](README.md#strings).
- The same `<entity>` block can appear inside a property. A prototype embeds its parts this way: an `Attached` AI, or the items in its hand and armour slots. See [type 4](#property-types).
- Maps never embed parts. They refer to other slots instead.

## `<esh>` 1

| Type | Field | Notes |
|---|---|---|
| token | `<esh>` | Always version 1 in shipped data. The readers we've seen don't check it. |
| u32 | count | The number of properties, not a byte size. The writer fills it in afterwards. |

Then each property:

| Type | Field | Notes |
|---|---|---|
| string | name | Usually narrow. Saves use the wide form. |
| u32 | type | See the table below. |
| u32 | size | Payload size in bytes, not counting these three fields. |
| bytes | payload | |

These rules govern reading and writing:

- **Absent means default.** A property is written only when its value differs from its compile-time default. A missing property has its default value.
- **Lookup is by exact name.** The game looks each property up by name, with a sorted map over the exact bytes, then checks its type and size. On a mismatch it skips the record and keeps the default. Whether the name comparison ignores case is unsettled. Our reader tries an exact match first, then a case-insensitive one.
- **Size limit.** A payload over 16,000,000 bytes is refused, except for type 11.
- **Who wins.** Properties are registered with a flag mask that decides whether the instance or the prototype wins when a map entity is loaded over its `.ent`. See [mis.md](mis.md#prototype-override).

## Property types

| Id | Payload | Meaning | Examples |
|---|---|---|---|
| 1 | 1 byte | bool | `Lootable` |
| 2 | 4 bytes | float | `Weight` |
| 3 | 4 bytes | int32 | `Player Index`, `Level` |
| 4 | i32 length + bytes | string, or an embedded `<entity>` 2 block | `Display Name`; `Attached` in a prototype |
| 5 | 12 bytes | three floats | Light `Color1` |
| 6 | 20 bytes | colour record: five floats `(r, g, b, s0, s1)` | `Base`, `Hair`, `Skin`, `Team`; see [spr-zar.md](spr-zar.md#tint-the-colour-records) |
| 7 | — | list of strings | Not seen in shipped data. |
| 8, 9, 10, 16, 19, 21, 25, 26 | i32 length + bytes | string | `Sprite` (8), `Race Type` (9), `MissionFile` (25) |
| 11 | i32 inner length + inner block | nested block; the size is the inner length + 4 | `Attributes`, `Current Attributes`, `Effect`, `esbin` |
| 12 | 4 bytes | reference to another entity: u16 slot, u16 sequence number | `Hand1`, `Armour`, `Inventory`, `ILink0` |
| 13 | 48 bytes | `frame`: a 3×3 orientation matrix, then position (x, y, z) in metres | `frame` |
| 14 | 16 bytes | four int32 (a rectangle) | `ImageRect` |
| 15 | u32 count + count × 4 bytes | int array | |
| 17 | 2 bytes | player reference. In trigger records the second byte is the player index. | trigger `Player` properties |
| 18 | u32 + u8 | unknown | |
| 20 | two strings, each i32 length + bytes | string pair | a quantity condition's kind and attribute |

The string ids store the same thing. The id records which editor control produced the value, such as a file picker or a choice list. The original's size switch groups them all as "string length + 4".

### Nested blocks (type 11)

Usually the inner block is another `<esh>`: `Current Attributes`, `Effect`, `Stats` and so on. The `esbin` property is different. It holds a binary record, or nothing (an inner length of 0):

- inside an `Attributes` block, an `<attribs>` 3 record (see below);
- at the top level of a map entity, an `<actor>` 4 or `<controller>` 1 record. Across the 72 maps in `Mis-Main_0.bos`, 208 entities have `<actor>`, 45 have `<controller>`, and the rest are empty.

### Embedded entities (type 4)

The payload is an i32 length, then an `<entity>` 2 block: class name, then `<esh>`. An empty slot has length 0.

## Attributes

A creature's stats are in its `Attributes` property, a nested block written in one of two forms:

- **Binary:** a `Binary` bool set to true, and an `esbin` holding an `<attribs>` 3 record;
- **Named:** child blocks `Stats`, `DerivedStats`, `Traits`, `OTraits`, `Skills` and `Perks`, each with properties named as in the table below, such as `strength`, `maxHitPoints` and `smallGuns`.

Both forms can also carry `levSincePerk` (int) on the `Attributes` block itself.

### `<attribs>` 3

After the token come 197 int32 (788 bytes). Index meanings:

| Index | Fields |
|---|---|
| 0–6 | strength, perception, endurance, charisma, intelligence, agility, luck |
| 7–17 | experience, skillPoints, tagsAvailable, statsAvailable, perksToTake, rank, reputation, age, bonusAC, sex, race |
| 18–25 | maxHitPoints, maxCarryWeight, maxActionPoints, radiationResist, poisonResist, armorClass, criticalChance, fallover |
| 26–31 | damage thresholds: normal, energy, fire, gas, explode, electrical |
| 32–37 | damage resistances, in the same order |
| 38–43 | camouflage (spelled `camoflage` in the game's table), healRate, meleeDamage, bonusDamage, skillPerLevel, levelsPerPerk |
| 44–61 | skills: smallGuns, bigGuns, energyWeapons, unarmed, meleeWeapons, throwing, firstAid, doctor, sneak, lockpick, steal, traps, science, repair, pilot, barter, gambling, outdoorsman |

From byte 248 the record is read as single bytes:

| Bytes | Fields |
|---|---|
| 248–265 | One byte per skill: tagged or not. |
| 266–303 | One byte per optional trait, 38 of them, in the order of the name table at `0x008a42d0`. The race flags are among them. |
| 304–787 | Unknown. |

The names and their order come from the game's own name tables at `0x008a6b70` (stats and traits), `0x008a41a0` (derived stats) and `0x008a6b18` (skills). They were confirmed by pairing binary records with named copies of the same characters.

### What the numbers mean

- **Derived stats and skills are stored as deltas.** The game computes each from the SPECIAL stats, race and traits, then adds the stored value (`Attributes::InitCopyAttributes@006080c0`, `Attributes::SumFinalAttributes@006088d0`, `Skills::AddStatBonusesToSkills@0060f320`).
- **`Modifiers`** is a second block with the same layout, added on top of `Attributes`.
- **`Current Attributes`** holds the creature's current state, not its maximums: `hitPoints` (int) and `actionPoints` (float).
- **An armour item's `Effect`** block holds derived-stat and skill changes that apply while the armour is worn.

## Unknown or unverified

- **Property types.** What type 18 holds, and whether type 7 is ever used.
- **Qualified numbers.** Which type id the 5-byte qualified number in trigger conditions uses. See [mis.md](mis.md#triggermanager-1-and-trigger-4).
- **Name lookup.** Whether it ignores case.
- **`<attribs>`.** Bytes 304 to 787.
- **Other binary records.** The layouts of `<actor>`, `<controller>`, `<cdelta>` and `<gameEvent>`.
