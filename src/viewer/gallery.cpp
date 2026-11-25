#include "gallery.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "imgui_impl_vulkan.h"
#include "stb_image.h"  // (the implementation is in environment.cpp)

namespace cfv {

// =============================================================================== the file

const double* GalleryItem::metric(std::string_view name) const {
    for (const auto& [k, v] : metrics)
        if (k == name) return &v;
    return nullptr;
}

const GalleryItem* GalleryData::find(int id) const {
    for (const GalleryItem& i : items)
        if (i.id == id) return &i;
    return nullptr;
}

std::string GalleryData::kindName(int kind) const {
    for (const auto& [id, name] : kinds)
        if (id == kind) return name;
    return kind >= 0 ? "kind " + std::to_string(kind) : std::string();
}

namespace {

using nlohmann::json;

std::string text(const json& j, const char* key, const std::string& fallback = std::string()) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : fallback;
}

double number(const json& j, const char* key, double fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

std::uint64_t count(const json& j, const char* key, std::uint64_t fallback = 0) {
    const auto it = j.find(key);
    if (it == j.end()) return fallback;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    if (it->is_number_integer()) {
        const std::int64_t v = it->get<std::int64_t>();
        return v < 0 ? fallback : static_cast<std::uint64_t>(v);
    }
    return fallback;
}

GalleryTileRef tileRef(const json& j, const char* key, std::size_t sheets) {
    GalleryTileRef r;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != 3) return r;
    for (const json& v : *it)
        if (!v.is_number_integer()) return r;
    const int sheet = (*it)[0].get<int>(), col = (*it)[1].get<int>(), row = (*it)[2].get<int>();
    if (sheet < 0 || static_cast<std::size_t>(sheet) >= sheets || col < 0 || row < 0) return r;
    r.sheet = sheet;
    r.col = col;
    r.row = row;
    return r;
}

GallerySheets sheetsOf(const json& j, const char* sizeKey, const char* filesKey, const std::filesystem::path& folder) {
    GallerySheets s;
    const auto size = j.find(sizeKey);
    if (size != j.end() && size->is_array() && size->size() == 2 && (*size)[0].is_number_integer() && (*size)[1].is_number_integer()) {
        s.tileW = (*size)[0].get<int>();
        s.tileH = (*size)[1].get<int>();
    }
    const auto files = j.find(filesKey);
    if (files != j.end() && files->is_array())
        for (const json& f : *files)
            if (f.is_string()) s.files.push_back((folder / std::filesystem::path(f.get<std::string>())).lexically_normal());
    if (s.tileW <= 0 || s.tileH <= 0 || s.tileW > 4096 || s.tileH > 4096) s.files.clear();
    return s;
}

}  // namespace

GalleryData loadGalleryFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + file.string());
    json j;
    try {
        j = json::parse(in);
    } catch (const json::exception& e) {
        throw std::runtime_error(file.string() + ": not a gallery file (" + e.what() + ")");
    }
    if (!j.is_object() || !j.contains("items") || !j["items"].is_array()) throw std::runtime_error(file.string() + ": no 'items' list");
    GalleryData d;
    std::error_code ec;
    d.file = std::filesystem::absolute(file, ec);
    if (ec) d.file = file;
    const std::filesystem::path folder = d.file.parent_path();
    d.source = text(j, "source");
    d.tiles = sheetsOf(j, "tile", "sheets", folder);
    if (const auto t = j.find("thumbs"); t != j.end() && t->is_object()) d.thumbs = sheetsOf(*t, "tile", "sheets", folder);
    if (const auto k = j.find("kinds"); k != j.end() && k->is_array())
        for (const json& kind : *k)
            if (kind.is_object() && kind.contains("id") && kind["id"].is_number_integer()) d.kinds.emplace_back(kind["id"].get<int>(), text(kind, "name"));
    if (const auto m = j.find("metrics"); m != j.end() && m->is_object())
        for (const auto& [name, about] : m->items()) d.metrics.emplace_back(name, about.is_string() ? about.get<std::string>() : std::string());

    for (const json& it : j["items"]) {
        if (!it.is_object() || !it.contains("id") || !it["id"].is_number_integer()) continue;
        GalleryItem g;
        g.id = it["id"].get<int>();
        g.origin = text(it, "origin");
        g.status = text(it, "status");
        g.kind = it.contains("kind") && it["kind"].is_number_integer() ? it["kind"].get<int>() : -1;
        g.seed = count(it, "seed", 1);
        g.init = text(it, "init", g.init);
        g.food = text(it, "food", g.food);
        if (const auto s = it.find("sources"); s != it.end() && s->is_array())
            for (const json& v : *s)
                if (v.is_string()) g.sources.push_back(v.get<std::string>());
        if (const auto p = it.find("params"); p != it.end() && p->is_object())
            for (const auto& [k, v] : p->items())
                if (v.is_number()) g.params.emplace_back(k, v.get<double>());
        g.target = count(it, "target");
        g.cells = count(it, "cells");
        g.step = count(it, "step");
        g.stoppedAt = count(it, "stopped_at");
        if (const auto m = it.find("metrics"); m != it.end() && m->is_object())
            for (const auto& [k, v] : m->items())
                if (v.is_number()) g.metrics.emplace_back(k, v.get<double>());
        g.rating = static_cast<int>(number(it, "rating", 0.0));
        g.category = text(it, "category");
        g.tile = tileRef(it, "tile", d.tiles.files.size());
        g.thumb = tileRef(it, "thumb", d.thumbs.files.size());
        d.items.push_back(std::move(g));
    }
    // Metrics the file lists only per form.
    for (const GalleryItem& g : d.items)
        for (const auto& [k, v] : g.metrics)
            if (std::none_of(d.metrics.begin(), d.metrics.end(), [&](const auto& m) { return m.first == k; })) d.metrics.emplace_back(k, std::string());
    return d;
}

