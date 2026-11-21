#include "io.hpp"

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace cf {
namespace {

constexpr char kMagic[8] = {'C', 'F', 'S', 'T', 'A', 'T', 'E', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kEndianTag = 0x01020304u;
constexpr std::uint32_t kKindCheckpoint = 1, kKindSnapshot = 2;
constexpr std::size_t kHeaderBytes = 48;
constexpr std::size_t kChunkHeaderBytes = 16;

std::uint64_t fnv1a(const char* p, std::size_t len) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(p[i]);
        h *= 0x100000001B3ull;
    }
    return h;
}

class Encoder {
public:
    explicit Encoder(std::size_t reserve) { out_.reserve(reserve); }
    template <class T>
    void value(const T& v) {
        raw(&v, sizeof(T));
    }
    void raw(const void* p, std::size_t len) {
        const char* c = static_cast<const char*>(p);
        out_.insert(out_.end(), c, c + len);
    }
    void chunk(const char tag[4], std::uint32_t elemBytes, const void* payload, std::uint64_t bytes) {
        raw(tag, 4);
        value(elemBytes);
        value(bytes);
        raw(payload, static_cast<std::size_t>(bytes));
    }
    void header(std::uint32_t kind, const State& s) {
        raw(kMagic, 8);
        value(kVersion);
        value(kEndianTag);
        value(kind);
        value(static_cast<std::uint32_t>(kMaxDeg));
        value(static_cast<std::uint64_t>(s.n));
        value(s.step);
        value(s.seed);
    }
    std::vector<char> finish() {
        const std::uint64_t h = fnv1a(out_.data(), out_.size());
        chunk("END ", 8, &h, 8);
        return std::move(out_);
    }

private:
    std::vector<char> out_;
};

struct Chunk {
    char tag[4];
    std::uint32_t elemBytes;
    std::uint64_t bytes;
    const char* payload;
    bool is(const char* t) const { return std::memcmp(tag, t, 4) == 0; }
};

struct Parsed {
    std::uint32_t kind = 0;
    std::uint32_t slots = 0;
    std::uint64_t n = 0, step = 0, seed = 0;
    std::vector<Chunk> chunks;
    const Chunk* find(const char* tag) const {
        for (const Chunk& c : chunks)
            if (c.is(tag)) return &c;
        return nullptr;
    }
    const Chunk& need(const char* tag, std::uint64_t bytes) const {
        const Chunk* c = find(tag);
        if (!c) throw std::runtime_error(std::string("state file: chunk '") + tag + "' is missing");
        if (c->bytes != bytes)
            throw std::runtime_error(std::string("state file: chunk '") + tag + "' has " + std::to_string(c->bytes) + " bytes, expected " +
                                     std::to_string(bytes));
        return *c;
    }
};

template <class T>
T read_at(const std::vector<char>& b, std::size_t off) {
    T v;
    std::memcpy(&v, b.data() + off, sizeof(T));
    return v;
}

Parsed parse(const std::vector<char>& b) {
    if (b.size() < kHeaderBytes) throw std::runtime_error("state file: too short for a header");
    if (std::memcmp(b.data(), kMagic, 8) != 0) throw std::runtime_error("state file: bad magic (not a Cellular Forms state file)");
    if (read_at<std::uint32_t>(b, 12) != kEndianTag) throw std::runtime_error("state file: written with a different byte order");
    const std::uint32_t version = read_at<std::uint32_t>(b, 8);
    if (version != kVersion) throw std::runtime_error("state file: version " + std::to_string(version) + " is not supported (this build reads version 1)");
    Parsed p;
    p.kind = read_at<std::uint32_t>(b, 16);
    p.slots = read_at<std::uint32_t>(b, 20);
    p.n = read_at<std::uint64_t>(b, 24);
    p.step = read_at<std::uint64_t>(b, 32);
    p.seed = read_at<std::uint64_t>(b, 40);
    if (p.kind != kKindCheckpoint && p.kind != kKindSnapshot) throw std::runtime_error("state file: unknown kind " + std::to_string(p.kind));
    if (p.n < 4 || p.n >= 0xFFFFFFFFull) throw std::runtime_error("state file: implausible cell count " + std::to_string(p.n));

    std::size_t off = kHeaderBytes;
    bool ended = false;
    while (off < b.size()) {
        if (b.size() - off < kChunkHeaderBytes) throw std::runtime_error("state file: truncated chunk header");
        Chunk c;
        std::memcpy(c.tag, b.data() + off, 4);
        c.elemBytes = read_at<std::uint32_t>(b, off + 4);
        c.bytes = read_at<std::uint64_t>(b, off + 8);
        if (c.bytes > b.size() - off - kChunkHeaderBytes) throw std::runtime_error("state file: truncated (a chunk runs past the end of the file)");
        c.payload = b.data() + off + kChunkHeaderBytes;
        if (c.is("END ")) {
            if (c.bytes != 8) throw std::runtime_error("state file: malformed end chunk");
            const std::uint64_t stored = read_at<std::uint64_t>(b, off + kChunkHeaderBytes);
            if (stored != fnv1a(b.data(), off)) throw std::runtime_error("state file: checksum mismatch (the file is corrupt)");
            ended = true;
            break;
        }
        p.chunks.push_back(c);
        off += kChunkHeaderBytes + static_cast<std::size_t>(c.bytes);
    }
    if (!ended) throw std::runtime_error("state file: truncated (no end chunk)");
    return p;
}

Params parse_params(const Parsed& p) {
    const Chunk* c = p.find("PARM");
    if (!c) throw std::runtime_error("state file: chunk 'PARM' is missing");
    Params params;
    const std::string err = apply_text(params, std::string_view(c->payload, static_cast<std::size_t>(c->bytes)));
    if (!err.empty()) throw std::runtime_error("state file: parameters: " + err);
    return params;
}

}  // namespace

