===============================================================================
OthelloSolverCommandLine
===============================================================================

Overview
--------
This project is a high-performance, command-line based Othello solver designed
to exhaustively map the game tree for 4x4, 6x6, and 8x8 boards. 

Unlike earlier pure-DFS CPU solvers, this architecture is built around a 
Wavefront Breadth-First Search (BFS) and heavily utilizes a Producer-Consumer 
multi-threading model designed to seamlessly integrate with GPU acceleration 
(CUDA).

Core Concepts
-------------
1. Wavefront Breadth-First Search (BFS)
   Instead of recursively diving down to terminal boards one path at a time 
   (DFS), this solver completely generates all distinct board states for Level N
   before moving to Level N+1. This eliminates deep call stacks and creates 
   massive, flat arrays of work—the ideal workload for a GPU.

2. Directed Acyclic Graph (DAG) Deduplication
   The game tree is simplified into a DAG using TieredStores:
   - NODES (TieredBoardStore): Unique, rotationally-canonical board states.
   - EDGES (TieredMoveStore): The plays connecting a parent to a child board.
   Identical game paths merge into the same canonical boards. Later, during 
   back-propagation, the solver simply calculates the totals for a node and 
   adds them across the outward move edges, intrinsically multiplying identical
   symmetrical paths without explicitly tracking them.

3. Byte-level B+ Tree Keys
   To maximize performance, the TieredStores use raw memory comparisons 
   (TS_DATATYPE_BYTE) over the first 24 bytes of the BOARD and MOVE structs. 
   Because structs contain invisible compiler padding, ALL structs must be 
   fully zeroed out (`memset(&board, 0, sizeof(BOARD))`) before population to 
   ensure padding garbage doesn't corrupt the deduplication engine.

Architecture & Workflow
-----------------------
The forward-play phase is orchestrated by a multi-stream worker pool:

1. The Producer (Main Thread)
   - Opens a sorted TieredStore iterator (`TSIterOpen`) on Level N.
   - Pulls boards in bulk via `TSIterNextN`, filling large CPU-side batches
     (e.g., 100,000 boards) as fast as possible.
   - Pushes the batches into a thread-safe Work Queue.

2. The Consumers (CPU Worker Pool)
   - A pool of CPU worker threads waits on the Work Queue.
   - When a batch arrives, a worker pops it from the queue.

3. GPU Compute (Multi-Stream Execution)
   - Every CPU worker thread owns its own dedicated CUDA Stream.
   - The worker issues an asynchronous memory copy (H2D) of the batch to the GPU.
   - The GPU kernel executes, finding all legal moves for every board in the
     batch, applying each move to produce a child board, and immediately
     canonicalizing it (rotations + mirror) via the BoardCreateUniqueBoard logic.
     Every value written to the output array is already in canonical form.
   - The worker copies the results back to the CPU (D2H).
   - Using multiple streams allows the GPU hardware scheduler to overlap PCIe
     transfers with compute execution continuously.

4. CPU Insertion
   - The worker iterates through the GPU's already-canonical results.
   - It inserts each child board directly into the Level N+1 Board Store.
   - It inserts the directional move edge into the Level N Move Store.
   - No CPU canonicalization step is needed — the invariant is that every board
     in every Board Store is always in canonical form.

5. Level Synchronization
   - A condition variable (`g_activeTasks`) tracks in-flight batches. 
   - The main thread waits for all workers to finish the current level before
     calling `TSCheckpoint` to flush the stores to disk safely.
   - The main thread then increments to Level N+1 and repeats the cycle until
     no legal moves remain.

Back-Propagation Phase
----------------------
Once all levels (0 to 60) are completely generated, the solver iterates backwards
from the terminal boards to the root, summing exact win/loss/tie totals.