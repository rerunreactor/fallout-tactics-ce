# Tile prototypes (.til)

A `.til` file describes one kind of map tile: its size, its screen anchor, its type and material, some flags, and one or more ZAR frames. Maps don't embed tile images. They refer to `.til` files by name, and keep a copy of each one's header (see [mis.md](mis.md#mapmanager-50)).

| Role | Function |
|---|---|
| Header reader | `Tile::ReadTileFileHeader@006f04c0` |
| Image reader | `ParseTileDataAndLoadImages@006f0740` |
| Source file, per the executable's own path string | `phoenix\map\tile.cpp` |
| Our reader | `mapview/til.cpp` |

## File layout

```
<tile> 6..10        header
<tiledata> 1        frames and palette
```

## Header: `<tile>`

Accepted when `5 < v < 11`, so versions 6 to 10.

| Order | Type | Field | Object offset | Notes |
|---|---|---|---|---|
| 1 | u8 | `bboxX` | `+0x12` | Size in world units. The order is x, y, z: in the first map, every one of the 2,772 placements whose prototype isn't square matches `max − min = (x, y, z)`. FreeFT reads these as z, y, x. |
| 2 | u8 | `bboxY` | `+0x13` | Height. |
| 3 | u8 | `bboxZ` | `+0x14` | |
| 4 | i32 | `offsetX` | `+0x15` | Screen anchor. |
| 5 | i32 | `offsetY` | `+0x19` | |
| 6 | i32 | `sizeX` | `+0x1d` | Pixel size of frame 0. |
| 7 | i32 | `sizeY` | `+0x21` | |
| 8 | u8 | `typeId` | `+0x25` | See [Field meanings](#field-meanings). |
| 9 | u8 | `material` | `+0x26` | |
| 10 | varies | `flags` | `+0x08` (u16) | The flag tail, below. |

**The flag tail** depends on the version:

1. Before version 9, one byte is read and ignored.
2. Then:
   - **Version 6 or 7**: flag bit 0 is the next byte's bit 0. In version 6 only, one more byte is read and ignored. Flag bit 1 is the next byte's bit 0.
   - **Version 8 or 9**: the flags are one byte.
   - **Version 10**: the flags are an i16.

A version-10 header is therefore 23 bytes after its token. Maps store each prototype's header in exactly this form.

FreeFT puts the flags byte before the ignored byte in versions 6–8. The total length is the same either way.

## Frames: `<tiledata>` 1

| Type | Field | Notes |
|---|---|---|
| token | `<tiledata>` | Accepted only for version 1. |
| u32 | `frameCount` | **If 0, the file ends here.** |
| `frameCount` × { `<zar>` image, i32 x, i32 y } | frames | Each image carries its own palette. `x, y` is the frame's own offset; almost always (0, 0). |
| u32 | palette count | |
| u32 × count | palette | 0xAARRGGBB. The game keeps it as the tile's palette, but each frame's own palette takes precedence. |

Most tiles have one frame. Animated tiles, such as fires and lamps, have 10 to 15.

The rate isn't recovered. We advance every animated tile from one map-wide counter, one frame per 70 ms game tick. That's inferred from a sprite that wraps the same images and plays them at one per tick, and from FreeFT, which also drives tiles from one global counter.

## Placing a tile

A placement in a map gives the tile's world box (`min`, `max`). Its frame is drawn at:

```
frame top-left = project(min) + (frame x − offsetX, frame y − offsetY)
```

Maps also cache each placement's screen rectangle, as `(−offsetX, −offsetY, sizeX − offsetX, sizeY − offsetY)`.

## Field meanings

### `typeId`

| Value | Meaning | Source |
|---|---|---|
| 0 | Wall, including tents | the data |
| 1 | Floor | `TileType::ParseTileTypeDefinitionFromStream@00646350` |
| 2 | Object | the same |
| 3 | Stair | the same |
| 4 | Roof | the data |

- A few old tiles carry other values, which look like their type was never set.
- Floors (1) and roofs (4) are the only surfaces a move order can target (`WGameWorld::FindPrevVisibleTileOfType@0049d500`).
- We draw `typeId` 1 in the first, floors-only pass.

### `material`

Selects the footstep sound. These meanings are from the data, by comparing tile names with their values.

| Value | Surface |
|---|---|
| 0 | stone |
| 1 | gravel, dirt, sand or mud |
| 2 | metal |
| 3 | wood |
| 4 | water |
| 5 | snow |

A few tiles carry other values.

### `flags`

| Bit | Meaning | Source |
|---|---|---|
| `0x0001` | Ethereal (doesn't block shots) | A text file shipped with the game's tiles, and the data |
| `0x0004` | Window | the same |
| `0x0008` | Cover | the same |
| `0x0010` | Climbable | the same |
| `0x0080` | Invisible | Set on exactly one prototype: the "Invisible Tile" used to block movement without drawing anything |

No function we've read tests bits 0x01–0x10. We use them for line of fire and cover.

The game decides whether to hide a placement by reading the placement's own flag word, not the prototype's. See [mis.md](mis.md#region-8).

Values seen but not understood: `0x0020`, `0x0400`, `0x0800`.

## Unknown or unverified

- **Meanings.** Flag bits other than those in the table; and `typeId` values 0 and 4, whose meanings come only from the data.
- **Draw passes.** The original keeps its own classification of each tile, computed from the box sizes. We use `typeId` 1 to split floors from the rest, and whether the original's split is the same is unknown.
- **Animation.** The rate is inferred.
- **The trailing palette.** Whether anything uses it when a frame has its own.