std::vector<char> encode_checkpoint(const State& s) {
    const std::size_t n = s.n;
    Encoder e(kHeaderBytes + n * (12 + 12 + 4 + 4 + 4 + 1 + 4 * kMaxDeg) + 1024);
    e.header(kKindCheckpoint, s);
    const std::string params = to_text(s.params);
    e.chunk("PARM", 1, params.data(), params.size());
    e.chunk("POS ", 12, s.pos.data(), n * sizeof(Vec3));
    e.chunk("NRM ", 12, s.normal.data(), n * sizeof(Vec3));
    e.chunk("FOOD", 4, s.food.data(), n * sizeof(float));
    e.chunk("BRTH", 4, s.birth.data(), n * sizeof(std::uint32_t));
    e.chunk("GEN ", 4, s.gen.data(), n * sizeof(std::uint32_t));
    e.chunk("DEG ", 1, s.deg.data(), n);
    e.chunk("NBR ", 4, s.nbr.data(), n * kMaxDeg * sizeof(std::uint32_t));
    if (!s.morphA.empty()) {
        e.chunk("MPHA", 4, s.morphA.data(), n * sizeof(float));
        e.chunk("MPHB", 4, s.morphB.data(), n * sizeof(float));
    }
    return e.finish();
}

std::vector<char> encode_snapshot(const State& s) {
    const std::size_t n = s.n;
    Encoder e(kHeaderBytes + n * (6 + 1 + 1 + 4) + 1024);
    e.header(kKindSnapshot, s);
    const std::string params = to_text(s.params);
    e.chunk("PARM", 1, params.data(), params.size());

    Vec3 lo = s.pos[0], hi = s.pos[0];
    for (std::size_t v = 0; v < n; ++v) {
        const Vec3 p = s.pos[v];
        lo = Vec3{p.x < lo.x ? p.x : lo.x, p.y < lo.y ? p.y : lo.y, p.z < lo.z ? p.z : lo.z};
        hi = Vec3{p.x > hi.x ? p.x : hi.x, p.y > hi.y ? p.y : hi.y, p.z > hi.z ? p.z : hi.z};
    }
    std::vector<char> posq(6 * sizeof(float) + n * 3 * sizeof(std::uint16_t));
    const float bounds[6] = {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z};
    std::memcpy(posq.data(), bounds, sizeof(bounds));
    const double sx = hi.x > lo.x ? 65535.0 / (static_cast<double>(hi.x) - lo.x) : 0.0;
    const double sy = hi.y > lo.y ? 65535.0 / (static_cast<double>(hi.y) - lo.y) : 0.0;
    const double sz = hi.z > lo.z ? 65535.0 / (static_cast<double>(hi.z) - lo.z) : 0.0;
    auto quant = [](double x) {
        const double r = std::floor(x + 0.5);
        return static_cast<std::uint16_t>(r < 0.0 ? 0.0 : (r > 65535.0 ? 65535.0 : r));
    };
    char* q = posq.data() + sizeof(bounds);
    for (std::size_t v = 0; v < n; ++v) {
        const std::uint16_t t[3] = {quant((static_cast<double>(s.pos[v].x) - lo.x) * sx), quant((static_cast<double>(s.pos[v].y) - lo.y) * sy),
                                    quant((static_cast<double>(s.pos[v].z) - lo.z) * sz)};
        std::memcpy(q + v * sizeof(t), t, sizeof(t));
    }
    e.chunk("POSQ", 6, posq.data(), posq.size());

    std::vector<std::uint8_t> small(n);
    const float invThreshold = 1.0f / s.params.foodThreshold;
    for (std::size_t v = 0; v < n; ++v) {
        const float f = s.food[v] * invThreshold;
        small[v] = static_cast<std::uint8_t>(f <= 0.0f ? 0.0f : (f >= 1.0f ? 255.0f : std::floor(f * 255.0f + 0.5f)));
    }
    e.chunk("FODQ", 1, small.data(), n);
    for (std::size_t v = 0; v < n; ++v) small[v] = static_cast<std::uint8_t>(s.gen[v] > 255u ? 255u : s.gen[v]);
    e.chunk("GENB", 1, small.data(), n);
    e.chunk("BRTH", 4, s.birth.data(), n * sizeof(std::uint32_t));
    return e.finish();
}

