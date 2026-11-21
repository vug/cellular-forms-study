// Export for offline rendering (M7; docs/design.md "Blender export"): the cells as a PLY point
// cloud with per-point attributes, and the membrane as a PLY triangle mesh built from the rings.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "io.hpp"
#include "state.hpp"

namespace cf {

// The triangles of the closed mesh the rings describe, each exactly once, counter-clockwise
// seen from outside: (v, ring[k], ring[k+1]) is emitted by its smallest corner.
// A valid state with V cells has 2 V - 4 triangles.
std::vector<std::array<std::uint32_t, 3>> mesh_faces(const State& s);

// Cells as points. Binary little-endian PLY, one vertex per cell with float properties
//   x y z, radius, age (steps since birth), food (0..1, relative to the threshold), generation,
// and nx ny nz when normals are available (checkpoints). `radius` is the sphere radius to
// instance in world units. Throws std::runtime_error when the file cannot be written.
void write_ply_points(const std::filesystem::path& file, const Snapshot& s, float radius);
void write_ply_points(const std::filesystem::path& file, const State& s, float radius);

// The membrane: the mesh of mesh_faces(), vertices with x y z nx ny nz age food generation.
// Needs the rings, so a checkpoint, not a snapshot.
void write_ply_mesh(const std::filesystem::path& file, const State& s);

}  // namespace cf
