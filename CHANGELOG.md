# Changelog

## [OLE v0.2.19] - 2026-06-01

### Changed
- **`OthelloLevelEnumerator` / `OLEMain`** — default `nasDir` changed from `F:\OthelloRuns\` to `Z:\OthelloRuns\`; the NAS (Z:) is now the canonical landing place for final per-level merge output, freeing F: (HDD) for solve overflow (Dir4) and Phase 1 intermediates only; solve for each level reads from Z: and all previous level outputs accumulate there across runs; use `--nas-dir` to override at runtime
- **`OthelloLevelEnumerator` / `OLEMain`** — when `MergePhaseRun` fails (e.g. disk-full during Phase 2), a partial level row is now printed to the log before aborting; all solve-phase columns (`BoardsIn`, `NewBoards`, `Pass`, `GpuDups`, `SlvFls`, `SlvGB`) are accurate; `MrgDups` and `MrgGB` are 0 to signal that the merge did not complete; preserves reconnaissance data (storage requirements) even when a level fails mid-merge
- **`OthelloLevelEnumerator` / `OLEMain`** — version bumped to 0.2.19

## [OLE v0.2.18] - 2026-05-31

### Fixed
- **`OthelloLevelEnumerator` / `OLEMain`** — two instances of OLE running simultaneously no longer corrupt each other's shared memory: each instance now creates its SHM under a unique per-run name (`Local\OthelloLevelEnumeratorStatus_YYYYMMDD_HHMMSS`) derived from the same timestamp used for the run directory; the chosen name is written to `%TEMP%\OLEStatus.shm` at startup and deleted on exit so `OLEStatusQuery` can discover it without a command-line argument

### Changed
- **`OthelloLevelEnumerator` / `OLEStatus.h`** — `OLEStatusOpen` gains an optional `const wchar_t* name` parameter (default `OLE_STATUS_SHM_NAME`) so callers can specify the per-run name; the legacy fixed name is preserved as the default for backwards compatibility
- **`OthelloLevelEnumeratorStatus` / `OLEStatusQuery`** — reads `%TEMP%\OLEStatus.shm` at startup to find the current instance's SHM name; if the file is absent or the open fails, falls back to the legacy fixed name so old OLE builds still work
- **`OthelloLevelEnumerator` / `OLEMain`** — version bumped to 0.2.18

## [OLE v0.2.17] - 2026-05-31

### Changed
- **`OthelloLevelEnumerator` / `GPUPipeline`** — simplified flush overlap: D2H + CPU gather remain synchronous on the main thread (heap buffers, no cache competition with AccumulateBatch); only `SFWrite` + `FRRegister` run in a background `WriteJob` thread; `hostBoards` vector is moved into the thread (zero-copy); the sequential NVMe write (~1-2 s) overlaps with the next accumulation window (~4-10 s) without competing for CPU L3 cache or memory bandwidth; `joinPending` before each sort is always a near-instant no-op in practice; no CUDA changes required
- **`OthelloLevelEnumerator` / `OLEKernel`** — reverted v0.2.16 additions (`copyStream`, `copyDoneEvent`, pinned staging, `BeginExtractUniqueBoards`, `GatherUniqueFromStaging`, `SyncCopyStream`, `AttachCurrentThread`, `WaitForCopyDone`); `WorkerGpuContextCreate` signature restored to two-parameter form
- **`OthelloLevelEnumerator` / `OLEMain`** — version bumped to 0.2.17

### Rationale
v0.2.16 benchmarked slower at L13 (119 s vs 90 s) because `GatherUniqueFromStaging` randomly accessed ~5 GB of pinned memory concurrently with `AccumulateBatch`, thrashing L3 cache and slowing GPU dispatch more than the overlap saved (~3-4 s extra per flush, vs ~2 s saved). The simpler v0.2.17 approach saves only the sequential write time per flush (~1-2 s × 9 flushes ≈ 10-15 s at L13) but does so cleanly without side effects.

## [OLE v0.2.16] - 2026-05-31

### Changed
- **`OthelloLevelEnumerator` / `GPUPipeline`** — overlapped flush: after `SortAndDedup` (synchronous, saturates all SMs), `BeginExtractUniqueBoards` enqueues three `cudaMemcpyAsync` D2H transfers on a dedicated `copyStream` (copy engine) and records a `cudaEvent_t`; a background `std::thread` then sleeps on `cudaEventSynchronize`, gathers unique boards from pinned staging, and calls `SFWrite`; `AccumulateBatch` for the next window starts immediately on the SM engines while D2H + gather + write run concurrently; `SyncCopyStream` is called before each `SortAndDedup` to ensure `d_indicesA` (shared sort scratch) is no longer in flight on the copy engine — in practice always a no-op since D2H finishes in ~250 ms and the next window takes several seconds to fill
- **`OthelloLevelEnumerator` / `OLEKernel.h`** — `WorkerGpuContext` gains `copyStream`, `copyDoneEvent` (created with `cudaEventBlockingSync`), and three pinned host staging arrays (`h_accumStage`, `h_indicesStage`, `h_flagsStage`) sized at `stageCapacity` slots; `WorkerGpuContextCreate` gains `stageCapacity` parameter; new functions: `BeginExtractUniqueBoards`, `GatherUniqueFromStaging`, `SyncCopyStream`
- **`OthelloLevelEnumerator` / `OLEMain`** — version bumped to 0.2.16

### Expected improvement
CPU gather (~0.5 s) + NVMe write (~1–2 s) per flush now overlaps with the next accumulation window rather than blocking it. Savings grow with flush count: L13 ~9 flushes → ~18 s recovered; L14 ~43 flushes → ~85 s; L15 ~170 flushes → ~340 s. GPU sort is still serial (saturates all SMs).

## [OLE v0.2.15] - 2026-05-31

### Fixed
- **`OthelloLevelEnumerator` / `MergePhase`** — L17 merge crash (errno=24 "Too many open files") fixed architecturally via two-phase merge: Phase 1 pre-merges each output directory's solve files into one sorted+deduped intermediate using a min-heap and large streaming read buffers; a shared dynamic semaphore (`_getmaxstdio() - 20`) tracks open FILE* handles across all Phase 1 directory threads and blocks a thread from starting a new batch until handles are available, preventing the CRT limit from being exceeded regardless of solve-file count; Phase 2 performs a lightweight N-way (≤5) merge of the per-directory intermediates, writing final output directly to `nasRunDir` (if enabled); source solve files are deleted progressively during Phase 1 as each is exhausted

### Changed
- **`OthelloLevelEnumerator` / `MergePhase`** — replaced O(M) linear-scan minimum in Phase 1 with O(log M) binary min-heap; Phase 2 retains linear scan (trivially fast at ≤5 sources); large streaming read buffers in Phase 1 (`~80% of per-thread budget / batch_size` per source file, typically 5–10 MB vs previous 256 KB) dramatically improve NVMe sequential read efficiency; F: (HDD) directory thread is queued first to maximise overlap with the faster NVMe directory threads
- **`OthelloLevelEnumerator` / `MergePhase.h`** — `MergePhaseRun` gains `nasRunDir` parameter (default `nullptr`); Phase 2 writes final merge output directly to `nasRunDir` when non-empty, eliminating the separate archive-copy step; falls back to `outputDirs[i]` when NAS is disabled
- **`OthelloLevelEnumerator` / `OLEMain`** — removed NAS archive copy machinery (`ArchiveCopyFile`, `g_archiveThreads`, `g_archiveMtx`, `JoinArchiveThreads`, `InputArchiveCtx`); `OnInputFileConsumed` callback simplified to `remove(path)` — merge output written directly to NAS is the canonical data for the next level; next level's solve reads from NAS (F:\OthelloRuns\, HDD ~130 MB/s); input files deleted as consumed so NAS stays lean ("nuke it" reconnaissance mode)
- **`OthelloLevelEnumerator` / `OLEMain`** — three new columns in per-level table: `SlvFls` (solve output file count = Phase 1 merge fan-in), `SlvGB` (temporary NVMe needed during solve = `(NewBoards − GpuDups) × 24 B / 1024³`), `MrgGB` (permanent canonical storage for this level = `NetUnique × 24 B / 1024³`); `LevelRecord` gains `solveFiles` field populated from `stats.filesWritten`; purpose: reconnaissance run collects per-level storage requirements for planning the final production run
- **`OthelloLevelEnumerator` / `OLEStatus.h`** — `OLE_STATUS_VERSION` bumped 4 → 5; six new fields added to `OLEStatusBlock`: `runStartMs` (GetTickCount64 at BFS loop start, enables total run-time display in query), `mergePreDirTotal[5]` + `mergePreDirConsumed[5]` (Phase 1 per-directory source-file counts, written by `RunPreMergeDir` so the query can show per-dir progress and per-dir ETA), `lastPassBoards` + `lastEndBoards` + `lastSolveFiles` (complement the existing `lastLevel` snapshot so the query can display the full level row including pass, terminal, and file count)
- **`OthelloLevelEnumeratorStatus` / `OLEStatusQuery`** — MERGE phase display updated for two-phase design: Phase 1 shows per-directory progress ("Dir 0: 451/451 [done] … Dir 4: 312/451 (69.2%) — ETA: ~48m") so the semaphore behaviour and HDD bottleneck are visible; Phase 2 shows "Ph2 (final): N/5 parts written  X GB total" with per-part breakdown; header gains "Run time: Xh Ym Zs" (from `runStartMs`); "Last completed" section gains `Pass`, `Ends`, `SlvFls`, `SlvGB`, `MrgGB`
- **`OthelloLevelEnumerator` / `OLEMain`** — version bumped to 0.2.15

## [OLE v0.2.14] - 2026-05-28

### Changed
- **`OthelloLevelEnumerator` / `OLEMain`** — added 5th default output directory `F:\OLEDataDir5\` (`numOutputDirs` now defaults to 5); solve output files distribute across D:×2, E:×2, F:×1 via round-robin, so ~20% of solve output (~1.55 TB at L17) lands on F: instead of the NVMe drives; prevents the ~290 GB NVMe overflow that would otherwise occur at L17+; F: (HDD) is not a bottleneck since the GPU pipeline writes at ~11.6 MB/s per dir, well within HDD sustained write speed; added `--output-dir5` CLI flag; `outputDirs[4]` → `[5]`; version bumped to 0.2.14
- **`OthelloLevelEnumerator` / `OLEStatus.h`** — `OLE_STATUS_MAX_PARTS` 4 → 5 to accommodate 5th merge output partition; `OLE_STATUS_VERSION` bumped 3 → 4

### Note
F: is also used for NAS archive writes; concurrent HDD usage is modest (~11.6 MB/s solve writes vs ~130 MB/s archive peak) and acceptable.  Use `--output-dir5` to redirect to a different drive if desired.

## [OLE v0.2.13] - 2026-05-28

### Fixed
- **`OthelloLevelEnumerator` / `OLEMain`** — v0.2.12 regression: archive copy threads started before `PipelineRun` ran concurrently with solve reads on the same input files, causing 2–4× GPU pipeline slowdown (likely CPU bus contention from CopyFileA's buffer copies interfering with CUDA dispatch); fixed by starting each file's copy+delete thread in the callback *after* the file is fully read and closed, so the copy of file N runs while the solve reads file N+1 (different file — no contention); copy and delete both run in the background thread, so neither blocks the GPU pipeline; `JoinArchiveThreads` at shutdown restored; version bumped to 0.2.13

## [OLE v0.2.12] - 2026-05-27

### Changed
- **`OthelloLevelEnumerator` / `GPUPipeline.h`** — added `onInputFileConsumed` callback (`void (*)(const char* path, void* ctx)`) and `inputFileCtx` to `OLEPipelineConfig`; `PipelineRun` calls the callback after each input file's handle is closed (all records consumed)
- **`OthelloLevelEnumerator` / `GPUPipeline.cpp`** — calls `cfg->onInputFileConsumed(fd.path, cfg->inputFileCtx)` after `SFReaderClose` for each input file, if the callback is set
- **`OthelloLevelEnumerator` / `OLEMain`** — archive now starts **before** `PipelineRun`: NAS copies of all input files are launched concurrently in background threads at the start of each level's solve phase; as each input file is fully read, `OnInputFileConsumed` joins that file's copy thread (copy is typically long complete by then) and immediately deletes the local copy, freeing disk space progressively during solve; removed `ArchiveLevelAsync`, `g_archiveThreads`, `g_archiveMtx`, `JoinArchiveThreads`, and the end-of-run NAS wait log; version bumped to 0.2.12

### Fixed
- **Disk overflow during large solve phases**: previously all input files (e.g. 1.47 TB of L16 merge output) remained on local NVMe for the entire duration of the solve phase because the NAS archive fired only after `PipelineRun` returned; at L17+ the combined peak of input files + accumulating solve output exceeded 8 TB NVMe capacity; with this fix each input file is deleted as soon as it is read, keeping peak local disk usage at roughly the solve output size alone

## [OLE v0.2.11] - 2026-05-26

### Changed
- **`OthelloLevelEnumerator` / `OLEMain`** — updated default output directories: `outputDirs[2]` → `E:\OLEDataDir3\`, `outputDirs[3]` → `E:\OLEDataDir4\` (second NVMe); default NAS archive dir changed from `Z:\OthelloRuns\` to `F:\OthelloRuns\` (local striped HDD pool); version bumped to 0.2.11

## [OLE v0.2.10] - 2026-05-25

### Changed
- **`OthelloLevelEnumerator` / `OLEMain`** — NAS archival changed from one-thread-per-file (all files copying in parallel) to a single sequential background thread per level; each file is copied and deleted before the next begins; total transfer time is unchanged (NAS link is the bottleneck) but local disk space is freed file-by-file as each copy completes rather than all at once, and the NAS receives sequential writes instead of competing concurrent writes

## [OLE v0.2.9] - 2026-05-25

### Changed
- **`OthelloLevelEnumeratorStatus` / `OLEStatusQuery`** — added elapsed time, boards/s rate, and ETA to both solve and merge phase displays; `--loop` default interval changed from 5 s to 600 s (10 min); each loop refresh prints a `[YYYY-MM-DD HH:MM:SS]` timestamp header; added version-mismatch warning when status block version does not match query binary
- **`OthelloLevelEnumerator` / `OLEStatus.h`** — added `phaseStartMs` field (`volatile uint64_t`, `GetTickCount64()` snapshot) after the `phase` field; written by OLEMain at each solve/merge phase transition so the query can compute elapsed time; `OLE_STATUS_VERSION` bumped 2 → 3 (struct layout changed)
- **`OthelloLevelEnumerator` / `OLEMain`** — writes `g_status->phaseStartMs = GetTickCount64()` at both phase transitions (solve start and merge start); version bumped to 0.2.9

### Query output (SOLVE example)
```
OthelloLevelEnumerator v0.2.9  [SOLVE]
Run:    D:\OLEDataDir\2026_05_25.19_05_39\BoardSize6x6\
Board:  6x6    Level: 14 / 33

  Phase: SOLVE
  Elapsed:   23m 14s
  Progress:  550,554,389 / 1,208,836,883 boards read  (45.5%)
  Rate:      394,762 boards/s
  ETA:       ~27m 55s remaining
  GPU:       8,402 dispatches   |   4,198,787,300 slots expanded
  Files:     19 written