void write_file(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("cannot open '" + tmp.string() + "' for writing");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) throw std::runtime_error("write to '" + tmp.string() + "' failed (disk full?)");
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) throw std::runtime_error("cannot rename '" + tmp.string() + "' to '" + path.string() + "': " + ec.message());
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open '" + path.string() + "'");
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    in.read(bytes.data(), size);
    if (!in) throw std::runtime_error("cannot read '" + path.string() + "'");
    return bytes;
}

State decode_checkpoint(const std::vector<char>& bytes) {
    const Parsed p = parse(bytes);
    if (p.kind != kKindCheckpoint) throw std::runtime_error("state file: this is a snapshot (positions only); a checkpoint is needed to resume");
    if (p.slots != kMaxDeg)
        throw std::runtime_error("state file: written with " + std::to_string(p.slots) + " neighbour slots, this build uses " + std::to_string(kMaxDeg));
    const std::uint64_t n = p.n;
    State s;
    s.resize(static_cast<std::uint32_t>(n));
    s.step = p.step;
    s.seed = p.seed;
    s.params = parse_params(p);
    std::memcpy(s.pos.data(), p.need("POS ", n * 12).payload, static_cast<std::size_t>(n * 12));
    std::memcpy(s.normal.data(), p.need("NRM ", n * 12).payload, static_cast<std::size_t>(n * 12));
    std::memcpy(s.food.data(), p.need("FOOD", n * 4).payload, static_cast<std::size_t>(n * 4));
    std::memcpy(s.birth.data(), p.need("BRTH", n * 4).payload, static_cast<std::size_t>(n * 4));
    std::memcpy(s.gen.data(), p.need("GEN ", n * 4).payload, static_cast<std::size_t>(n * 4));
    std::memcpy(s.deg.data(), p.need("DEG ", n).payload, static_cast<std::size_t>(n));
    std::memcpy(s.nbr.data(), p.need("NBR ", n * kMaxDeg * 4).payload, static_cast<std::size_t>(n * kMaxDeg * 4));
    if (p.find("MPHA") != nullptr || p.find("MPHB") != nullptr) {
        s.morphA.resize(static_cast<std::size_t>(n));
        s.morphB.resize(static_cast<std::size_t>(n));
        std::memcpy(s.morphA.data(), p.need("MPHA", n * 4).payload, static_cast<std::size_t>(n * 4));
        std::memcpy(s.morphB.data(), p.need("MPHB", n * 4).payload, static_cast<std::size_t>(n * 4));
    }
    return s;
}

