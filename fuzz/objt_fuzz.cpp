#include "formats/objt.hpp"
#include "formats/lot_records.hpp"
#include "formats/world_db.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        // What the writer gives reads back to the same values, so it writes the same bytes again.
        const std::vector<std::uint8_t> written = ts2::objt::write(ts2::objt::parse(bytes));
        if (ts2::objt::write(ts2::objt::parse(written)) != written) {
            std::abort();
        }
    } catch (const std::exception&) {
    }
    // A world database record the writer reads writes back to the very same bytes.
    if (const auto record = ts2::worlddb::readRecord(bytes);
        record && ts2::worlddb::writeRecord(*record) != std::vector<std::uint8_t>(bytes.begin(), bytes.end())) {
        std::abort();
    }
    // The small lot records the writers read write back to the very same bytes.
    const std::vector<std::uint8_t> whole(bytes.begin(), bytes.end());
    using namespace ts2::lotrecords;
    if (const auto r = readHouse(bytes); r && writeHouse(*r) != whole) {
        std::abort();
    }
    for (const bool texts : {false, true}) {
        if (const auto r = readPairTable(bytes, texts); r && writePairTable(*r) != whole) {
            std::abort();
        }
    }
    if (const auto r = readWordTable(bytes); r && writeWordTable(*r) != whole) {
        std::abort();
    }
    if (const auto r = readVertexLayer(bytes); r && writeVertexLayer(*r) != whole) {
        std::abort();
    }
    if (const auto r = readFlagRecord(bytes); r && writeFlagRecord(*r) != whole) {
        std::abort();
    }
    if (const auto r = readRoutingAggregate(bytes); r && writeRoutingAggregate(*r) != whole) {
        std::abort();
    }
    if (const auto r = readSgSystem(bytes); r && writeSgSystem(*r) != whole) {
        std::abort();
    }
    if (const auto r = readArchiveCounter(bytes); r && writeArchiveCounter(*r) != whole) {
        std::abort();
    }
    if (const auto r = readDnaIndex(bytes); r && writeDnaIndex(*r) != whole) {
        std::abort();
    }
    if (const auto r = readNeighborhoodInfo(bytes); r && writeNeighborhoodInfo(*r) != whole) {
        std::abort();
    }
    if (const auto r = readShortRecord(bytes); r && writeShortRecord(*r) != whole) {
        std::abort();
    }
    if (const auto r = readKeyedLists(bytes); r && writeKeyedLists(*r) != whole) {
        std::abort();
    }
    return 0;
}
