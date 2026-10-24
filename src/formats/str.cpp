#include "formats/str.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <stdexcept>

namespace ts2::str {

namespace {

constexpr std::size_t kNameSize = 64;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("STR#: " + message);
}

} // namespace

std::vector<const Entry*> StringTable::forLanguage(std::uint8_t language) const {
    std::vector<const Entry*> out;
    for (const auto& e : entries) {
        if (e.language == language) {
            out.push_back(&e);
        }
    }
    return out;
}

StringTable parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    StringTable t;

    const auto nameBytes = r.bytes(kNameSize);
    const auto nameEnd = std::find(nameBytes.begin(), nameBytes.end(), std::uint8_t{0});
    t.name.assign(nameBytes.begin(), nameEnd);

    if (r.remaining() < 2) {
        return t;
    }

    const std::uint16_t raw = r.u16();
    std::size_t count = 0;
    if (raw == 0xFFFF || raw == 0xFFFE || raw == 0xFFFD) {
        t.format = raw;
        count = r.u16();
    } else if ((raw & 0xFF) == 0) {
        t.format = 0x0000;
        count = raw >> 8;
    } else {
        t.format = raw;
        t.supported = false;
        return t;
    }

    if (count > r.remaining()) {
        fail("entry count larger than the data");
    }
    t.entries.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        Entry e;
        switch (t.format) {
        case 0x0000:
            e.value = r.text(r.u8());
            break;
        case 0xFFFF:
            e.value = r.cString();
            break;
        case 0xFFFE:
            e.value = r.cString();
            e.description = r.cString();
            break;
        default:
            e.language = r.u8();
            e.value = r.cString();
            e.description = r.cString();
            break;
        }
        t.entries.push_back(std::move(e));
    }
    return t;
}

} // namespace ts2::str
