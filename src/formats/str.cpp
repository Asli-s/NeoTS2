#include "formats/str.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

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

namespace {

// Where the game sorts a language among the others.
int languageOrder(std::uint8_t language) {
    return static_cast<std::int8_t>(static_cast<std::uint8_t>(language - 1));
}

void putText(BinaryWriter& w, const std::string& text) {
    if (text.find('\0') != std::string::npos) {
        fail("a text with a zero byte cannot be written");
    }
    for (const char c : text) {
        w.u8(static_cast<std::uint8_t>(c));
    }
    w.u8(0);
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        const StringTable table = parse(data);
        Record record{table.name, table.entries};
        if (!table.supported || table.format != kWrittenFormat ||
            writeRecord(record) != std::vector<std::uint8_t>(data.begin(), data.end())) {
            return std::nullopt;
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    if (record.name.size() > kNameSize || record.name.find('\0') != std::string::npos) {
        fail("a name the game cannot write");
    }
    if (record.entries.size() > 0xFFFF) {
        fail("too many entries to write");
    }
    BinaryWriter w;
    for (const char c : record.name) {
        w.u8(static_cast<std::uint8_t>(c));
    }
    w.zeros(kNameSize - record.name.size());
    w.u16(kWrittenFormat);
    w.u16(static_cast<std::uint16_t>(record.entries.size()));
    for (std::size_t i = 0; i < record.entries.size(); ++i) {
        const Entry& e = record.entries[i];
        if (i > 0 && languageOrder(e.language) < languageOrder(record.entries[i - 1].language)) {
            fail("entries out of the game's language order");
        }
        w.u8(e.language);
        putText(w, e.value);
        putText(w, e.description);
    }
    return w.take();
}

} // namespace ts2::str
