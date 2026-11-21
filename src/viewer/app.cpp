#include "app.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "capture.hpp"
#include "cloud.hpp"
#include "frame_source.hpp"

namespace cfv {

// ================================================================================ headless

int runHeadless(const ViewOptions& o) {
    ContextDesc desc;
    desc.validation = o.validate;
    desc.wantSwapchain = false;
    Context ctx(desc);
    ctx.createDevice(VK_NULL_HANDLE);

    FrameSource source(o);
    CloudGpu gpu(ctx);

    std::vector<int> indices;
    auto resolve = [&](int f) { return f < 0 ? std::max(source.count() + f, 0) : std::min(f, source.count() - 1); };
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
        const int extraKind = extraKindFor(o.settings.colorMode);
        gpu.upload(cloud, extraKind);
        if (extraKind != 0 && gpu.buffers().extraKind != extraKind)
            std::fprintf(stderr, "note: this file has no values for the chosen colour mode (curvature needs a checkpoint, the activator a state with morphogens)%s", "\n");
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
