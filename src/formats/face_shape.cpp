#include "formats/face_shape.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace ts2::faceshape {

namespace {

constexpr std::uint8_t kBabyAge = 5, kElderAge = 4, kFarGroup = 15;
constexpr std::uint32_t kPoolTag = 0x6B657965, kListsTag = 0x6D737472, kDataTag = 0x64617461;

} // namespace

const FaceShape::Mesh* FaceShape::find(std::uint8_t age, std::uint8_t gender, std::uint8_t group) const {
    for (const auto& m : meshes) {
        if (m.age == age && m.gender == gender && m.group == group) {
            return &m;
        }
    }
    return nullptr;
}

FaceShape parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    FaceShape shape;
    shape.version = r.u32();
    if (shape.version < 3 || shape.version > 5) {
        throw std::runtime_error("face shape: version " + std::to_string(shape.version) + " not read");
    }
    const std::size_t keys = r.u32Count(4);
    std::vector<std::uint32_t> keyValues(keys);
    for (auto& k : keyValues) {
        k = r.u32();
    }
    if (r.u32() != kPoolTag) {
        throw std::runtime_error("face shape: no point pool");
    }
    // The pool holds each distinct vector once. The lists name them by index, a point being an
    // offset then texture coordinates.
    const std::size_t poolSize = r.checkedCount(r.u16(), 12);
    std::vector<std::array<float, 3>> pool(poolSize);
    for (auto& v : pool) {
        v = {r.f32(), r.f32(), r.f32()};
        // A broken file's value that isn't a finite number would spoil the face it moves.
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
            throw std::runtime_error("face shape: a point that isn't a number");
        }
    }
    if (r.u32() != kListsTag) {
        throw std::runtime_error("face shape: no point lists");
    }
    for (const std::uint32_t key : keyValues) {
        FaceShape::Mesh mesh;
        mesh.age = static_cast<std::uint8_t>(key & 0xFF);
        mesh.gender = static_cast<std::uint8_t>((key >> 8) & 0xFF);
        mesh.group = static_cast<std::uint8_t>((key >> 16) & 0xFF);
        const std::size_t count = r.checkedCount(r.u16(), 2);
        mesh.points.resize(count / 2);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint16_t index = r.u16();
            if (index >= pool.size()) {
                throw std::runtime_error("face shape: point past the pool");
            }
            if (i / 2 < mesh.points.size()) {
                auto& p = mesh.points[i / 2];
                if (i % 2 == 0) {
                    p.offset = pool[index];
                } else {
                    p.uv = {pool[index][0], pool[index][1]};
                }
            }
        }
        // Older versions' baby offsets are not used: their baby face is the base face, which a
        // newborn's face is still blended with. Version 3 also leaves out the far-away face of an
        // elder of gender 1.
        const bool elderFar = mesh.age == kElderAge && mesh.gender == 1 && mesh.group == kFarGroup;
        if (shape.version < 5 && mesh.age == kBabyAge) {
            for (auto& p : mesh.points) {
                p.offset = {0, 0, 0};
            }
        }
        if (!(shape.version == 3 && elderFar)) {
            shape.meshes.push_back(std::move(mesh));
        }
    }
    if (r.u32() != kDataTag) {
        throw std::runtime_error("face shape: no data tag");
    }
    return shape;
}

