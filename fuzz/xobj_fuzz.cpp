#include "formats/object_archive.hpp"
#include "formats/xobj.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <optional>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size < 2) {
        return 0;
    }
    const auto kind = static_cast<ts2::xobj::RecordKind>(data[1] % 3);
    const std::span<const std::uint8_t> record(data + 2, size - 2);
    try {
        ts2::xobj::parse(record, data[0], kind);
    } catch (const std::exception&) {
    }
    // A record of the version the game writes, once read, writes back to the very same bytes.
    // Writing a record that read must never fail, and what is written reads and writes back to the
    // same bytes. (Input bytes can differ: a text length padded to more bytes is written the shortest
    // way, as the game writes it.)
    std::optional<ts2::xobj::SavedObject> object;
    try {
        object = ts2::xobj::parse(record, ts2::xobj::kWrittenVersion, kind);
    } catch (const std::exception&) {
    }
    if (object) {
        try {
            const std::vector<std::uint8_t> written = ts2::xobj::write(*object, kind);
            if (ts2::xobj::write(ts2::xobj::parse(written, ts2::xobj::kWrittenVersion, kind), kind) != written) {
                std::abort();
            }
        } catch (const std::exception&) {
            std::abort();
        }
    }
    try {
        std::size_t consumed = 0;
        ts2::xobj::parseEmbedded(record, data[0], kind, consumed);
        if (consumed > record.size()) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    // An object archive the reader takes writes back to the very same bytes.
    const std::span<const std::uint8_t> all(data, size);
    if (const auto archive = ts2::archive::readRecord(all);
        archive && ts2::archive::writeRecord(*archive) != std::vector<std::uint8_t>(all.begin(), all.end())) {
        std::abort();
    }
    return 0;
}