std::filesystem::path defaultGalleryFile() {
    std::error_code ec;
    std::vector<std::filesystem::path> starts;
#ifdef _MSC_VER
    char* program = nullptr;
    if (_get_pgmptr(&program) == 0 && program != nullptr) starts.push_back(std::filesystem::path(program).parent_path());
#endif
    starts.push_back(std::filesystem::current_path(ec));
    for (std::filesystem::path dir : starts) {
        for (int up = 0; up < 6 && !dir.empty(); ++up) {
            const std::filesystem::path native = dir / "runs" / "atlas" / "gallery" / "gallery.json";
            const std::filesystem::path web = dir / "web" / "app" / "gallery" / "gallery.json";
            if (std::filesystem::is_regular_file(native, ec)) return native;
            if (std::filesystem::is_regular_file(web, ec)) return web;
            if (dir == dir.parent_path()) break;
            dir = dir.parent_path();
        }
    }
    return {};
}

std::optional<GalleryStart> galleryStart(const GalleryItem& item, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<GalleryStart> {
        if (error) *error = "gallery form " + std::to_string(item.id) + ": " + why;
        return std::nullopt;
    };
    GalleryStart s;
    s.id = item.id;
    s.shape = item.init;
    s.seed = item.seed;
    s.pauseAtStep = item.step;
    s.cells = item.cells;
    if (!cf::parse_start_food(item.food, s.food)) return fail("unknown initial food '" + item.food + "'");
    for (const auto& [key, value] : item.params) {
        // As the atlas passed them to cf_gpu: --param key=<value with nine significant digits>.
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.9g", value);
        const std::string err = cf::apply_text(s.params, key + "=" + buf);
        if (!err.empty()) return fail(err);
    }
    const cf::ParamIssues issues = cf::validate(s.params);
    if (!issues.ok()) return fail(issues.errors.front());
    return s;
}

// =============================================================================== the panel

namespace {

void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("gallery: ") + what + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
}

std::uint32_t memoryType(VkPhysicalDevice gpu, std::uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    throw std::runtime_error("gallery: no suitable memory type");
}

ImTextureRef textureOf(VkDescriptorSet set) { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(set))); }

struct Sheet {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
    std::string error;
};

std::shared_ptr<Sheet> decodeSheet(const std::filesystem::path& file) {
    auto s = std::make_shared<Sheet>();
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        s->error = "cannot open " + file.string();
        return s;
    }
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    int w = 0, h = 0, channels = 0;
    unsigned char* px = bytes.size() < static_cast<std::size_t>(INT_MAX)
                            ? stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4)
                            : nullptr;
    if (px == nullptr) {
        const char* why = stbi_failure_reason();
        s->error = "cannot read " + file.string() + (why ? std::string(": ") + why : std::string());
        return s;
    }
    s->w = w;
    s->h = h;
    s->rgba.assign(px, px + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(px);
    return s;
}

// Tile (col, row) of a sheet, `srcW` x `srcH` pixels, averaged down (or repeated up) to dstW x dstH.
bool cropTile(const Sheet& sheet, int col, int row, int srcW, int srcH, int dstW, int dstH, std::vector<unsigned char>& out) {
    const long long x0 = static_cast<long long>(col) * srcW, y0 = static_cast<long long>(row) * srcH;
    if (x0 + srcW > sheet.w || y0 + srcH > sheet.h) return false;
    out.assign(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH) * 4u, 255);
    for (int y = 0; y < dstH; ++y) {
        const int sy0 = static_cast<int>(y0) + y * srcH / dstH;
        const int sy1 = std::max(sy0 + 1, static_cast<int>(y0) + (y + 1) * srcH / dstH);
        for (int x = 0; x < dstW; ++x) {
            const int sx0 = static_cast<int>(x0) + x * srcW / dstW;
            const int sx1 = std::max(sx0 + 1, static_cast<int>(x0) + (x + 1) * srcW / dstW);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1; ++sy)
                for (int sx = sx0; sx < sx1; ++sx) {
                    const unsigned char* p = &sheet.rgba[(static_cast<std::size_t>(sy) * static_cast<std::size_t>(sheet.w) + static_cast<std::size_t>(sx)) * 4u];
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                    ++n;
                }
            unsigned char* q = &out[(static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW) + static_cast<std::size_t>(x)) * 4u];
            for (int c = 0; c < 3; ++c) q[c] = static_cast<unsigned char>((sum[c] + n / 2) / n);
        }
    }
    return true;
}

// A texture of equal slots, each holding one picture, reused least recently drawn first.
struct TileCache {
    std::uint32_t slotW = 0, slotH = 0, cols = 0, rows = 0;
    bool useThumbs = false;  // its pictures come from the thumbnail sheets
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDeviceSize bytes = 0;
    struct Slot {
        int item = -1;
        int lastUsed = INT_MIN;  // ImGui frame it was last drawn in
        bool ready = false;      // its picture is on the GPU
        bool uploading = false;
    };
    std::vector<Slot> slots;
    std::unordered_map<int, std::uint32_t> slotOf;  // item index -> slot
    std::unordered_set<int> pending, failed;         // asked of the worker; no picture to be had
    ImVec2 uv0(std::uint32_t s) const {
        const float W = static_cast<float>(cols * slotW), H = static_cast<float>(rows * slotH);
        return ImVec2((static_cast<float>((s % cols) * slotW) + 0.5f) / W, (static_cast<float>((s / cols) * slotH) + 0.5f) / H);
    }
    ImVec2 uv1(std::uint32_t s) const {
        const float W = static_cast<float>(cols * slotW), H = static_cast<float>(rows * slotH);
        return ImVec2((static_cast<float>((s % cols + 1) * slotW) - 0.5f) / W, (static_cast<float>((s / cols + 1) * slotH) - 0.5f) / H);
    }
};