```

## [OLE v0.2.8] - 2026-05-25

### Added
- **`OthelloLevelEnumerator` / `OLEStatus.h`** — new shared header defining `OLEStatusBlock` (volatile struct in named shared memory `Local\OthelloLevelEnumeratorStatus`), `OLEPhase` enum, and `OLEStatusOpen` / `OLEStatusClose` inline helpers
- **`OthelloLevelEnumeratorStatus` / `OLEStatusQuery.cpp`** — new standalone query exe; opens the shared memory read-only and displays live progress; `--loop [N]` refreshes every N seconds (default 5); no CUDA or library dependencies
- **`OthelloLevelEnumerator` / `OLEMain`** — creates shared memory at startup, updates `phase` / `currentLevel` / solve and merge counters throughout the BFS loop, writes `lastLevel` stats after each level, sets `OLE_PHASE_DONE` and closes at shutdown
- **`OthelloLevelEnumerator` / `GPUPipeline`** — updates `solveBoardsRead`, `solveGpuDispatches`, `solveSlotsExpanded` after each GPU dispatch, and `solveFilesWritten` after each flush; `OLEPipelineConfig` gains `statusBlock*` field
- **`OthelloLevelEnumerator` / `MergePhase`** — updates `mergeRecordsWritten[partIdx]` on each output flush, `mergePartsDone` when a partition completes, and `mergeSrcFilesConsumed` in `SignalFileDone` when a source file is fully consumed by all partitions; `MergePhaseRun` gains `statusBlock*` parameter (default nullptr)

### Query output (MERGE example)
```
OthelloLevelEnumerator v0.2.8  [MERGE]
Run:    D:\OLEDataDir\2026_05_25...\BoardSize6x6\
Board:  6x6    Level: 17 / 33

  Phase: MERGE
  Sources:   677 total   |   342 consumed (50.5%)
  Part 0:    9,449,800,000 records  (223.74 GB)
  Part 1:    10,567,000,000 records  (250.21 GB)
  Part 2:    7,642,000,000 records  (180.98 GB)
  Part 3:    5,682,000,000 records  (134.57 GB)
  Total:     789.50 GB written
  Parts done: 1 / 4
