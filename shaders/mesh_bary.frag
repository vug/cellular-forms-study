#version 460
#extension GL_EXT_fragment_shader_barycentric : require
// The membrane, with the cell every fragment belongs to (for the cell walls).
#define CF_BARYCENTRIC 1
#include "mesh_common.glsl"
