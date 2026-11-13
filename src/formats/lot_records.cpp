#include "formats/lot_records.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ts2::lotrecords {

namespace {

constexpr std::size_t kNameSize = 64;

// Any part that does not match what the game writes.
struct NotWritten {};

class Reader {
public:
    // A reader for records with no zero bytes first.
    struct NoName {};
    Reader(std::span<const std::uint8_t> data, NoName) : r_(data) {}

    explicit Reader(std::span<const std::uint8_t> data) : r_(data) {
        const auto name = r_.bytes(kNameSize);
        if (std::any_of(name.begin(), name.end(), [](std::uint8_t b) { return b != 0; })) {
            throw NotWritten{};
        }
    }

    Word word() { return r_.u32(); }
    std::uint16_t u16() { return r_.u16(); }
    bool atEnd() const { return r_.remaining() == 0; }
    std::vector<std::uint8_t> rest() {
        const auto bytes = r_.bytes(r_.remaining());
        return {bytes.begin(), bytes.end()};
    }
    std::int32_t i32() { return r_.i32(); }

    void expect(Word value) {
        if (r_.u32() != value) {
            throw NotWritten{};
        }
    }

    bool flag() {
        const std::int32_t v = r_.i32();
        if (v != 0 && v != 1) {
            throw NotWritten{};
        }
        return v == 1;
    }

    std::size_t count(std::size_t minBytes) { return r_.u32Count(minBytes); }
    std::uint8_t u8() { return r_.u8(); }

    // Text with a 32-bit length, as the neighborhood package writes text.
    std::string fullText() {
        const auto bytes = r_.bytes(r_.u32());
        return {bytes.begin(), bytes.end()};
    }

    // The game writes a length in as few 7-bit groups as it needs, so a longer one would not write back.
    std::string text() {
        const std::size_t start = r_.position();
        std::string t = r_.varString();
        std::size_t n = 1;
        for (std::size_t length = t.size(); length >= 0x80; length >>= 7) {
            ++n;
        }
        if (r_.position() - start != n + t.size()) {
            throw NotWritten{};
        }
        return t;
    }

    void end() {
        if (r_.remaining() != 0) {
            throw NotWritten{};
        }
    }

private:
    BinaryReader r_;
};

template <typename Read>
auto attempt(std::span<const std::uint8_t> data, Read read) -> std::optional<decltype(read(std::declval<Reader&>()))> {
    try {
        Reader r(data);
        auto value = read(r);
        r.end();
        return value;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

BinaryWriter start(std::uint32_t type, std::uint32_t version) {
    BinaryWriter w;
    w.zeros(kNameSize);
    w.u32(type);
    w.u32(version);
    return w;
}

constexpr std::uint32_t kHouseVersion = 0x55;
constexpr std::uint32_t kTableVersion = 1;
constexpr std::uint32_t kFlagRecordVersion = 3;
constexpr const char* kVertexLayerName = "cVertexLayer";

} // namespace

std::optional<House> readHouse(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        r.expect(kHouseType);
        r.expect(kHouseVersion);
        House h;
        h.value = r.word();
        h.text = r.text();
        return h;
    });
}

std::vector<std::uint8_t> writeHouse(const House& house) {
    BinaryWriter w = start(kHouseType, kHouseVersion);
    w.u32(house.value);
    w.text(house.text);
    return w.take();
}

std::optional<PairTable> readPairTable(std::span<const std::uint8_t> data, bool texts) {
    return attempt(data, [texts](Reader& r) {
        r.expect(kPairTableType);
        r.expect(kTableVersion);
        PairTable t;
        t.texts = texts;
        t.value = r.word();
        for (std::size_t n = r.count(texts ? 2 : 8); n > 0; --n) {
            if (texts) {
                std::string first = r.text();
                t.textPairs.emplace_back(std::move(first), r.text());
            } else {
                const Word first = r.word();
                t.numbers.emplace_back(first, r.word());
            }
        }
        return t;
    });
}

std::vector<std::uint8_t> writePairTable(const PairTable& table) {
    BinaryWriter w = start(kPairTableType, kTableVersion);
    w.u32(table.value);
    if (table.texts) {
        w.count(table.textPairs.size());
        for (const auto& [first, second] : table.textPairs) {
            w.text(first);
            w.text(second);
        }
    } else {
        w.count(table.numbers.size());
        for (const auto& [first, second] : table.numbers) {
            w.u32(first);
            w.u32(second);
        }
    }
    return w.take();
}

std::size_t entryWords(std::uint32_t type) {
    return type == kNineWordTableType ? 9 : type == kFourWordTableType ? 4 : 0;
}

std::optional<WordTable> readWordTable(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        WordTable t;
        t.type = r.word();
        const std::size_t width = entryWords(t.type);
        if (width == 0) {
            throw NotWritten{};
        }
        r.expect(kTableVersion);
        for (std::size_t n = r.count(4 * width); n > 0; --n) {
            for (std::size_t i = 0; i < width; ++i) {
                t.words.push_back(r.word());
            }
        }
        return t;
    });
}

