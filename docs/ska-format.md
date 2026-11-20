# `.ska` binary format, version 1.0

`.ska` stores one 2D skeleton and any number of animation clips. The reference reader/writer and runtime is the header-only `include/ska/ska.h` (C++17, `namespace ska`).

## Conventions

- All integers and floats are **little-endian**. `f32` is IEEE-754 binary32. Fields are written byte by byte; there is no struct padding anywhere.
- Types: `u8`, `u16`, `u32`, `i32`, `f32`.
- All floats must be finite (no NaN/Inf); readers reject files that contain them.

## File header (20 bytes)

| Offset | Type    | Field                                             |
|-------:|---------|---------------------------------------------------|
| 0      | u8[4]   | magic `'S','K','A','1'`                           |
| 4      | u16     | versionMajor = 1                                  |
| 6      | u16     | versionMinor = 0                                  |
| 8      | u32     | flags = 0 (reserved; readers ignore)              |
| 12     | u32     | chunkCount                                        |
| 16     | u32     | CRC-32 of every byte after the 20-byte header     |

CRC-32 is IEEE (reflected polynomial `0xEDB88320`, init `0xFFFFFFFF`, final xor `0xFFFFFFFF`; `crc32("abc") = 0x352441C2`).

## Chunks

`chunkCount` chunks follow the header back to back:

| Offset | Type  | Field                      |
|-------:|-------|----------------------------|
| 0      | u8[4] | id                         |
| 4      | u32   | size (payload bytes)       |
| 8      | u8[size] | payload                 |

The v1.0 writer emits `META`, `STRS`, `BONE`, then one `CLIP` per clip. `META`, `STRS` and `BONE` must each appear exactly once. Readers locate chunks by id, so order is not significant to them; they parse STRS, then META, then BONE, then CLIPs in file order. Clips keep their file order.

### Strings

A string is `u32 byteLen` followed by `byteLen` UTF-8 bytes (no terminator). Every other place refers to strings by a `u32` index into the STRS table. `0xFFFFFFFF` (`kNoString`) means empty/none. Writers store each non-empty string once.

### `META` (payload ≥ 8 bytes)

| Offset | Type | Field          |
|-------:|------|----------------|
| 0      | f32  | pixelsPerUnit  |
| 4      | u32  | name (string index) |

### `STRS`

| Offset | Type | Field                       |
|-------:|------|-----------------------------|
| 0      | u32  | count                       |
| 4      | …    | `count` strings, back to back |

### `BONE`

| Offset | Type | Field                       |
|-------:|------|-----------------------------|
| 0      | u32  | count                       |
| 4      | u32  | recordSize (= 40 in v1.0)   |
| 8      | …    | `count` records of `recordSize` bytes |

Bone record (40 bytes):

| Offset | Type | Field                                      |
|-------:|------|--------------------------------------------|
| 0      | u32  | name (string index)                        |
| 4      | i32  | parent (-1 = root; must be < own index)    |
| 8      | f32  | length (pixels, along local +x)            |
| 12     | f32  | rest tx                                    |
| 16     | f32  | rest ty                                    |
| 20     | f32  | rest rotation (radians)                    |
| 24     | f32  | rest sx                                    |
| 28     | f32  | rest sy                                    |
| 32     | u32  | color, RGBA packed R in bits 0–7 … A in bits 24–31 |
| 36     | u32  | flags = 0                                  |

Because parents always precede children, a single forward pass computes world transforms.

### `CLIP`

