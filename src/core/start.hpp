// The start state of a run, from the same description every tool takes: a shape string, a seed,
// the parameters and the initial food (cf_run --new, cf_gpu --new, the viewer's live mode and
// its gallery). One definition, so a form described the same way starts the same everywhere.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "params.hpp"
#include "state.hpp"

namespace cf {

enum class StartFood {
    Random,  // uniform in [0, foodThreshold), from the seed (randomise_initial_food)
    Zero,    // every cell starts with no food
};

// "random" or "zero"; false for anything else.
bool parse_start_food(std::string_view text, StartFood& food);
const char* start_food_name(StartFood food);

// The shapes, as cf_run --help lists them:
//   icosphere:<level>                         subdivided icosahedron, level 0..9
//   ellipsoid:<level>:<ax>:<ay>:<az>          the same mesh stretched to an ellipsoid with these axis ratios
//   noisy:<level>:<amplitude>:<frequency>     sphere with radius 1 + amplitude * smooth noise (from the seed)
// The state carries `params` and `seed`, is at step 0 and passes the invariant checker. Throws
// std::invalid_argument for a shape string that is not one of these (or has bad numbers) and
// std::runtime_error when the shape's own checks refuse the values (a level out of range, an
// axis <= 0, an amplitude >= 1, ...). The parameters are not validated here (cf::validate).
State make_start_state(std::string_view shape, std::uint64_t seed, const Params& params, StartFood food = StartFood::Random);

// The text of the shape strings above, for messages.
extern const char* const kStartShapeHelp;

}  // namespace cf