std::vector<std::uint8_t> writeWordTable(const WordTable& table) {
    const std::size_t width = entryWords(table.type);
    if (width == 0 || table.words.size() % width != 0) {
        throw std::runtime_error("lot record: not a table of whole entries");
    }
    BinaryWriter w = start(table.type, kTableVersion);
    w.count(table.words.size() / width);
    for (const Word v : table.words) {
        w.u32(v);
    }
    return w.take();
}

std::optional<std::vector<Vertex>> readVertexLayer(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        r.expect(kVertexLayerType);
        r.expect(kTableVersion);
        if (r.text() != kVertexLayerName) {
            throw NotWritten{};
        }
        std::vector<Vertex> vertices(r.count(16));
        for (Vertex& v : vertices) {
            v.level = r.i32();
            v.x = r.word();
            v.y = r.word();
            v.value = r.word();
        }
        return vertices;
    });
}

std::vector<std::uint8_t> writeVertexLayer(const std::vector<Vertex>& vertices) {
    BinaryWriter w;
    w.recordHeader(kVertexLayerType, kTableVersion, kVertexLayerName);
    w.count(vertices.size());
    for (const Vertex& v : vertices) {
        w.i32(v.level);
        w.u32(v.x);
        w.u32(v.y);
        w.u32(v.value);
    }
    return w.take();
}

std::optional<FlagRecord> readFlagRecord(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        r.expect(kFlagRecordVersion);
        FlagRecord f;
        f.flag = r.flag();
        f.first = r.i32();
        f.second = r.i32();
        return f;
    });
}

std::vector<std::uint8_t> writeFlagRecord(const FlagRecord& record) {
    BinaryWriter w;
    w.zeros(kNameSize);
    w.u32(kFlagRecordVersion);
    w.i32(record.flag ? 1 : 0);
    w.i32(record.first);
    w.i32(record.second);
    return w.take();
}

namespace {
constexpr std::uint32_t kRoutingAggregateVersion = 6;
constexpr const char* kRoutingAggregateName = "cTSRoutingObjectAggregate";
constexpr std::uint32_t kSgSystemVersion = 4;
constexpr const char* kSgSystemName = "cTSSGSystem";
} // namespace

std::optional<RoutingAggregate> readRoutingAggregate(std::span<const std::uint8_t> data) {
    try {
        Reader r(data);
        r.expect(kRoutingAggregateType);
        r.expect(kRoutingAggregateVersion);
        if (r.text() != kRoutingAggregateName) {
            return std::nullopt;
        }
        RoutingAggregate a;
        a.packed = r.word();
        a.value = r.word();
        if (r.flag()) {
            a.extra.emplace();
            for (std::uint16_t& v : *a.extra) {
                v = r.u16();
            }
        }
        for (std::size_t n = r.count(2); n > 0; --n) {
            a.list.push_back(r.u16());
        }
        a.objects = r.i32();
        a.objectBytes = r.rest();
        // An object writes at least its first number, and with one object its bytes run to the end.
        if (a.objects < 0 || a.objects > 1 || (a.objects == 0) != a.objectBytes.empty()) {
            return std::nullopt;
        }
        return a;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRoutingAggregate(const RoutingAggregate& aggregate) {
    if (aggregate.objects < 0 || aggregate.objects > 1 || (aggregate.objects == 0) != aggregate.objectBytes.empty()) {
        throw std::runtime_error("routing aggregate: objects that cannot be written");
    }
    BinaryWriter w;
    w.recordHeader(kRoutingAggregateType, kRoutingAggregateVersion, kRoutingAggregateName);
    w.u32(aggregate.packed);
    w.u32(aggregate.value);
    w.i32(aggregate.extra ? 1 : 0);
    if (aggregate.extra) {
        for (const std::uint16_t v : *aggregate.extra) {
            w.u16(v);
        }
    }
    w.count(aggregate.list.size());
    for (const std::uint16_t v : aggregate.list) {
        w.u16(v);
    }
    w.i32(aggregate.objects);
    w.bytes(aggregate.objectBytes);
    return w.take();
}

std::optional<SgSystem> readSgSystem(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        r.expect(kSgSystemType);
        r.expect(kSgSystemVersion);
        if (r.text() != kSgSystemName) {
            throw NotWritten{};
        }
        SgSystem s;
        for (Word& v : s.values) {
            v = r.word();
        }
        // The pairs run to the end, one per item of the game's list.
        while (!r.atEnd()) {
            const Word first = r.word();
            s.pairs.emplace_back(first, r.word());
        }
        return s;
    });
}

std::vector<std::uint8_t> writeSgSystem(const SgSystem& system) {
    BinaryWriter w;
    w.recordHeader(kSgSystemType, kSgSystemVersion, kSgSystemName);
    for (const Word v : system.values) {
        w.u32(v);
    }
    for (const auto& [first, second] : system.pairs) {
        w.u32(first);
        w.u32(second);
    }
    return w.take();
}

