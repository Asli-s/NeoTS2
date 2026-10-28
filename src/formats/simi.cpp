#include "formats/simi.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::simi {

SimulatorState parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.bytes(64);
    if (r.u32() != 0x53494D49) {  // "IMIS"
        throw std::runtime_error("SIMI: no magic");
    }
    SimulatorState s;
    s.version = r.u32();
    r.u32();
    s.globals.resize(kGlobalCount);
    for (auto& g : s.globals) {
        g = r.i16();
    }
    // Versions before 0xD4 save fewer globals, so the fields after them sit elsewhere.
    constexpr std::uint32_t kSixtyGlobals = 0xD4;
    constexpr std::size_t kBeforeRandomState = 4 + 2 + 4;
    if (s.version >= kSixtyGlobals && r.remaining() >= kBeforeRandomState + 4) {
        r.skip(kBeforeRandomState);
        s.randomState = r.u32();
    }
    return s;
}

} // namespace ts2::simi
