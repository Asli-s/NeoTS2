#include "formats/face_shape.hpp"

#include "formats/binary_reader.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

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
        // Older versions' baby offsets are not used: their baby face is the base face. Version 3
        // also leaves out the far-away face of an elder of gender 1.
        const bool elderFar = mesh.age == kElderAge && mesh.gender == 1 && mesh.group == kFarGroup;
        const bool kept = shape.version >= 5 || (mesh.age != kBabyAge && !(shape.version == 3 && elderFar));
        if (kept) {
            shape.meshes.push_back(std::move(mesh));
        }
    }
    if (r.u32() != kDataTag) {
        throw std::runtime_error("face shape: no data tag");
    }
    return shape;
}

} // namespace ts2::faceshape
