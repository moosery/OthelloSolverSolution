# Changelog

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
