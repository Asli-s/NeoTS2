#include "formats/famt.hpp"

#include "formats/binary_reader.hpp"

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

} // namespace ts2::famt
