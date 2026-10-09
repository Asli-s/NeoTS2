#include "formats/xobj.hpp"

#include "formats/binary_reader.hpp"

#include <stdexcept>

namespace ts2::xobj {

namespace {

constexpr std::uint32_t kFirstDecodedTailVersion = 0x93;
constexpr std::uint32_t kPersonMarker = 0xDEADBEEF;
constexpr std::size_t kTimerEntrySize = 38;
constexpr std::size_t kMaterialBlockSize = 26;

struct Step {
    std::uint32_t below;
    std::size_t count;
};

template <std::size_t N>
std::size_t countFor(const Step (&steps)[N], std::uint32_t version, std::size_t last) {
    for (const auto& step : steps) {
        if (version < step.below) {
            return step.count;
        }
    }
    return last;
}

// Every element takes at least `minBytes`, so a count larger than this cannot be real.
std::size_t checkedCount(BinaryReader& r, std::int64_t count, std::size_t minBytes) {
    if (count < 0 || static_cast<std::uint64_t>(count) * minBytes > r.remaining()) {
        throw std::runtime_error("XOBJ: bad count");
    }
    return static_cast<std::size_t>(count);
}

std::vector<std::int16_t> readShorts(BinaryReader& r, std::size_t count) {
    std::vector<std::int16_t> values(count);
    for (auto& v : values) {
        v = r.i16();
    }
    return values;
}

std::vector<std::int16_t> readCountedShorts(BinaryReader& r) {
    return readShorts(r, checkedCount(r, r.i16(), 2));
}

// Length as 7-bit groups, low group first; the top bit of each byte says another byte follows.
std::string readString(BinaryReader& r) {
    std::size_t length = 0;
    for (int shift = 0;; shift += 7) {
        if (shift > 28) {
            throw std::runtime_error("XOBJ: bad string length");
        }
        const std::uint8_t byte = r.u8();
        length |= static_cast<std::size_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            break;
        }
    }
    const auto bytes = r.bytes(length);
    return {bytes.begin(), bytes.end()};
}

void readHeader(BinaryReader& r, std::uint32_t version, Header& h) {
    if (version < 0x67) {
        for (auto& f : h.reserved) {
            f = static_cast<float>(r.i32());
        }
        for (auto& f : h.location) {
            f = static_cast<float>(r.i32());
        }
    } else {
        for (auto& f : h.reserved) {
            f = r.f32();
        }
        for (auto& f : h.location) {
            f = r.f32();
        }
    }
    if (version > 0x18) {
        h.value1 = r.i32();
    }
    if (version < 0x2F) {
        r.i16();
    }
    h.value2 = r.i16();
    if (version == 0x37) {
        r.i32();
    } else if (version > 0x68) {
        h.value3 = r.f32();
    }
    if (version >= 0x74) {
        h.value4 = r.i32();
    }
    if (version >= 0x86) {
        h.value5 = r.i16();
    }
    if (version > 0xD6) {
        h.value6 = r.f32();
    }
}

void copyData(SavedObject& o, const std::vector<std::int16_t>& values) {
    for (std::size_t i = 0; i < values.size() && i < kDataCount; ++i) {
        o.data[i] = values[i];
    }
}

StackFrame readFrame(BinaryReader& r, std::uint32_t version) {
    StackFrame f;
    f.objectId = r.i16();
    f.treeId = r.i16();
    f.node = r.u16();
    const std::uint8_t localCount = r.u8();
    const std::uint8_t paramCount = r.u8();
    if (version >= 0xA5) {
        f.value1 = r.i16();
        f.value2 = r.i16();
    }
    f.params = readShorts(r, paramCount);
    f.locals = readShorts(r, localCount);
    f.value3 = r.i32();
    f.codeOwner = r.i16();
    return f;
}

Slots readSlots(BinaryReader& r) {
    Slots s;
    s.marker = r.i16();
    if (s.marker < 0) {
        if (s.marker == -100) {
            s.extra = r.i16();
        }
        s.values = readCountedShorts(r);
    } else if (s.marker > 0) {
        s.values = readShorts(r, checkedCount(r, (s.marker) * 2, 2));
    }
    return s;
}

Tail readTail(BinaryReader& r, std::uint32_t version, RecordKind kind) {
    Tail t;
    t.token = readToken(r, version);

    const std::size_t arrayCount = checkedCount(r, r.i16(), 2);
    for (std::size_t i = 0; i < arrayCount; ++i) {
        t.arrays.push_back(readCountedShorts(r));
    }
    t.shorts = readCountedShorts(r);
    t.value1 = r.i32();
    const std::size_t stringCount = checkedCount(r, r.i16(), 1);
    for (std::size_t i = 0; i < stringCount; ++i) {
        t.strings.push_back(readString(r));
    }
    t.value2 = r.u16();

    const std::size_t frames = checkedCount(r, r.i32(), 14);
    r.i32();
    for (std::size_t i = 0; i < frames; ++i) {
        t.stack.push_back(readFrame(r, version));
    }

    t.relationshipVersion = r.i32();
    if (t.relationshipVersion == 2 || t.relationshipVersion == 3) {
        const std::size_t ids = checkedCount(r, r.u32(), 4);
        for (std::size_t i = 0; i < ids; ++i) {
            t.relationshipIds.push_back(r.u32());
        }
    } else {
        throw std::runtime_error("XOBJ: relationship block version not supported");
    }

    t.slots = readSlots(r);

    if (r.i16() != 0) {
        t.effectVersion = r.u16();
        if (t.effectVersion != 1 && t.effectVersion != 2) {
            throw std::runtime_error("XOBJ: effect list version not supported");
        }
        const std::size_t effects = checkedCount(r, r.i32(), 10);
        for (std::size_t i = 0; i < effects; ++i) {
            Effect e;
            e.name = readString(r);
            e.slot = readString(r);
            e.value1 = r.u32();
            e.value2 = r.u32();
            if (t.effectVersion >= 2) {
                e.value3 = r.u8();
            }
            t.effects.push_back(std::move(e));
        }
    }

    if (version > 0x94) {
        const std::size_t materials = checkedCount(r, r.i16(), 3);
        for (std::size_t i = 0; i < materials; ++i) {
            MaterialOverride m;
            m.resource = readString(r);
            m.subset = readString(r);
            m.material = readString(r);
            t.materials.push_back(std::move(m));
        }
    }

    if (version > 0x95) {
        const std::size_t timers = checkedCount(r, r.u32(), kTimerEntrySize);
        for (std::size_t i = 0; i < timers; ++i) {
            TimerEntry e;
            e.value1 = r.u32();
            e.value2 = r.u32();
            e.value3 = r.u16();
            e.value4 = r.u32();
            e.value5 = r.i32();
            e.value6 = r.i32();
            for (auto& v : e.values) {
                v = r.i16();
            }
            t.timers.push_back(e);
        }
    }

    if (kind == RecordKind::ObjectWithMaterial) {
        const auto block = r.bytes(kMaterialBlockSize);
        t.materialBlock.assign(block.begin(), block.end());
    }
    return t;
}

PersonPart readPerson(BinaryReader& r, std::uint32_t version) {
    PersonPart p;
    if (r.u32() != kPersonMarker) {
        throw std::runtime_error("XOBJ: person part without its marker (old layout not supported)");
    }
    p.version = r.i32();
    if (p.version <= 1) {
        throw std::runtime_error("XOBJ: person part version not supported");
    }
    for (auto& m : p.motives) {
        m = r.f32();
    }
    for (auto& m : p.motives2) {
        m = r.f32();
    }
    p.personData = readShorts(r, personDataCountForVersion(version));
    return p;
}

} // namespace

