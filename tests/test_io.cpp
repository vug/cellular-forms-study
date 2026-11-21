// Tests for the state file format: exact round trip, resume equals an uninterrupted run,
// snapshot quantisation error, and rejection of damaged files.
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>

#include "core/io.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"
#include "test.hpp"

using namespace cf;

namespace {

State grown(std::uint64_t seed, int steps) {
    Params p;
    p.foodMax = 0.05f;
    State s = make_icosphere(2, p);
    s.seed = seed;
    randomise_initial_food(s);
    CpuSim sim(s, 1);
    for (int k = 0; k < steps; ++k) {
        const StepReport r = sim.step();
        CF_CHECK_MSG(r.ok(), r.message);
    }
    return sim.state();
}

bool throws_with(const std::vector<char>& bytes, const char* expected, bool asSnapshot = false) {
    try {
        if (asSnapshot) decode_snapshot(bytes);
        else decode_checkpoint(bytes);
    } catch (const std::runtime_error& e) {
        if (std::strstr(e.what(), expected) != nullptr) return true;
        cftest::note(std::string("unexpected message: ") + e.what());
        return false;
    }
    return false;
}

}  // namespace

CF_TEST(checkpoint_round_trip_is_exact) {
    const State s = grown(3, 80);
    const std::vector<char> bytes = encode_checkpoint(s);
    const State back = decode_checkpoint(bytes);
    CF_CHECK_EQ(checksum(back), checksum(s));
    CF_CHECK_EQ(back.n, s.n);
    CF_CHECK_EQ(back.step, s.step);
    CF_CHECK_EQ(back.seed, s.seed);
    CF_CHECK(check_invariants(back).ok());

    // Through a real file, with the asynchronous writer.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cf_test_io";
    std::filesystem::create_directories(dir);
    {
        AsyncWriter writer;
        writer.submit(dir / "a.cfs", encode_checkpoint(s));
        writer.submit(dir / "b.cfs", encode_snapshot(s));
        CF_CHECK(writer.drain().empty());
    }
    CF_CHECK_EQ(checksum(load_checkpoint(dir / "a.cfs")), checksum(s));
    CF_CHECK_EQ(load_snapshot(dir / "b.cfs").n, s.n);
    CF_CHECK(!std::filesystem::exists(dir / "a.cfs.tmp"));
    // A write into a directory that does not exist is reported, not thrown across threads.
    {
        AsyncWriter writer;
        writer.submit(dir / "no_such_dir" / "c.cfs", encode_snapshot(s));
        CF_CHECK(!writer.drain().empty());
    }
    std::filesystem::remove_all(dir);
}

// Stop, save, load, continue: the result is bit-identical to never stopping.
CF_TEST(resume_from_checkpoint_equals_uninterrupted_run) {
    const State straight = grown(11, 120);
    State half = grown(11, 60);
    State resumed = decode_checkpoint(encode_checkpoint(half));
    CpuSim sim(resumed, 2);
    for (int k = 0; k < 60; ++k) CF_CHECK(sim.step().ok());
    CF_CHECK_EQ(checksum(sim.state()), checksum(straight));
    CF_CHECK(straight.n > 162u);
    cftest::note(std::to_string(straight.n) + " cells, checksum " + std::to_string(checksum(straight)));
}

CF_TEST(snapshot_positions_are_within_quantisation_error) {
    const State s = grown(5, 100);
    const Snapshot snap = decode_snapshot(encode_snapshot(s));
    CF_CHECK_EQ(snap.n, s.n);
    CF_CHECK_EQ(snap.step, s.step);
    CF_CHECK(!snap.fromCheckpoint);
    const Vec3 extent = snap.boundsMax - snap.boundsMin;
    const float tol = std::max(extent.x, std::max(extent.y, extent.z)) / 65535.0f;  // one quantisation step
    float worst = 0.0f;
    for (std::uint32_t v = 0; v < s.n; ++v) {
        worst = std::max(worst, length(snap.pos[v] - s.pos[v]));
        CF_CHECK_EQ(snap.birth[v], s.birth[v]);
        CF_CHECK_EQ(snap.gen[v], s.gen[v] > 255u ? 255u : s.gen[v]);
        CF_CHECK(std::fabs(snap.food[v] - std::min(1.0f, s.food[v] / s.params.foodThreshold)) <= 0.5f / 255.0f + 1e-6f);
    }
    CF_CHECK(worst <= tol);
    cftest::note("worst position error " + std::to_string(worst) + " of rest length (bound " + std::to_string(tol) + ")");
    // A checkpoint can be read as a snapshot too (exact positions).
    const Snapshot full = decode_snapshot(encode_checkpoint(s));
    CF_CHECK(full.fromCheckpoint);
    for (std::uint32_t v = 0; v < s.n; ++v) CF_CHECK(length(full.pos[v] - s.pos[v]) == 0.0f);
    // A snapshot cannot be used to resume.
    CF_CHECK(throws_with(encode_snapshot(s), "snapshot"));
}

CF_TEST(damaged_files_are_rejected_with_a_reason) {
    const State s = grown(7, 40);
    const std::vector<char> good = encode_checkpoint(s);

    CF_CHECK(throws_with(std::vector<char>(10, 'x'), "too short"));
    {
        std::vector<char> b = good;
        b[0] = 'X';
        CF_CHECK(throws_with(b, "bad magic"));
    }
    {
        std::vector<char> b = good;
        b[8] = 9;  // version
        CF_CHECK(throws_with(b, "version"));
    }
    {
        std::vector<char> b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2));
        CF_CHECK(throws_with(b, "truncated"));
    }
    {
        std::vector<char> b = good;
        b[good.size() / 2] = static_cast<char>(b[good.size() / 2] ^ 0x40);  // one flipped bit in the payload
        CF_CHECK(throws_with(b, "checksum mismatch"));
    }
    {
        std::vector<char> b(good.begin(), good.end() - 24);  // end chunk cut off
        CF_CHECK(throws_with(b, "truncated"));
    }
    CF_CHECK(throws_with(std::vector<char>(), "too short", true));
}
