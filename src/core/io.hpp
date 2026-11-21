// State files: one versioned, chunked binary container for checkpoints (everything needed to
// resume) and snapshots (quantised positions and a few scalars for viewing).
//
// Layout (little-endian):
//   header, 48 bytes
//      0  char[8]  magic "CFSTATE\0"
//      8  u32      version (1)
//     12  u32      endian tag 0x01020304
//     16  u32      kind: 1 = checkpoint, 2 = snapshot
//     20  u32      neighbour slots per cell (kMaxDeg)
//     24  u64      cell count n
//     32  u64      step
//     40  u64      seed
//   chunks, each:  char[4] tag | u32 element size | u64 payload bytes | payload
//     "PARM"  text, "key=value" lines (cf::to_text(Params))                  both kinds
//     "POS "  f32 x 3 x n   positions                                         checkpoint
//     "NRM "  f32 x 3 x n   normals                                           checkpoint
//     "FOOD"  f32 x n                                                         checkpoint
//     "BRTH"  u32 x n       step at which each cell was created               both kinds
//     "GEN "  u32 x n       divisions in the cell's lineage                   checkpoint
//     "DEG "  u8  x n       used neighbour slots                              checkpoint
//     "NBR "  u32 x slots x n   neighbour rings, counter-clockwise            checkpoint
//     "MPHA", "MPHB"  f32 x n each: morphogen concentrations, only when       checkpoint
//             the state has them
//     "POSQ"  f32 min[3], f32 max[3], then u16 x 3 x n: positions quantised   snapshot
//             to 16 bits per axis inside the bounding box
//     "FODQ"  u8 x n        food / foodThreshold, clamped, scaled to 0..255   snapshot
//     "GENB"  u8 x n        generation, clamped to 255                        snapshot
//     "END "  u64           FNV-1a 64 of every byte before this chunk
//   Unknown chunks are skipped, so later versions can add data.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "state.hpp"

namespace cf {

// ---- encoding (in memory, so the caller can hand the bytes to a writer thread)
std::vector<char> encode_checkpoint(const State& s);
std::vector<char> encode_snapshot(const State& s);

// Writes atomically: to "<path>.tmp", then renames. Throws std::runtime_error on failure.
void write_file(const std::filesystem::path& path, const std::vector<char>& bytes);

// ---- decoding. Both throw std::runtime_error with the reason (bad magic, version, truncated
// file, checksum mismatch, wrong sizes, ...). Neither runs the invariant checker.
State decode_checkpoint(const std::vector<char>& bytes);
State load_checkpoint(const std::filesystem::path& path);

// What a viewer needs. Loads either kind of file (a checkpoint is also a snapshot).
struct Snapshot {
    std::uint32_t n = 0;
    std::uint64_t step = 0;
    std::uint64_t seed = 0;
    Params params;
    Vec3 boundsMin{}, boundsMax{};
    std::vector<Vec3> pos;             // n
    std::vector<float> food;           // n, in units of foodThreshold for snapshot files (0..1), raw for checkpoints
    std::vector<std::uint32_t> birth;  // n
    std::vector<std::uint32_t> gen;    // n
    bool fromCheckpoint = false;
};
Snapshot decode_snapshot(const std::vector<char>& bytes);
Snapshot load_snapshot(const std::filesystem::path& path);

std::vector<char> read_file(const std::filesystem::path& path);

// ---- asynchronous writer: the simulation never waits on the disk (docs/design.md,
// "Snapshot cadence"). Files are written in submission order on one worker thread.
class AsyncWriter {
public:
    AsyncWriter();
    ~AsyncWriter();  // drains the queue
    AsyncWriter(const AsyncWriter&) = delete;
    AsyncWriter& operator=(const AsyncWriter&) = delete;

    // Queues a file. Blocks only when more than `maxPending` files are already waiting.
    void submit(std::filesystem::path path, std::vector<char> bytes);
    // Waits until everything submitted so far is on disk. Returns "" or the first error.
    std::string drain();

private:
    void run();
    struct Job {
        std::filesystem::path path;
        std::vector<char> bytes;
    };
    static constexpr std::size_t maxPending = 4;
    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<Job> queue_;
    bool busy_ = false, stop_ = false;
    std::string error_;
    std::thread worker_;
};

}  // namespace cf
