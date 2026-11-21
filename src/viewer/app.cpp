#include "app.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "capture.hpp"
#include "cloud.hpp"
#include "frame_source.hpp"

namespace cfv {

// ================================================================================ headless

namespace {

// A check of the blending of frames over time on a form that moves (--motion-test <frames>).
// The form turns and swells a little from frame to frame, which moves every cell by a different
// amount, a few pixels a frame, as a fast live run does. The last frame of the sequence is
// compared with a still image of the form as it stands then (64 frames averaged). The sequence
// is drawn four ways:
//   follow   the renderer is told where each cell was in the frame before (what a live run
//            publishes with "follow the cells' own motion")
//   still    it is told the cells did not move: the same long blend, looked up in the wrong
//            place. The control: this is what following must beat.
//   short    it is told nothing, and blends frames in which the cells changed over two frames
//   none     no blending over time
// Returns 0 when following the cells is closer to the still image than the control and than
// the two-frame blend, which is what the cells' motion is published for. (No blending at all is
// printed for comparison: a single frame is sharp but jagged and noisy, and on a form with
// little noise it can be the closest of all.)
int runMotionTest(Context& ctx, CloudGpu& gpu, const Cloud& base, const ViewOptions& o) {
    const int frames = std::clamp(o.motionTest, 2, 400);
    const float kTurn = 0.5f * 3.14159265f / 180.0f;  // per frame, about the vertical axis through the centre
    const float kSwell = 0.004f;                      // per frame
    auto at = [&](int i) {
        Cloud c = base;
        const float angle = kTurn * static_cast<float>(i), scale = std::pow(1.0f + kSwell, static_cast<float>(i));
        const float cs = std::cos(angle), sn = std::sin(angle);
        auto turn = [&](cf::Vec3 v) { return cf::Vec3{cs * v.x + sn * v.z, v.y, -sn * v.x + cs * v.z}; };
        for (cf::Vec3& p : c.pos) p = base.centre + scale * turn(p - base.centre);
        for (cf::Vec3& p : c.smooth) p = base.centre + scale * turn(p - base.centre);
        for (cf::Vec3& n : c.normal) n = turn(n);
        return c;
    };
    RenderSettings settings = o.settings;
    applyAutoRange(settings, base);
    const OrbitCamera cam = cameraFor(base, o);
    const float radius = base.radius * std::pow(1.0f + kSwell, static_cast<float>(frames));
    const float step = static_cast<float>(base.step);

    RenderSettings still = settings;
    still.aaAccumulate = true;
    still.aaFrames = 64;
    gpu.upload(at(frames - 1));
    const std::vector<std::uint8_t> reference = renderOffscreen(ctx, gpu.buffers(), cam, still, base.centre, radius, step, o.width, o.height, o.ssaa);

    struct Way {
        const char* name;
        const char* what;
        bool blend, tell, truth;
        double rms = 0.0, off = 0.0;
    } ways[4] = {{"follow", "told where each cell was", true, true, true},
                 {"still", "told the cells stood still (control)", true, true, false},
                 {"short", "told nothing: two-frame blend", true, false, false},
                 {"none", "no blending over time", false, false, false}};
    auto save = [&](const char* name, const std::vector<std::uint8_t>& pixels) {
        if (o.screenshot.empty()) return;
        std::filesystem::path out = o.screenshot;
        out.replace_filename(o.screenshot.stem().string() + "_" + name + o.screenshot.extension().string());
        if (writePng(out, o.width, o.height, pixels)) std::printf("wrote %s%s", out.string().c_str(), "\n");
    };
    save("reference", reference);
    std::printf("motion test: %d frames of %u cells, turning %.2f degrees and swelling %.1f %% a frame; difference from a still image of the last frame%s", frames,
                base.count(), static_cast<double>(kTurn) * 180.0 / 3.14159265, static_cast<double>(kSwell) * 100.0, "\n");
    for (Way& w : ways) {
        RenderSettings s = settings;
        s.taa = w.blend;
        s.cellMotion = w.tell;
        s.aaAccumulate = false;
        Cloud before;
        const std::vector<std::uint8_t> pixels = renderSequence(
            ctx, frames,
            [&](int i) {
                Cloud c = at(i);
                if (w.tell) c.prev = w.truth && i > 0 ? before.pos : c.pos;
                gpu.upload(c);
                before = std::move(c);
                return gpu.buffers();
            },
            cam, s, base.centre, radius, step, o.width, o.height, o.ssaa);
        double squares = 0.0;
        std::size_t off = 0;
        const std::size_t count = static_cast<std::size_t>(o.width) * o.height;
        for (std::size_t p = 0; p < count; ++p) {
            int most = 0;
            for (int ch = 0; ch < 3; ++ch) {
                const int d = static_cast<int>(pixels[4 * p + ch]) - static_cast<int>(reference[4 * p + ch]);
                squares += static_cast<double>(d * d);
                most = std::max(most, std::abs(d));
            }
            off += most > 16 ? 1u : 0u;
        }
        w.rms = std::sqrt(squares / static_cast<double>(3 * count));
        w.off = 100.0 * static_cast<double>(off) / static_cast<double>(count);
        std::printf("  %-7s %-38s rms %6.3f of 255, %6.3f %% of the pixels off by more than 16%s", w.name, w.what, w.rms, w.off, "\n");
        save(w.name, pixels);
    }
    const bool ok = ways[0].rms < ways[1].rms && ways[0].rms < ways[2].rms;
    std::printf("motion test: %s%s",
                ok ? "following the cells is closer to the still image than the control and than the two-frame blend"
                   : "FAILED: following the cells is not closer to the still image than the control and the two-frame blend",
                "\n");
    if (o.validate) {
        std::printf("validation: %u errors, %u warnings\n", ctx.validationErrorCount(), ctx.validationWarningCount());
        if (ctx.validationErrorCount() > 0) return 3;
    }
    return ok ? 0 : 4;
}

}  // namespace

int runHeadless(const ViewOptions& o) {
    ContextDesc desc;
    desc.validation = o.validate;
    desc.allowRayTracing = !o.noRayTracing;
    desc.allowBarycentric = !o.noBarycentric;
    desc.wantSwapchain = false;
    Context ctx(desc);
    ctx.createDevice(VK_NULL_HANDLE);

    FrameSource source(o);
    CloudGpu gpu(ctx);

    std::vector<int> indices;
    auto resolve = [&](int f) { return f < 0 ? std::max(source.count() + f, 0) : std::min(f, source.count() - 1); };
    if (o.motionTest > 0) return runMotionTest(ctx, gpu, source.get(resolve(o.frame)), o);
    if (o.allFrames) {
        for (int i = 0; i < source.count(); ++i) indices.push_back(i);
    } else if (!o.frames.empty()) {
        for (int f : o.frames) indices.push_back(resolve(f));
    } else {
        indices.push_back(resolve(o.frame));
    }

    const int n = static_cast<int>(indices.size());
    int columns = o.sheetColumns > 0 ? o.sheetColumns : 1;
    if (o.sheetColumns <= 0)
        while (columns * columns < n) ++columns;
    const int rows = (n + columns - 1) / columns;
    std::vector<std::uint8_t> sheet;
    if (o.sheet) sheet.assign(static_cast<size_t>(columns) * o.width * rows * o.height * 4, 0);

    for (int k = 0; k < n; ++k) {
        const int index = indices[static_cast<size_t>(k)];
        const Cloud& cloud = source.get(index);
        gpu.upload(cloud);
        if ((extraNeeds(o.settings) & ~gpu.buffers().extraMask) != 0)
            std::fprintf(stderr, "note: this file has no values for a cell property the look uses (curvature needs a checkpoint, the activator a state with morphogens)%s", "\n");
        RenderSettings settings = o.settings;
        applyAutoRange(settings, cloud);
        const OrbitCamera cam = cameraFor(cloud, o);
        const std::vector<std::uint8_t> pixels =
            renderOffscreen(ctx, gpu.buffers(), cam, settings, cloud.centre, cloud.radius, static_cast<float>(cloud.step), o.width,
                            o.height, o.ssaa);
        std::printf("frame %d: %u cells, step %llu%s", index, cloud.count(), static_cast<unsigned long long>(cloud.step), "\n");

        if (o.sheet) {
            const int cx = k % columns, cy = k / columns;
            for (int y = 0; y < o.height; ++y)
                std::memcpy(sheet.data() + ((static_cast<size_t>(cy) * o.height + y) * columns * o.width + static_cast<size_t>(cx) * o.width) * 4,
                            pixels.data() + static_cast<size_t>(y) * o.width * 4, static_cast<size_t>(o.width) * 4);
            continue;
        }
        std::filesystem::path out = o.screenshot;
        if (n > 1) {
            char suffix[32];
            std::snprintf(suffix, sizeof suffix, "_%04d", index);
            out.replace_filename(o.screenshot.stem().string() + suffix + o.screenshot.extension().string());
        }
        if (!writePng(out, o.width, o.height, pixels)) {
            std::fprintf(stderr, "could not write %s%s", out.string().c_str(), "\n");
            return 2;
        }
        std::printf("wrote %s  (%s)%s", out.string().c_str(), ctx.gpuName.c_str(), "\n");
    }
    if (o.sheet) {
        if (!writePng(o.screenshot, columns * o.width, rows * o.height, sheet)) {
            std::fprintf(stderr, "could not write %s%s", o.screenshot.string().c_str(), "\n");
            return 2;
        }
        std::printf("wrote %s  (%d x %d tiles, %s)%s", o.screenshot.string().c_str(), columns, rows, ctx.gpuName.c_str(), "\n");
    }
    if (o.validate) {
        std::printf("validation: %u errors, %u warnings\n", ctx.validationErrorCount(), ctx.validationWarningCount());
        if (ctx.validationErrorCount() > 0) return 3;
    }
    return 0;
}

}  // namespace cfv