| Offset | Type | Field                                  |
|-------:|------|----------------------------------------|
| 0      | u32  | headerSize (= 36 in v1.0)              |
| 4      | u32  | name (string index)                    |
| 8      | u32  | fpsNum (> 0)                           |
| 12     | u32  | fpsDen (> 0)                           |
| 16     | u32  | frameCount (> 0)                       |
| 20     | u32  | videoPath (string index; editor-only, games ignore it; relative to the `.ska` file's directory) |
| 24     | u32  | boneCount (must equal BONE count)      |
| 28     | u32  | keyCount                               |
| 32     | u32  | keyRecordSize (= 8 in v1.0)            |
| 36     | u32  | poseRecordSize (= 20 in v1.0)          |
| 4+headerSize | … | `keyCount` keys                    |

Each key is one key record followed by `boneCount` pose records (one per bone, in bone order).

Key record (8 bytes):

| Offset | Type  | Field                                   |
|-------:|-------|-----------------------------------------|
| 0      | i32   | frame, in `[0, frameCount-1]`, strictly ascending across keys |
| 4      | u8    | interp: 0 = Linear, 1 = Step            |
| 5      | u8[3] | zero                                    |

Pose record (20 bytes): `f32 tx, ty, rotation, sx, sy`. Poses are local (relative to the parent bone).

Keys are whole-skeleton keys: every key stores a pose for every bone.

## Versioning

- `versionMajor != 1` → reject (`"unsupported major version N"`).
- Any `versionMinor` is accepted.
- Unknown chunk ids are skipped using `size`.
- If a `size`, `recordSize`, `headerSize`, `keyRecordSize` or `poseRecordSize` is larger than v1.0 knows, the reader parses the known prefix and skips the remainder. This is how later minors append fields.
- A size smaller than the v1.0 minimum is an error.
- Bumping rules: append-only field additions or new chunks → minor + 1. Any change to the layout or meaning of an existing field → major + 1.

## Reader validation

A reader rejects the file on: bad magic (`"not a .ska file"`); CRC mismatch (`"CRC mismatch"`); any read or chunk past the end (`"truncated file"`); duplicate or missing META/STRS/BONE; string index ≥ STRS count (other than `kNoString`); bone `parent < -1 || parent >= index`; `fpsNum == 0`, `fpsDen == 0` or `frameCount == 0`; clip `boneCount` ≠ bone count; key frames not strictly ascending or outside `[0, frameCount-1]`; `interp > 1`; any non-finite float.

## Coordinate convention

- Skeleton space equals the source video's pixel space: origin top-left, +x right, **+y down**. Positive rotation is clockwise on screen.
- A bone's head is at its world translation; its tail is `apply(world, {length, 0})`.
- `pixelsPerUnit` converts pixels to game world units (`units = pixels / pixelsPerUnit`).

## Transforms

A pose maps to an affine `T * R * S`:

```
a = cos(r)*sx   c = -sin(r)*sy   tx
b = sin(r)*sx   d =  cos(r)*sy   ty
x' = a*x + c*y + tx
y' = b*x + d*y + ty
```

`world[i] = parent < 0 ? local[i] : world[parent] * local[i]`.

## Sampling

`sampleFrame(doc, clip, frame)` with fractional `frame`:

- No keys → the bones' rest poses.
- `frame <= first.frame` → first key's poses; `frame >= last.frame` → last key's poses.
- Otherwise take key `k` with `k.frame <= frame < k1.frame` (binary search):
  - `k.interp == Step` → `k.poses`.
  - Linear → `t = (frame - k.frame) / (k1.frame - k.frame)`; translation and scale lerp; rotation is `a + wrapPi(b - a) * t` (shortest path), with `wrapPi` mapping to `(-π, π]`.

`sampleTime(doc, clip, seconds, loop)` uses `frame = seconds * fpsNum / fpsDen`; with `loop` the frame is wrapped by `fmod(frame, frameCount)`.

## Game usage

```cpp
#include <ska/ska.h>

ska::Document doc;
std::string err;
if (!ska::readFile("hero.ska", doc, &err)) { log(err); return; }

std::vector<ska::Pose> local;
std::vector<ska::Affine> world;
void drawHero(float t, int clip) {
    ska::sampleTime(doc, doc.clips[clip], t, /*loop=*/true, local);
    ska::computeWorld(doc, local, world);
    for (size_t i = 0; i < doc.bones.size(); ++i) {
        const ska::Affine& m = world[i];       // pixels; divide by doc.pixelsPerUnit for world units
        drawSprite(boneSprite[i], m.a, m.b, m.c, m.d, m.tx, m.ty);
    }
}
```