namespace {

constexpr float kSameValue = 0.0001f;
constexpr std::size_t kMaxCount = 0xFFFF;
// Values further from 0 than this, which no face moves or names, are matched against the whole
// pool instead of by grid cell.
constexpr float kGridLimit = 100.0f;

using Value = std::array<float, 3>;

bool sameValue(const Value& a, const Value& b) {
    return std::fabs(a[0] - b[0]) < kSameValue && std::fabs(a[1] - b[1]) < kSameValue && std::fabs(a[2] - b[2]) < kSameValue;
}

// The pool of distinct values. A value within 0.0001 of an entry on every axis lies in a grid cell
// at most two away from the entry's on each axis (one for the distance and one for rounding at a
// cell's edge), so only those cells are searched for the first match.
class Pool {
public:
    std::uint16_t indexOf(const Value& value) {
        const bool onGrid = std::fabs(value[0]) < kGridLimit && std::fabs(value[1]) < kGridLimit && std::fabs(value[2]) < kGridLimit;
        std::size_t first = entries_.size();
        if (onGrid) {
            const auto c = cell(value);
            for (std::int64_t dx = -2; dx <= 2; ++dx) {
                for (std::int64_t dy = -2; dy <= 2; ++dy) {
                    for (std::int64_t dz = -2; dz <= 2; ++dz) {
                        const auto it = cells_.find(key({c[0] + dx, c[1] + dy, c[2] + dz}));
                        if (it == cells_.end()) {
                            continue;
                        }
                        for (const std::size_t i : it->second) {
                            if (i < first && sameValue(entries_[i], value)) {
                                first = i;
                                break;
                            }
                        }
                    }
                }
            }
            for (const std::size_t i : offGrid_) {
                if (i < first && sameValue(entries_[i], value)) {
                    first = i;
                    break;
                }
            }
        } else {
            for (std::size_t i = 0; i < entries_.size(); ++i) {
                if (sameValue(entries_[i], value)) {
                    first = i;
                    break;
                }
            }
        }
        if (first == entries_.size()) {
            if (entries_.size() >= kMaxCount) {
                throw std::runtime_error("face shape: too many distinct points to write");
            }
            entries_.push_back(value);
            if (onGrid) {
                cells_[key(cell(value))].push_back(first);
            } else {
                offGrid_.push_back(first);
            }
        }
        return static_cast<std::uint16_t>(first);
    }

    const std::vector<Value>& entries() const { return entries_; }

private:
    static std::array<std::int64_t, 3> cell(const Value& v) {
        return {static_cast<std::int64_t>(std::floor(v[0] / kSameValue)), static_cast<std::int64_t>(std::floor(v[1] / kSameValue)),
                static_cast<std::int64_t>(std::floor(v[2] / kSameValue))};
    }
    // Cells within the grid limit, and two either side, fit in 21 bits per axis.
    static std::uint64_t key(const std::array<std::int64_t, 3>& c) {
        constexpr std::int64_t kBias = std::int64_t{1} << 20;
        return static_cast<std::uint64_t>(c[0] + kBias) << 42 | static_cast<std::uint64_t>(c[1] + kBias) << 21 |
               static_cast<std::uint64_t>(c[2] + kBias);
    }

    std::vector<Value> entries_;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> cells_;
    std::vector<std::size_t> offGrid_;
};

} // namespace

std::vector<std::uint8_t> write(const FaceShape& shape) {
    Pool pool;
    std::vector<std::vector<std::uint16_t>> lists;
    for (const auto& mesh : shape.meshes) {
        if (mesh.points.size() * 2 > kMaxCount) {
            throw std::runtime_error("face shape: a mesh with too many points to write");
        }
        auto& list = lists.emplace_back();
        for (const auto& p : mesh.points) {
            list.push_back(pool.indexOf(p.offset));
            list.push_back(pool.indexOf({p.uv[0], p.uv[1], 0.0f}));
        }
    }
    BinaryWriter w;
    w.u32(5);
    w.u32(static_cast<std::uint32_t>(shape.meshes.size()));
    for (const auto& mesh : shape.meshes) {
        w.u32(static_cast<std::uint32_t>(mesh.age) | static_cast<std::uint32_t>(mesh.gender) << 8 | static_cast<std::uint32_t>(mesh.group) << 16);
    }
    w.u32(kPoolTag);
    w.u16(static_cast<std::uint16_t>(pool.entries().size()));
    for (const auto& v : pool.entries()) {
        w.f32(v[0]);
        w.f32(v[1]);
        w.f32(v[2]);
    }
    w.u32(kListsTag);
    for (const auto& list : lists) {
        w.u16(static_cast<std::uint16_t>(list.size()));
        for (const std::uint16_t i : list) {
            w.u16(i);
        }
    }
    w.u32(kDataTag);
    return w.take();
}

} // namespace ts2::faceshape
