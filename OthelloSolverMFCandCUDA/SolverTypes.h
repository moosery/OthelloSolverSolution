#pragma once
#include <atomic>
#include <mutex>
#include <condition_variable>

// ─────────────────────────────────────────────────────────────────────────────
// GPU queue dimensions
// ─────────────────────────────────────────────────────────────────────────────
#define SC_GPU_BATCH_SIZE   65536
#define SC_GPU_QUEUE_DEPTH  4

// ─────────────────────────────────────────────────────────────────────────────
// Board fed to the CUDA kernel.  Layout matches CpuEnumerator.h FrontierBoard.
// ─────────────────────────────────────────────────────────────────────────────
struct FrontierBoard
{
    unsigned long long ullCellsInUse;
    unsigned long long ullCellColors;
    unsigned short     usBoardInfo;
    unsigned long long pathCount;   // compiler pads to offset 24
};

// Ring-buffer slot for the CPU→GPU batch queue.
struct GpuBatchSlot
{
    FrontierBoard boards[SC_GPU_BATCH_SIZE];
    int           count;
};

// Results returned by GpuDispatchThreadFunc when it exits.
struct GpuDispatchResults
{
    unsigned long long blackWins;
    unsigned long long whiteWins;
    unsigned long long ties;
    unsigned long long totalFrontier;
    unsigned long long batchesDispatched;
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-board state stored in TieredStore.
// ─────────────────────────────────────────────────────────────────────────────
enum BoardState : unsigned short
{
    BS_DISCOVERED  = 0,   // inserted, not yet CPU-processed
    BS_EXPANDED    = 1,   // CPU expanded (non-terminal)
    BS_TERMINAL_B  = 2,   // terminal: black wins
    BS_TERMINAL_W  = 3,   // terminal: white wins
    BS_TERMINAL_T  = 4,   // terminal: tie
    BS_FRONTIER    = 5,   // at CPU depth threshold; pushed to GPU
};

// TieredStore record.  keySize = 24 (matches offsetof(BOARD, ullPossibleMoves)).
// recordSize = 40.  All bytes initialised to 0 before use.
#pragma pack(push, 1)
struct UniqueRecord
{
    unsigned long long ullCellsInUse;  // [0 -  7]  key
    unsigned long long ullCellColors;  // [8 - 15]  key
    unsigned short     usBoardInfo;    // [16- 17]  key
    unsigned char      _keyPad[6];     // [18- 23]  key (always 0)
    unsigned short     state;          // [24- 25]  BoardState
    unsigned short     _statePad;      // [26- 27]
    unsigned int       _pad;           // [28- 31]
    unsigned long long pathCount;      // [32- 39]
};                                     // 40 bytes total
#pragma pack(pop)

// ─────────────────────────────────────────────────────────────────────────────
// Shared globals (defined in SolverController.cpp)
// ─────────────────────────────────────────────────────────────────────────────
extern GpuBatchSlot      g_gpuSlots[SC_GPU_QUEUE_DEPTH];
extern std::atomic<bool> g_stop;

// ─────────────────────────────────────────────────────────────────────────────
// GPU dispatch thread entry point (implemented in SolverGpu.cu).
// ─────────────────────────────────────────────────────────────────────────────
void GpuDispatchThreadFunc(
    int*                             pHead,
    int*                             pTail,
    int*                             pCount,
    std::mutex*                      pMtx,
    std::condition_variable*         pCvNotEmpty,
    std::condition_variable*         pCvNotFull,
    std::atomic<bool>*               pCpuDone,
    std::atomic<unsigned long long>* pGpuDispatched,
    std::atomic<unsigned long long>* pGpuQueueDepth,
    GpuDispatchResults*              pResults);
