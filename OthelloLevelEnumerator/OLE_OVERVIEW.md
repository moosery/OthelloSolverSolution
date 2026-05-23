# OthelloLevelEnumerator — What It Does and Why

## The Problem

Othello (also called Reversi) is a two-player board game.  Even on a small 6×6
board, the number of different positions the game can reach is enormous — many
billions.  To answer questions like "how many games are possible?" or "who wins
with perfect play?" you first need to find every unique board position that can
actually occur.

**OthelloLevelEnumerator (OLE)** solves that problem: starting from the opening
position, it systematically finds every reachable board position, grouped by how
many moves deep it is.  Level 0 = the starting board.  Level 1 = every position
reachable in one move.  Level N = every position reachable in exactly N moves.

---

## Why It's Hard

Two things make this difficult at scale:

1. **Volume.** The number of positions grows roughly 7–10× per level on a 6×6
   board.  By level 13 there are already hundreds of millions of distinct
   positions being generated in a single step.

2. **Duplicates.** Many different sequences of moves lead to the same board
   position.  You have to recognize and discard those duplicates, or you end up
   counting the same position thousands of times.  Doing that correctly and
   efficiently across hundreds of millions of boards is the central engineering
   challenge.

---

## The Big Picture: Two Repeating Phases

OLE works level by level.  For each level it does two things in sequence:

### Phase 1 — Solve (GPU Pipeline)

The GPU takes every known board from the current level and plays every legal move
on each one, generating all possible "child" boards for the next level.  These
children are dumped into a large buffer in GPU memory.

Once that buffer is full (or the level is exhausted), the GPU sorts all the
boards and removes duplicates.  The surviving unique boards are written out to
fast NVMe storage as sorted files.

### Phase 2 — Merge

The solve phase may produce many separate sorted files (one per buffer flush).
A board that appeared in two different flushes would not be caught as a duplicate
by the GPU alone.  The merge phase reads all those files simultaneously, streams
them through a k-way merge, and removes any remaining cross-file duplicates.
The result is a small set of clean, sorted files that become the input for the
next level.

---

## The Sawtooth Pattern

If you watch a system performance monitor while OLE is running you will see an
alternating pattern:

- **GPU burst** — the graphics card is running flat-out expanding and sorting boards.
- **NVMe burst** — the GPU goes quiet while the disk drives write the sorted results.
- **GPU burst** — back to GPU work on the next buffer.
- And so on.

This is expected.  OLE uses two large buffers in GPU memory ("ping-pong
buffers").  While one buffer is being sorted and its contents written to disk,
the other is ready to receive the next batch of expansions.  The current version
does these two activities back-to-back; a future version could overlap them so
the GPU is filling one buffer while the disk is draining the other,
turning the sawtooth into a flat line.

---

## What Makes It Fast

- **The GPU does the sorting.** Sorting hundreds of millions of records is where
  most of the time goes.  A modern GPU can sort ~100 million items in a few
  seconds using a radix sort algorithm; the same work on CPU cores would take
  many minutes.

- **Large sequential I/O.** Data is written and read in multi-gigabyte blocks,
  which is exactly what NVMe drives are optimized for.  No random access.

- **Minimal coordination overhead.** There are no locks, no hash tables, and no
  complex memory allocators in the hot path.  The GPU fills a buffer, sorts it,
  hands it off — repeat.

- **Canonical forms.** Before storing a board, OLE rotates and mirrors it into a
  standard orientation (the "canonical form").  This collapses all the
  rotationally equivalent positions into one, dramatically reducing the number of
  boards that need to be tracked.

---

## What OLE Produces

For each level, OLE reports:

| Column | Meaning |
|--------|---------|
| BoardsIn | How many unique boards were expanded this level |
| NewBoards | How many child boards were generated (gross, before dedup) |
| GpuDups | Duplicates caught within the GPU buffer |
| MrgDups | Additional duplicates caught across GPU-buffer boundaries during merge |
| Pass | Boards where the current player had no legal move (opponent plays instead) |
| Ends | Terminal boards (neither player has a legal move — game over) |
| Tm(s) | Wall-clock seconds for the level |

At the end of the run, totals are printed for unique boards, total moves
generated, and duplicate boards eliminated at each stage.

---

## Files on Disk

OLE writes files into timestamped subdirectories under each output directory you
specify — something like:

```
D:\OLEDataDir\2026_05_23.11_46_41\BoardSize6x6\
D:\OLEDataDir2\2026_05_23.11_46_41\BoardSize6x6\
D:\OLEDataDir3\2026_05_23.11_46_41\BoardSize6x6\
D:\OLEDataDir4\2026_05_23.11_46_41\BoardSize6x6\
```

Inside each run directory you will find three kinds of files:

### Solve files — `ole_solve_L{NN}_{NNNNNN}.sf`

These are created during the GPU pipeline phase.  Each one is a sorted snapshot
of some of the boards generated at that level — "some" because a single buffer
in GPU memory can only hold ~100 million boards at a time, so large levels
produce many of these files.  They are spread across all configured drives in
round-robin order so reads and writes hit multiple NVMe drives in parallel.

Example names: `ole_solve_L12_000003.sf`, `ole_solve_L13_000017.sf`

**These files are intermediate.**  Once the merge phase for that level finishes,
OLE deletes them automatically.  If a deletion fails for any reason the run
continues — the files can be cleaned up by hand.

### Merge files — `ole_merge_L{NN}_D{N}.sf`

After the GPU pipeline finishes, the merge phase reads all the solve files for
that level and combines them into one sorted, fully-deduplicated file per drive.
These are the "finished" boards for that level and serve as the input when
computing the next level.

Example names: `ole_merge_L12_D0.sf`, `ole_merge_L12_D1.sf`

### Checkpoint file — `ole_merge_level{NN}.meta`

A tiny file (a few kilobytes) stored in the **primary output directory only**.
It records the names and key-range summaries of that level's merge files.  When
OLE starts with `--restart`, it checks for this file.  If it exists, the level
is already complete and OLE skips directly to the next one.

Example name: `ole_merge_level12.meta`

### Disk space

The merge files are large.  At level 13 of a 6×6 run there are roughly
1.2 billion unique board positions, each stored as 64 bytes — around 77 GB
for that level's merge files alone.  The intermediate solve files are
automatically deleted as soon as each level's merge phase completes, so they
do not accumulate.  The merge files must be kept as long as you want resume
to work.

---

## Future Work

Once OLE has found all board positions, a separate "back-propagation" pass can
trace the results backwards from the terminal positions (who won?) through the
move graph to compute exact win/loss/tie counts for every board — and ultimately
for the starting position, answering "who wins with perfect play?"