struct Request {
    int cache = 0, item = 0;
    std::filesystem::path sheet;
    int col = 0, row = 0, srcW = 0, srcH = 0, dstW = 0, dstH = 0;
    std::uint64_t generation = 0;
    int frame = 0;  // when it was asked for
};

struct Result {
    int cache = 0, item = 0;
    std::uint64_t generation = 0;
    bool ok = false, stale = false;
    std::string error;
    std::vector<unsigned char> rgba;
};

// One upload: a staging buffer, the commands that copy it into the textures, and their fence.
struct Batch {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool busy = false;
    struct Entry {
        int cache;
        std::uint32_t slot;
        int item;
    };
    std::vector<Entry> entries;
};

enum SortKey { kSortId = 0, kSortCells, kSortStep, kSortMetrics };

}  // namespace

struct GalleryPanel::Impl {
    GalleryVulkan vk;
    GalleryOptions options;
    VkCommandPool pool = VK_NULL_HANDLE;
    Batch batches[2];
    TileCache caches[2];  // 0: the grid, 1: the larger pictures
    bool haveData = false;
    GalleryData data;
    std::unordered_map<int, int> indexOf;  // id -> item index
    std::string status;
    int current = 0;
    int lastUpdate = INT_MIN;
    float thumbWidth = 128.0f;

    // The worker that decodes sheets and cuts out tiles.
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    std::vector<Request> requests;  // newest last: served first
    std::deque<Result> results;
    std::atomic<std::uint64_t> generation{1};
    std::atomic<int> frameNow{0};

    // What is shown: indices into data.items, after the filters and the sort.
    std::vector<int> view;
    bool viewDirty = true;
    char search[32] = {};
    int kindFilter = 0, statusFilter = 0, sourceFilter = 0, ratingFilter = 0, startFilter = 0, originFilter = 0;
    int sortKey = kSortId;
    bool descending = false;
    std::vector<int> kindChoices;               // after "any": kind ids (-1 for none)
    std::vector<std::string> statusChoices, sourceChoices, startChoices, originChoices;
    std::vector<std::string> kindLabels, statusLabels, sourceLabels, startLabels, originLabels, sortLabels;

    explicit Impl(const GalleryVulkan& v, const GalleryOptions& o) : vk(v), options(o), thumbWidth(o.thumbWidth) {}

