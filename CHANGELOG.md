# Changelog

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
