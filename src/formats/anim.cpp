#include "formats/anim.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace ts2::anim {

namespace {

// Where a pointer was in the game's memory image.
constexpr std::uint32_t kPointer = 0x11BA05F0;

void pointer(BinaryReader& r) {
    if (r.u32() != kPointer) {
        throw std::runtime_error("animation: layout not as expected");
    }
}

std::string text(BinaryReader& r, std::size_t length) {
    const auto bytes = r.bytes(length);
    std::string s(bytes.begin(), bytes.end());
    return s.substr(0, s.find('\0'));
}

// A stored slope: sign, a 5-bit exponent and a 10-bit mantissa, times 2^-31.
float slope(std::uint16_t bits) {
    const float v = std::ldexp(static_cast<float>(bits & 0x3FF), static_cast<int>((bits >> 10) & 0x1F) - 31);
    return (bits & 0x8000) != 0 ? -v : v;
}

struct Component {
    std::uint16_t count = 0;
    std::uint8_t bits = 0;
};

// One component's keys. Bits 0-1: one value per frame (0), keys with one slope (1) or with an in
// and an out slope (2); bits 2-4: values as 16-bit integers (formats 0, 1 and 3, each its own
// scale) or floats (4). Packed keys hold 15 bits of time and 17 of value in 32 bits.
Curve readCurve(BinaryReader& r, const Component& c) {
    const int curve = c.bits & 3, format = (c.bits >> 2) & 7;
    float frameScale = 0, packedScale = 0;
    switch (format) {
    case 0: frameScale = 1.0f / 128, packedScale = 1.0f / 128; break;
    case 1: frameScale = 1.0f / 1024, packedScale = 1.0f / 2048; break;
    case 3: frameScale = 1.0f / 4096, packedScale = 1.0f / 8192; break;
    case 4: break;
    default: throw std::runtime_error("animation: unknown key format");
    }
    if (curve == 3) {
        throw std::runtime_error("animation: unknown curve");
    }
    std::vector<Key> keys(c.count);
    std::vector<std::uint16_t> inBits(c.count), outBits(c.count);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        Key& k = keys[i];
        if (curve == 0) {
            // One value per frame, 30 frames a second.
            k.time = std::round(static_cast<float>(i) * (100.0f / 3.0f) + 1.0f / 6.0f);
            k.value = format == 4 ? std::bit_cast<float>(r.u32()) : static_cast<float>(r.i16()) * frameScale;
            continue;
        }
        if (format == 4) {
            k.time = r.u16();
            k.value = std::bit_cast<float>(r.u32());
        } else {
            const auto packed = static_cast<std::int32_t>(r.u32());
            k.time = static_cast<float>(packed & 0x7FFF);
            k.value = static_cast<float>(packed >> 15) * packedScale;
        }
        inBits[i] = r.u16();
        outBits[i] = curve == 2 ? r.u16() : inBits[i];
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        Key& k = keys[i];
        if (curve == 0) {
            k.in = i > 0 ? k.value - keys[i - 1].value : 0;
            k.out = i + 1 < keys.size() ? keys[i + 1].value - k.value : 0;
        } else {
            k.in = i > 0 ? slope(inBits[i]) * (k.time - keys[i - 1].time) : 0;
            k.out = i + 1 < keys.size() ? slope(outBits[i]) * (keys[i + 1].time - k.time) : 0;
        }
    }
    return Curve{std::move(keys)};
}

} // namespace

float Curve::sample(float ms) const {
    if (keys.empty()) {
        return 0;
    }
    if (ms <= keys.front().time || keys.size() == 1) {
        return keys.front().value;
    }
    if (ms >= keys.back().time) {
        return keys.back().value;
    }
    const auto next = std::upper_bound(keys.begin(), keys.end(), ms, [](float t, const Key& k) { return t < k.time; });
    const Key& a = *(next - 1);
    const Key& b = *next;
    const float s = (ms - a.time) / std::max(b.time - a.time, 1e-6f), s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * a.value + (s3 - 2 * s2 + s) * a.out + (-2 * s3 + 3 * s2) * b.value + (s3 - s2) * b.in;
}

