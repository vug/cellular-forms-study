#include "cloud.hpp"

#include <algorithm>
#include <stdexcept>

#include "state_file.hpp"

namespace cfv {

void Cloud::finalize() {
    if (pos.empty()) {
        centre = {};
        radius = 1.0f;
        return;
    }
    cf::Vec3 lo = pos[0], hi = pos[0];
    for (const cf::Vec3& p : pos) {
        lo.x = std::min(lo.x, p.x);  lo.y = std::min(lo.y, p.y);  lo.z = std::min(lo.z, p.z);
        hi.x = std::max(hi.x, p.x);  hi.y = std::max(hi.y, p.y);  hi.z = std::max(hi.z, p.z);
    }
    centre = (lo + hi) * 0.5f;
    float r2 = 0.0f;
    for (const cf::Vec3& p : pos) r2 = std::max(r2, cf::length2(p - centre));
    radius = std::max(std::sqrt(r2), linkRestLength);

    minBirth = birth.empty() ? 0u : *std::min_element(birth.begin(), birth.end());

    // 90th percentile of the age, from at most ~100k evenly spaced cells.
    std::vector<float> ages;
    const std::size_t stride = std::max<std::size_t>(1, birth.size() / 100000);
    for (std::size_t i = 0; i < birth.size(); i += stride)
        ages.push_back(static_cast<float>(step >= birth[i] ? step - birth[i] : 0));
    if (!ages.empty()) {
        const std::size_t k = ages.size() * 9 / 10;
        std::nth_element(ages.begin(), ages.begin() + static_cast<std::ptrdiff_t>(std::min(k, ages.size() - 1)), ages.end());
        agePercentile90 = std::max(ages[std::min(k, ages.size() - 1)], 1.0f);
    }
}

Cloud cloudFromState(const cf::State& s, std::string label) {
    Cloud c;
    c.pos.assign(s.pos.begin(), s.pos.begin() + s.n);
    c.birth.assign(s.birth.begin(), s.birth.begin() + s.n);
    c.food.assign(s.food.begin(), s.food.begin() + s.n);
    c.step = s.step;
    c.foodThreshold = s.params.foodThreshold;
    c.linkRestLength = s.params.linkRestLength;
    c.label = std::move(label);
    c.finalize();
    return c;
}

Cloud loadCloud(const std::filesystem::path& file) {
    Cloud c = readCloudFile(file);
    if (c.label.empty()) c.label = file.filename().string();
    c.finalize();
    return c;
}

std::vector<std::filesystem::path> listRun(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::is_directory(dir)) throw std::runtime_error("not a directory: " + dir.string());
    // A run directory holds snapshots (snap_c<cells>_s<step>.cfs) next to checkpoints and
    // latest.cfs; scrubbing goes through the snapshots. Without any, take every state file.
    std::vector<std::filesystem::path> others;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (!e.is_regular_file() || !isCloudFile(e.path())) continue;
        const std::string name = e.path().filename().string();
        (name.rfind("snap_", 0) == 0 ? files : others).push_back(e.path());
    }
    if (files.empty()) files = std::move(others);
    // Order by the cell count in the name when there is one (growth order), then by name.
    auto cellsOf = [](const std::filesystem::path& p) -> long long {
        const std::string name = p.filename().string();
        const std::size_t at = name.find("_c");
        if (at == std::string::npos) return -1;
        long long v = 0;
        bool any = false;
        for (std::size_t i = at + 2; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i) {
            v = v * 10 + (name[i] - '0');
            any = true;
        }
        return any ? v : -1;
    };
    std::sort(files.begin(), files.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
        const long long ca = cellsOf(a), cb = cellsOf(b);
        return ca != cb ? ca < cb : a.filename().string() < b.filename().string();
    });
    return files;
}

CloudGpu::~CloudGpu() {
    ctx_.destroy(pos_);
    ctx_.destroy(birth_);
    ctx_.destroy(food_);
    ctx_.destroy(normal_);
    ctx_.destroy(index_);
}

void CloudGpu::upload(const Cloud& cloud) {
    const std::uint32_t n = cloud.count();
    if (n > capacity_) {
        ctx_.destroy(pos_);
        ctx_.destroy(birth_);
        ctx_.destroy(food_);
        capacity_ = std::max<std::uint32_t>(n + n / 4, 1024u);
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        pos_ = ctx_.createBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(cf::Vec3), usage, props);
        birth_ = ctx_.createBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(std::uint32_t), usage, props);
        food_ = ctx_.createBuffer(static_cast<VkDeviceSize>(capacity_) * sizeof(float), usage, props);
    }
    ctx_.upload(pos_, cloud.pos.data(), static_cast<VkDeviceSize>(n) * sizeof(cf::Vec3));
    ctx_.upload(birth_, cloud.birth.data(), static_cast<VkDeviceSize>(n) * sizeof(std::uint32_t));
    ctx_.upload(food_, cloud.food.data(), static_cast<VkDeviceSize>(n) * sizeof(float));
    count_ = n;

    // Membrane data, when the cloud has it (replaced on every upload: sizes differ per file).
    ctx_.destroy(normal_);
    ctx_.destroy(index_);
    indexCount_ = 0;
    if (!cloud.indices.empty() && cloud.normal.size() == cloud.pos.size()) {
        const VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        normal_ = ctx_.createBuffer(static_cast<VkDeviceSize>(n) * sizeof(cf::Vec3),
                                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, props);
        index_ = ctx_.createBuffer(cloud.indices.size() * sizeof(std::uint32_t),
                                   VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, props);
        ctx_.upload(normal_, cloud.normal.data(), static_cast<VkDeviceSize>(n) * sizeof(cf::Vec3));
        ctx_.upload(index_, cloud.indices.data(), cloud.indices.size() * sizeof(std::uint32_t));
        indexCount_ = static_cast<std::uint32_t>(cloud.indices.size());
    }
}

void applyAutoRange(RenderSettings& settings, const Cloud& cloud) {
    if (!settings.autoRange) return;
    switch (settings.colorMode) {
        case ColorMode::Age:
            settings.scalarMin = 0.0f;
            settings.scalarMax = cloud.agePercentile90;
            break;
        case ColorMode::Food:
            settings.scalarMin = 0.0f;
            settings.scalarMax = cloud.foodThreshold;
            break;
        case ColorMode::Birth:
            settings.scalarMin = static_cast<float>(cloud.minBirth);
            settings.scalarMax = std::max(static_cast<float>(cloud.step), settings.scalarMin + 1.0f);
            break;
        case ColorMode::Uniform:
            break;
    }
}

}  // namespace cfv
