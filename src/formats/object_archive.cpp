#include "formats/object_archive.hpp"

#include "formats/binary_reader.hpp"
#include "formats/binary_writer.hpp"

#include <stdexcept>

namespace ts2::archive {

namespace {

// The block of named values written after each part: its version, then the count.
constexpr std::uint32_t kValuesVersion = 1;

// Lists of values may hold lists. The limit guards against broken files, the game sets none.
constexpr int kMaxDepth = 16;

constexpr std::uint8_t kFloat = 0, kSigned = 1, kUnsigned = 2, kBool = 3, kText = 4, kList = 5;

std::string readText(BinaryReader& r) {
    const auto bytes = r.bytes(r.u32());
    return std::string(bytes.begin(), bytes.end());
}

void readPairs(BinaryReader& r, std::vector<Value>& pairs, int depth);

Value readValue(BinaryReader& r, int depth) {
    Value v;
    v.type = r.u8();
    switch (v.type) {
    case kFloat:
    case kSigned:
    case kUnsigned:
    case kBool:
        v.word = r.u32();
        break;
    case kText:
        v.text = readText(r);
        break;
    case kList:
        readPairs(r, v.pairs, depth + 1);
        break;
    default:
        throw std::runtime_error("object archive: a value type the game does not write");
    }
    return v;
}

void readPairs(BinaryReader& r, std::vector<Value>& pairs, int depth) {
    if (depth > kMaxDepth) {
        throw std::runtime_error("object archive: values nested too deep");
    }
    // A pair takes at least two type bytes and two words.
    for (std::size_t n = r.u32Count(10); n > 0; --n) {
        pairs.push_back(readValue(r, depth));
        pairs.push_back(readValue(r, depth));
    }
}

void writePairs(BinaryWriter& w, const std::vector<Value>& pairs, int depth);

void writeValue(BinaryWriter& w, const Value& v, int depth) {
    w.u8(v.type);
    switch (v.type) {
    case kFloat:
    case kSigned:
    case kUnsigned:
        w.u32(v.word);
        break;
    case kBool:
        w.u32(v.word != 0 ? 1 : 0);
        break;
    case kText:
        w.fullText(v.text);
        break;
    case kList:
        writePairs(w, v.pairs, depth + 1);
        break;
    default:
        throw std::runtime_error("object archive: a value type the game does not write");
    }
}

void writePairs(BinaryWriter& w, const std::vector<Value>& pairs, int depth) {
    if (depth > kMaxDepth) {
        throw std::runtime_error("object archive: values nested too deep");
    }
    if (pairs.size() % 2 != 0) {
        throw std::runtime_error("object archive: a key without a value");
    }
    w.count(pairs.size() / 2);
    for (const Value& v : pairs) {
        writeValue(w, v, depth);
    }
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        BinaryReader r(data);
        if (r.u32() != kWrittenVersion || r.u32() != xobj::kWrittenVersion) {
            return std::nullopt;
        }
        Record record;
        record.keys.resize(r.u32Count(12));
        for (Key& k : record.keys) {
            k.type = r.u32();
            k.instance = r.u32();
            k.instanceHigh = r.u32();
        }
        // An object takes at least its GUID in the list and again before its parts.
        record.objects.resize(r.u32Count(8));
        for (Object& o : record.objects) {
            o.guid = r.u32();
        }
        for (Object& o : record.objects) {
            if (r.u32() != o.guid) {
                return std::nullopt;
            }
            o.value = r.u32();
            // A part starts with four zero floats, the scene state with its class tag.
            while (r.remaining() >= 4 && BinaryReader(data.subspan(r.position())).u32() == 0) {
                Part& part = o.parts.emplace_back();
                std::size_t consumed = 0;
                part.state = xobj::parseArchived(data.subspan(r.position()), xobj::RecordKind::Object, consumed);
                r.bytes(consumed);
                if (r.u32() != kValuesVersion) {
                    return std::nullopt;
                }
                // Named values take at least their key, an empty name and an empty list.
                part.values.resize(r.u32Count(12));
                for (NamedValues& named : part.values) {
                    named.key = r.i32();
                    named.name = readText(r);
                    readPairs(r, named.pairs, 1);
                }
            }
            std::size_t consumed = 0;
            auto scene = cobject::readArchived(data.subspan(r.position()), consumed);
            if (o.parts.empty() || !scene) {
                return std::nullopt;
            }
            o.scene = std::move(*scene);
            r.bytes(consumed);
        }
        if (r.remaining() != 0 || writeRecord(record) != std::vector<std::uint8_t>(data.begin(), data.end())) {
            return std::nullopt;
        }
        return record;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    BinaryWriter w;
    w.u32(kWrittenVersion);
    w.u32(xobj::kWrittenVersion);
    w.count(record.keys.size());
    for (const Key& k : record.keys) {
        w.u32(k.type);
        w.u32(k.instance);
        w.u32(k.instanceHigh);
    }
    w.count(record.objects.size());
    for (const Object& o : record.objects) {
        w.u32(o.guid);
    }
    for (const Object& o : record.objects) {
        if (o.parts.empty()) {
            throw std::runtime_error("object archive: an object without parts cannot be written");
        }
        w.u32(o.guid);
        w.u32(o.value);
        for (const Part& part : o.parts) {
            w.bytes(xobj::writeArchived(part.state, xobj::RecordKind::Object));
            w.u32(kValuesVersion);
            w.count(part.values.size());
            for (const NamedValues& named : part.values) {
                w.i32(named.key);
                w.fullText(named.name);
                writePairs(w, named.pairs, 1);
            }
        }
        w.bytes(cobject::writeArchived(o.scene));
    }
    return w.take();
}

} // namespace ts2::archive
