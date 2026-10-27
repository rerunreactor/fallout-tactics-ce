# Sprites (.spr) and ZAR images

A `.spr` file holds every animation of one character, creature, item or object. It contains:

- **sequences**: named animations such as `StandWalk`, each a list of frames and events;
- **collections**: grids of frames × directions, each frame made of up to four layered ZAR images;
- **a palette per layer**, which the game tints with the entity's colour records.

| Role | Function |
|---|---|
| Reader | `SpriteAnimation::LoadSpriteAnimationsFromStream@0070af30` |
| Image-block reader | `LoadSpranimImageSectionFromStream@00716390` |
| ZAR reader | `ImageZAR::ReadFromStream@006f7d50` |
| Our reader | `sprview/spr.cpp` and `common/zar.cpp` |

## File layout

```
<sprite> 4                          token
header                              14 bytes
i32 sequenceCount
sequence records                    sequenceCount of them
i32 collectionCount
<spranim> 1 headers                 collectionCount of them
<spranim_img> blocks                one per collection, at the offset its header gives
```

The token is accepted only for version 4 (`3 < v < 5`).

### Header

Offsets count from the end of the token.

| Offset | Type | Field | Notes |
|---|---|---|---|
| 0 | u8 × 3 | `bboxX`, `bboxY`, `bboxZ` | The entity's box size, in world units. Order is x, y, z: `Entity::UpdateSprite@005dfc70` assigns them to the box's x, y and z. A humanoid is 3 × 8 × 3. |
| 3 | i32 | `offsetX` | Screen anchor. See [Placing a frame](#placing-a-frame). |
| 7 | i32 | `offsetY` | |
| 11 | u8 × 3 | unknown | The original stores them as a 16-bit field and an 8-bit field. |
| 14 | i32 | `sequenceCount` | |

The entity's world box is `bbox` in size, centred on its stored position in x and z, and starting at the position in y.

### Sequence record

| Type | Field | Notes |
|---|---|---|
| i16 | `rawCount` | Entries in the frame table, event parameters included. |
| i16 | unused | 0 in every shipped sprite. |
| i16 × rawCount | frame table | See below. |
| f32 × rawCount | step distances | Metres moved during each entry (`Actor::Move@0055ba20`). For example, 0.14 per frame for a standing walk and 0.5 for a run. 0 in sequences that don't move. |
| string | name | For example `StandBreathe`, or `default`. |
| i16 | `collectionId` | Index into the collection table. |

**Frame table.** An entry of 0 or more shows that collection frame. A negative entry is an event. Take the event number from the entry's low byte, as a signed value; some events consume the following entries as parameters:

| Event | Parameters | Meaning |
|---|---|---|
| −2 | 1 | Stop: hold the current frame for the parameter, in ms. |
| −3 | 1 | Set the frame delay to the parameter, in ms. |
| −4 | 0 | Restart from entry 0. |
| −5 | 1 | Jump to raw entry *parameter*. |
| −6 | 0 | Unknown. Appears only in death and rocket-attack sequences. |
| −40, −41 | 0 | Footfalls: one per foot, in walk, run and climb cycles. The meaning is inferred. |
| −42 | 0 | Hit: the attack resolves here. |
| −43 | 3 | Fire: three parameters (the muzzle offset), then the attack resolves as for −42. |
| −44 | 0 | Play the sequence's sound. Appears on the first frame of deaths, dodges, attacks and door movements. |
| −45 | 0 | Pick up. |

`Entity::ProcessAnimation@005e2b00` confirms the parameter counts. The event names come from FreeFT, and our own reading of where they occur.

**Playback.**

- The default frame delay is 66 ms (`Entity::SetAnimationRelated@005df6d0`). The game runs on a 70 ms tick, so in practice it shows one frame per tick.
- A freshly loaded entity plays the sequence named `default`. Two shipped sprites lack one; the original reports an error for them.
- The two inventory icons are the sequences `guibig` and `guismall`: frame 0, direction 0, drawn unscaled (`WIIBase::Draw@00490b50`).

### Collection header: `<spranim>` 1

| Type | Field | Notes |
|---|---|---|
| token | `<spranim>` | Accepted only for version 1. |
| i32 | `imageOffset` | Absolute file offset of this collection's `<spranim_img>` block. |
| string | name | |
| i32 | `frameCount` | **If 0, the header ends here, with no `dirCount`.** |
| i32 | `dirCount` | Number of directions. |
| `frameCount × dirCount` × {i32 x0, y0, x1, y1} | frame rectangles | Indexed `frame × dirCount + dir`. Screen space, relative to the anchor. |

A rectangle with negative width or height produces a warning, not a failure.

### Image block: `<spranim_img>` 1 or 2

A block starts at its collection's `imageOffset`, and runs to the next collection's offset or to the end of the file.

| Version | Content after the token |
|---|---|
| 1 | The payload, uncompressed. |
| 2 | u32 uncompressed size, then the payload as a gzip stream. |

The payload:

```
4 × { i32 n ; u32 colour[n] }        one palette per layer, 0xAARRGGBB
frameCount × dirCount × 4 entries:
    u8 type
    type 0     no image in this slot
    type 1     i32 x ; i32 y ; <zar> image
```

FreeFT also treats a type byte of `<` as an end marker. No shipped sprite has one, and it isn't confirmed in the original.

The entries come in a different order in each version. With `f` frames, `d` directions and layer `l`:

| Version | Loop order | Index of (frame, dir, layer) |
|---|---|---|
| 1 | frame, then direction, then layer | `(frame × d + dir) × 4 + layer` |
| 2 | layer, then direction, then frame | `layer × f × d + dir × f + frame` |

In both versions, layer `l` uses palette `l` and colour record `l` (see [Tint](#tint-the-colour-records)).

- **Image positions.** An image's `x, y` are relative to its frame rectangle's origin.
- **Rectangles.** A frame rectangle is exactly the union of its layers' images. This holds for every shipped sprite.
- **Empty collections.** A collection with `frameCount` 0 has no image block of its own, but its `imageOffset` must still be set: the next collection's offset bounds the previous block. No shipped sprite has an empty collection. Our trimming tool writes them.

## Placing a frame

```
frame top-left = project(entity position) + (rect.x0, rect.y0) − (offsetX, offsetY)
layer top-left = frame top-left + (image x, image y)
```

`project` is the isometric projection in the [conventions](README.md#units-and-coordinates), applied to the position in world units.

## ZAR images

ZAR is the engine's run-length image format. It's used for sprite frames, tile frames, interface art, and the pictures in save headers.

| Type | Field | Notes |
|---|---|---|
| token | `<zar>` | Accepted when `1 < v < 5`. Shipped files use 3 and 4. |
| i32 | width | |
| i32 | height | |
| u8 | `hasPalette` | |
| u32 | palette count | Only if `hasPalette`. At most 256. |
| u32 × count | palette | Only if `hasPalette`. 0xAARRGGBB, so the bytes on disk are B, G, R, A. |
| u8 | `constantIndex` | Only if `hasPalette` and version 4. Version 4 differs from version 3 only by this byte. |
| u32 | `dataSize` | |
| bytes | runs | `dataSize` bytes. |

- Images inside a `.spr` have no palette of their own. They use their layer's palette, with `constantIndex` 0.
- Tile frames and save-header pictures carry their own palette.
- FreeFT reads the constant index as a u32. The original reads a u8, and the tile files we've checked parse to their exact end that way.

### Runs

Each run starts with a control byte: `type = byte & 3`, `count = byte >> 2` (up to 63 pixels).

| Type | Payload | Pixels |
|---|---|---|
| 0 | none | `count` transparent pixels |
| 1 | `count` bytes | Palette indices, fully opaque |
| 2 | `count` × (index, alpha) | Palette indices, each with its own alpha |
| 3 | `count` alpha bytes | The palette colour at `constantIndex`, with each byte's alpha. Sprites use this for drop shadows. |

- A pixel's colour is the palette entry's RGB with the run's alpha. The palette's own alpha is ignored (`DecodeZarImageTo32Bit@006f9a10`).
- Runs never cross a row boundary.
- The decoder finds rows by walking the runs and adding up pixels. Each time the total reaches the width exactly, a new row starts. A run that goes past the width is an error in the original ("ZAR run went past edge of image").

## Tint: the colour records

Every entity carries four colour records, in its `<esh>` record as type-6 properties (see [ent-esh.md](ent-esh.md)). Layer `i` of the sprite uses record `i`:

| Layer | Record |
|---|---|
| 0 | `Base` |
| 1 | `Hair` |
| 2 | `Skin` |
| 3 | `Team` |

The records aren't combined with each other.

A record is five floats: `(r, g, b, s0, s1)`.

| Field | Meaning |
|---|---|
| `r, g, b` | A colour multiplier, not clamped |
| `s0` | Strength |
| `s1` | Desaturation weight |

The default record is `(1, 1, 1, 0, 0)`. A record with `s0 = 0` changes nothing.

The tint is applied to each **palette entry**, not to pixels (`ApplyPaletteColorAdjustments@00708e70`). For each entry, with `lerp(a, b, t) = a + (b − a) × t`:

1. Take the entry's colour, `c = palette / 255`.
2. If `s0 > 0`: `L = luma(c)`, `d = lerp(c, L, s1)`, `c = lerp(c, d × (r, g, b), s0)`.
3. Multiply by the ambient light: `c = c × A`, where A is the element's lighting.
4. Store `round(255 × c)`, clamped to 0–255.

Run type 3 pixels (shadows) go through the same adjusted palette, so shadows are tinted too.

### Team colour and selection

- **Team colour** is replaced when the map loads (`World::ValidateSpawnPointsAndAssignTeamColors@004f53d0`). The new value comes from a 16-entry table of records, picked by the owner's team colour index, then randomly varied.
- **Selection highlight** swaps in a separate set of four records, with `s0 = s1 = 1`.

Recruit prototypes carry `Hair`, `Skin` and sometimes `Team`, but never `Base`.

## Unknown or unverified

- **Header.** The three bytes at offset 11.
- **Sequence records.** The unused second field (always 0).
- **Events.** Event −6, and the exact meaning of −40 and −41.
- **Image blocks.** The `<` end marker.
- **Tint.**
  - The luma weights used for desaturation are unresolved. We use BT.601 (0.299, 0.587, 0.114). They have no effect when `s1 = 0`, which is true of every shipped record.
  - The team-colour table's contents, and how much the random variation changes it.
- **Walking speed.** How the game scales the step distances: it multiplies by a per-entity factor whose code we haven't found.