```

## [OLE v0.2.7] - 2026-05-25

### Changed
- `OthelloLevelEnumerator` / `MergePhase`: progressive solve-file deletion — source (solve) files are now deleted during the merge phase as each file is fully consumed by all partition threads, rather than waiting until after merge completes; each partition signals "done" with a source file the moment its `SortedFileReader` is closed (whether quick-rejected, empty-range, or read to exhaustion in the k-way merge loop); when all `numParts` partitions have signaled a file, it is deleted via `remove()`; readers are also closed eagerly when a source is exhausted mid-merge (previously closed in a batch at end)
- `OthelloLevelEnumerator` / `MergePhase`: `SourceState` gains a `fileIdx` field tracking its index in `srcFiles`; `RunMergePartition` gains a `MergeDeleteState*` parameter (new internal struct with per-file `std::atomic<int>` completion counts); init loop is now index-based to feed `fileIdx`

### Rationale
At deep BFS levels (≥16) the solve files (~2.5 TB) and merge output (~1.85 TB) cannot coexist on a single 4 TB NVMe — the disk fills before merge completes.  Progressive deletion frees solve-file space as it is consumed, so the merge can run to completion using only `max(solve_remaining + merge_written)` peak disk — roughly equal to `solve_total`, which fits.

## [OLE v0.2.6] - 2026-05-24

### Added
- `OthelloLevelEnumerator` / `OLEMain`: NAS archival — after each level's `PipelineRun` returns (input files fully consumed), consumed merge files are asynchronously copied to a NAS drive and deleted locally; one thread per file, all running concurrently with `MergePhaseRun`; threads are joined cleanly at program exit
- `OthelloLevelEnumerator` / `OLEMain`: `--nas-dir [path]` arg — NAS root dir; defaults to `Z:\OthelloRuns\`; if path omitted, uses default; NAS archival is **ON by default**; run-dir suffix (`YYYY_MM_DD.HH_MM_SS\BoardSizeNxN\`) appended to construct the NAS run directory
- `OthelloLevelEnumerator` / `OLEMain`: `--no-nas` arg — disables NAS archival; if NAS directory cannot be created at startup, archival auto-disables with a warning
- `OthelloLevelEnumerator` / `OLEMain`: `LogPrintf` is now protected by `g_logMtx` (std::mutex) so archive completion messages from worker threads interleave safely with main-thread output
- `OthelloLevelEnumerator` / `OLEMain`: NAS archive dir shown in startup banner; archive queue and completion logged per file (`[Archive] Level NN  filename  (X.XX GB, Y.Y s)`)

### Rationale
All-levels merge files must be retained permanently (retrograde pass binary-searches them).  With D: running low at deep levels, archiving consumed input files to a NAS with 30 TB free keeps local NVMe clear for the current level's solve+merge working set.

## [OLE v0.2.5] - 2026-05-24

### Changed
- `OthelloBasics` / `OthelloBasics.h`: added `BOARD_KEY` struct (24 bytes: `ullCellsInUse` + `ullCellColors` + `usBoardInfo` + `_pad1[3]`), `static_assert(sizeof(BOARD_KEY)==24)`, and declaration of `BoardKeyGetMoves`; `BOARD_KEY` is the first 24 bytes of `BOARD` and works with all existing macros (`GETBOARDSIZE`, `GETBOARDNEXTPLAYER`, etc.) via C macro duck typing
- `OthelloBasics` / `BoardMoveCalculator.cpp`: added `BoardKeyGetMoves(const PBOARD_KEY)` — same 8-direction bitboard fill as `BoardMoveCalculator` but takes a `BOARD_KEY*` and returns the valid-move mask as `unsigned long long` instead of mutating a `BOARD`
- `OthelloBasicsForCUDA` / `OthelloBasicsForCUDA.h`: added a complete BOARD_KEY device-function family for OLE's CUDA-only path — `dev_applyMove_key`, `dev_rotate90Right_key`, `dev_mirrorVerticalAxis_key`, `dev_boardFlip_key`, `dev_boardLT_key`, `dev_playMove_key`, `dev_boardKeyGetMoves` (returns move mask), and `dev_canonicalize_key` (no moves computed — 384-byte `BOARD_KEY arr[16]` vs 1024-byte `BOARD arr[16]` scratch); `dev_canonicalize_key` does not take `DevBoardConsts` because it no longer calls `dev_boardMoveCalculator`; existing BOARD-based device functions unchanged (SolverKernel.cu still uses them)
- `OthelloLevelEnumerator` / `OLEKernel.h`: `GpuResult::childBoard` changed from `BOARD` (64 bytes) to `BOARD_KEY` (24 bytes); `WorkerGpuContext::d_inputBoards` / `h_inputBoards` changed from `BOARD*` to `BOARD_KEY*`; `OLEGpuBuffers::d_accumA` / `d_accumB` changed from `BOARD*` to `BOARD_KEY*`; `ExtractUniqueBoards` last parameter changed from `BOARD*` to `BOARD_KEY*`
- `OthelloLevelEnumerator` / `OLEKernel.cu`: `OthelloExpandKernel` now takes `const BOARD_KEY* inputBoards`; moves are computed on-GPU via `dev_boardKeyGetMoves` (no pre-computed `ullPossibleMoves` field required); `dev_playMove_key` + `dev_canonicalize_key` replace `dev_playMove` + `dev_canonicalize`; `ScatterToAccumKernel` now writes `BOARD_KEY` slots; all field/gather/dup-flag kernels changed from `const BOARD*` to `const BOARD_KEY*`; `ExtractUniqueBoards` updated end-to-end for `BOARD_KEY`; per-slot VRAM cost drops from `2×64+2×8+2×4+2×1=154` to `2×24+2×8+2×4+2×1=74` bytes, growing accumulation-buffer slot count from ~104 M to ~215 M on the same VRAM
- `OthelloLevelEnumerator` / `GPUPipeline.cpp`: `FlushBuffer` writes 24-byte `BOARD_KEY` records (`SFWrite` record/key size both `sizeof(BOARD_KEY)=24`); `PipelineRun` reads `BOARD_KEY` records from solve files; `BoardMoveCalculator` per-batch CPU pre-pass removed (GPU now computes moves internally via `dev_boardKeyGetMoves`)
- `OthelloLevelEnumerator` / `OLEMain.cpp`: seed file extracts `BOARD_KEY` fields from `BoardAllocateFirstBoard()` result and writes 24-byte records; `MergePhaseRun` uses `sizeof(BOARD_KEY)` for both record-size and key-size arguments; `kBytesPerSlot` and banner display math updated for 24-byte accum slots

### Rationale
Storing full 64-byte `BOARD` structs in solve/merge files and GPU buffers required ~6.8 TB of intermediate disk space at level 16, exceeding available storage.  Switching to 24-byte `BOARD_KEY` records reduces disk usage 62.5% (~2.55 TB at level 16) and nearly doubles GPU accumulation-buffer capacity on the same VRAM.

## [OLE v0.2.4] - 2026-05-24

### Fixed
- `OthelloLevelEnumerator` / `GPUPipeline`: `SFWrite` failure in `FlushBuffer` now captures `errno`, embeds `strerror_s` in the error message, and calls `ErrorPrint(stderr)` — previously the only error path that caused `PipelineRun` to return false was completely silent, making the level 16 failure impossible to diagnose
- `OthelloLevelEnumerator` / `GPUPipeline`: `SFReaderOpen` failure now prints via `ErrorPrint(stderr)` and sets `ok = false; break` instead of silently `continue`-ing — skipping an input file would have produced silently incomplete results
- `OthelloLevelEnumerator` / `GPUPipeline`: `recordSize` mismatch error now prints via `ErrorPrint(stderr)` and fails instead of silently `continue`-ing

## [OLE v0.2.3] - 2026-05-23

### Fixed
- `OthelloLevelEnumerator` / `MergePhase`: all four `Error()` call sites in `RunMergePartition` now capture `errno` and embed it in the error message, then call `ErrorPrint(stderr)` so merge failures print a full diagnostic line (`code`, message, and OS error string) to stderr rather than being silently swallowed in thread-local storage; previously a merge failure produced no visible output and the run terminated with no explanation in the log
- `OthelloLevelEnumerator` / `MergePhase`: fixed short-circuit evaluation bug in `MergePhaseRun` — the loop `for (auto& f : futures) allOk = allOk && f.get()` would skip `.get()` calls on later futures once one returned false, leaving those thread-pool tasks potentially still running while `srcFiles` (captured by reference) went out of scope; changed to `{ bool ok = f.get(); allOk = allOk && ok; }` so all futures are always waited on before the function returns
- `OthelloLevelEnumerator` / `OLEMain`: merge and pipeline failures now emit a visible `LogPrintf` line to the log file in addition to going to stderr; previously a failure exited the BFS loop silently with no log entry
- `OthelloLevelEnumerator` / `OLEMain`: added `_setmaxstdio(2048)` at startup to raise the CRT per-process file-handle limit from the default 512 to 2048, preventing `fopen_s` failures when merge phases open large numbers of source files simultaneously at deep BFS levels
- `OthelloLevelEnumerator` / `MergePhase`: replaced `strerror` with `strerror_s` (local `char eb[64]` buffer) at all four errno-reporting sites to resolve MSVC C4996 deprecation errors

## [OLE v0.2.2] - 2026-05-23

### Changed
- `OthelloLevelEnumerator` / `OLEMain`: switched all timing from raw `std::chrono` to the `ClockTick` utility (`ClockStart` / `ClockNanosSinceStart`); `#include <ClockTick.h>` added; `<chrono>` retained only for the memory-stats thread `condition_variable::wait_until` call
- `OthelloLevelEnumerator` / `OLEMain`: progress table now reports four timing columns per level instead of one — `SlvTm(s)` (GPU solve phase), `MrgTm(s)` (merge phase), `Tm(s)` (total, unchanged), and `PredTm(s)` (predicted total for the **next** level, computed as `Tm(s) × newBoardsNet / boardsIn`); `LevelRecord` extended with `solveNs` and `mergeNs` fields; a second `ClockNanosSinceStart` call is made immediately after `PipelineRun` returns to capture the solve-phase boundary

## [OLE v0.2.1] - 2026-05-23

### Changed
- `OthelloLevelEnumerator` / `OLEMain`: solve files are now deleted automatically
  after each level's merge phase completes and the checkpoint is written —
  `remove()` is called on every path in `solveReg`; failures are silently ignored
  so the run continues even if a file is locked or already gone; previously these
  intermediate files accumulated on disk for the entire run

## [OLE v0.2.0] - 2026-05-23

### Fixed
- `OthelloLevelEnumerator` / `OLEKernel`: replaced `BOARDKeyDecomposer` (24-element `uint8_t` CUB tuple decomposer on `BOARD`) with a three-pass primitive `uint64_t` CUB `DeviceRadixSort::SortPairs` approach — the decomposer caused CUB's histogram kernel to require 64 KB of shared memory (256 buckets × 64 threads × 4 bytes), exceeding CUB's 48 KB policy default on all SM versions including SM 8.9; the policy limit is internal to CUB and cannot be raised by changing the GPU target architecture; the new approach sorts LSB-first across three 8-byte fields (f2=bytes 16-23 first, f1, then f0=bytes 0-7 as MSB), using CUB's perfectly-tuned uint64_t radix-sort policies with no shared-memory overhead; a `uint32_t` index permutation is maintained alongside the sort so BOARDs stay in place in the accumulation buffer and are gathered in sorted order during D2H extraction; confirmed: 6×6 level 13 sorts ~71.8 M items in ~245 s (previously 30+ min under thrust comparison sort)

### Changed
- `OthelloLevelEnumerator` / `OLEKernel`: `OLEGpuBuffers` extended with `d_fieldA`/`d_fieldB` (`uint64_t*`, one per slot, shared scratch for the three sort passes) and restored `d_indicesA`/`d_indicesB` (`uint32_t*`, one per slot, for the index permutation); both arrays are shared between the two ping-pong buffers (only one sort runs at a time); per-slot VRAM cost: `2×sizeof(BOARD) + 2×sizeof(uint64_t) + 2×sizeof(uint32_t) + 2×sizeof(uint8_t)` = 154 bytes; yields ~104.5 M slots on an RTX 4080 SUPER (16 GB VRAM, 1 GB headroom)
- `OthelloLevelEnumerator` / `OLEMain`: `accumBufSlots` formula updated from `vramAvail / (2×sizeof(BOARD))` to `vramAvail / 154` to account for all scratch arrays
- `OthelloLevelEnumerator` / `MergePhase`: replaced all `memcmp`-based ordering comparisons with `BoardKeyCompare` (raw `uint64_t` field comparison, no bswap) — pivot sort in `ComputePivots`, file-range overlap checks, and k-way merge minimum selection; dedup equality check unchanged (memcmp equality is equivalent)
- `OthelloLevelEnumerator` / `SortedFile`: `SFLowerBound` binary-search comparator updated from `memcmp` to `BoardKeyCompare` to match the GPU sort order; `cmpLen` local removed (unused after the change)
- `OthelloLevelEnumerator` / `MarkDupFlagsKernel`: updated signature to accept the sorted index permutation (`const uint32_t* perm`); compares `boards[perm[i-1]]` vs `boards[perm[i]]` rather than adjacent array positions
- `OthelloLevelEnumerator` / `ExtractUniqueBoards`: D2H-copies the boards, permutation, and dup flags; gathers unique boards on the CPU using the permutation (`outBoards[out++] = hAccum[hIndices[i]]` for `hFlags[i]==0`)

### Fixed (minor)
- `OthelloSolverCommandLine`: two resume-path GPU-device banner lines aligned with two extra spaces to match the fresh-run banner (`doRestartProcess`)

## [v2.5.4] - 2026-05-21

