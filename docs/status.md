# Status

`fotgame` loads the 1.27 game data and plays the original campaign's missions. Most of its rules are ported from the original executable. This page says:

- what works;
- what is approximated;
- what is missing;
- what to work on next.

It was checked against the code. In the source, every approximation is labelled where it's used: search for `RECONSTRUCTION` (worked out from data or measurement), `PLACEHOLDER` (a value we chose) and `PORT ADDITION` (a deliberate difference from the original).

## What works

### Data

- `fotgame` reads the retail `core` folder in place: its zip-based `.bos` archives and its loose files, with the engine's override order.
- Every shipped sprite, tile prototype and map parses, and every map renders. The [data tests](porting-guide.md#6-check-your-work) check this against your copy.

### Drawing

- Isometric maps are drawn with the original's projection and its two-pass draw order: floors first, then objects and entities through the original's pairwise sort.
- Sprite layers are tinted by the entity's colour records.
- Time-of-day lighting and light entities work, with flicker, arcs and per-corner shading.
- Animated tiles, roof fading and hidden "invisible" tiles work.

### Simulation

- The original's fixed 70 ms tick drives everything.
- Animation is driven by the sprites, including frame events (footsteps, sounds, the moment a shot lands) and idle behaviour.
- Movement uses an A* search over a one-unit grid, with the neighbour rule from the original, speeds taken from the sprites, facing, stances and auto-opening doors.
- Vehicles can be mounted, driven and fired in a simplified form, but so far only through the scripted `--vehicle` option.

### Combat

- The to-hit sum uses the original's terms and constants.
- Also ported:
  - critical chance and critical effects;
  - burst and spray allotment;
  - the shotgun cone's range multiplier;
  - damage, armour and ammunition variants;
  - action-point costs;
  - the turn queue;
  - hit reactions, deaths and blood.
- Real-time combat works, and so does individual turn-based combat.
- Several of these numbers have been checked against recordings of the original. See [difftest/README.md](../difftest/README.md).

### Characters

- Attributes, derived stats and skills are computed as the original computes them.
- Unarmed attacks come from the race table.
- The levels stored on map actors are applied, with their hit points and the deterministic part of the skill spending.
- All eight skills work: sneak, lockpick, steal, traps, first aid, doctor, science and repair.
- Looting works for containers, corpses and items on the ground, with the original's reach and weight rules and loot window.

### Mission scripting

- Triggers, conditions and actions work, with the original's evaluation model.
- Also working: objectives, speech text, world text, experience and reputation, mission variables, timers, the debrief, and hostility from the map's team matrix.
- The first campaign mission plays from its opening to its debrief.

### AI

- The desire model is read from `tables/aitable.txt`.
- Ported from the original: detection, target choice, the think cadence, controllers and waypoints, the squad's sentry modes, and idle wandering and looking around.

### Interface

- Everything is drawn with the game's own fonts, style file, art and cursors:
  - the game bar;
  - the inventory and equip screen;
  - the character sheet, Pip-Boy, skilldex and in-mission menu;
  - the loot window, tooltips and the message list.
- Text comes from the game's locale files.

### Campaign

- The front end works:
  - the title and single-player menus;
  - the tutorial and custom mission lists;
  - character selection with the five pregenerated characters;
  - Pip-Boy briefings and debriefs;
  - loading and end screens.
- On the world map, travel uses the terrain-speed map, and random encounters are rolled.
- The bunker works: recruits, and the quartermaster's issue.
- The squad carries over between missions.

### Saves

- `fotgame` reads and writes the original's `.sav` container, header and campaign block.
- Saves written by the original load in `fotgame`, apart from their mission script state (see [Saves](#saves-1) under Approximations).
- Mission worlds are written in the original's layout. A shipped save's mission world re-emits byte for byte.

### Sound

- Sound effects are positional, with the original's distance rolloff and voice limits.
- Also working: footsteps by floor material, ambient sound zones, music, speech, and MP3 on every platform.

### Builds

- Native builds with CMake, and a browser build with Emscripten.

## History

| Milestone | Delivered |
|---|---|
| 1 | `sprview`: the `.spr` container and ZAR images. |
| 2 | `mapview`: `.til` and `.mis` files, the isometric projection, whole-map renders. |
| 3 | Entities on the map: `<esh>` records, `.ent` prototypes, colour records, facing. |
| 4 | Every map renders. The tint as a per-palette transform, roof fading, invisible tiles, spawn-point selection. |
| 5 | Scene lighting from the world clock and light entities. The draw-order repair pass. |
| 6 | The first interactive build: an SDL2 window, the 70 ms tick, animation, idle behaviour, scrolling, picking. A browser build. |
| 7 | Movement: the walkability grid, the A* search, speed from the sprites, facing, doors. |
| 8 | Combat: to-hit, criticals, bursts, damage, armour and ammunition, action points, the turn queue, hit reactions, blood. |
| 9 | The interface, with the game's own fonts, style file and art. |
| 10 | Mission scripting. The first mission plays to its debrief. |
| 11 | The AI. |
| 12 | The campaign: the campaign file, the world map, the bunker, recruits, the quartermaster, vehicles, saves. |
| 13 | Fixes from play-testing: facing, actors stacking up, animated tiles, idle behaviour. Sound and music. |
| 14 | Render interpolation, as a switchable port addition. A fix for actors stuck on their first frame. |
| 15 | `.bos` archives read in place. The browser build stopped bundling game data. |
| 16–18 | Fixes from play-testing: names, selection, the game's own mouse controls, looting, unarmed attacks, item icons, cursors, talking, the eight skills, run modes. |
| 19 | Lighting from the clock at mission start, tooltips, the sentry buttons. |
| 20 | The front end: menus, character selection, the Pip-Boy briefing. `sprtrim`, minimp3, derived stats on every combatant. |
| 21 | JPEG backdrops, loading screens, end screens. |
| 22 | World overlays and interface details, matched to reference footage of the original. |
| 23 | The save format, recovered from a save written by the original. Real saves load. |
| 24–25 | Our saves fixed after the original refused two test saves. Levelled actors, the shotgun cone rule and the difficulty table, all from the original. |
| 26 | The difftest logger records positions and stances. |

Differential-testing passes ran alongside milestones 22–25. Code comments tagged `MILESTONE n` follow this numbering, give or take one for work that ran in parallel.

## Approximations

### Simulation and movement

| Rule | What we do | Why |
|---|---|---|
| Walking speed | The sprite's per-frame distance, multiplied by 1.0. | The original also multiplies by a per-entity speed factor whose code we haven't found. |
| Idle pauses | Actors breathe continuously. | The original pauses between idle cycles for a time we haven't recovered. |
| What blocks movement | A tile blocks a cell if it rises more than 3 world units above it. | The original's occupancy test isn't recovered. |
| Crowding | Searching for a free spot stops after 8 rings. An actor replans after 3 blocked ticks. Actors don't step aside for each other; they wait. | The original's limits, and its local avoidance, aren't recovered. |
| Doors | A door opens when someone comes within 9 world units, and stays open 2 seconds. | The real radius and hold time aren't in the data we've read. |
| Formations | The squad spawns on a ring of radius 6 world units around the spawn point. A group move spreads members over a fixed eight-slot ring. | `SpawnEntityGroupInFormation@004f5a10` and `ComputePlayerGroupCentroid@0049b770` aren't recovered. |
| Animated tiles | One map-wide counter, one frame per tick. | Inferred from a sprite that plays the same frames, and from FreeFT. |
| Smooth movement | Positions are interpolated between ticks. This is a port addition; `--no-interp` turns it off. | The original moves actors once per 70 ms tick and doesn't interpolate. |

### Combat

| Rule | What we do | Why |
|---|---|---|
| Light in the to-hit sum | Fixed at 1. | `TraceLightVisibilityToEntity` isn't ported. |
| Vehicle, attacker-injury and targeting-computer to-hit terms | Not applied. | Recovered from the original, but not implemented. |
| Difficulty | Set only by `--difficulty`, and applied only to shots by the player's squad. | The original reads `game.difficulty` from `bos.cfg`, and also adjusts shots *at* the player (`GetDifficultyModifierForAttackerTarget@0061b990`). |
| Line of fire and cover | Walks the navigation grid, using tile flags (ethereal, window, cover, climbable) whose meanings come from the data. | The original uses a swept-box search through the map. |
| Bursts | Shots are allotted with the original's rule. No burst shot can be a critical. | Whether the mode's damage multiplier applies to burst rolls is still open. |
| Large and Huge bodies | Get nothing from worn armour. | Inferred: no armoured sprites exist for them. |
| Death animations | Chosen from damage type and overkill. | Mapped from the sprite sequence names. |
| Melee modes with no stated range | 2 m. | Every shipped melee mode that does state a range says 2 m. |

### AI

| Rule | What we do | Why |
|---|---|---|
| Choosing a behaviour | Ranks behaviours by desire × the nature's preference. | The original sorts by current desire alone. It reaches the same behaviour by another route, so this hasn't been changed yet. |
| Detection | Uses our scene lighting sampled at the feet. One branch of the formula is never taken. | Inputs approximated. |
| Advance, retreat, flank, cover search, hearing combat | Fixed distances: 80% of weapon reach, a 60° flank, a 12-unit cover search, a 40 m hearing radius. | Reconstructions. |
| Healing others, eating corpses, laying or disarming traps, raising alarms | Recognised, but do nothing. | Not ported. |
| Random wandering | Gated on the map's `Rand Move` property, with a fixed stride and timing. | The original's flag is set only by the encounter spawner, and its stride isn't recovered. |

### Mission scripting gaps

- **Trigger conditions.** These always return false:
  - GUI Open, ObjDude Alive, Skill Use Occured, Right Click on Tab;
  - Weapon Mode Equals, World Play Mode Equals;
  - Quantity - Player Kills, Multiplayer Map Restart.
- **Quantity kinds.** These count as 0:
  - Skill, Trait, OptionalTrait, ItemsTagged, ItemsTaggedEquipped;
  - Reputation, Moving, Running, Sneaking, IsAlert, Unconscious.

  Attribute checks support only hit points and action points.
- **Actions.** These are logged and do nothing:
  - Play Bink Movie;
  - Play Wav and Play Sound;
  - Lock/Unlock Door.
- **Timing.** Speech stays up for 4 s and world text for 5 s. Both are placeholders.
- **Random ranges.** A random range in a condition uses its lower bound. Every shipped range has equal bounds, so this is exact on shipped data.

### Interface

- **Layouts.** Many are measured from reference footage of the original: the minimap's zoom levels, the hit-point bar colours, the end-turn panel, the menu and skilldex positions, and tooltips.
- **Options.** The in-mission Options page shows only the four volume settings.
- **Loot window.** Takes whole stacks; it has no quantity prompt. Its Done button is ours.

### Campaign

| Rule | What we do | Why |
|---|---|---|
| Travel speed scale, encounter probability scale | Placeholders: 1.0 and 1.5e-4. | The constants in the original aren't resolved. |
| Campaign clock | One game minute per world-map pixel travelled. | Nothing found reads the original's campaign clock. |
| Fog of war | Clears one tile around the squad. | The reveal function isn't recovered. |
| Squad experience | Mission experience is shared evenly. A level is 1 + XP / 1,000. | Our own curve. The original's experience ladder is ported, but only used for map actors. |
| Quartermaster | Each unarmed member gets a rifle, else an SMG, a pistol or a blade; armour if they have none; and 50 rounds. | There's no quartermaster window yet. |
| Vehicles | Turning snaps, and speed is clamped to the vehicle's maximum. | Simplified. |

### Sound

These are reconstructions:

- which sound file family an event plays;
- the per-channel distance table;
- the pan law;
- the night window for ambient zones;
- the choice of combat music.

There are no fades, no pitch variation, no music overlays and no vehicle engine sounds.

### Lighting

- Daylight ground draws about 16% brighter than reference footage of the original. The cause isn't known.
- Colours near sunrise and sunset haven't been checked.
- Luma weights for desaturation use BT.601, because the original's constants are unresolved. This has no visible effect on shipped data.
- The size of the depth interval used by the draw-order sort is a reconstruction. Its direction is the original's.

### Saves

Our writer differs from the original's in these ways:

- **Mission script state isn't saved or restored.** The script sections are copied from the map. When any save loads, ours or the original's, the mission's script restarts from the map's initial state.
- **Some records are missing.** No `<baseAI>` or `<combatFX>` records are written.
- **The backpack** is kept in a block after the save's file list, not as inventory entities.
- **World-state fields** in `<sgd>` are written as zeros, apart from the clocks and the tick count.
- **The world-map fog** is written as all zeros.
- **The movie queue** is written empty.

See [the save format](../kit/formats/saves.md#how-our-writer-differs).

## Missing

| Feature | Notes |
|---|---|
| Character creation and modification | Character selection's CREATE and MODIFY buttons only print a message (`WCharacter@00458b40`). |
| Multiplayer | Not part of this port. |
| Options and credits on the front end | The title menu's entries only print a message. |
| Movies | Bink isn't decoded. The intro, mission movies and the Play Bink Movie action are skipped. |
| Turn-based controls | Turn-based play works but is reachable only through `--turnbased`. There's no key binding (the game uses Return), and squad turn-based mode can't be selected. |
| Vehicle controls | The interface has no way to board or drive a vehicle. Vehicles are reachable only through `--vehicle`. |
| Trading and gambling | No barter or gambling screens. The quartermaster's issue is automatic. |
| Targeted shots | No called-shot interface, so every shot is uncalled. |
| Injuries | No crippled limbs or blinding. Knock-over and knockout criticals knock the target down; drop-weapon and weapon-damage criticals do nothing. |
| Area damage | Grenades and other radial weapons hit only their target. |
| Door locks | Doors have no lock state. Containers do, so lockpicking works on them. |
| Levelling the squad | No level-up gains, perks or skill-point spending for squad members. |
| Random encounter forces | The roll and its spawn list are computed, but the rolled force isn't placed on the encounter map. |
| Other campaigns | Only `bos.cam` loads. Custom single missions do load. |
| Multi-level movement | Ctrl + click (force move to the same level) is accepted but ignored. |

## Not yet verified

- **Whether the original loads our saves.** Two earlier attempts were refused, and both causes have been fixed. A test with the current writer, plus saves that mix one section of ours into a real save, is still to be run. The original has no command-line option to load a save, so this needs a person.
- **A GOG installation.** We develop against the Steam release. GOG's `BOS.exe` is identical to Steam's.
- **Combat numbers still open in differential testing.** See [difftest/README.md](../difftest/README.md#what-the-recordings-found):
  - a constant +2 on the player's shots;
  - the burst damage multiplier;
  - per-record to-hit and critical replays;
  - one recruit's maximum hit points.

## Roadmap

### Good first issues

Each of these was checked against the current code.

| Item | Where in our code | In the original | How to check |
|---|---|---|---|
| **Initial facing of map actors.** When an actor stores no `animationRotation`, `PickSprite` derives the facing from floats 0 and 2 of the `frame` matrix. The original's forward axis is the third row (floats 6 and 8), and the property's default is 0. In the 72 maps of `Mis-Main_0.bos`, the 47 actors without a stored rotation all have identity matrices, so we draw them at direction 2 instead of 0 (with eight directions). See [the format notes](../kit/formats/mis.md#facing). | `mapview/ent.cpp`, `PickSprite` | `Entity::Entity_UpdateFacingFromVelocityAndBounds@005e7080` | Render the first mission with `mapview` and compare idle actors with the original. |
| **The other half of the difficulty term**, and reading `game.difficulty` from `bos.cfg`. | `Scene::ComputeHitChance`; the `bos.cfg` reader in `game/main.cpp` | `GetDifficultyModifierForAttackerTarget@0061b990` | difftest: the `DIFF` record, and HIT records where the player is the target. |
| **Play Wav and Play Sound script actions.** The mixer already exists. | `MissionScript::ExecuteAction` in `game/script.cpp` | the trigger action classes | A mission that uses them, with `--audio-log`. |
| **Critical hits on burst shots.** | `Scene::ResolveBurst` | `RollCriticalHitChance@0060fdf0`, `ResolveBurstAttackTargets@00614ac0` | difftest `CRIT` and `ATTACK` records for burst fire. |
| **A turn-based key.** Bind Return as the game does, and support squad mode as well as individual mode. | the `Tick` key handling in `game/main.cpp`; `Scene::SetTurnBased` | `World::StartCombat@00506b40` | Play a fight both ways. |
| **The unported trigger conditions and quantity kinds** listed under [Mission scripting gaps](#mission-scripting-gaps). | `MissionScript::EvaluateCondition`, `MissionScript::Quantity` | `EvaluateEntityQuantityCondition@004e0e20` and the condition classes | `--triggers` and `--script-log` on the tutorial maps. |
| **Squad levelling** with the recovered experience ladder and level-up grant. | `HarvestRosterFromScene` in `game/main.cpp`; `LevelForExperience` in `game/combat.cpp` | `GetLevelForXPAmount@00607950`, `Attributes::CheckLevelUpAndGiveStatGainIfSo@00607b30` | Compare a squad member's sheet with the original after a mission. |
| **Read the campaign file's variable table names first**, as saves are read. `bos.cam`'s table is empty, so only custom campaigns would notice. | `CampaignFile::Load` in `game/campaign.cpp` | `VarTable::SaveVarTableProperties@00500b50` | A campaign file with variables. |
| **difftest tooling.** `compare.py` looks for the binary in `game/` and resolves `FOT_ASSETS` against two different working directories. | `difftest/compare.py` | none | Run it against a recorded session. |

### Bigger items

| Item | Where to start |
|---|---|
| **Grenades and radial damage.** | `ApplyRadialWeaponAttack@006157b0`, reached from `FinalizeCombatMessageFields@00611730`; `Scene::ApplyHit`. Check with difftest `DMG` and `ATTACK` records for thrown weapons. |
| **The burst damage multiplier.** We apply the mode's multiplier, as `Weapon::GetMinDmg@005d9930` does. Two recordings disagree on whether the original's rolls include it. | Record a session with burst fire, using the current logger. See [difftest/README.md](../difftest/README.md). |
| **Saving mission script state.** Write the live triggers, variables and timers, and restore them on load. | `Scene::WriteWorldSave`, `MissionScript`. Check the round trip with `mapview --resave`, and against a save from the original. |
| **Testing our saves in the original.** | Copy a save written by `fotgame` into a separate copy of the game, and open it from Load Game. Report what happens. |
| **Spawning random encounter forces.** | `World::SpawnRandomEncounterEntities@004f7690`; the ignored spawn list in `CampaignEnterMission`. |
| **Squad formation geometry.** | `SpawnEntityGroupInFormation@004f5a10` and `ComputePlayerGroupCentroid@0049b770` aren't recovered yet. |
| **Character creation and modification.** | `WCharacter@00458b40` in its create mode. The art ships with the game. `ui_screens.cpp` already draws the character sheet. |
| **Front-end options and credits.** | `WOptionsGameMenu@004acfe0`, and the main menu's command handler. |
| **Local avoidance, door locks, injuries.** | `ComputeObstacleAvoidingMoveVector@00536290`, `Door::SetActivationState@005b2690`, and the critical-effect table in `RollCriticalEffectForBodyLocation@0061c550`. |
| **Light in the to-hit sum and in detection.** | `TraceLightVisibilityToEntity`, called from `ComputeWeaponAttackHitChance@00615e80`. |

### Not planned

- **Multiplayer.**
- **Bink video.** Only if a clean-room decoder becomes available.