Token readToken(BinaryReader& r, std::uint32_t version) {
    Token t;
    t.guid = r.u32();
    t.flags = r.u16();
    if (version > 0xC1) {
        t.value1 = r.u16();
    }
    if (version > 0xB3) {
        t.value2 = r.u32();
    }
    if (version > 0xCA) {
        t.value3 = r.u16();
    }
    t.properties = readShorts(r, checkedCount(r, r.i32(), 2));
    return t;
}

std::size_t dataCountForVersion(std::uint32_t version) {
    static constexpr Step steps[] = {
        {0x7D, 0x48}, {0x7E, 0x4E}, {0x86, 0x4F}, {0x89, 0x50}, {0x91, 0x53}, {0x94, 0x54},
        {0x98, 0x55}, {0x99, 0x56}, {0xA3, 0x57}, {0xB2, 0x58}, {0xB3, 0x62}, {0xB6, 0x63},
        {0xBB, 0x65}, {0xBC, 0x66}, {0xBF, 0x67}, {0xC0, 0x68}, {0xC6, 0x6C}, {0xC8, 0x6F},
        {0xC9, 0x70}, {0xD6, 0x71},
    };
    return countFor(steps, version, kDataCount);
}

std::size_t personDataCountForVersion(std::uint32_t version) {
    static constexpr Step steps[] = {
        {0x29, 0x40}, {0x6A, 0x50}, {0x6C, 0x51}, {0x6D, 0x53}, {0x6E, 0x58}, {0x73, 0x59},
        {0x75, 0x5B}, {0x77, 0x5C}, {0x78, 0x66}, {0x7B, 0x68}, {0x7C, 0x6B}, {0x7D, 0x6F},
        {0x7F, 0x70}, {0x80, 0x76}, {0x82, 0x77}, {0x83, 0x79}, {0x85, 0x7A}, {0x88, 0x7B},
        {0x8A, 0x7C}, {0x8B, 0x90}, {0x97, 0x9D}, {0x99, 0x9E}, {0x9A, 0xA1}, {0x9C, 0xA2},
        {0x9F, 0xA3}, {0xA0, 0xA5}, {0xA2, 0xA6}, {0xA8, 0xA7}, {0xAE, 0xAA}, {0xB0, 0xB2},
        {0xB3, 0xB3}, {0xB7, 0xB4}, {0xB8, 0xBA}, {0xB9, 0xBD}, {0xBA, 0xC0}, {0xBC, 0xC1},
        {0xBD, 0xC2}, {0xC3, 0xC3}, {0xC7, 0xC7}, {0xCC, 0xC8}, {0xCE, 0xC9}, {0xCF, 0xCC},
        {0xD0, 0xD8}, {0xD1, 0xDB}, {0xD2, 0xE2}, {0xD3, 0xE4}, {0xD5, 0xE5}, {0xD8, 0xE6},
    };
    return countFor(steps, version, kPersonDataCount);
}