State load_checkpoint(const std::filesystem::path& path) { return decode_checkpoint(read_file(path)); }

Snapshot decode_snapshot(const std::vector<char>& bytes) {
    const Parsed p = parse(bytes);
    const std::uint64_t n = p.n;
    Snapshot out;
    out.n = static_cast<std::uint32_t>(n);
    out.step = p.step;
    out.seed = p.seed;
    out.params = parse_params(p);
    out.pos.resize(out.n);
    out.food.resize(out.n);
    out.birth.resize(out.n);
    out.gen.resize(out.n);
    std::memcpy(out.birth.data(), p.need("BRTH", n * 4).payload, static_cast<std::size_t>(n * 4));
    if (p.kind == kKindCheckpoint) {
        out.fromCheckpoint = true;
        std::memcpy(out.pos.data(), p.need("POS ", n * 12).payload, static_cast<std::size_t>(n * 12));
        std::memcpy(out.food.data(), p.need("FOOD", n * 4).payload, static_cast<std::size_t>(n * 4));
        std::memcpy(out.gen.data(), p.need("GEN ", n * 4).payload, static_cast<std::size_t>(n * 4));
        Vec3 lo = out.pos[0], hi = out.pos[0];
        for (const Vec3& q : out.pos) {
            lo = Vec3{q.x < lo.x ? q.x : lo.x, q.y < lo.y ? q.y : lo.y, q.z < lo.z ? q.z : lo.z};
            hi = Vec3{q.x > hi.x ? q.x : hi.x, q.y > hi.y ? q.y : hi.y, q.z > hi.z ? q.z : hi.z};
        }
        out.boundsMin = lo;
        out.boundsMax = hi;
        return out;
    }
    const Chunk& posq = p.need("POSQ", 24 + n * 6);
    float bounds[6];
    std::memcpy(bounds, posq.payload, sizeof(bounds));
    out.boundsMin = Vec3{bounds[0], bounds[1], bounds[2]};
    out.boundsMax = Vec3{bounds[3], bounds[4], bounds[5]};
    const double sx = (static_cast<double>(bounds[3]) - bounds[0]) / 65535.0;
    const double sy = (static_cast<double>(bounds[4]) - bounds[1]) / 65535.0;
    const double sz = (static_cast<double>(bounds[5]) - bounds[2]) / 65535.0;
    const char* q = posq.payload + sizeof(bounds);
    for (std::uint64_t v = 0; v < n; ++v) {
        std::uint16_t t[3];
        std::memcpy(t, q + v * sizeof(t), sizeof(t));
        out.pos[v] = Vec3{static_cast<float>(bounds[0] + t[0] * sx), static_cast<float>(bounds[1] + t[1] * sy), static_cast<float>(bounds[2] + t[2] * sz)};
    }
    const Chunk& food = p.need("FODQ", n);
    const Chunk& gen = p.need("GENB", n);
    for (std::uint64_t v = 0; v < n; ++v) {
        out.food[v] = static_cast<float>(static_cast<unsigned char>(food.payload[v])) / 255.0f;
        out.gen[v] = static_cast<unsigned char>(gen.payload[v]);
    }
    return out;
}

Snapshot load_snapshot(const std::filesystem::path& path) { return decode_snapshot(read_file(path)); }

// ---------------------------------------------------------------- AsyncWriter

AsyncWriter::AsyncWriter() : worker_([this] { run(); }) {}

AsyncWriter::~AsyncWriter() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    worker_.join();
}

void AsyncWriter::submit(std::filesystem::path path, std::vector<char> bytes) {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.size() < maxPending; });
    queue_.push_back(Job{std::move(path), std::move(bytes)});
    wake_.notify_one();
}

std::string AsyncWriter::drain() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    return error_;
}

void AsyncWriter::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stop requested and nothing left
            job = std::move(queue_.front());
            queue_.pop_front();
            busy_ = true;
        }
        std::string error;
        try {
            write_file(job.path, job.bytes);
        } catch (const std::exception& e) {
            error = e.what();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            busy_ = false;
            if (!error.empty() && error_.empty()) error_ = error;
        }
        idle_.notify_all();
    }
}

}  // namespace cf
