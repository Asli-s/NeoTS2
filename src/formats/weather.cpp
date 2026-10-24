#include "formats/weather.hpp"

#include "formats/binary_reader.hpp"

namespace ts2::weather {

Weather parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    Weather w;
    const std::uint32_t version = r.u32();
    if (version == 0) {
        return w;
    }
    if (version > 2) {
        r.skip(r.u32());
    }
    for (int i = 0; i < Weather::kPondFrozenState; ++i) {
        w.values[static_cast<std::size_t>(i)] = r.i32();
    }
    w.values[Weather::kPondFrozenState] = version > 1 ? r.i32() : 0;
    return w;
}

} // namespace ts2::weather
