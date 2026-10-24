#include "formats/neighborhood_id.hpp"

#include "formats/binary_reader.hpp"

namespace ts2::nhoodid {

namespace {

std::string lengthText(BinaryReader& r) {
    return r.text(r.u32Count(1));
}

} // namespace

NeighborhoodId parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    NeighborhoodId n;
    n.version = r.u32();
    if (n.version < 2) {
        lengthText(r);
    }
    n.prefix = lengthText(r);
    n.id = r.i32();
    if (n.version > 3) {
        n.type = r.i32();
    }
    if (n.version > 4) {
        n.subPrefix = lengthText(r);
    }
    if (n.version >= 6) {
        n.value1 = r.i32();
    } else if (n.type == 3) {
        n.value1 = 1;
    }
    if (n.version > 6) {
        n.requiredProducts = r.u32();
    }
    if (n.version >= 8) {
        n.value2 = r.i32();
    } else {
        switch (n.type) {
        case 2: n.value2 = 1; break;
        case 3: n.value2 = 2; break;
        case 5:
        case 6:
        case 7: n.value2 = 10; break;
        default: n.value2 = 0; break;
        }
    }
    if (n.version > 8) {
        n.flags = r.i32();
    }
    if (n.version > 9) {
        for (auto& s : n.seasons) {
            s = static_cast<std::int8_t>(r.u8());
        }
    }
    return n;
}

} // namespace ts2::nhoodid