### Fixed
- `OthelloSolverCommandLine` / `SolverKernel`: eliminated the remaining DedupKernel false-positive by switching the reader path to `volatile` loads for words 1 and 2 of the hash-table slot — plain loads go through the reader SM's L1 cache, which can return a stale 0 for `ullCellColors` even after the writer's `__threadfence()` has committed the value to L2/global; a board with `ullCellColors == 0` (all-white) could therefore match on word 1 against a stale L1-cached zero while simultaneously seeing the already-L2-visible word 2 (`usBoardInfo`), producing a false-positive duplicate; the fix reads both words via `volatile const uint64_t*` (bypassing L1) and checks word 2 first — a zero word 2 means the writer has not yet committed (usBoardInfo is always non-zero), so the slot is treated as not-yet-visible and probing continues; once word 2 reads non-zero the writer's earlier `__threadfence()` guarantees word 1 is also committed to L2, so the subsequent volatile read of word 1 is correct; confirmed: reduced level-13 discrepancy from 3 boards (pre-v2.5.2) to 1 board (v2.5.3) to zero (v2.5.4 expected)

---

## [v2.5.3] - 2026-05-21

### Fixed
- `OthelloSolverCommandLine` / `SolverKernel`: added `__threadfence()` between the word-1 (`ullCellColors`) and word-2 (`usBoardInfo`) writes in `DedupKernel` — without the fence, CUDA memory reordering could make word-2 visible to other threads before word-1; for boards whose `ullCellColors == 0` (all-white boards), the stale word-1 read of 0 was indistinguishable from the correctly written value, causing a false-positive duplicate match and silently dropping valid boards; confirmed by comparison run showing 3 all-white level-13 boards present in no-GPU_DEDUP but absent from GPU_DEDUP

### Added
- `OthelloSolverCommandLine` / `SolverKernel`: within-batch deduplication via GPU sort — before the cross-batch hash-table `DedupKernel`, two new kernels run on the same CUDA stream: `BuildSortKeysKernel` extracts the 3-word board key for every valid result slot into `d_batchKeys` and initialises `isNewBoard` (1=valid, 0=padding); `thrust::sort_by_key` then sorts `d_batchKeys`/`d_batchIndices` so within-batch duplicates become adjacent; `MarkIntraBatchDupsKernel` scans consecutive equal keys and marks the later occurrences `isNewBoard=0`; `DedupKernel` then skips any slot already marked 0, only spending hash-table probes on boards that survived within-batch dedup; catches 100% of within-batch duplicates (multiple parents in the same batch producing the same canonical child) with exact byte-for-byte comparison — no hash involved
- `OthelloSolverCommandLine` / `SolverKernel.h`: `d_batchKeys` and `d_batchIndices` (`void*`) added to `WorkerGpuContext` under `GPU_DEDUP`; allocated in `WorkerGpuContextCreate` (~27 MB per worker at 65536×15 slots) and freed in `WorkerGpuContextDestroy`
- `TieredStoreComparisonTool`: new standalone console project — streams two board-level TieredStores in sorted order and reports records present in only one; enforces strictly-ascending, unique keys from each iterator (duplicates skipped, out-of-order records flagged and skipped, anomaly counts reported in summary); used to diagnose the 3-board discrepancy between GPU_DEDUP and no-GPU_DEDUP runs at level 13

---

## [v2.5.2] - 2026-05-20

### Fixed
- `OthelloSolverCommandLine` / `SolverKernel`: GPU dedup table switched from 64-bit hash keys to exact 3-word board identity storage — each logical slot now holds `ullCellsInUse` (word 0), `ullCellColors` (word 1), and `usBoardInfo` (word 2) as three consecutive `uint64_t` words; slot selection still uses a hash (for distribution), but comparison is bitwise exact so false positives are impossible; `dev_boardKey` replaced by `dev_boardSlot` (hash for index only, not stored); `DedupKernel` claims a slot via `atomicCAS` on word 0, writes words 1–2, then on revisit compares all three words — a race that leaves words 1/2 momentarily unwritten produces a false negative (dup not caught by GPU, caught later by TS) but never a false positive (`usBoardInfo` is never 0 for a valid board, so a stale zero in word 2 cannot match); VRAM footprint increases from 4 GB (512 M × 8 B) to 12 GB (512 M × 24 B) on the RTX 4080 SUPER (16 GB VRAM); this eliminates the 3-board shortfall observed at level 13 in the prior hash-based implementation

---

## [v2.5.1] - 2026-05-20

### Fixed
- `OthelloSolverCommandLine`: column key legend corrected for GPU-dedup mode — two formulas were stale:
  - `Mvs[N] = NewBoards[N] + Pass[N]` → `NewBoards[N] + GpuDups[N] + Pass[N]` (GPU-deduped moves contribute to total moves generated but are excluded from both NewBoards and Pass)
  - `BoardsIn[N] = (NewBoards[N-1] - Dups[N-1]) + Pass[N]` → `(NewBoards[N-1] + GpuDups[N-1] - Dups[N-1]) + Pass[N]` (NewBoards no longer counts GPU-deduped boards, so GpuDups must be added back to recover the net unique count)
  - `NewBoards[N]` description updated from "gross inserts into next level (includes dups)" to "boards uniquely new at level N+1 after GPU + TSInsert dedup"
  - Added `GpuDups[N]` description and corrected net-unique formula to `NewBoards - (Dups - GpuDups)`
  - Both the live BFS progress output and the saved results report updated identically

---

## [v2.5.0] - 2026-05-20

### Added
- `OthelloSolverCommandLine` / `SolverKernel`: GPU-side board deduplication via a VRAM hash table (`#define GPU_DEDUP` toggle; comment out to revert to CPU-only TSInsert dedup) — a flat `uint64_t[]` array of 512 M slots (4 GB) is allocated once at run start and cleared at each BFS level boundary; `dev_boardKey` mixes `ullCellsInUse`, `ullCellColors`, and `usBoardInfo` (player-to-move bit) into a 64-bit key (forced odd so 0 remains the empty sentinel); `DedupKernel` runs after `OthelloExpandKernel` with one thread per result slot, using `atomicCAS` linear probing (max 32 probes) and a conservative fallback (mark new) when the probe limit is reached; `ctx->h_isNewBoard[]` is copied D2H alongside results so `WorkerProcessBatch` can skip `TSInsert` for GPU-identified duplicates; move edge inserts are always performed regardless of dedup status (back-propagation needs the complete edge graph)
- `OthelloSolverCommandLine` / `SolverKernel.h`: `GpuDedupTable` struct (`d_slots`, `tableSlots`, `tableMask`); `d_isNewBoard`/`h_isNewBoard` buffers added to `WorkerGpuContext`; `GpuDedupTableAlloc`, `GpuDedupTableClear`, `GpuDedupTableDestroy` API; `DispatchBatch` signature extended with optional `GpuDedupTable*` parameter under `GPU_DEDUP`
- `OthelloSolverCommandLine` / `SolverWorker.h`: `gpuDedupCount` (`std::atomic<uint64_t>`) added to `WorkerLevelStats` under `GPU_DEDUP`
- `OthelloSolverCommandLine`: `GpuDups` column added to both the live BFS progress table and the final results report — shows the count of boards caught by the GPU hash table before reaching TSInsert; `Dups` column shows the combined total (GPU dups + TSInsert dups)

