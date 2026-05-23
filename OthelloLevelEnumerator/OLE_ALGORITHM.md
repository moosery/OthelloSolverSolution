# OthelloLevelEnumerator — Algorithm and Implementation Reference

## Board Representation

Every board position is stored as a `BOARD` struct (64 bytes):

| Field | Bytes | Purpose |
|-------|-------|---------|
| `ullCellsInUse` | 0–7 | Bitmask: which cells are occupied |
| `ullCellColors` | 8–15 | Bitmask: color of each occupied cell |
| `usBoardInfo` | 16–17 | Player-to-move bit + board-size encoding |
| *(padding)* | 18–23 | Zero-filled; rounds key to 3 × 8 bytes |
| `ullPossibleMoves` | 24–31 | Legal move bitmask for current player |
| `ullBlackWins` | 32–39 | Back-prop accumulator (zeroed during BFS) |
| `ullWhiteWins` | 40–47 | Back-prop accumulator (zeroed during BFS) |
| `ullTies` | 48–55 | Back-prop accumulator (zeroed during BFS) |
| *(reserved)* | 56–63 | |

**Identity key = bytes 0–23 (24 bytes, 3 × `uint64_t`).**  Two boards are the
same position iff these 24 bytes are identical.  `ullPossibleMoves` and the
win-count fields are derived/accumulated and are not part of the key.

---

## Sort Order

All sorting — GPU, merge phase, and binary search — uses the same comparator:

```cpp
// Ascending by (f0, f1, f2) where each field is 8 raw bytes read as
// a little-endian uint64_t (no byte-swap).
int BoardKeyCompare(const uint8_t* a, const uint8_t* b) {
    const uint64_t* ka = (const uint64_t*)a;
    const uint64_t* kb = (const uint64_t*)b;
    if (ka[0] != kb[0]) return ka[0] < kb[0] ? -1 : 1;
    if (ka[1] != kb[1]) return ka[1] < kb[1] ? -1 : 1;
    if (ka[2] != kb[2]) return ka[2] < kb[2] ? -1 : 1;
    return 0;
}
```

This is **not** the same as `memcmp` on little-endian x86.  `uint64_t`
comparison treats byte[7] of each 8-byte group as the most significant byte,
while `memcmp` treats byte[0] as most significant.  The only requirement is that
the GPU, MergePhase, and SFLowerBound all use the identical comparator —
which they do.

---

## GPU Buffer Layout (OLEGpuBuffers)

Two ping-pong accumulation buffers live in VRAM.  All scratch arrays are shared
between them (only one sort runs at a time):

| Array | Type | Size | Purpose |
|-------|------|------|---------|
| `d_accumA` | `BOARD*` | N × 64 B | Primary accumulation buffer |
| `d_accumB` | `BOARD*` | N × 64 B | Alternate accumulation buffer |
| `d_fieldA` | `uint64_t*` | N × 8 B | Extracted field values for sort pass |
| `d_fieldB` | `uint64_t*` | N × 8 B | CUB double-buffer alternate for field |
| `d_indicesA` | `uint32_t*` | N × 4 B | Index permutation (primary) |
| `d_indicesB` | `uint32_t*` | N × 4 B | CUB double-buffer alternate for indices |
| `d_dupFlagsA` | `uint8_t*` | N × 1 B | Dup flags for buffer A |
| `d_dupFlagsB` | `uint8_t*` | N × 1 B | Dup flags for buffer B |

**Per-slot cost: 2×64 + 2×8 + 2×4 + 2×1 = 154 bytes.**

On an RTX 4080 SUPER (16 GB VRAM, 1 GB reserved for runtime):

    N = 15 GB / 154 bytes ≈ 104.5 M slots per buffer

---

## Phase 1: GPU Solve Pipeline

### Step 1 — Expand (OthelloExpandKernel)

One GPU thread per input board.  For each board:

- If the current player has legal moves: play every move, canonicalize each
  child (rotate/mirror to standard orientation), store in a per-board result
  slot array.
- If the current player has no moves: fold the pass (flip player, recompute
  moves) and expand the grandchildren.  This ensures every child board lands at
  level N+1 regardless of pass structure.
- Terminal boards (neither player has moves) are counted but produce no children.

Atomic stats (`batchStats[0..2]`) track pass boards, terminal boards, and the
maximum move count seen.

