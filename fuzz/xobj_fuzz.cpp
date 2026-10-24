#include "formats/xobj.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

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
    try {
        std::size_t consumed = 0;
        ts2::xobj::parseEmbedded(record, data[0], kind, consumed);
        if (consumed > record.size()) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    return 0;
}
