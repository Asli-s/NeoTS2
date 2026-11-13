#include "formats/famt.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <stdexcept>
#include <utility>

namespace ts2::famt {

namespace {
constexpr std::uint32_t kMaxCount = 0x8000;  // neighbor ids are 16-bit
}

FamilyTies parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    FamilyTies f;
    f.version = r.u32();
    const std::uint32_t count = r.u32();
    if (count > kMaxCount) {
        throw std::runtime_error("famt: too many Sims");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        SimTies s;
        s.sim = r.i16();
        s.value = r.u32();
        const std::uint32_t ties = r.u32();
        if (ties > kMaxCount) {
            throw std::runtime_error("famt: too many ties");
        }
        for (std::uint32_t t = 0; t < ties; ++t) {
            Tie tie;
            tie.type = r.u32();
            tie.other = r.i16();
            s.ties.push_back(tie);
        }
        f.sims.push_back(std::move(s));
    }
    f.extraBytes = r.remaining();
    return f;
}

std::vector<std::uint8_t> write(const FamilyTies& ties) {
    BinaryWriter w;
    w.u32(kWrittenVersion);
    if (ties.sims.size() > kMaxCount) {
        throw std::runtime_error("FAMT: more Sims than a family ties record can hold");
    }
    w.count(ties.sims.size());
    for (const SimTies& s : ties.sims) {
        w.u16(static_cast<std::uint16_t>(s.sim));
        w.u32(s.value);
        if (s.ties.size() > kMaxCount) {
            throw std::runtime_error("FAMT: more ties than a family ties record can hold");
        }
        w.count(s.ties.size());
        for (const Tie& t : s.ties) {
            w.u32(t.type);
            w.u16(static_cast<std::uint16_t>(t.other));
        }
    }
    return w.take();
}

} // namespace ts2::famt