### Step 2 — Scatter (ScatterToAccumKernel)

One GPU thread per result slot.  Valid slots (move index < `outputCounts[board]`)
are atomically scattered into the accumulation buffer via `atomicAdd` on a write
pointer.  The child `BOARD` is stored; the move edge is recorded separately for
future back-propagation.

### Step 3 — Sort and Dedup (SortAndDedup)

When the accumulation buffer reaches capacity (or the level is exhausted), a
three-pass stable radix sort builds a sorted index permutation over the N filled
slots.  The BOARDs themselves do not move — only the permutation is updated.

**Why three passes instead of a single 24-byte sort:**

CUB's `DeviceRadixSort` requires its histogram kernel to maintain one counter
per radix digit per thread in shared memory.  A 24-element `uint8_t` decomposer
on a 64-byte `BOARD` struct causes CUB to need 256 × 64 × 4 = 65,536 bytes of
shared memory per thread block — exceeding CUB's 48 KB policy default on all
current GPU architectures.  This limit is baked into CUB's policy tables and
cannot be raised by changing the compile target (SM 8.9 has the same limit as
SM 7.5 for this purpose).

Instead, three passes of `DeviceRadixSort::SortPairs<uint64_t, uint32_t>` are
used.  Each pass sorts on one 8-byte field using CUB's perfectly-tuned
primitive-type policies (no shared-memory pressure):

```
Pass 1: keys = boards[i].bytes[16..23] as uint64_t   (f2, LSB field)
        values = identity indices {0, 1, ..., N-1}
        → permutation P1 sorted by f2

Pass 2: keys = boards[P1[i]].bytes[8..15] as uint64_t  (f1, gathered via P1)
        values = P1
        → permutation P2 sorted by (f1, f2)

Pass 3: keys = boards[P2[i]].bytes[0..7]  as uint64_t  (f0, MSB field)
        values = P2
        → permutation P3 sorted by (f0, f1, f2)
```

CUB's radix sort is stable, so sorting LSB-first and working up to the MSB
field correctly produces the multi-field ascending order — the standard LSD
radix sort principle applied to 64-bit fields.

After each pass, if CUB placed the result in the alternate buffer
(`valDb.selector != 0`), a D2D `cudaMemcpy` normalizes the permutation back to
`d_indicesA`.  This uses the null stream (stream 0), so the subsequent kernel
launch is automatically serialized behind it.

**Duplicate detection (MarkDupFlagsKernel):**

One thread per sorted position.  Compares adjacent boards via the permutation:

```cpp
const uint64_t* a = (uint64_t*)&boards[perm[i-1]];
const uint64_t* b = (uint64_t*)&boards[perm[i]];
flags[i] = (a[0]==b[0] && a[1]==b[1] && a[2]==b[2]) ? 1 : 0;
```

Only equality is tested — no byte-swap or comparison order matters here.

### Step 4 — Extract (ExtractUniqueBoards)

D2H copies of the board array, permutation, and dup flags are pulled to host
memory.  Unique boards (`flags[i] == 0`) are gathered in sorted order:

```cpp
for (uint32_t i = 0; i < N; i++)
    if (!flags[i]) outBoards[out++] = hAccum[hIndices[i]];
```

The output is a host-side contiguous sorted array of unique `BOARD` structs,
ready for `SFWrite`.

---

## Phase 1: Ping-Pong and the Sawtooth

Two accumulation buffers (A and B) alternate roles each flush cycle:

1. **GPU fills buffer A** — `AccumulateBatch` is called in a loop, scattering
   children into buffer A until it is full or the level input is exhausted.
   *(GPU pegged; NVMe idle.)*

2. **Sort + write buffer A** — `SortAndDedup` runs on buffer A, then
   `ExtractUniqueBoards` D2H-copies the results, then writer threads call
   `SFWrite` to stream the sorted unique boards to NVMe.
   *(NVMe pegged; GPU idle.)*

3. **GPU fills buffer B** — same process on the other buffer.
   *(GPU pegged; NVMe idle.)*

4. Repeat, alternating A and B.