    // ------------------------------------------------------------------ Vulkan
    void createCommon() {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = vk.queueFamily;
        vkCheck(vkCreateCommandPool(vk.device, &pi, nullptr, &pool), "vkCreateCommandPool");
        for (Batch& b : batches) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            vkCheck(vkAllocateCommandBuffers(vk.device, &ai, &b.cmd), "vkAllocateCommandBuffers");
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCheck(vkCreateFence(vk.device, &fi, nullptr, &b.fence), "vkCreateFence");
        }
    }

    void destroyStaging(Batch& b) {
        if (b.mapped) vkUnmapMemory(vk.device, b.memory);
        if (b.buffer) vkDestroyBuffer(vk.device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(vk.device, b.memory, nullptr);
        b.buffer = VK_NULL_HANDLE;
        b.memory = VK_NULL_HANDLE;
        b.mapped = nullptr;
        b.size = 0;
    }

    void ensureStaging(Batch& b, VkDeviceSize size) {
        if (b.size >= size) return;
        destroyStaging(b);
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCheck(vkCreateBuffer(vk.device, &bi, nullptr, &b.buffer), "vkCreateBuffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk.device, b.buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(vk.physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &b.memory), "vkAllocateMemory (gallery staging)");
        vkCheck(vkBindBufferMemory(vk.device, b.buffer, b.memory, 0), "vkBindBufferMemory");
        vkCheck(vkMapMemory(vk.device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "vkMapMemory");
        b.size = size;
    }

    void destroyCache(TileCache& c) {
        if (c.set) ImGui_ImplVulkan_RemoveTexture(c.set);
        if (c.view) vkDestroyImageView(vk.device, c.view, nullptr);
        if (c.image) vkDestroyImage(vk.device, c.image, nullptr);
        if (c.memory) vkFreeMemory(vk.device, c.memory, nullptr);
        c = TileCache{};
    }

    // Records and submits `record` on batch 0's command buffer and waits for it.
    void immediate(const std::function<void(VkCommandBuffer)>& record) {
        Batch& b = batches[0];
        vkCheck(vkWaitForFences(vk.device, 1, &b.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        finishBatch(b);
        vkCheck(vkResetFences(vk.device, 1, &b.fence), "vkResetFences");
        vkCheck(vkResetCommandBuffer(b.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(b.cmd, &bi), "vkBeginCommandBuffer");
        record(b.cmd);
        vkCheck(vkEndCommandBuffer(b.cmd), "vkEndCommandBuffer");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &b.cmd;
        vkCheck(vkQueueSubmit(vk.queue, 1, &si, b.fence), "vkQueueSubmit");
        vkCheck(vkWaitForFences(vk.device, 1, &b.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    }

    void createCache(TileCache& c, std::uint32_t slotW, std::uint32_t slotH, std::uint32_t slots) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(vk.physicalDevice, &props);
        const std::uint32_t maxDim = std::min<std::uint32_t>(props.limits.maxImageDimension2D, 4096u);
        slotW = std::clamp<std::uint32_t>(slotW, 1u, maxDim);
        slotH = std::clamp<std::uint32_t>(slotH, 1u, maxDim);
        slots = std::max<std::uint32_t>(slots, 1u);
        c.slotW = slotW;
        c.slotH = slotH;
        c.cols = std::min(slots, maxDim / slotW);
        c.rows = std::min((slots + c.cols - 1) / c.cols, maxDim / slotH);
        c.slots.assign(std::min(slots, c.cols * c.rows), TileCache::Slot{});

        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;  // the sheets' pixels as they are, as ImGui's own textures
        ii.extent = {c.cols * slotW, c.rows * slotH, 1};
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(vk.device, &ii, nullptr, &c.image), "vkCreateImage (gallery)");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk.device, c.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(vk.physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(vk.device, &ai, nullptr, &c.memory), "vkAllocateMemory (gallery)");
        vkCheck(vkBindImageMemory(vk.device, c.image, c.memory, 0), "vkBindImageMemory");
        c.bytes = req.size;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = c.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(vk.device, &vi, nullptr, &c.view), "vkCreateImageView");

        // A defined layout and colour before ImGui samples it.
        immediate([&](VkCommandBuffer cmd) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = c.image;
            b.subresourceRange = vi.subresourceRange;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
            const VkClearColorValue grey{{0.13f, 0.13f, 0.15f, 1.0f}};
            vkCmdClearColorImage(cmd, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1, &vi.subresourceRange);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        });
        c.set = ImGui_ImplVulkan_AddTexture(c.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (c.set == VK_NULL_HANDLE) throw std::runtime_error("gallery: ImGui_ImplVulkan_AddTexture failed (is ImGui's descriptor pool large enough?)");
    }

    // A batch whose fence has signalled: its pictures can be drawn now.
    void finishBatch(Batch& b) {
        if (!b.busy) return;
        for (const Batch::Entry& e : b.entries) {
            if (e.slot >= caches[e.cache].slots.size()) continue;
            TileCache::Slot& s = caches[e.cache].slots[e.slot];
            if (s.item == e.item) {
                s.ready = true;
                s.uploading = false;
            }
        }
        b.entries.clear();
        b.busy = false;
    }

    void waitIdle() {
        vkQueueWaitIdle(vk.queue);
        for (Batch& b : batches) finishBatch(b);
    }

    // ------------------------------------------------------------------ the worker
    void startWorker() { worker = std::thread([this] { workerMain(); }); }

    void stopWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            quit = true;
        }
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void workerMain() {
        std::list<std::pair<std::filesystem::path, std::shared_ptr<Sheet>>> recent;  // a few decoded sheets, newest first
        for (;;) {
            std::vector<Request> batch;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return quit || !requests.empty(); });
                if (quit) return;
                // The newest request, and every other one on the same sheet.
                const Request newest = requests.back();
                for (auto it = requests.begin(); it != requests.end();) {
                    if (it->sheet == newest.sheet) {
                        batch.push_back(std::move(*it));
                        it = requests.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            const std::uint64_t gen = generation.load();
            const int frame = frameNow.load();
            std::vector<Result> out;
            std::shared_ptr<Sheet> sheet;
            for (Request& r : batch) {
                Result res;
                res.cache = r.cache;
                res.item = r.item;
                res.generation = r.generation;
                // Asked for long ago and not since: no longer on screen.
                if (r.generation != gen || r.frame < frame - 30) {
                    res.stale = true;
                    out.push_back(std::move(res));
                    continue;
                }
                if (!sheet) {
                    for (auto it = recent.begin(); it != recent.end(); ++it)
                        if (it->first == r.sheet) {
                            sheet = it->second;
                            recent.splice(recent.begin(), recent, it);
                            break;
                        }
                    if (!sheet) {
                        sheet = decodeSheet(r.sheet);
                        recent.emplace_front(r.sheet, sheet);
                        if (recent.size() > 4) recent.pop_back();
                    }
                }
                if (!sheet->error.empty()) {
                    res.error = sheet->error;
                } else if (!cropTile(*sheet, r.col, r.row, r.srcW, r.srcH, r.dstW, r.dstH, res.rgba)) {
                    res.error = r.sheet.filename().string() + " has no tile at column " + std::to_string(r.col) + ", row " + std::to_string(r.row);
                } else {
                    res.ok = true;
                }
                out.push_back(std::move(res));
            }
            std::lock_guard<std::mutex> lock(mutex);
            for (Result& r : out) results.push_back(std::move(r));
        }
    }

    // ------------------------------------------------------------------ per frame
    // The slot of an item's picture in a cache when it is ready to draw, else -1 (and it is asked for).
    int want(int cacheIndex, int item, int frame) {
        TileCache& c = caches[cacheIndex];
        if (c.slots.empty()) return -1;
        if (const auto it = c.slotOf.find(item); it != c.slotOf.end()) {
            TileCache::Slot& s = c.slots[it->second];
            s.lastUsed = frame;
            return s.ready ? static_cast<int>(it->second) : -1;
        }
        if (c.failed.count(item) || c.pending.count(item) || c.pending.size() >= 2048) return -1;
        const GalleryItem& g = data.items[static_cast<std::size_t>(item)];
        const GalleryTileRef ref = c.useThumbs ? g.thumb : g.tile;
        const GallerySheets& sheets = c.useThumbs ? data.thumbs : data.tiles;
        if (!ref.valid()) {
            c.failed.insert(item);
            return -1;
        }
        Request r;
        r.cache = cacheIndex;
        r.item = item;
        r.sheet = sheets.files[static_cast<std::size_t>(ref.sheet)];
        r.col = ref.col;
        r.row = ref.row;
        r.srcW = sheets.tileW;
        r.srcH = sheets.tileH;
        r.dstW = static_cast<int>(c.slotW);
        r.dstH = static_cast<int>(c.slotH);
        r.generation = generation.load();
        r.frame = frame;
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(std::move(r));
        }
        wake.notify_one();
        c.pending.insert(item);
        return -1;
    }

    int pickSlot(TileCache& c, int frame) {
        int best = -1, bestUsed = INT_MAX;
        for (std::size_t i = 0; i < c.slots.size(); ++i) {
            const TileCache::Slot& s = c.slots[i];
            if (s.uploading) continue;
            if (s.item < 0) return static_cast<int>(i);
            // Not drawn in this frame (whose draw data is not submitted yet); earlier frames are
            // ordered before the copy by its barrier.
            if (s.lastUsed < frame && s.lastUsed < bestUsed) {
                best = static_cast<int>(i);
                bestUsed = s.lastUsed;
            }
        }
        return best;
    }

    void update() {
        const int frame = ImGui::GetFrameCount();
        if (frame == lastUpdate) return;
        lastUpdate = frame;
        frameNow.store(frame);
        for (Batch& b : batches)
            if (b.busy && vkGetFenceStatus(vk.device, b.fence) == VK_SUCCESS) finishBatch(b);
        Batch* free = nullptr;
        for (Batch& b : batches)
            if (!b.busy) free = &b;
        if (free == nullptr || !haveData) return;

        std::vector<Result> got;
        {
            std::lock_guard<std::mutex> lock(mutex);
            while (!results.empty() && got.size() < options.uploadsPerFrame) {
                got.push_back(std::move(results.front()));
                results.pop_front();
            }
        }
        if (got.empty()) return;
        const std::uint64_t gen = generation.load();
        struct Copy {
            int cache;
            std::uint32_t slot;
            VkDeviceSize offset;
        };
        std::vector<Copy> copies;
        VkDeviceSize used = 0;
        for (Result& r : got) {
            if (r.generation != gen) continue;
            TileCache& c = caches[r.cache];
            c.pending.erase(r.item);
            if (r.stale) continue;
            if (!r.ok) {
                c.failed.insert(r.item);
                if (status.empty()) status = r.error;
                continue;
            }
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(c.slotW) * c.slotH * 4u;
            if (r.rgba.size() != bytes || used + bytes > free->size || c.slotOf.count(r.item)) continue;
            const int slot = pickSlot(c, frame);
            if (slot < 0) continue;  // every slot is on screen: asked for again when there is room
            TileCache::Slot& s = c.slots[static_cast<std::size_t>(slot)];
            if (s.item >= 0) c.slotOf.erase(s.item);
            s.item = r.item;
            s.ready = false;
            s.uploading = true;
            s.lastUsed = frame;
            c.slotOf[r.item] = static_cast<std::uint32_t>(slot);
            std::memcpy(static_cast<unsigned char*>(free->mapped) + used, r.rgba.data(), static_cast<std::size_t>(bytes));
            copies.push_back({r.cache, static_cast<std::uint32_t>(slot), used});
            free->entries.push_back({r.cache, static_cast<std::uint32_t>(slot), r.item});
            used += bytes;
        }
        if (copies.empty()) return;

        vkCheck(vkResetFences(vk.device, 1, &free->fence), "vkResetFences");
        vkCheck(vkResetCommandBuffer(free->cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(free->cmd, &bi), "vkBeginCommandBuffer");
        for (int ci = 0; ci < 2; ++ci) {
            TileCache& c = caches[ci];
            std::vector<VkBufferImageCopy> regions;
            for (const Copy& cp : copies) {
                if (cp.cache != ci) continue;
                VkBufferImageCopy r{};
                r.bufferOffset = cp.offset;
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageOffset = {static_cast<std::int32_t>((cp.slot % c.cols) * c.slotW), static_cast<std::int32_t>((cp.slot / c.cols) * c.slotH), 0};
                r.imageExtent = {c.slotW, c.slotH, 1};
                regions.push_back(r);
            }
            if (regions.empty()) continue;
            // After every earlier use on this queue: the frames that sampled the texture (their
            // slots may be overwritten now) and earlier copies.
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = c.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(free->cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                 nullptr, 0, nullptr, 1, &b);
            vkCmdCopyBufferToImage(free->cmd, free->buffer, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<std::uint32_t>(regions.size()),
                                   regions.data());
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(free->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        vkCheck(vkEndCommandBuffer(free->cmd), "vkEndCommandBuffer");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &free->cmd;
        vkCheck(vkQueueSubmit(vk.queue, 1, &si, free->fence), "vkQueueSubmit (gallery)");
        free->busy = true;
    }

    // ------------------------------------------------------------------ loading
    bool load(const std::filesystem::path& requested) {
        std::filesystem::path file = requested.empty() ? defaultGalleryFile() : requested;
        if (file.empty()) {
            status = "no gallery found: neither runs/atlas/gallery/gallery.json (python scripts/atlas native-gallery) nor web/app/gallery/gallery.json";
            return false;
        }
        GalleryData fresh;
        try {
            fresh = loadGalleryFile(file);
        } catch (const std::exception& e) {
            status = e.what();
            return false;
        }
        if (fresh.tiles.files.empty() && fresh.thumbs.files.empty()) {
            status = file.string() + ": no contact sheets";
            return false;
        }
        // Nothing of the old gallery may be in use any more.
        ++generation;
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.clear();
            results.clear();
        }
        waitIdle();
        for (TileCache& c : caches) destroyCache(c);
        data = std::move(fresh);
        haveData = true;
        status.clear();
        indexOf.clear();
        for (std::size_t i = 0; i < data.items.size(); ++i) indexOf.emplace(data.items[i].id, static_cast<int>(i));

        // The grid: thumbnails when there are, else the tiles at half size when they are large.
        const bool thumbs = !data.thumbs.files.empty();
        const GallerySheets& gridFrom = thumbs ? data.thumbs : data.tiles;
        std::uint32_t gw = static_cast<std::uint32_t>(gridFrom.tileW), gh = static_cast<std::uint32_t>(gridFrom.tileH);
        if (!thumbs && gw > 200u) {
            gw /= 2u;
            gh /= 2u;
        }
        const std::uint32_t items = static_cast<std::uint32_t>(std::max<std::size_t>(data.items.size(), 1));
        caches[0].useThumbs = thumbs;
        createCache(caches[0], gw, gh, std::min(options.gridSlots, items));
        const bool tiles = !data.tiles.files.empty();
        const GallerySheets& bigFrom = tiles ? data.tiles : data.thumbs;
        caches[1].useThumbs = !tiles;
        createCache(caches[1], static_cast<std::uint32_t>(bigFrom.tileW), static_cast<std::uint32_t>(bigFrom.tileH), std::min(options.previewSlots, items));
        const VkDeviceSize slotBytes =
            std::max<VkDeviceSize>(static_cast<VkDeviceSize>(caches[0].slotW) * caches[0].slotH, static_cast<VkDeviceSize>(caches[1].slotW) * caches[1].slotH) * 4u;
        for (Batch& b : batches) ensureStaging(b, slotBytes * std::max<std::uint32_t>(options.uploadsPerFrame, 1u));

        buildChoices();
        viewDirty = true;
        return true;
    }

    // ------------------------------------------------------------------ filters
    static std::string startOf(const GalleryItem& g) { return g.food == "random" ? g.init : g.init + ", " + g.food + " food"; }

    void buildChoices() {
        std::map<int, int> kinds;
        std::map<std::string, int> statuses, sources, starts, origins;
        int base = 0;
        for (const GalleryItem& g : data.items) {
            ++kinds[g.kind];
            ++statuses[g.status.empty() ? std::string("(none)") : g.status];
            for (const std::string& s : g.sources) ++sources[s];
            if (g.sources.empty()) ++base;
            ++starts[startOf(g)];
            ++origins[g.origin.empty() ? std::string("(none)") : g.origin];
        }
        const std::string all = std::to_string(data.items.size());
        kindChoices.clear();
        kindLabels = {"any kind (" + all + ")"};
        for (const auto& [id, name] : data.kinds)
            if (kinds.count(id)) {
                kindChoices.push_back(id);
                kindLabels.push_back(std::to_string(id) + ": " + name + " (" + std::to_string(kinds[id]) + ")");
            }
        for (const auto& [id, n] : kinds)
            if (std::find(kindChoices.begin(), kindChoices.end(), id) == kindChoices.end()) {
                kindChoices.push_back(id);
                kindLabels.push_back((id < 0 ? std::string("no kind") : "kind " + std::to_string(id)) + " (" + std::to_string(n) + ")");
            }
        auto fill = [&](const std::map<std::string, int>& m, const char* any, std::vector<std::string>& choices, std::vector<std::string>& labels) {
            choices.clear();
            labels = {std::string(any) + " (" + all + ")"};
            for (const auto& [k, n] : m) {
                choices.push_back(k);
                labels.push_back(k + " (" + std::to_string(n) + ")");
            }
        };
        fill(statuses, "any status", statusChoices, statusLabels);
        fill(sources, "any mechanism", sourceChoices, sourceLabels);
        sourceChoices.insert(sourceChoices.begin(), std::string());  // the base model alone
        sourceLabels.insert(sourceLabels.begin() + 1, "none: the base model (" + std::to_string(base) + ")");
        fill(starts, "any start", startChoices, startLabels);
        fill(origins, "any origin", originChoices, originLabels);
        sortLabels = {"id", "cells", "step"};
        for (const auto& [name, about] : data.metrics) sortLabels.push_back(name);
        kindFilter = statusFilter = sourceFilter = startFilter = originFilter = ratingFilter = 0;
        sortKey = std::min<int>(sortKey, static_cast<int>(sortLabels.size()) - 1);
    }

    bool passes(const GalleryItem& g) const {
        if (search[0] != '\0' && std::to_string(g.id).find(search) == std::string::npos) return false;
        if (kindFilter > 0 && g.kind != kindChoices[static_cast<std::size_t>(kindFilter - 1)]) return false;
        if (statusFilter > 0 && (g.status.empty() ? std::string("(none)") : g.status) != statusChoices[static_cast<std::size_t>(statusFilter - 1)]) return false;
        if (sourceFilter > 0) {
            const std::string& s = sourceChoices[static_cast<std::size_t>(sourceFilter - 1)];
            if (s.empty() ? !g.sources.empty() : std::find(g.sources.begin(), g.sources.end(), s) == g.sources.end()) return false;
        }
        if (startFilter > 0 && startOf(g) != startChoices[static_cast<std::size_t>(startFilter - 1)]) return false;
        if (originFilter > 0 && (g.origin.empty() ? std::string("(none)") : g.origin) != originChoices[static_cast<std::size_t>(originFilter - 1)]) return false;
        switch (ratingFilter) {
            case 1: return g.rating > 0;
            case 2: return g.rating >= 3;
            case 3: return g.rating >= 4;
            case 4: return g.rating >= 5;
            case 5: return g.rating == 0;
            default: return true;
        }
    }

    void rebuildView() {
        view.clear();
        for (std::size_t i = 0; i < data.items.size(); ++i)
            if (passes(data.items[i])) view.push_back(static_cast<int>(i));
        auto key = [&](int i) -> std::optional<double> {
            const GalleryItem& g = data.items[static_cast<std::size_t>(i)];
            if (sortKey == kSortCells) return static_cast<double>(g.cells);
            if (sortKey == kSortStep) return static_cast<double>(g.step);
            if (sortKey >= kSortMetrics) {
                const double* v = g.metric(data.metrics[static_cast<std::size_t>(sortKey - kSortMetrics)].first);
                return v ? std::optional<double>(*v) : std::nullopt;
            }
            return static_cast<double>(g.id);
        };
        std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
            const std::optional<double> ka = key(a), kb = key(b);
            if (!ka || !kb) return ka.has_value() && !kb.has_value();  // forms without the value last
            if (*ka != *kb) return descending ? *ka > *kb : *ka < *kb;
            return data.items[static_cast<std::size_t>(a)].id < data.items[static_cast<std::size_t>(b)].id;
        });
        viewDirty = false;
    }

    // ------------------------------------------------------------------ drawing
    void picture(int cacheIndex, int item, ImVec2 p0, ImVec2 p1, int frame) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const TileCache& c = caches[cacheIndex];
        const int slot = want(cacheIndex, item, frame);
        if (slot >= 0) {
            dl->AddImage(textureOf(c.set), p0, p1, c.uv0(static_cast<std::uint32_t>(slot)), c.uv1(static_cast<std::uint32_t>(slot)));
            return;
        }
        dl->AddRectFilled(p0, p1, IM_COL32(34, 34, 40, 255));
        if (c.failed.count(item)) {
            const char* what = "no picture";
            const ImVec2 ts = ImGui::CalcTextSize(what);
            dl->AddText(ImVec2(0.5f * (p0.x + p1.x - ts.x), 0.5f * (p0.y + p1.y - ts.y)), IM_COL32(150, 150, 150, 255), what);
        }
    }

    void tooltip(int item, int frame) {
        const GalleryItem& g = data.items[static_cast<std::size_t>(item)];
        ImGui::BeginTooltip();
        const TileCache& big = caches[1];
        const ImVec2 size(static_cast<float>(big.slotW), static_cast<float>(big.slotH));
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        picture(1, item, p0, ImVec2(p0.x + size.x, p0.y + size.y), frame);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(size.x, ImGui::GetFontSize() * 22.0f));
        ImGui::Text("Form %d", g.id);
        const std::string kind = data.kindName(g.kind);
        if (!kind.empty()) ImGui::TextWrapped("kind %d: %s", g.kind, kind.c_str());
        if (!g.origin.empty() || !g.status.empty())
            ImGui::TextWrapped("%s%s%s", g.origin.c_str(), (!g.origin.empty() && !g.status.empty()) ? ", " : "", g.status.c_str());
        ImGui::TextWrapped("start %s, %s food, seed %llu", g.init.c_str(), g.food.c_str(), static_cast<unsigned long long>(g.seed));
        std::string mech;
        for (const std::string& s : g.sources) mech += (mech.empty() ? "" : ", ") + s;
        ImGui::TextWrapped("mechanisms: %s", mech.empty() ? "none (the base model)" : mech.c_str());
        ImGui::TextWrapped("%llu cells at step %llu (grown to %llu%s)", static_cast<unsigned long long>(g.cells), static_cast<unsigned long long>(g.step),
                           static_cast<unsigned long long>(g.target), g.stoppedAt ? ", stopped early" : "");
        if (g.rating > 0 || !g.category.empty())
            ImGui::TextWrapped("rated %d%s%s", g.rating, g.category.empty() ? "" : ", ", g.category.c_str());
        auto table = [&](const char* id, const std::vector<std::pair<std::string, double>>& rows) {
            if (rows.empty()) return;
            if (ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit)) {
                for (const auto& [k, v] : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextDisabled("%s", k.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%.6g", v);
                }
                ImGui::EndTable();
            }
        };
        ImGui::SeparatorText("measures");
        table("##metrics", g.metrics);
        ImGui::SeparatorText("parameters (the rest are the defaults)");
        table("##params", g.params);
        ImGui::TextDisabled("click: grow it again");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    bool combo(const char* label, int& index, const std::vector<std::string>& labels) {
        bool changed = false;
        index = std::clamp(index, 0, std::max(static_cast<int>(labels.size()) - 1, 0));
        if (ImGui::BeginCombo(label, labels.empty() ? "" : labels[static_cast<std::size_t>(index)].c_str())) {
            for (std::size_t i = 0; i < labels.size(); ++i) {
                const bool selected = static_cast<int>(i) == index;
                if (ImGui::Selectable(labels[i].c_str(), selected)) {
                    index = static_cast<int>(i);
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    const GalleryItem* draw(ImVec2 size) {
        update();
        const int frame = ImGui::GetFrameCount();
        const GalleryItem* chosen = nullptr;
        ImGui::PushID(this);
        if (!ImGui::BeginChild("##gallery", size)) {
            ImGui::EndChild();
            ImGui::PopID();
            return nullptr;
        }
        if (!haveData) {
            ImGui::TextWrapped("%s", status.empty() ? "no gallery loaded" : status.c_str());
            ImGui::EndChild();
            ImGui::PopID();
            return nullptr;
        }
        const ImGuiStyle& style = ImGui::GetStyle();
        const float em = ImGui::GetFontSize();
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        // Controls flow onto the next line when the panel is narrow.
        auto next = [&](float width) {
            if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right) ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
        };
        ImGui::SetNextItemWidth(em * 6.0f);
        if (ImGui::InputTextWithHint("##search", "form id", search, sizeof search, ImGuiInputTextFlags_CharsDecimal)) viewDirty = true;
        next(em * 13.0f);
        if (combo("##kind", kindFilter, kindLabels)) viewDirty = true;
        next(em * 9.0f);
        if (combo("##status", statusFilter, statusLabels)) viewDirty = true;
        next(em * 11.0f);
        if (combo("##mechanism", sourceFilter, sourceLabels)) viewDirty = true;
        next(em * 8.0f);
        static const std::vector<std::string> ratings = {"any rating", "rated", "rated 3 or more", "rated 4 or more", "rated 5", "not rated"};
        if (combo("##rating", ratingFilter, ratings)) viewDirty = true;
        next(em * 11.0f);
        if (combo("##start", startFilter, startLabels)) viewDirty = true;
        next(em * 9.0f);
        if (combo("##origin", originFilter, originLabels)) viewDirty = true;
        next(em * 9.0f);
        if (combo("order by##sort", sortKey, sortLabels)) viewDirty = true;
        ImGui::SameLine();
        if (ImGui::Checkbox("descending", &descending)) viewDirty = true;
        next(em * 8.0f);
        ImGui::SliderFloat("##size", &thumbWidth, 48.0f, 320.0f, "pictures %.0f px");
        if (viewDirty) rebuildView();
        ImGui::SameLine();
        ImGui::TextDisabled("%zu of %zu forms", view.size(), data.items.size());
        if (!status.empty()) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "%s", status.c_str());

        if (ImGui::BeginChild("##grid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
            const GallerySheets& shape = data.tiles.files.empty() ? data.thumbs : data.tiles;
            const float cellW = std::max(thumbWidth, 16.0f);
            const float cellH = cellW * static_cast<float>(std::max(shape.tileH, 1)) / static_cast<float>(std::max(shape.tileW, 1));
            const float gap = style.ItemSpacing.y;
            const int cols = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cellW + gap)));
            const int rows = (static_cast<int>(view.size()) + cols - 1) / cols;
            ImGuiListClipper clipper;
            clipper.Begin(rows, cellH + gap);
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const std::size_t at = static_cast<std::size_t>(r) * static_cast<std::size_t>(cols) + static_cast<std::size_t>(c);
                        if (at >= view.size()) break;
                        const int item = view[at];
                        const GalleryItem& g = data.items[static_cast<std::size_t>(item)];
                        if (c > 0) ImGui::SameLine(0.0f, gap);
                        ImGui::PushID(g.id);
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const ImVec2 p1(p0.x + cellW, p0.y + cellH);
                        if (ImGui::InvisibleButton("##form", ImVec2(cellW, cellH))) {
                            chosen = &g;
                            current = g.id;
                        }
                        const bool hovered = ImGui::IsItemHovered();
                        picture(0, item, p0, p1, frame);
                        ImDrawList* dl = ImGui::GetWindowDrawList();
                        char label[48];
                        if (g.rating > 0) std::snprintf(label, sizeof label, "%d  (%d/5)", g.id, g.rating);
                        else std::snprintf(label, sizeof label, "%d", g.id);
                        const ImVec2 ts = ImGui::CalcTextSize(label);
                        dl->AddRectFilled(ImVec2(p0.x, p1.y - ts.y - 2.0f), ImVec2(p0.x + ts.x + 6.0f, p1.y), IM_COL32(0, 0, 0, 150));
                        dl->AddText(ImVec2(p0.x + 3.0f, p1.y - ts.y - 1.0f), IM_COL32(235, 235, 235, 255), label);
                        if (g.id == current) dl->AddRect(p0, p1, IM_COL32(255, 196, 64, 255), 0.0f, 0, 3.0f);
                        else if (hovered) dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 170), 0.0f, 0, 1.5f);
                        if (hovered) tooltip(item, frame);
                        ImGui::PopID();
                    }
                }
            }
            if (view.empty()) ImGui::TextDisabled("No form matches.");
        }
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::PopID();
        return chosen;
    }

    void drawTile(int id, ImVec2 size) {
        update();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);
        const auto it = indexOf.find(id);
        if (!haveData || it == indexOf.end()) {
            ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y), IM_COL32(34, 34, 40, 255));
            return;
        }
        picture(1, it->second, p0, ImVec2(p0.x + size.x, p0.y + size.y), ImGui::GetFrameCount());
    }

    // Everything, after the queue has finished with it. Safe on a partly created panel.
    void release() {
        stopWorker();
        if (vk.queue) waitIdle();
        for (TileCache& c : caches) destroyCache(c);
        for (Batch& b : batches) {
            destroyStaging(b);
            if (b.fence) vkDestroyFence(vk.device, b.fence, nullptr);
            b.fence = VK_NULL_HANDLE;
        }
        if (pool) vkDestroyCommandPool(vk.device, pool, nullptr);  // frees the command buffers
        pool = VK_NULL_HANDLE;
    }
};

GalleryPanel::GalleryPanel(const GalleryVulkan& vk, const GalleryOptions& options) : impl_(std::make_unique<Impl>(vk, options)) {
    try {
        impl_->createCommon();
        impl_->startWorker();
        impl_->load(options.file);
    } catch (...) {
        impl_->release();
        throw;
    }
}

GalleryPanel::~GalleryPanel() {
    if (impl_) impl_->release();
}

bool GalleryPanel::load(const std::filesystem::path& file) { return impl_->load(file); }
const GalleryItem* GalleryPanel::draw(ImVec2 size) { return impl_->draw(size); }
void GalleryPanel::drawTile(int id, ImVec2 size) { impl_->drawTile(id, size); }
void GalleryPanel::setCurrent(int id) { impl_->current = id; }
int GalleryPanel::current() const { return impl_->current; }
bool GalleryPanel::loaded() const { return impl_->haveData; }
const GalleryData& GalleryPanel::data() const { return impl_->data; }
const std::string& GalleryPanel::status() const { return impl_->status; }
std::uint64_t GalleryPanel::textureBytes() const { return impl_->caches[0].bytes + impl_->caches[1].bytes; }

}  // namespace cfv