Animation parse(std::span<const std::uint8_t> data) {
    // The resource block names it (cSGResource: id, version, then the name with a one-byte
    // length); the memory image follows, after its size.
    const std::string_view all(reinterpret_cast<const char*>(data.data()), data.size());
    const auto block = all.find("cSGResource");
    if (block == std::string_view::npos) {
        throw std::runtime_error("animation: no name");
    }
    BinaryReader r(data);
    r.bytes(block + 11 + 8);
    Animation a;
    a.name = text(r, r.u8());
    r.u32();  // the image's size
    r.u16();
    const std::uint16_t targets = r.u16();
    r.u16();  // events
    const std::uint8_t userDataLength = r.u8();
    r.u8();
    const std::uint32_t header = r.u32();
    const std::uint32_t skeletonLength = header >> 24;
    for (int i = 0; i < 4; ++i) {
        pointer(r);
    }
    r.bytes(9 * 4);
    a.skeleton = text(r, skeletonLength + 1u);
    text(r, userDataLength + 1u);
    r.bytes((skeletonLength + userDataLength + 2u) % 4);

    // Targets (skeletons), then all their channels, the channels' names, their components and the
    // components' keys, each list in turn.
    struct Target {
        std::uint16_t channels = 0, nameLength = 0;
        std::uint8_t chains = 0;
    };
    std::vector<Target> ts(targets);
    for (auto& t : ts) {
        pointer(r);
        pointer(r);
        r.u16();
        t.channels = r.u16();
        t.chains = r.u8();
        r.u8();
        t.nameLength = r.u16();
        pointer(r);
        pointer(r);
        pointer(r);
    }
    std::size_t total = 0;
    for (const auto& t : ts) {
        text(r, t.nameLength + 1u);
        total += t.nameLength + 1u;
    }
    r.bytes(total % 4);
    std::vector<std::vector<Channel>> channels(ts.size());
    for (std::size_t t = 0; t < ts.size(); ++t) {
        channels[t].resize(ts[t].channels);
        for (auto& c : channels[t]) {
            pointer(r);
            pointer(r);
            r.u32();  // name hash
            pointer(r);
            c.flags = r.u32();
            pointer(r);
        }
    }
    total = 0;
    for (auto& list : channels) {
        for (auto& c : list) {
            for (std::uint8_t ch = r.u8(); ch != 0; ch = r.u8()) {
                c.name += static_cast<char>(ch);
                ++total;
            }
            ++total;
        }
    }
    r.bytes(total % 4);
    std::vector<std::vector<std::vector<Component>>> components(ts.size());
    for (std::size_t t = 0; t < ts.size(); ++t) {
        for (const auto& c : channels[t]) {
            auto& list = components[t].emplace_back(c.flags >> 29);
            for (auto& comp : list) {
                comp.count = r.u16();
                comp.bits = r.u8();
                r.u8();
                pointer(r);
            }
        }
    }
    for (std::size_t t = 0; t < ts.size(); ++t) {
        for (std::size_t c = 0; c < channels[t].size(); ++c) {
            for (const auto& comp : components[t][c]) {
                channels[t][c].components.push_back(readCurve(r, comp));
            }
        }
    }
    // Then the targets' IK chains, and each chain's targets in turn.
    std::vector<std::vector<IkChain>> chains(ts.size());
    for (std::size_t t = 0; t < ts.size(); ++t) {
        for (std::uint8_t i = 0; i < ts[t].chains; ++i) {
            IkChain& c = chains[t].emplace_back();
            pointer(r);
            pointer(r);
            r.u16();
            c.targets.resize(r.u16());
            auto name = [&r] {
                const std::uint32_t v = r.u32();
                r.u8();
                return v;
            };
            c.name = name();
            c.mirror = name();
            c.strategy = r.u32();
            c.begin = name();
            c.beginMirror = name();
            c.end = name();
            c.endMirror = name();
            c.pole = name();
            c.poleFrame = name();
            c.poleFrameMirror = name();
            c.extra = name();
            pointer(r);
        }
    }
    for (auto& list : chains) {
        for (auto& c : list) {
            for (auto& target : c.targets) {
                pointer(r);
                pointer(r);
                r.u16();
                r.u16();
                target.kind = r.u8();
                std::uint32_t v[6];
                for (auto& x : v) {
                    x = r.u32();
                    r.u8();
                }
                target.frame = v[0];
                target.frameMirror = v[1];
                target.node = v[2];
                target.rotationNode = v[3];
                target.weight = v[5];
            }
        }
    }
    if (!channels.empty()) {
        a.channels = std::move(channels[0]);
        a.chains = std::move(chains[0]);
    }
    for (const auto& c : a.channels) {
        for (const auto& curve : c.components) {
            if (!curve.keys.empty()) {
                a.duration = std::max(a.duration, curve.keys.back().time);
            }
        }
    }
    return a;
}

} // namespace ts2::anim
