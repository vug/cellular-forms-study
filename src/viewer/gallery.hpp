// The gallery: forms that were grown before (the atlas, or the web app's small gallery), shown
// as a grid of their pictures in Dear ImGui, to choose one and grow it again with its start
// shape, seed and parameters.
//
// The panel is plain ImGui and Vulkan: no window system, no renderer of the viewer. The desktop
// viewer (cf_view) shows it in a window of its own; the VR app can draw it into whatever ImGui
// window or panel it has, at the size it gives.
//
// The pictures are the contact sheets the atlas already has (gallery.json says which tile of
// which sheet is a form's). Sheets are decoded on a worker thread, only when a form becomes
// visible, and the tiles are copied into two fixed-size textures (one for the grid, one for
// the larger picture shown on hover), so the memory taken does not grow with the gallery:
// about 25 MB for the atlas, 12 MB for the web gallery.
//
// Setup the panel expects from its caller:
//   - an ImGui context and the ImGui Vulkan backend (ImGui_ImplVulkan_Init) are set up; the
//     panel registers its two textures with ImGui_ImplVulkan_AddTexture, so the backend's
//     descriptor pool needs room for two more sampled images than ImGui itself uses;
//   - `queue` is the queue the ImGui draw data is submitted on (the panel's uploads are ordered
//     against the frames on that queue with pipeline barriers), and every call below is made on
//     the thread that submits to it;
//   - draw() (or drawTile()) is called between ImGui::NewFrame() and ImGui::Render(), at most
//     once per frame for draw();
//   - the panel is destroyed before ImGui_ImplVulkan_Shutdown(); its destructor waits for the
//     queue to be idle and frees its textures.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/params.hpp"
#include "core/start.hpp"
#include "imgui.h"

namespace cfv {

// Which tile of which contact sheet: sheet index into GallerySheets::files, column and row.
struct GalleryTileRef {
    int sheet = -1, col = 0, row = 0;
    bool valid() const { return sheet >= 0; }
};

struct GalleryItem {
    int id = 0;                       // the run's id in the atlas
    std::string origin;               // what asked for the run: survey, novelty, emitter, ... (may be empty)
    std::string status;               // ok, stopped-early, stalled (may be empty)
    int kind = -1;                    // the kind it was sorted into, -1 for none
    std::uint64_t seed = 1;
    std::string init = "icosphere:2"; // start shape (cf::make_start_state)
    std::string food = "random";      // initial food: random or zero
    std::vector<std::string> sources; // the optional mechanisms that are on: light, pulse, crowd, field, ...
    std::vector<std::pair<std::string, double>> params;  // the parameters given (the rest are the defaults)
    std::uint64_t target = 0;         // the cell count it was grown to
    std::uint64_t cells = 0, step = 0;  // where it stopped and was pictured
    std::uint64_t stoppedAt = 0;      // > 0: an early stop ended it at this cell count
    std::vector<std::pair<std::string, double>> metrics;
    int rating = 0;                   // 1..5, 0 when not rated
    std::string category;
    GalleryTileRef tile, thumb;       // its picture in the tile sheets and in the thumbnail sheets
    const double* metric(std::string_view name) const;
};

struct GallerySheets {
    int tileW = 0, tileH = 0;               // pixels of one tile
    std::vector<std::filesystem::path> files;  // absolute paths
};

struct GalleryData {
    std::filesystem::path file;
    std::string source;                     // "atlas" for the atlas's gallery, empty for the web app's
    GallerySheets tiles, thumbs;            // thumbs.files is empty when the file has none
    std::vector<std::pair<int, std::string>> kinds;            // id, name
    std::vector<std::pair<std::string, std::string>> metrics;  // name, what it measures
    std::vector<GalleryItem> items;
    const GalleryItem* find(int id) const;
    std::string kindName(int kind) const;   // empty for none
};

// Reads a gallery.json (the schema of scripts/atlas/webgallery.py, with the additions of
// nativegallery.py). Sheet paths are resolved against the file's folder. Throws
// std::runtime_error naming the file and the problem.
GalleryData loadGalleryFile(const std::filesystem::path& file);

// runs/atlas/gallery/gallery.json when it exists, else web/app/gallery/gallery.json, in the
// repository found upwards from the executable's folder (then from the working directory).
// Empty when there is neither.
std::filesystem::path defaultGalleryFile();

// What growing a form again takes.
struct GalleryStart {
    int id = 0;
    std::string shape;
    cf::StartFood food = cf::StartFood::Random;
    std::uint64_t seed = 1;
    cf::Params params;               // the defaults with the item's parameters on top
    std::uint64_t pauseAtStep = 0;   // the step it was pictured at
    std::uint64_t cells = 0;         // the cell count it had there
};

// The start of a form: its parameters are applied as cf_gpu received them (key=value text with
// nine significant digits), so the form grows as it did. Empty, with `error` set, when a
// parameter is unknown, the parameters are invalid or the food is neither random nor zero.
std::optional<GalleryStart> galleryStart(const GalleryItem& item, std::string* error = nullptr);

struct GalleryVulkan {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // the queue ImGui's draw data is submitted on
    std::uint32_t queueFamily = 0;
};

struct GalleryOptions {
    std::filesystem::path file;            // empty: defaultGalleryFile()
    float thumbWidth = 128.0f;             // starting width of a picture in the grid, pixels (the panel has a slider)
    std::uint32_t gridSlots = 512;         // pictures the grid's texture holds (at most this many visible at once)
    std::uint32_t previewSlots = 8;        // larger pictures (hover, drawTile)
    std::uint32_t uploadsPerFrame = 32;    // pictures copied to the GPU per frame, at most
};

class GalleryPanel {
public:
    // Loads options.file (or the default); a file that cannot be loaded leaves the panel empty
    // with the reason in status(). Throws only when the Vulkan objects cannot be created.
    GalleryPanel(const GalleryVulkan& vk, const GalleryOptions& options = {});
    ~GalleryPanel();
    GalleryPanel(const GalleryPanel&) = delete;
    GalleryPanel& operator=(const GalleryPanel&) = delete;

    // Replaces the gallery by another file. False (and status() says why) when it cannot be
    // loaded; the gallery there was is kept. Waits for the queue to be idle.
    bool load(const std::filesystem::path& file);

    // Draws the panel (filters, sort, the grid) into the current ImGui window, in a region of
    // `size` (0 or negative: the space that is left, as ImGui::BeginChild takes it). Returns the
    // form clicked this frame, or nullptr. The pointer stays valid until the next load().
    const GalleryItem* draw(ImVec2 size = ImVec2(0.0f, 0.0f));

    // A form's picture at `size` (a placeholder while it is being loaded), as an ImGui item.
    void drawTile(int id, ImVec2 size);

    void setCurrent(int id);          // the form drawn with a frame (0: none); a click sets it too
    int current() const;
    bool loaded() const;              // a gallery is loaded
    const GalleryData& data() const;  // empty when none is
    const std::string& status() const;  // why loading failed, or pictures that could not be read
    std::uint64_t textureBytes() const;  // GPU memory of the two textures

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cfv