SavedObject parse(std::span<const std::uint8_t> data, std::uint32_t version, RecordKind kind) {
    BinaryReader r(data);
    r.skip(64);

    SavedObject o;
    o.version = version;
    readHeader(r, version, o.header);

    if (version < 0x0B) {
        o.attributes = readShorts(r, 8);
        const std::size_t temps = version == 0 ? 32 : 8;
        const std::size_t fields = version == 0 ? 32 : 0x24;
        const auto t = readShorts(r, temps);
        for (std::size_t i = 0; i < kTempCount; ++i) {
            o.temps[i] = t[i];
        }
        copyData(o, readShorts(r, fields));
    } else {
        if (version < 0x18) {
            o.attributes = readShorts(r, 8);
        } else {
            o.attributes = readCountedShorts(r);
            if (version > 0x79) {
                o.semiAttributes = readCountedShorts(r);
            }
        }
        for (auto& t : o.temps) {
            t = r.i16();
        }
        copyData(o, readShorts(r, dataCountForVersion(version)));
        if (version > 0x65) {
            for (auto& e : o.extra) {
                e = r.i16();
            }
        }
    }

    if (version >= kFirstDecodedTailVersion) {
        o.tail = readTail(r, version, kind);
        if (kind == RecordKind::Person) {
            o.person = readPerson(r, version);
        } else if (r.remaining() != 0) {
            throw std::runtime_error("XOBJ: data left after the record");
        }
    }

    const auto rest = r.bytes(r.remaining());
    o.rest.assign(rest.begin(), rest.end());
    return o;
}

} // namespace ts2::xobj