### Fixed
- `OthelloSolverCommandLine`: all board/move/dup/child counters widened from `long long`/`int` to `uint64_t` — `WorkerLevelStats` fields (`newBoards`, `totalChildren`, `terminalBoards`), `LevelRecord` fields (`boardsIn`, `newBoardsOut`, `totalChildren`, `terminalBoardsOut`, `dupBoards`, `gpuDedups`), `RunSolverCore` locals (`boardsIn`, `gpuDispatches`, `totalBoardsProcessed`, `totalUniqueBoards`, `totalGpuDispatches`, `totalChildren`, `totalTerminals`, `trueDups`, `dups`, `passes`), and `doReportResults` parameters; timing values (`elapsedNs`, `predictedNs`, `nsPerBd`) remain `long long` with explicit casts where divided by `uint64_t` board counts to prevent signed/unsigned promotion
- `OthelloSolverCommandLine`: `Pass` column formula corrected for GPU-dedup mode — was `totalChildren - newBoardsOut` (inflated by GPU-deduped regular moves that also don't increment `newBoardsOut`); fixed to `totalChildren - newBoardsOut - gpuDedups`; both the live progress table and final report use the corrected formula; recovers the true pass-move count (e.g. level 8: 356038 - 298454 - 57569 = 15, matching the pre-dedup baseline)

---

## [v2.4.3] - 2026-05-20

### Fixed
- `TieredStoreHybrid` / `TSStatus`: moved `TSI_WaitForBgMerge` to the top of `TSStatus` before acquiring the read lock — callers that invoked `TSStatus` while a background flush was in flight saw stale `diskRecords`, `filesInUse`, and `totalRecords`; five `TieredStoreTester` tests (`FlushToDisk`, `MultiFlush`, `Merge`, `Split`, `DeletedDataFile`) all depended on post-flush state and failed until the wait was added
- `TieredStoreHybrid` / `TSI_FinalizeJob`: records that accumulated in `memTree` during a background flush were silently stranded — the soft trigger skips when `bgPending == 1`, so after finalization the in-memory tree sat above threshold with no flush scheduled; fixed by auto-retrigger: when the last slice completes and `memTree >= maxMemoryRecords`, `TSI_FinalizeJob` calls `TSI_TriggerBgFlush` directly instead of clearing `bgPending`, chaining flushes without `bgPending` ever dropping to 0; `TSI_WaitForBgMerge` only unblocks once `memTree` has fully drained
- `TieredStoreHybrid` / `TSI_FinalizeJob`: `statSplits` was never incremented for parallel-flush jobs — each slice produces exactly one output file so the per-slice `splitOccurred` flag is never set, even when the job as a whole produces multiple output files; fixed by also counting `job->toRegister.size() >= 2`
- `TieredStoreHybrid` / `TSI_PrepMergeJob`, `TSI_FlushMemTree`: `BPGetLevelStartKeys` returns `N = 0` when the B+tree's direct-children level already contains more nodes than `targetNodeCount` (shallow wide trees); the previous code assumed `N > 0` and either crashed or dispatched a single oversize partition; fixed by falling back to the linear `addGapSlices(nullptr, nullptr)` / `flushGap(nullptr, nullptr)` path when `N == 0`, matching the behavior used for inter-file gap scans
- `TieredStoreHybrid` / `flushGap`, `addGapSlices`: both lambdas assumed a non-null `gapStart` and always called `BPIterateStartFrom`; the N=0 fallback needs to scan from the tree beginning; fixed by branching on `gapStart != nullptr` and calling `BPIterateStart` when null
- `TieredStoreTester` / `TestDeletedDataFile`: test read `internal->numFiles` directly without waiting for the background flush, racing the async merge; adding a `TSStatus` call first ensures flush completion before inspecting internal file count

### Added
- `TieredStoreTester` / Group 10: `TestLargeMultithreaded` — concurrent insert stress test from T threads (T = `hwConc/2`, clamped `[2, 8]`); shared key zone `[1, 10000]` where every thread inserts `value=1` (exercises merge-time deduplication and value accumulation), plus per-thread exclusive zones; generates enough records to guarantee multiple merges and splits; after `TSClose`/reopen, verifies correctness via two independent passes — `TSEnumerate` (unordered count + merged values) and `TSIterator` (strict ascending order, merged values, total count); asserts `totalMerges > 0` and `totalSplits > 0`

---

## [v2.4.2] - 2026-05-20

### Fixed
- `TieredStoreHybrid` / `TSI_PrepMergeJob`, `TSI_FlushMemTree`: `BPGetLevelStartKeys` was called with `maxFileRecords` (16.8 M) as `targetNodeCount`; that parameter is the maximum sibling count at the chosen level, not the target records per output file; for a 35.5 M-record tree no internal level has 16.8 M siblings, so the walker descended to the leaf-adjacent level (~542 nodes) and returned 541 partition boundaries instead of the intended 2–3, dispatching hundreds of tiny pool jobs and silently discarding extra output files; fixed by passing `ceil(count / maxFileRecords)` as `targetNodeCount`
- `TieredStoreHybrid` / `TSI_RunSliceJob`: `srcFile` was passed to `remove()` on `DoMerge` failure even when no output had been written, permanently destroying existing file data; fixed by keeping `srcFile` on disk on failure; `TSI_FinalizeJob` re-inserts unmerged `srcFile`s into `ts->files[]` so they remain accessible in the current session (metaStore discrepancy self-heals at the next merge)

---

## [v2.4.1] - 2026-05-19

### Fixed
- `OthelloSolverCommandLine` / `SolverWorker`: `WorkerLevelStats::newBoards` was `std::atomic<int>` (signed 32-bit, max ~2.1 B); at level 14 of the 6×6 solve (~2.87 B new boards) it overflowed to a large negative value, which cascaded into the `Pass` column (`totalChildren - newBoardsOut`) printing an impossibly large positive number; fixed by widening to `std::atomic<long long>`, matching `totalChildren` and `terminalBoards`
- `OthelloSolverCommandLine`: removed spurious `(int)` cast on `passBatch.size()` before accumulating into `long long boardsIn`; the cast was harmless in practice (pass batches are bounded by `batchSize`) but incorrect and masked the same class of overflow

---

## [v2.4.0] - 2026-05-19

### Added
- `BPlusTreeHybrid`: `BPGetLevelStartKeys` — new API that walks the B+tree internal node levels to find natural partition boundaries; descends the leftmost path counting siblings at each level and stops at the deepest level where sibling count ≤ `targetNodeCount`; for each non-first sibling at that level descends to its leftmost leaf to retrieve the true subtree minimum key (a key node's `ppDataPtrArray[0]` is a child separator, not the subtree minimum); returns raw `void*` pointers into live tree node data — caller must copy key bytes before the tree is freed or swapped; safe to call on a frozen snapshot tree with no concurrent writers
- `Utility/Error.h`: `FATAL_TS_UNBALANCED_TREE` (RC_FATAL_BASE + 29) — fatal code raised when a level-walker partition contains more records than `maxFileRecords`, indicating the B+tree's internal structure is unexpectedly unbalanced

### Changed
- `TieredStoreHybrid`: fully parallel background flush (v2.4.0) — replaces the sequential per-slice loop in `TSI_BackgroundMerge` with one independent pool job per slice; all jobs are submitted simultaneously to `mergePool` and run concurrently; `TSMergeJob` gains `pendingSlices` (atomic countdown), `collectMutex` (guards `toRegister` and `anyFailed`), `toRegister` (output files collected by completed slices), `anyFailed`, and `splitOccurred`; the new `TSI_RunSliceJob` handles one slice's DoMerge + srcFile deletion + output collection; `TSI_FinalizeJob` is called by whichever slice completes last (countdown hits 0), re-acquires the write lock, registers all outputs, recycles the arena, and signals `bgPending = 0`; no file-file merges ever occur — files already carry non-overlapping key ranges, so each job is always a 2-way merge of a B+tree slice with at most one existing file
- `TieredStoreHybrid`: new B+tree-walk routing in both `TSI_PrepMergeJob` and `TSI_FlushMemTree` — instead of per-file zones `[file.minKey, nextFile.minKey)`, the code now walks the B+tree forward key-by-key; for each current key an `upper_bound` search on the sorted file list finds whether the key falls inside an existing file's `[minKey, maxKey]` range (file merge) or in a gap between files (gap merge); file merges set the tree cursor's `hiKey` to the first B+tree key strictly after `file->maxKey` (via `BPIterateStartFrom(maxKey, returnEqual=false)`) so the boundary record is never excluded; gap merges scan linearly and split every `maxFileRecords` records using a peek-ahead to collect split boundaries before submitting slices; file cursors always read to EOF (no `hiKey` bound) since files carry disjoint ranges; the new no-files path uses `BPGetLevelStartKeys` when the tree exceeds `maxFileRecords` to split it into parallel partitions without any linear scan, with a `Fatal(FATAL_TS_UNBALANCED_TREE)` guard if any partition exceeds the record cap

---

## [v2.3.10] - 2026-05-19

### Changed
- `TieredStoreHybrid`: replaced the k-way merge strategy with per-file independent 2-way merges — on every B+tree flush, disk files are sorted by minKey to establish non-overlapping key zones; for each zone `[file.minKey, nextFile.minKey)` the B+tree is peeked (via `BPIterateStartFrom`) to see if it has any records in that range; if yes, a 2-way merge (B+tree slice + that one file) is run producing 1–2 output files; if no, the file is left completely untouched (zero I/O); B+tree records before all files produce a new pre-zone file; files are **never merged with each other** — they already carry disjoint key ranges so merging them would only burn disk bandwidth with zero benefit; this eliminates the original `FindOverlappingFiles` + `DoParallelMerge(all overlapping files)` pattern that caused files with non-overlapping key ranges to be read and rewritten whenever the B+tree's range happened to span them all
- `TieredStoreHybrid`: removed intra-merge parallelism (`DoParallelMerge`, `RunPartitionMerge`, `ComputeNumParts`, `ComputePartitionKeys`, `PartitionJob`) — no longer applicable now that each merge is a simple 2-way operation between one B+tree slice and one file; the partition machinery assumed multiple source files that could be distributed among threads, which no longer occurs
- `TieredStoreInternal.h`: `TSMergeJob` restructured — replaces flat `srcFiles` + `outDescs` vectors with `std::vector<TSFileMergeSlice> slices` where each slice carries its own `srcFile`, `loKey`, `hiKey`, and `outDescs`; allows `TSI_PrepMergeJob` to capture per-zone routing at prep time and `TSI_BackgroundMerge` to process each slice independently

---

## [v2.3.9] - 2026-05-19

### Added
- `TieredStoreHybrid`: true duplicate counter — `std::atomic<uint64_t> statDups` added to `_TieredStore`; incremented lock-free from two paths: (1) `TSInsert` when `BPInsertCopy` returns `BP_RC_Duplicate_Found` (B+tree-level dup, key already in the in-memory tree), and (2) `DoMerge` inner loop when consecutive cursors compare equal (merge-time dup, same key in two source files/tree during a flush); merge path uses a local accumulator and a single `fetch_add` per merge to minimize atomic traffic; `TSGetDupCount(PTS)` API added to `TierdStore.h` and implemented in `TieredStoreStatus.cpp`
- `OthelloSolverCommandLine`: `Pass` column in level reports — `Pass[N] = totalChildren[N] - newBoardsOut[N]` (pass moves generated and worked within level N; their children land in `NewBoards[N]`); added to both the live progress table and the final summary level-analysis table; `Mvs = NewBoards + Pass` holds exactly at every row
- `OthelloSolverCommandLine`: column-key legend printed before the level table in both live output and saved results file — explains `BoardsIn[N] = (NewBoards[N-1] - Dups[N-1]) + Pass[N]`, the role of `Pass`, and includes the hardcoded 4×4 lv4→5 example `(105 - 9) + 2 = 98`

### Fixed
- `OthelloSolverCommandLine`: `Dups` column was previously computed as `totalChildren - newBoardsOut`, which captured pass moves rather than true duplicates (because `TSInsert` always returned `TS_RC_Success` for dups, so `newBoardsOut` counted both unique and duplicate boards); replaced with `TSCheckpoint(board[level+1])` + `TSGetDupCount(board[level+1])` to read the true count of duplicates suppressed by the TieredStore across both insert-time and merge-time deduplication paths; the 6×6 run now correctly shows millions of dups at deeper levels (e.g. 132 M at level 12) that were previously invisible

---

## [v2.3.8] - 2026-05-18

### Changed
- `TieredStoreHybrid`: intra-merge parallelism — large merges (total records > 2 × maxFileRecords) are now split into N independent partitions (N = min(poolThreads/2, numSrcFiles, 8)) and run concurrently via `std::async(std::launch::async)` rather than sequentially; partition key boundaries are derived from source-file minKeys (whole files are assigned to one partition, so no binary-search seek within files is needed); the memory-tree cursor for each partition uses `BPIterateStartFrom` to jump directly to its lower bound instead of scanning from the start; partitions 1..N-1 run on OS threads, partition 0 runs on the calling thread (avoids nested-job deadlock with the shared merge pool); `TSI_PrepMergeJob` and `TSI_FlushMemTree` pre-allocate `numOutFiles + numParts` output descriptors so every partition is guaranteed at least one output file; empty output files (partitions that produce zero records after deduplication) are dropped as before; expected speedup for level 14+ merges: ~4× (from ~7 min to ~1–2 min per merge)

---

## [v2.3.7] - 2026-05-18

### Changed
- `TieredStoreHybrid`: `FILE_FLAG_SEQUENTIAL_SCAN` on all merge file opens — added `OpenSeq()` helper that uses `CreateFileA` + `_open_osfhandle` + `_fdopen` with `FILE_FLAG_SEQUENTIAL_SCAN`; tells Windows to prefetch aggressively for sequential reads and deprioritize caching of written pages; used for all cursor input files (`InitCursorFile`) and all merge output files (`DoMerge`); random-access paths (`TSI_DeleteFromFile`, `TSI_UpdateInFile`, binary search) continue to use plain `fopen_s`; `setvbuf(4 MB)` is now applied inside `OpenSeq` so both hints are always paired

---

## [v2.3.6] - 2026-05-18

### Changed
- `TieredStoreHybrid`: 4 MB stdio buffers on all merge I/O — `setvbuf(f, NULL, _IOFBF, 4*1024*1024)` added after every `fopen_s` in the merge path (both input cursors in `InitCursorFile` and output files in `DoMerge`); replaces stdio's default 8 KB buffer with 4 MB, reducing OS calls from ~200 records/call to ~100K records/call for sequential merge reads and writes
- `TieredStoreHybrid`: combined slot write in `DoMerge` inner loop — previously wrote record bytes and flag byte in two separate `fwrite` calls; now sizes the merge buffer as `slotSize` bytes (record + flag), zeroes the flag byte at construction (merge output is always live), and writes the whole slot in a single `fwrite`

---

## [v2.3.5] - 2026-05-18

### Changed
- `TieredStoreHybrid`: shared merge thread pool — `TSCreate`/`TSOpen` now accept an optional `ThreadPool* pMergePool` (default `nullptr`); when a caller-provided pool is supplied the store uses it without owning it (`ownsPool = false`) so `TSClose` does not `Stop()`/`delete` it; when `nullptr` is passed the store creates its own private `ThreadPool(1)` as before (`ownsPool = true`); `TSI_FreeStore` always calls `TSI_WaitForBgMerge` before releasing the pool pointer regardless of ownership; meta-stores always pass `nullptr` (own private pool); forward declaration in `TierdStore.h` corrected from `struct ThreadPool` to `class ThreadPool` to match the definition in `ThreadPool.h` and avoid MSVC linker name-mangling mismatch
- `OthelloSolverCommandLine`: wires `chunkPoolThreads` (~26 threads) to TieredStore — `doStartProcess` and `doRestartProcess` each create a `ThreadPool mergePool(chunkPoolThreads, "TSMerge")` before the first store opens and stop it after `RunSolverCore` returns (all stores are already closed at that point); all four store helpers (`CreateBoardStore`, `CreateMoveStore`, `OpenBoardStore`, `OpenMoveStore`) pass the global `g_mergePool` pointer to `TSCreate`/`TSOpen`; concurrent merges across board and move stores are now possible

---

## [v2.3.4] - 2026-05-18

### Added
- `OthelloSolverCommandLine`: multi-directory output striping — `SolverConfig.outputDirs[4]` replaces the single `outputDir`; `outputDirs[0]` is the primary dir (logs, manifests, `.tsf` files); extra dirs receive the same timestamp/board-size subpath automatically; default is `{"D:\\CommandLineSolverDataDir", "C:\\CommandLineSolverDataDir"}` with `numOutputDirs=2`; overridden per-slot via `--data-dir2`, `--data-dir3`, `--data-dir4` CLI flags; not needed on `restart` (TieredStore manifests already record all dirs); `CreateBoardStore`/`CreateMoveStore` build the N-element `dirs[]` array for `TSCreate`, creating extra-drive subdirectories automatically

---

## [v2.3.3] - 2026-05-18

### Fixed
- `TieredStoreHybrid`: N-way file split — `DoMerge` previously capped output at 2 files (`desc1` + optional `desc2`); when total merged records exceeded `2 × maxFileRecords` the second file grew without bound, producing files 37–53× larger than the 1 GB target in the v2.3.2 6×6 run (Board/Level14 = 37 GB × 2, Board/Level15 = 53 GB × 2, Move/Level13 = 45 GB × 2); fixed by replacing the `desc1`/`desc2` pair with `std::vector<TSFileDesc*> outDescs` of `N = ceil(total / maxFileRecords)` output descriptors; `DoMerge` advances to the next descriptor when the current one reaches `maxFileRecords` records; empty trailing descriptors (possible after heavy deduplication) are cleaned up before registration; `TSMergeJob` struct updated to carry `outDescs` vector; both the synchronous (`TSI_FlushMemTree`) and background (`TSI_PrepMergeJob` / `TSI_BackgroundMerge`) merge paths updated; `statSplits` now counts any merge that produces more than one output file

---

## [v2.3.2] - 2026-05-18

### Added
- `OthelloSolverCommandLine`: background memory-stats logger — a dedicated 5-second polling thread appends one line per tick to `memory_stats_YYYYMMDD_HHMMSS.log` in the run output directory; columns: `DateTime`, `WorkSet(GB)` (physical RAM in use — the primary leak signal), `Commit(GB)` (private committed virtual bytes), `SysFree(GB)` (system-wide free physical RAM); timestamp format matches the per-level `DateTime` column in the main log so the two files can be correlated directly; thread uses `condition_variable::wait_until` for clean shutdown and holds no solver locks

---

## [v2.3.1] - 2026-05-18

### Fixed
- `TieredStoreHybrid`: arena pool slot destroyed on store close — `TSI_FreeStore` called `ArenaMemDestroy(ts->spareArena)` unconditionally; after exactly one background flush `spareArena` held the external pool arena passed by the caller, so destroying it freed the pool slot; the next `TSCreate` for the same level passed the freed pointer to `BPCreateTree`, left `memTree = null`, and the first `TSInsert` returned `TS_RC_Out_Of_Memory` (rc=3); fixed by adding `externalArena` field to `_TieredStore` (set at create/open to the caller-supplied arena) and guarding all `ArenaMemDestroy` calls with `ptr != ts->externalArena`; same guard applied in `TSI_BackgroundMerge`
- `TieredStoreHybrid`: dynamic spare arena leaked on store close — after an odd number of background flushes `pMemArena` held a dynamically created arena (`ArenaMemCreate` in `TSI_TriggerBgFlush`); `BPFreeTree(tree, true)` resets the arena offset but does not free the heap allocation; `TSI_FreeStore` was nulling `pMemArena` without calling `ArenaMemDestroy`, leaking ~2.5 GB per store close with odd flush count (visible as a sawtooth upward trend); fixed by checking `pMemArena != externalArena` before destroying

### Changed
- `OthelloSolverCommandLine`: removed all 19 `#ifdef TS_USE_BPTREE_ARENA` / `#else` / `#endif` guards — arena path is now unconditional; dead non-arena `else` branches deleted; `MAX_MEMORY_PER_STORE` constant removed (was only referenced in the deleted branches)
- `OthelloSolverCommandLine`: replaced UTF-8 `→` (U+2192) with ASCII `->` in memory banner format strings — the Windows console default code page rendered the 3-byte UTF-8 sequence as mojibake (`ΓåÆ`)

---

## [v2.3.0] - 2026-05-17

### Added
- `TieredStoreHybrid`: background merge — `TSInsert` now triggers an asynchronous flush via `TSI_TriggerBgFlush` instead of blocking on disk I/O; the full in-memory tree is swapped to `bgTree` under the write lock (fast), a fresh empty tree is installed for continued inserts, and the actual merge + file write runs on a dedicated 1-thread `ThreadPool` worker per store
- `TieredStoreHybrid`: `bgTree` search in `TSFind` — reads check `memTree`, then `bgTree` (the tree being merged in the background), then disk files, so no records are invisible during an in-flight merge
- `TieredStoreHybrid`: arena double-buffering — `spareArena` field recycles the flushed tree's arena (reset by `BPFreeTree`) for the next flush cycle; only one one-time allocation on the first flush; subsequent flushes alternate between two arenas at zero allocation cost
- `TieredStoreHybrid`: `TSI_WaitForBgMerge` — callers that need a complete on-disk snapshot (`TSCheckpoint`, `TSEnumerate`, `TSIterOpen`, `TSUpdate`, `TSDelete`) wait for any in-flight merge to finish before proceeding, using a `condition_variable` signaled by the bg thread

### Changed
- `TieredStoreHybrid`: `TSCheckpoint`, `TSEnumerate`, `TSIterOpen`, `TSUpdate`, `TSDelete` all use a wait loop (`release write lock → wait for bg → re-acquire`) to avoid deadlock with the bg merge thread's own write-lock acquisition at finalization time
- All `TSCreate`/`TSOpen`/`BPCreateTree` arena-mode call sites in `OthelloSolverCommandLine`, `OthelloSolverMFCandCUDA`, and `TieredStoreTester` corrected — `pArena` argument moved to the final (optional) position to match the current API signatures

---

## [v2.2.0] - 2026-05-17

### Added
- `BPlusTreeHybrid`: new project — single B+ tree library that accepts an optional `PArenaMem`; uses arena allocation when provided, `malloc`/`free` otherwise; replaces the separate `BPlusTree` + `BPlusTreeArena` compile-switch pair with a single runtime-selectable library
- `TieredStoreHybrid`: new project — TieredStore variant backed by `BPlusTreeHybrid`; the optional `PArenaMem` passed to `TSCreate`/`TSOpen` flows through to the in-memory B+ tree; no compile switch required
- `TieredStoreAndBPlusTreeTester`: new MFC dialog test application — comprehensive performance and correctness test suite for both `BPlusTreeHybrid` and `TieredStoreHybrid`:
  - Test phases: Sequential Insert, Random Insert, Duplicate Insert, Find/Verify, Update, Delete, Mixed Slam (concurrent readers + writers), Bulk Insert (single-threaded large-scale)
  - Verification phases: Integrity Check, Iterator Enumerate, Checkpoint+Reopen, Corrupt Open
  - **Compare mode**: runs malloc and arena back-to-back using identical memory budgets; displays Ops/s, Avg ns, and speedup ratio side-by-side in the same result row
  - N-run averaging: repeats each test set N times and posts averaged results
  - Live stats panel: inserts/s, finds/s, active threads, progress bar, current phase
  - Configurable: writer/reader threads, records/thread, key range, dup %, arena MB, node order, bulk record count
  - Save results to UTF-8 text file with fixed-width columns and a test-parameters header; offers to open in Notepad on completion

### Changed
- `TieredStoreAndBPlusTreeTester` / `TestEngine`: Arena MB now controls the memory budget for **both** malloc and arena paths equally — malloc B+ tree uses `arenaSizeMB` and malloc TieredStore uses `arenaSizeMB × ¾`, matching arena mode; previously the malloc paths used hardcoded sizes (infinite for BP, 16 MB for TS), making malloc vs arena comparisons unfair
- `Utility` / `Utility.h`: added `ArenaMem.h` to the umbrella include

---

## [v2.1.1] - 2026-05-16

### Fixed
- `BPlusTree` / `BPlusTreeArena`: replaced all `RMemCpy` calls (right-shift overlapping copies in insert path) with `memmove`; replaced all same-array left-shift `memcpy` calls in delete path (`BPDelete.cpp`) with `memmove` — both were undefined behaviour per C standard even though MSVC happened not to corrupt data in practice
- `Utility` / `Mem.h` / `Mem.cpp`: removed `RMemCpy` declaration and definition entirely; standard `memmove` handles both shift directions correctly

---

## [v2.1.0] - 2026-05-16

### Added
- `Utility` / `ArenaMem`: new bump-pointer arena allocator — `ArenaMemCreate` pre-allocates a contiguous block (malloc + memset); `ArenaMemAlloc` advances an atomic offset for lock-free per-thread allocation; overflow chain falls back to a linked list of extension blocks; `ArenaMemReset` resets the offset to zero for reuse without re-allocating
- `Utility` / `SysMemInfo.h`: new header-only memory budget helpers — `MemoryMode` enum (`MM_RECOMMENDED`, `MM_USE_MAX`, `MM_SPECIFIED`), `ParseMemorySize()` (parses "34GB", "16000MB", etc.), `CalcMemoryBudget()` (calls `GlobalMemoryStatusEx`; returns 75% of available RAM for recommended, 95% for max, or a specified amount; capped at 95% of total physical RAM)
- `BPlusTreeArena`: new project — B+ tree variant backed by an `ArenaMem` arena; node allocation draws from the pre-allocated arena instead of `malloc`/`free`, eliminating per-node heap overhead and fragmentation; interface-compatible with `BPlusTree` via compile switch
- `TieredStore`: optional arena-backed in-memory B+ tree — `TS_USE_BPTREE_ARENA` compile switch; `TSCreate` and `TSOpen` accept an optional `PArenaMem`; when provided, the in-memory B+ tree uses `BPlusTreeArena` and `ArenaMemReset` reclaims all node memory on each flush without individual frees
- `OthelloSolverCommandLine`: `--use-max-memory`, `--use-recommended-memory` (default), `--max-memory <size>` CLI arguments control the physical arena budget; budget is divided evenly across all six arenas (three board + three move) so total physical allocation equals the budget exactly; budget and per-store sizes printed in the startup and restart banners
- `OthelloSolverMFCandCUDA` / `SolverController`: `SCSetMemoryMode()` — sets memory mode and optional byte count before a run starts; `s_shardArenaBytes` is computed per-run from `CalcMemoryBudget()` divided by `NUM_SHARDS`

### Changed
- `Utility` / `ArenaMemReset`: removed bulk `memset` of used bytes on reset — was ~70 ms overhead per large level; B+ tree nodes are fully initialized before first read so the zero-fill is not needed
- `OthelloSolverCommandLine`: level progress output is now a columnar table — a header row (`Lv`, `BoardsIn`, `NewBoards`, `Dups`, `Mvs`, `Ends`, `Pred(s)`, `Tm(s)`, `ns/brd`, `Nxt(s)`, `DateTime`) is printed once before the BFS loop; each level line is plain aligned values with no `field=` labels; `Pred(s)` and `Tm(s)` are now adjacent columns; levels with no prediction yet show `---`
- `OthelloSolverCommandLine`: final report table column order updated to match — `Pred(s)` now immediately precedes `Tm(s)`
- `OthelloBasics` / `BOARD`: added explicit `_pad1[3]` (6 bytes, offsets 18–23) and `_pad2[3]` (6 bytes, offsets 58–63) fields; size comment corrected to 64 bytes; `BoardFlip`, `BoardMirrorVerticalAxis`, and `BoardRotate90DegreesRight` explicitly zero both pad fields
- `OthelloBasics` / `MOVE`: added explicit `_pad1` (4 bytes, offsets 20–23) and `_pad2[3]` (6 bytes, offsets 42–47) fields; size comment corrected to 48 bytes
- `Utility` / `RWLock`: added explicit `_pad[7]` field; zeroed in `RWLockInit`
- `BPlusTree` / `BPIterator`: added explicit `_pad[7]` field; zeroed in `BPIterateStart` and `BPIterateStartFrom`

---

## [v2.0.1] - 2026-05-15

### Changed
- `OthelloSolverCommandLine`: level progress line is now a single line per level; columns widened to 13 digits (handles 10+ digit board/move counts without skewing); removed duplicate `act=` field (actual time already shown as `tm=`); removed `brd/s` (redundant with `ns/brd`); abbreviated `moves`→`mvs`, `next`→`nxt`, `time`→`tm`; added current date/time at end of each line
- `OthelloSolverCommandLine`: final report table updated to match — removed `brd/s` column, widened numeric columns, abbreviated `Moves`→`Mvs` and `Elapsed(s)`→`Tm(s)`

---

## [v2.0.0] - 2026-05-15

### Added
- `OthelloSolverCommandLine`: new console project — full GPU-accelerated exhaustive BFS solver; processes each level (piece count) by iterating canonical boards from a TieredStore, dispatching batches to the CUDA kernel, and inserting child boards and move edges into per-level TieredStores
- `OthelloSolverCommandLine` / `SolverKernel` (CUDA): `OthelloExpandKernel` — one thread per input board; iterates legal move bits, applies each move, canonicalizes the child via `dev_canonicalize`, and writes a `GpuResult` (child board + move edge) per slot; handles pass-move (opponent can still play) and terminal-board (neither player can move) cases; `DispatchBatch`, `LaunchOthelloKernel`, and `WorkerGpuContext` manage per-worker CUDA streams, device buffers, and pinned host buffers
- `OthelloSolverCommandLine` / `SolverWorker`: `WorkerProcessBatch` — stages a batch into pinned host memory, dispatches to GPU, detects slot overflow, initializes terminal board win/loss/tie counts, and inserts all child boards and move edges into the appropriate TieredStores
- `OthelloSolverCommandLine`: back-propagation sweep — iterates levels from `maxLevel` down to 0; for each move edge in the move store, adds the child board's win/loss/tie counts into the parent board record via `TSUpdate`
- `OthelloSolverCommandLine`: progress reporting — per-level table with `boardsIn`, `newBoardsOut`, dups, moves, terminal boards (`ends`), elapsed time, ns/board, boards/sec, and predicted vs actual time; rolling-average prediction with spike detection (switches to latest ns/board when it exceeds 1.4× the rolling average, catching the RAM→disk I/O inflection point cleanly)
- `OthelloSolverCommandLine`: restart support (`doRestartProcess`) — scans existing TieredStore directories, identifies the last fully checkpointed level, reopens all stores, and resumes BFS from that level
- `OthelloSolverCommandLine`: overflow detection — if any board produces more children than `maxMovesPerBoard` allows, prints the actual maximum and exits with an actionable message
- `OthelloSolverCommandLine`: pass-move handling — boards with no legal moves for the current player but legal moves for the opponent produce a pass child (player-flipped board at the same piece count) queued for a separate post-sweep pass
- `OthelloSolverCommandLine` / `Logger`: timestamped file logger
- `OthelloBasicsForCUDA`: new project — CUDA-compatible OthelloBasics for use in `.cu` translation units
- `TieredStore` / `TSUpdate`: new `TSUpdate` function (`TieredStoreUpdate.cpp`) — finds the on-disk record matching the supplied key and overwrites it in place; used by back-propagation to accumulate win/loss/tie counts without re-inserting

### Fixed
- `TieredStore`: `BPFreeTree` was called with `false` (free nodes only, not record data) in both `TSI_FreeStore` and `TSI_FlushMemTree`; since `BPInsertCopy` makes a separate heap allocation per record, every flush leaked all record data — approximately 16 GB per flush of a 287 M-record tree during the 6×6 solve; three such flushes at level 12 exhausted VM on a 64 GB machine, causing heap corruption that manifested as `STATUS_STACK_BUFFER_OVERRUN` (0xC0000409); fix: changed both calls to `BPFreeTree(ts->memTree, true)`

---

## [v1.9.0] - 2026-05-13

### Changed
- `TieredStore`: replaced `TS_COMPARE_FN compareFn` + `keySize` parameters with structured field definitions (`TSKeyFld* keyFlds`, `numKeyFlds`, `idxSettings`) mirroring BPlusTree's approach — callers declare which record fields form the key and their types; comparison is derived automatically via `BPKeyCmpPPRaw`; `TSKeyFld` is layout-compatible with `BPIdxFld` for safe casting
- `TieredStore`: `TS_MERGE_FN mergeFn` is now nullable in `TSCreate` and `TSOpen` — passing `nullptr` means keep the existing record unchanged on duplicate key (no-op merge)
- `TieredStore`: manifest path is now always `dirs[0]\manifest.tsm` — removed the caller-supplied manifest path parameter from the public API
- `TieredStore`: `TSEnumerate` now flushes the in-memory tree before iterating disk files only — eliminates the bug where a record present in both the tree and an older disk file would be returned twice; write lock held for the duration
- `OthelloSolverMFCandCUDA` (`SolverController`): replaced `CompareUniqueRecord` callback with a `k_uniqueKeyFlds[]` constant (3 fields: `ullCellsInUse`, `ullCellColors`, `usBoardInfo`); updated `TSCreate`/`TSOpen` calls
- `OthelloSolverCommandLine`: updated `TSCreate` calls to new field-definition API; added `k_boardKeyFlds` and `k_moveKeyFlds` constants

### Added
- `TieredStore`: new sorted pull-style iterator — `TSIterOpen`, `TSIterNext`, `TSIterNextN`, `TSIterClose`; iterates all live records in ascending key order across all disk files; `TSIterOpen` flushes the in-memory tree and opens all file handles while holding the write lock (prevents concurrent merges from deleting snapshotted files on Windows where `remove()` fails on open handles); `TSIterClose` performs orphan cleanup — removes any snapshotted files that a concurrent merge removed from the store registry but could not physically delete while the iterator held them open
- `TieredStoreTester`: Group 8 — four iterator tests: `IteratorEmpty` (empty store returns Not_Found immediately), `IteratorSortedOrder` (150 records across 3 files returned in strict ascending key order), `IteratorSkipsTombstones` (3 deleted keys absent from iteration), `IteratorNextN` (100 records consumed in batches of 30: 3 full + 1 partial)

### Fixed
- `OthelloSolverCommandLine`: MOVE stores were incorrectly inserting into `g_tieredBoardStores[i]` instead of `g_tieredMoveStores[i]`

---

## [v1.8.0] - 2026-05-11

### Performance
- `BoardMirrorVerticalAxis`: replaced O(N²) cell-by-cell loop with a 9-operation bitwise implementation — reverse bits within each byte of the 64-bit word (swap nibbles → swap bit-pairs → swap adjacent bits); works for all board sizes since non-active column bits are 0; original loop preserved under `#define BITBOARD_MIRROR` fallback

### Refactored
- Replaced all active-code usages of `GETBOARDSTARTIDX`, `GETBOARDENDIDX`, and `GETBOARDSIZE` macros with the `g_boardSi`, `g_boardEi`, and `g_boardSize` globals across all projects; macros retained in the header and legacy `#else` blocks only
- Updated files: `BoardFlip.cpp`, `BoardPrint.cpp`, `OthelloEnumeratorThread.cpp`, `OthelloSolverMFCandCUDADlg.cpp`, `OthelloSolverMultithreadedDlg.cpp`, `Worker.cpp`, `Main.cpp` (OthelloSolverUsingBplusOnly)

---

## [v1.7.0] - 2026-05-11

### Refactored
- `OthelloBasics`: removed `startIdx`/`endIdx` parameters from `BoardMoveCalculator`, `MovePlayAndSetResultBoard`, and `BoardCreateUniqueBoard`; these values are now derived from the board-size globals and never recomputed per-call
- `OthelloBasics`: added six `inline` globals (`g_boardSize`, `g_boardSi`, `g_boardEi`, `g_boardLeftEdge`, `g_boardRightEdge`, `g_boardMask`) declared in `OthelloBasics.h` and pre-initialized to the 4×4 defaults so no explicit init call is required for that size
- `OthelloBasics`: added `SetBoardSizeForRun(boardSize)` — call once at startup to set all six globals; `BoardMoveCalculator` now uses the precomputed edge/mask values directly instead of rebuilding them on every call (potential billions of calls per solve)
- `OthelloBasics`: `BoardAllocateFirstBoard()` takes no parameters; callers call `SetBoardSizeForRun(size)` first, then `BoardAllocateFirstBoard()` — board size is read from `g_boardSize`
- All projects updated to the new parameter-free API: `OthelloSolverCuda`, `OthelloSolverMFCandCUDA`, `OthelloSolverMultithreaded`, `OthelloSolverUsingBplusOnly`, `OthelloEnumerator`

### Added
- `OthelloSolverMFCandCUDA`: new MFC+CUDA solver — `Solver`, `SolverController`, `SolverGpu` (CUDA kernel) modules; wave-BFS CPU phase feeds frontier boards to GPU DFS kernel; results reported on completion
- `OthelloSolverCommandLine`: new console project skeleton added to solution

### Fixed
- `OthelloSolverMFCandCUDA`: removed `.detach()` from controller thread; added `JoinSolverThread()` so the process exits cleanly when the dialog closes
- `OthelloSolverMFCandCUDA`: joining the old thread before reassigning `s_controllerThread` prevents `std::terminate()` (and downstream BSOD) on second run
- `OthelloSolverMFCandCUDA`: capped `s_openThresh` at 8 open spaces to prevent GPU DFS kernel from running exponential work (O(b^n), n≤8 is safe); without the cap a 6×6 board with small `cpuDepth` would hang `cudaDeviceSynchronize()` indefinitely
- `OthelloSolverMFCandCUDA`: `OnClose()` pumps messages while waiting for the solver to stop; 15-second `ExitProcess(0)` failsafe handles the case where the GPU is truly hung
- `OthelloSolverMFCandCUDA`: added CUDA error checking after `cudaMemcpy`, kernel launch, and `cudaDeviceSynchronize`

### Build
- Added `<LanguageStandard>stdcpp17</LanguageStandard>` to all configurations of `OthelloBasics`, `OthelloEnumerator`, `OthelloSolverMFCandCUDA`, `OthelloSolverMultithreaded`, `OthelloSolverUsingBplusOnly`, and `OthelloSolverCuda` — required for `inline` variable support

---

## [v1.6.0] - 2026-05-09

### Added
- `TieredStore`: new LSM-tree-style external storage library — in-memory B+ tree tier flushes to sorted on-disk `.tsf` files; a meta-store (itself a TieredStore) persists the file registry; supports insert, find, delete, enumerate, checkpoint/restart, and multi-directory round-robin file placement
- `TieredStoreTester`: 15-test suite covering all TieredStore operations including insert/find/delete, checkpoint+reopen, multi-directory distribution, and a full stress test (1 000 inserts, checkpoint, reopen, verify)
- `OthelloSolverMFCandCUDA`: new MFC+CUDA dialog project skeleton

### Changed
- `OthelloSolverCuda` — major `CpuEnumerator` rewrite: CPU phase is now multi-threaded (LIFO work queue, DFS order); sharded B+ trees (16 shards) deduplicate canonical boards; each board accumulates a `pathCount` (distinct game paths); terminal boards are collected for separate GPU processing; producer-consumer coordination via condition variable
- `Utility` — `BinarySearch` and `BinSearchLE` rewritten to the half-open interval (lower_bound) pattern: cleaner branch structure, eliminates the `mid == 0` underflow special case in `BinSearchLE`, ascending/descending branches made explicit
- `OthelloSolverMultithreaded` — removed precompiled headers; replaced with explicit MFC includes (`targetver.h`, `afxwin.h`, `afxdialogex.h`, `afxshellmanager.h`, `afxvisualmanagerwindows.h`, `afxdlgs.h`) in the appropriate headers and source files; deleted `pch.h`, `pch.cpp`, `framework.h`

### Build
- `OthelloSolverCuda`: added `/LTCG` to Release linker settings — eliminates the "restarting link" message and avoids the redundant link-pass restart caused by `/GL`-compiled objects

---

## [v1.5.0] - 2026-05-07

### Added
- `OthelloSolverCuda`: new CUDA hybrid solver project — CPU DFS enumerates the game tree down to a configurable open-spaces threshold, then dispatches batches of frontier boards to the GPU; each GPU thread runs an iterative DFS to exhaustively enumerate all terminal positions from that frontier
- GPU kernel uses a thread-local explicit DFS stack (`DfsFrame stack[MAX_DFS_DEPTH]`) to avoid GPU call-stack overflow
- Flip logic (`d_flipit`) is fully iterative: walks each direction counting opponent cells, flips on anchor found — no recursion
- Validated correct on 4×4 board: Black=24,632, White=30,116, Ties=5,312, Total=60,060

---

## [v1.4.0] - 2026-05-07

### Performance
- Replaced O(N²) cell-by-cell loop in `BoardRotate90DegreesRight` with a bitwise implementation: reverse row order (`_byteswap_uint64`) then transpose via the diagonal-flip algorithm (`flipDiagA1H8`) — ~15 operations vs ~256–384 for an 8×8 board
- Correct for all board sizes (4×4, 6×6, 8×8): centered sub-boards are self-contained under full-word rotation
- Original loop preserved under `#define USE_ORIGINAL_ROTATION` for reference/fallback

---

## [v1.3.0] - 2026-05-07

### Added
- `OthelloEnumerator`: new MFC GUI sub-project that exhaustively enumerates all possible Othello game positions for 4×4, 6×6, and 8×8 boards
- Start/Stop/Restart controls with live status display (every second or every N boards)
- Checkpoint/restore: saves progress to a binary file, can resume from interruption
- Writes a `Status.txt` results report on completion (black wins, white wins, ties, total boards, moves, ns/board, max moves per board)

### Fixed
- Extra closing parenthesis removed from `GETNUMWHITE` macro in `OthelloBasics.h`

---

## [v1.2.0] - 2026-05-06

### Performance
- Sharded `s_pUniqueBoards` and `s_pMoves` B+ trees into 16 independent shards each, routed by a hash of canonical key fields — reduces per-tree lock contention by ~16× under high thread counts
- Added bit-folding hash mix (four XOR-shifts) so shard distribution is uniform across all 16 shards
- Added per-thread local BTP write staging buffer (up to 64 boards): flushes once per played board instead of once per legal move, cutting BTP write-lock acquisitions by ~8–16×

### Added
- Shard distribution display: multiline edit shows per-shard unique board counts and total, updated every stat cycle

---

## [v1.1.0] - 2026-05-06

### Performance
- Removed BTP file sort (`BTPSortFile` eliminated): sort was redundant since B+ tree already deduplicates; with 1 GB files the sort would have frozen all workers for minutes
- Batch dequeue from BTP: workers now pull 256 boards per lock acquisition (`BTPGetNextRecordBatch`) instead of one, reducing read-lock contention 256×

### Fixed
- `CopyFile` → `CopyFileA` in BTP.cpp to suppress IntelliSense Unicode warning

### Refactored
- Extracted `EnsureReadFileOpen` helper in BTP.cpp so `BTPGetNextRecord` and `BTPGetNextRecordBatch` share the file-open/checkpoint logic without duplication

---

## [v1.0.0] - 2026-05-06

### Added
- ns/Board (Throughput) stat: effective nanos per board accounting for all threads in parallel
- Version string (`APP_VERSION`) displayed in dialog title bar
- 1 GB BTP file segments (was ~1 MB due to misread parameter — third arg to BTPCreate is bytes, not record count)

### Fixed
- Dialog closing on Restart: BTP checkpoint replays in-flight boards; workers now skip already-played boards instead of fataling
- BTP.h declaration corrected: parameter renamed from `maxRecordsPerFile` to `maxFileSize` to match implementation

---

## [Initial] - 2026-05-06

Initial check-in of OthelloSolverSolution. Includes:
- MFC dialog-based multithreaded Othello solver GUI
- Configurable board size (4×4, 6×6, 8×8), thread count, rotation factor, and stats interval
- B+ tree (in-memory) for unique boards and moves
- BTP (file-backed queue) for boards to process
- Stop/Restart with checkpoint support
- Results displayed in Notepad on completion