namespace {
constexpr std::uint32_t kArchiveCounterVersion = 4;
} // namespace

std::optional<std::uint32_t> readArchiveCounter(std::span<const std::uint8_t> data) {
    if (data.size() != 8) {
        return std::nullopt;
    }
    BinaryReader r(data);
    if (r.u32() != kArchiveCounterVersion) {
        return std::nullopt;
    }
    return r.u32();
}

std::vector<std::uint8_t> writeArchiveCounter(std::uint32_t lastArchive) {
    BinaryWriter w;
    w.u32(kArchiveCounterVersion);
    w.u32(lastArchive);
    return w.take();
}

namespace {
constexpr std::uint32_t kDnaIndexVersion = 1;

bool ascending(const std::vector<std::uint32_t>& instances) {
    for (std::size_t i = 1; i < instances.size(); ++i) {
        if (instances[i] <= instances[i - 1]) {
            return false;
        }
    }
    return true;
}
} // namespace

std::optional<std::vector<std::uint32_t>> readDnaIndex(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kDnaIndexVersion) {
            return std::nullopt;
        }
        std::vector<std::uint32_t> instances(r.u32Count(4));
        for (std::uint32_t& i : instances) {
            i = r.u32();
        }
        if (r.remaining() != 0 || !ascending(instances)) {
            return std::nullopt;
        }
        return instances;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeDnaIndex(const std::vector<std::uint32_t>& instances) {
    if (!ascending(instances)) {
        throw std::runtime_error("DNA index: instances not in ascending order");
    }
    BinaryWriter w;
    w.u32(kDnaIndexVersion);
    w.count(instances.size());
    for (const std::uint32_t i : instances) {
        w.u32(i);
    }
    return w.take();
}

namespace {
constexpr std::uint32_t kShortRecordVersion = 0xCB;
constexpr std::uint32_t kKeyedListsVersion = 0x55;
} // namespace

std::optional<std::uint16_t> readShortRecord(std::span<const std::uint8_t> data) {
    if (data.size() != 14) {
        return std::nullopt;
    }
    BinaryReader r(data);
    if (r.u32() != kShortRecordType || r.u32() != kShortRecordVersion || r.u32() != 0) {
        return std::nullopt;
    }
    return r.u16();
}

std::vector<std::uint8_t> writeShortRecord(std::uint16_t value) {
    BinaryWriter w;
    w.u32(kShortRecordType);
    w.u32(kShortRecordVersion);
    w.u32(0);
    w.u16(value);
    return w.take();
}

std::optional<KeyedLists> readKeyedLists(std::span<const std::uint8_t> data) {
    return attempt(data, [](Reader& r) {
        r.expect(kKeyedListsType);
        r.expect(kKeyedListsVersion);
        r.expect(0);
        KeyedLists k;
        for (std::size_t n = r.count(8); n > 0; --n) {
            const Word key = r.word();
            std::vector<std::uint16_t> list(r.count(2));
            for (std::uint16_t& v : list) {
                v = r.u16();
            }
            k.lists.emplace_back(key, std::move(list));
        }
        return k;
    });
}

std::vector<std::uint8_t> writeKeyedLists(const KeyedLists& lists) {
    BinaryWriter w = start(kKeyedListsType, kKeyedListsVersion);
    w.u32(0);
    w.count(lists.lists.size());
    for (const auto& [key, list] : lists.lists) {
        w.u32(key);
        w.count(list.size());
        for (const std::uint16_t v : list) {
            w.u16(v);
        }
    }
    return w.take();
}

namespace {
constexpr std::uint32_t kNeighborhoodInfoVersion = 10;
} // namespace

std::optional<NeighborhoodInfo> readNeighborhoodInfo(std::span<const std::uint8_t> data) {
    try {
        Reader r(data, Reader::NoName{});
        r.expect(kNeighborhoodInfoVersion);
        NeighborhoodInfo n;
        n.id = r.fullText();
        n.value1 = r.i32();
        n.value2 = r.word();
        n.linked = r.fullText();
        n.value3 = r.word();
        n.products = r.word();
        n.value4 = r.word();
        n.value5 = r.word();
        for (std::uint8_t& b : n.bytes) {
            b = r.u8();
        }
        r.end();
        return n;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeNeighborhoodInfo(const NeighborhoodInfo& info) {
    BinaryWriter w;
    w.u32(kNeighborhoodInfoVersion);
    w.fullText(info.id);
    w.i32(info.value1);
    w.u32(info.value2);
    w.fullText(info.linked);
    w.u32(info.value3);
    w.u32(info.products);
    w.u32(info.value4);
    w.u32(info.value5);
    for (const std::uint8_t b : info.bytes) {
        w.u8(b);
    }
    return w.take();
}

} // namespace ts2::lotrecords
