# Changelog

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
