// Internal to the viewer: where frames come from in snapshot mode (run files or a demo), and
// the default camera for a cloud.
#pragma once
#include <algorithm>
#include <cstdio>
#include <optional>
#include <vector>

#include "app.hpp"
#include "cloud.hpp"
#include "core/sim_cpu.hpp"
#include "core/topology.hpp"

namespace cfv {

inline constexpr float kDegToRad = 3.14159265358979f / 180.0f;

inline Cloud makeDemoCloud(const ViewOptions& o) {
    cf::Params params;
    cf::State s = cf::make_icosphere(std::max(o.demoLevel, 0), params);
    s.params = params;
    if (o.demoSteps > 0) {
        cf::randomise_initial_food(s);
        cf::CpuSim sim(std::move(s));
        for (int i = 0; i < o.demoSteps; ++i) {
            const cf::StepReport r = sim.step();
            if (!r.ok()) {
                std::fprintf(stderr, "demo simulation stopped: %s\n", r.message.c_str());
                break;
            }
        }
        return cloudFromState(sim.state(), "demo");
    }
    return cloudFromState(s, "demo");
}

// The frames of a run: files loaded on demand (with a small cache), or one in-memory cloud.
class FrameSource {
public:
    explicit FrameSource(const ViewOptions& o) : files_(o.files) {
        if (files_.empty()) demo_ = makeDemoCloud(o);
        cache_.resize(files_.size());
    }
    int count() const { return files_.empty() ? 1 : static_cast<int>(files_.size()); }
    const Cloud& get(int index) {
        if (files_.empty()) return *demo_;
        index = std::clamp(index, 0, count() - 1);
        if (!cache_[static_cast<size_t>(index)]) {
            // Keep the cache below ~2 GB of cell data.
            size_t bytes = 0;
            for (const auto& c : cache_)
                if (c) bytes += static_cast<size_t>(c->count()) * 20;
            if (bytes > (size_t{2} << 30))
                for (auto& c : cache_) c.reset();
            cache_[static_cast<size_t>(index)] = loadCloud(files_[static_cast<size_t>(index)]);
        }
        return *cache_[static_cast<size_t>(index)];
    }

private:
    std::vector<std::filesystem::path> files_;
    std::vector<std::optional<Cloud>> cache_;
    std::optional<Cloud> demo_;
};

inline OrbitCamera cameraFor(const Cloud& cloud, const ViewOptions& o) {
    OrbitCamera cam;
    cam.yaw = o.yawDeg * kDegToRad;
    cam.pitch = o.pitchDeg * kDegToRad;
    cam.frame(cloud.centre, cloud.radius + o.settings.sphereRadius);
    cam.zoom(o.zoom);
    return cam;
}

}  // namespace cfv
