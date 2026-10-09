#include "formats/ttab.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>
#include <utility>

namespace ts2::ttab {

namespace {

constexpr std::size_t kNameSize = 64;
constexpr std::uint32_t kMaxCount = 0x10000;
// Tables older than this use the original interaction layout with a single motive list.
constexpr std::uint32_t kGroupedMotivesVersion = 0x44;
// From this version the motives are saved as counted lists instead of fixed group counts.
constexpr std::uint32_t kListedMotivesVersion = 0x54;

std::uint32_t count(BinaryReader& r) {
    const std::uint32_t n = r.u32();
    if (n > kMaxCount) {
        throw std::runtime_error("ttab: count too large");
    }
    return n;
}

std::vector<MotiveEffect> readEffects(BinaryReader& r, std::uint32_t n) {
    std::vector<MotiveEffect> effects;
    for (std::uint32_t i = 0; i < n; ++i) {
        MotiveEffect m;
        m.min = r.i16();
        m.delta = r.i16();
        m.type = r.i16();
        effects.push_back(m);
    }
    return effects;
}

void readSettings(BinaryReader& r, Interaction& in) {
    in.flags = r.u32();
    in.stringIndex = r.u32();
    in.attenuationCode = r.u32();
    in.attenuation = r.f32();
    in.autonomy = r.u32();
    in.joiningIndex = r.i32();
}

Interaction readInteraction(BinaryReader& r, std::uint32_t version) {
    Interaction in;
    in.action = r.u16();
    in.test = r.u16();
    if (version < kGroupedMotivesVersion) {
        const std::uint32_t n = count(r);
        readSettings(r, in);
        in.motives.push_back(readEffects(r, n));
        return in;
    }
    std::vector<std::uint32_t> groupSizes;
    if (version < kListedMotivesVersion) {
        const int groups = version < 0x53 ? 7 : 11;
        for (int i = 0; i < groups; ++i) {
            groupSizes.push_back(count(r));
        }
    }
    readSettings(r, in);
    // Older tables saved two flags the other way round.
    if (version < 0x52) {
        in.flags ^= 0x10 | 0x40;
    }
    if (version >= 0x45) in.value1 = r.u16();
    if (version >= 0x46) in.value2 = r.u32();
    if (version > 0x49) in.value3 = r.f32();
    if (version > 0x4B) {
        in.value4 = r.u32();
        in.value5 = r.u32();
    }
    if (version < kListedMotivesVersion) {
        for (const auto n : groupSizes) {
            in.motives.push_back(readEffects(r, n));
        }
    } else {
        for (std::uint32_t g = count(r); g > 0; --g) {
            in.motives.push_back(readEffects(r, count(r)));
        }
        for (std::uint32_t g = count(r); g > 0; --g) {
            auto& group = in.moreMotives.emplace_back();
            for (std::uint32_t k = count(r); k > 0; --k) {
                group.push_back(readEffects(r, count(r)));
            }
        }
    }
    return in;
}

// The table's name at the end: a 32-bit length in newer tables, a one-byte (7-bit encoded)
// length in older ones; some tables of the same version use either.
std::string readTableName(BinaryReader& r) {
    if (r.remaining() >= 4) {
        BinaryReader probe = r;
        if (probe.u32() == r.remaining() - 4) {
            r.u32();
            const auto text = r.bytes(r.remaining());
            return {text.begin(), text.end()};
        }
    }
    std::size_t length = 0;
    for (int shift = 0; shift < 28; shift += 7) {
        const std::uint8_t b = r.u8();
        length |= static_cast<std::size_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) {
            break;
        }
    }
    const auto text = r.bytes(length);
    return {text.begin(), text.end()};
}

} // namespace

InteractionTable parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    InteractionTable t;
    const auto name = r.bytes(kNameSize);
    t.name.assign(name.begin(), name.end());
    t.name.resize(t.name.find('\0') == std::string::npos ? t.name.size() : t.name.find('\0'));

    const std::int16_t first = r.i16();
    const std::int16_t second = r.i16();
    std::uint32_t entries = 0;
    if (first == -1 && second == -1) {
        t.version = r.u32();
        r.u32();
        entries = static_cast<std::uint16_t>(r.i16());
    } else {
        // The original format: a count and a version (8 or later).
        entries = static_cast<std::uint16_t>(first);
        t.version = static_cast<std::uint16_t>(second);
        if (t.version < 8) {
            throw std::runtime_error("ttab: unknown version");
        }
        if (t.version > 8) {
            r.u8();
        }
    }
    for (std::uint32_t i = 0; i < entries; ++i) {
        t.interactions.push_back(readInteraction(r, t.version));
    }
    if (first == -1 && t.version > 0x46 && r.remaining() > 0) {
        t.tableName = readTableName(r);
    }
    t.extraBytes = r.remaining();
    return t;
}

} // namespace ts2::ttab
