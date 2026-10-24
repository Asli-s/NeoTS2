#include "formats/objf.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::objf {

std::vector<Function> parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    // A name, two zero words and "fJBO", which the game doesn't check: some files leave it 0.
    r.bytes(64 + 12);
    const std::uint32_t count = r.u32();
    if (count > 1024) {
        throw std::runtime_error("OBJf: too many functions");
    }
    std::vector<Function> functions(count);
    for (auto& f : functions) {
        f.check = r.u16();
        f.action = r.u16();
    }
    return functions;
}

} // namespace ts2::objf