This alternating load is visible as a sawtooth in GPU and NVMe utilization
monitors.  **Future optimization:** start filling buffer B while buffer A's
write is still in flight — turning the sawtooth into simultaneous GPU+NVMe
activity.  The infrastructure is already in place (the TODO comment in
`GPUPipeline.cpp`); it requires a separate writer-queue thread and careful
synchronization to ensure `ExtractUniqueBoards` for buffer A completes before
buffer B is re-used.

---

## Phase 2: Merge

### Why a Merge Phase Is Needed

If a level's boards do not all fit in one GPU accumulation window, the solve
phase produces multiple separate sorted files.  A board that appeared in two
different windows was deduplicated within each window but not across them.  The
merge phase handles cross-window deduplication.

### Algorithm

**Pivot computation (`ComputePivots`):** The minKeys of all source files are
collected and sorted using `BoardKeyCompare`.  N−1 evenly-spaced pivots divide
the key space into N non-overlapping ranges, one per output directory/drive.

**Per-partition merge (`RunMergePartition`):** Each partition runs on its own
thread.  For each source file whose key range overlaps the partition:

1. `SFLowerBound` binary-searches to the first record ≥ `pivotLo` and the first
   record ≥ `pivotHi`, giving the slice boundaries.
2. A `SortedFileReader` is opened and seeked to `pivotLo`.

The N open readers are merged with a linear-scan minimum (O(M) per record, fine
for small M).  Equal-key records are deduplicated by comparing to the last
written key.  Output is streamed through a large in-memory buffer to a new
sorted file.

**Consistency requirement:** `SFLowerBound`'s binary search comparator must
exactly match the GPU sort order.  Both use `BoardKeyCompare` (raw `uint64_t`
field comparison).  Using `memcmp` in either place while the other uses
`uint64_t` comparison would produce incorrect partition boundaries.

---

## Sorted File Format (SortedFile)

Each file begins with a fixed header:

```cpp
struct SortedFileHeader {
    uint64_t recordCount;   // number of records in the file
    uint32_t recordSize;    // bytes per record (= sizeof(BOARD) = 64)
    uint32_t keySize;       // bytes compared for ordering (= 24)
    uint8_t  minKey[24];    // key of the first record
    uint8_t  maxKey[24];    // key of the last record
};
```

Records follow immediately, packed with no padding.  `minKey` and `maxKey` are
the raw first-24-bytes of the first and last records respectively, stored in
GPU sort order.  The merge phase uses these to reject files whose key range does
not overlap a partition without opening the file.

---

## File Naming and Lifecycle

### Directory structure

All output files live inside a timestamped subdirectory created at run start:

```
<outputDir>\<YYYY_MM_DD.HH_MM_SS>\BoardSize<N>x<N>\
```

For a run with four configured drives this produces four parallel trees:

```
D:\OLEDataDir\2026_05_23.11_46_41\BoardSize6x6\   ← primary (also holds logs + .meta)
D:\OLEDataDir2\2026_05_23.11_46_41\BoardSize6x6\
D:\OLEDataDir3\2026_05_23.11_46_41\BoardSize6x6\
D:\OLEDataDir4\2026_05_23.11_46_41\BoardSize6x6\
```

### Solve files

**Pattern:** `ole_solve_L{NN}_{NNNNNN}.sf`  (e.g. `ole_solve_L13_000017.sf`)

**Created by:** `FlushBuffer` in `GPUPipeline.cpp` via a thread-safe
monotonically-increasing sequence counter (`s_fileSeq`).

**Contents:** The unique boards produced by one flush of the GPU accumulation
buffer — sorted by `BoardKeyCompare`, deduplicated within that window only.
The level number `NN` refers to the level being *expanded* (the source level),
so the boards stored are at level `NN+1`.

**Drive assignment:** Files are written round-robin across all configured
output directories — `drive = flushIndex % numOutputDirs`.  This spreads I/O
across all NVMe drives simultaneously.

**Lifecycle:** These files are the sole input to the merge phase.  Once the
merge for that level completes and the checkpoint is written, OLE deletes
them via `remove()` on every path in `solveReg`.  Deletion failures are
silently ignored (run continues; files can be cleaned up by hand).

### Merge files

**Pattern:** `ole_merge_L{NN}_D{N}.sf`  (e.g. `ole_merge_L13_D2.sf`)

**Created by:** `RunMergePartition` in `MergePhase.cpp`.

