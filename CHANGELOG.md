# Changelog

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