**Contents:** All unique boards at level `NN+1`, fully sorted and deduplicated
across all solve-phase flush windows.  One file per configured drive (`D0`–`D3`),
each covering a non-overlapping key range (the partition assigned to that drive's
merge thread).

**Lifecycle:** These are the permanent canonical board set for that level.  They
serve as the input `currentReg` when processing the next level.  They must not
be deleted while any higher-level computation depends on them.  Once the entire
BFS is complete (and back-propagation is done) they could be archived or
discarded.

### Checkpoint / metadata file

**Pattern:** `ole_merge_level{NN}.meta`  (e.g. `ole_merge_level13.meta`)

**Location:** Primary output directory only (`outputDirs[0]`).

**Contents:** Binary dump of an `OLEFileRegistry` — a 4-byte count followed by
one `OLEFileDesc` struct per merge file.  Each descriptor records the file path,
drive index, record count, and the first/last 24-byte board keys (minKey/maxKey).

**Written by:** `FRSave` immediately after `MergePhaseRun` returns.  Its
existence is the definition of "this level is complete."

**Read by:** Resume detection at the top of the BFS loop — if `FRLoad` succeeds
and the registry is non-empty, OLE skips both the solve and merge phases for
that level.

**Lifecycle:** These files are tiny (a few kilobytes each, one per level).
They must be kept as long as `--restart` support is needed for this run.

### Disk space

All solve and merge files use the binary `.sf` format: a fixed 72-byte header
followed by packed 64-byte `BOARD` records.  A rough estimate for level L of a
6×6 run:

```
solve files = (uniqueBoards_L+1 - MrgDups_L) × 64 bytes   (GPU-deduped boards before cross-window merge)
merge files = uniqueBoards_L+1               × 64 bytes   (fully deduped)
```

At level 13 of a 6×6 run that is roughly:
- Solve files: ~1.44 B boards × 64 B ≈ 92 GB (spread across drives)
- Merge files: ~1.21 B boards × 64 B ≈ 77 GB (one partition per drive)

Solve files are deleted automatically after each level's merge completes, so
only the merge files accumulate on disk.

## FileRegistry and Checkpoint/Resume

`OLEFileRegistry` is an in-memory list of `OLEFileDesc` entries (path, drive
index, record count, minKey, maxKey) for all files belonging to a level.  At the
end of the merge phase, `FRSave` serializes the registry to a metadata file.

On startup with `--restart`, `FRLoad` reconstructs the registry from the
metadata file.  If a level's merged registry file exists, OLE skips both the
solve phase and the merge phase for that level and opens the next one directly.
No explicit checkpoint code is needed — completing the merge IS the checkpoint.

---

## VRAM and RAM Sizing

### VRAM (GPU solve phase)

```
vramBudget  = totalVRAM - 1 GB (runtime overhead)
accumSlots  = vramBudget / 154          // 154 bytes per slot (see table above)
```

The two accum buffers and all scratch arrays are allocated once at startup and
reused across all levels and all flush cycles.

### RAM (CPU merge phase)

```
ramBudget   = freeRAM * 0.75  -  2 GB (OS headroom)
perThread   = ramBudget / numMergeThreads
```

Each merge thread holds one large output buffer (capped at 1 GB, floored at
4 MB).  Source files are read through `SortedFileReader` with a smaller
per-reader buffer (256 KB default during merge).

---

## Key Invariants

1. **Sort order is global.** Every sorted file written by OLE — whether by the
   GPU pipeline or reconstructed after resume — uses `BoardKeyCompare`.  The
   merge phase relies on this to compute correct pivots and slice boundaries.

2. **Accum buffers are never shared.** The GPU fills exactly one buffer at a
   time.  `d_fieldA/B` and `d_indicesA/B` are safe to share because no sort
   overlaps with accumulation in the current implementation.

3. **D2D memcpy on the null stream is synchronous on the host.** After
   `cudaMemcpy(d_indicesA, d_indicesB, ...)` returns, `d_indicesA` is valid for
   the next kernel launch (also on the null stream).

4. **MrgDups == 0 means the level fits in one GPU window.** When this is true,
   the merge phase is doing I/O consolidation only (combining multiple flush
   files into one per drive), not actual deduplication.  As board counts grow,
   MrgDups will increase — indicating that cross-window dedup is contributing.
