#include "formats/xobj.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace ts2::xobj {

namespace {

constexpr std::uint32_t kFirstDecodedTailVersion = 0x93;
constexpr std::uint32_t kPersonMarker = 0xDEADBEEF;
constexpr std::size_t kTimerEntrySize = 38;
constexpr std::size_t kMaterialBlockSize = 26;

// One step of a saved record's growth: records saved by a version older than `below` hold `count`
// values. Records newer than every step hold the reader's full count.
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

std::vector<std::int16_t> readShorts(BinaryReader& r, std::size_t count) {
    std::vector<std::int16_t> values(count);
    for (auto& v : values) {
        v = r.i16();
    }
    return values;
}

std::vector<std::int16_t> readCountedShorts(BinaryReader& r) {
    return readShorts(r, r.checkedCount(r.i16(), 2));
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
        s.values = readShorts(r, r.checkedCount((s.marker) * 2, 2));
    }
    return s;
}

// Texts with 32-bit lengths inside an object archive, with 7-bit lengths elsewhere.
std::string readText(BinaryReader& r, bool fullLengths) {
    if (!fullLengths) {
        return r.varString();
    }
    const auto bytes = r.bytes(r.u32());
    return std::string(bytes.begin(), bytes.end());
}

Tail readTail(BinaryReader& r, std::uint32_t version, RecordKind kind, bool fullLengths) {
    Tail t;
    t.token = readToken(r, version);

    const std::size_t arrayCount = r.checkedCount(r.i16(), 2);
    for (std::size_t i = 0; i < arrayCount; ++i) {
        t.arrays.push_back(readCountedShorts(r));
    }
    t.shorts = readCountedShorts(r);
    t.value1 = r.i32();
    const std::size_t stringCount = r.checkedCount(r.i16(), 1);
    for (std::size_t i = 0; i < stringCount; ++i) {
        t.strings.push_back(readText(r, fullLengths));
    }
    t.value2 = r.u16();

    const std::size_t frames = r.checkedCount(r.i32(), 14);
    t.stackValue = r.i32();
    for (std::size_t i = 0; i < frames; ++i) {
        t.stack.push_back(readFrame(r, version));
    }

    t.relationshipVersion = r.i32();
    if (t.relationshipVersion == 2 || t.relationshipVersion == 3) {
        const std::size_t ids = r.checkedCount(r.u32(), 4);
        for (std::size_t i = 0; i < ids; ++i) {
            t.relationshipIds.push_back(r.u32());
        }
    } else {
        throw std::runtime_error("XOBJ: relationship block version not supported");
    }

    t.slots = readSlots(r);

    t.effectFlag = r.i16();
    if (t.effectFlag != 0) {
        t.effectVersion = r.u16();
        if (t.effectVersion != 1 && t.effectVersion != 2) {
            throw std::runtime_error("XOBJ: effect list version not supported");
        }
        const std::size_t effects = r.checkedCount(r.i32(), 10);
        for (std::size_t i = 0; i < effects; ++i) {
            Effect e;
            e.name = readText(r, fullLengths);
            e.slot = readText(r, fullLengths);
            e.value1 = r.u32();
            e.value2 = r.u32();
            if (t.effectVersion >= 2) {
                e.value3 = r.u8();
            }
            t.effects.push_back(std::move(e));
        }
    }

    if (version > 0x94) {
        const std::size_t materials = r.checkedCount(r.i16(), 3);
        for (std::size_t i = 0; i < materials; ++i) {
            MaterialOverride m;
            m.resource = readText(r, fullLengths);
            m.subset = readText(r, fullLengths);
            m.material = readText(r, fullLengths);
            t.materials.push_back(std::move(m));
        }
    }

    if (version > 0x95) {
        const std::size_t timers = r.checkedCount(r.u32(), kTimerEntrySize);
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

// Lists of saved interactions and runs each hold at least this many bytes per entry.
constexpr std::size_t kMinInteractionSize = 50;
constexpr std::size_t kRunSize = 7;
constexpr std::size_t kValueEntrySize = 12;

SavedInteraction readInteraction(BinaryReader& r, std::uint32_t version) {
    SavedInteraction i;
    i.value1 = r.i32();
    i.number = r.i32();
    i.actor = r.i16();
    i.target = r.i16();
    i.iconObject = r.i16();
    if (version >= 0xA6) {
        i.value2 = r.u16();
    }
    i.index = r.i32();
    for (auto& v : i.values) {
        v = r.u16();
    }
    i.priority = r.i32();
    i.tree = r.u16();
    i.value5 = r.f32();
    i.flags = r.i32();
    i.lineFlag3 = r.u16();
    i.lineFlag1 = r.u16();
    i.lineFlag2 = r.u16();
    i.id = r.i16();
    i.linkId = r.i16();
    if (version >= 0x97) {
        i.name = r.varString();
        i.iconGuid = r.i32();
        i.iconIndex = r.i32();
    }
    if (version >= 0xBB) {
        i.value6 = r.u16();
    }
    return i;
}

std::vector<SavedInteraction> readInteractions(BinaryReader& r, std::uint32_t version) {
    std::vector<SavedInteraction> list(r.i32Count(kMinInteractionSize));
    for (auto& i : list) {
        i = readInteraction(r, version);
    }
    return list;
}

// The Sim's interactions, read for versions 0x93 and later (the only ones whose person part is read).
PersonState readPersonState(BinaryReader& r, std::uint32_t version) {
    PersonState s;
    r.i32();  // not used, and written as 0
    s.running = readInteraction(r, version);
    s.interrupted = readInteraction(r, version);
    s.queue = readInteractions(r, version);
    s.runs.resize(r.i32Count(kRunSize));
    for (auto& run : s.runs) {
        run.object = r.i16();
        run.depth = static_cast<std::int16_t>(r.i32());
        run.flag = r.u8() != 0;
    }
    s.entries.resize(r.i32Count(kValueEntrySize));
    for (auto& e : s.entries) {
        e.key = static_cast<std::int16_t>(r.i32());
        e.value1 = r.f32();
        e.value2 = r.f32();
    }
    if (version >= 0x9B) {
        s.subStartCount = r.u16();
    }
    if (version >= 0x9E) {
        s.value1 = r.u16();
    }
    if (r.i32() != 0) {
        s.runningSub = readInteraction(r, version);
    }
    s.subQueue = readInteractions(r, version);
    if (version >= 0x9D) {
        s.text = r.varString();
    }
    if (version >= 0xA9) {
        s.pickedIconIndex = r.i32();
        s.pickedIconGuid = r.i32();
        s.pickedIconObject = r.i32();
        for (auto& v : s.values) {
            v = r.u16();
        }
    }
    if (version >= 0xB4) {
        s.value2 = r.i32();
    }
    if (version >= 0xB5) {
        s.list = readInteractions(r, version);
    }
    if (version >= 0xCD) {
        s.value3 = r.u32();
        s.value4 = r.u32();
    }
    return s;
}

PersonPart readPerson(BinaryReader& r, std::uint32_t version, bool toEnd) {
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
    // In a record that ends with the Sim, saved interactions that don't read stay as bytes after the
    // person part, so a damaged or unusual Sim still loads without them.
    if (!toEnd) {
        p.state = readPersonState(r, version);
        return p;
    }
    BinaryReader probe = r;
    try {
        p.state = readPersonState(probe, version);
        r = probe;
    } catch (const std::exception&) {
        p.state = {};
        p.stateUnread = true;
    }
    return p;
}

} // namespace

bool isBlank(const SavedInteraction& i) {
    return i.priority == 0 && i.tree == 0 && i.index == -1 && i.iconObject == -1 && i.number == 0 && i.actor <= 0 &&
           i.target <= 0;
}

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
    t.properties = readShorts(r, r.checkedCount(r.i32(), 2));
    return t;
}

// How many object data values an object saved by `version` holds.
std::size_t dataCountForVersion(std::uint32_t version) {
    static constexpr Step steps[] = {
        {0x7D, 0x48}, {0x7E, 0x4E}, {0x86, 0x4F}, {0x89, 0x50}, {0x91, 0x53}, {0x94, 0x54},
        {0x98, 0x55}, {0x99, 0x56}, {0xA3, 0x57}, {0xB2, 0x58}, {0xB3, 0x62}, {0xB6, 0x63},
        {0xBB, 0x65}, {0xBC, 0x66}, {0xBF, 0x67}, {0xC0, 0x68}, {0xC6, 0x6C}, {0xC8, 0x6F},
        {0xC9, 0x70}, {0xD6, 0x71},
    };
    return countFor(steps, version, kDataCount);
}

// How many person data values a Sim saved by `version` holds.
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

namespace {

// Reads one saved object from the reader's position. With `toEnd`, it must be the last thing in
// the data (a resource of its own). Otherwise it may be followed by more.
SavedObject readObject(BinaryReader& r, std::uint32_t version, RecordKind kind, bool toEnd, bool fullLengths = false) {
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
            for (auto& e : o.tokenSource) {
                e = r.i16();
            }
        }
    }

    if (version >= kFirstDecodedTailVersion) {
        o.tail = readTail(r, version, kind, fullLengths);
        if (kind == RecordKind::Person) {
            o.person = readPerson(r, version, toEnd);
        } else if (toEnd && r.remaining() != 0) {
            throw std::runtime_error("XOBJ: data left after the record");
        }
    }

    if (toEnd) {
        const auto rest = r.bytes(r.remaining());
        o.rest.assign(rest.begin(), rest.end());
    }
    return o;
}

} // namespace

SavedObject parse(std::span<const std::uint8_t> data, std::uint32_t version, RecordKind kind) {
    BinaryReader r(data);
    const auto name = r.bytes(64);
    SavedObject o = readObject(r, version, kind, true);
    std::copy(name.begin(), name.end(), o.name.begin());
    return o;
}

SavedObject parseEmbedded(std::span<const std::uint8_t> data, std::uint32_t version, RecordKind kind,
                          std::size_t& consumed) {
    // The length of such a record is only known where all of it is decoded.
    if (version < kFirstDecodedTailVersion || kind == RecordKind::Person) {
        throw std::runtime_error("XOBJ: embedded record of unknown length");
    }
    BinaryReader r(data);
    SavedObject o = readObject(r, version, kind, false);
    consumed = data.size() - r.remaining();
    return o;
}

namespace {

class Writer {
public:
    void u8(std::uint8_t v) { out.push_back(v); }
    void u16(std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void u32(std::uint32_t v) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(const std::string& t) {
        if (fullLengths) {
            if (t.size() > 0x7FFFFFFF) {
                throw std::runtime_error("XOBJ: a text is too long to write");
            }
            u32(static_cast<std::uint32_t>(t.size()));
            out.insert(out.end(), t.begin(), t.end());
            return;
        }
        std::size_t length = t.size();
        do {
            const auto low = static_cast<std::uint8_t>(length & 0x7F);
            length >>= 7;
            out.push_back(static_cast<std::uint8_t>(low | (length != 0 ? 0x80 : 0)));
        } while (length != 0);
        out.insert(out.end(), t.begin(), t.end());
    }
    bool fullLengths = false;
    // A count the record keeps in 32 bits.
    void count32(std::size_t n) {
        if (n > 0x7FFFFFFF) {
            throw std::runtime_error("XOBJ: a list is too long to write");
        }
        u32(static_cast<std::uint32_t>(n));
    }
    // A count the record keeps in 16 bits.
    void count16(std::size_t n) {
        if (n > 0x7FFF) {
            throw std::runtime_error("XOBJ: a list is too long to write");
        }
        u16(static_cast<std::uint16_t>(n));
    }
    template <typename Values>
    void shorts(const Values& values) {
        for (const std::int16_t v : values) {
            i16(v);
        }
    }
    template <typename Values>
    void countedShorts(const Values& values) {
        count16(values.size());
        shorts(values);
    }

    std::vector<std::uint8_t> out;
};

void writeToken(Writer& w, const Token& t) {
    w.u32(t.guid);
    w.u16(t.flags);
    w.u16(t.value1);
    w.u32(t.value2);
    w.u16(t.value3);
    w.i32(static_cast<std::int32_t>(t.properties.size()));
    w.shorts(t.properties);
}

void writeFrame(Writer& w, const StackFrame& f) {
    if (f.locals.size() > 0xFF || f.params.size() > 0xFF) {
        throw std::runtime_error("XOBJ: a stack frame has too many values to write");
    }
    w.i16(f.objectId);
    w.i16(f.treeId);
    w.u16(f.node);
    w.u8(static_cast<std::uint8_t>(f.locals.size()));
    w.u8(static_cast<std::uint8_t>(f.params.size()));
    w.i16(f.value1);
    w.i16(f.value2);
    w.shorts(f.params);
    w.shorts(f.locals);
    w.i32(f.value3);
    w.i16(f.codeOwner);
}

void writeSlots(Writer& w, const Slots& s) {
    w.i16(s.marker);
    if (s.marker < 0) {
        if (s.marker == -100) {
            w.i16(s.extra);
        }
        w.countedShorts(s.values);
    } else if (s.marker > 0) {
        if (s.values.size() != static_cast<std::size_t>(s.marker) * 2) {
            throw std::runtime_error("XOBJ: slot values do not match their marker");
        }
        w.shorts(s.values);
    }
}

void writeTail(Writer& w, const Tail& t, RecordKind kind) {
    writeToken(w, t.token);
    w.count16(t.arrays.size());
    for (const auto& array : t.arrays) {
        w.countedShorts(array);
    }
    w.countedShorts(t.shorts);
    w.i32(t.value1);
    w.count16(t.strings.size());
    for (const auto& s : t.strings) {
        w.text(s);
    }
    w.u16(t.value2);

    w.i32(static_cast<std::int32_t>(t.stack.size()));
    w.i32(t.stackValue);
    for (const auto& frame : t.stack) {
        writeFrame(w, frame);
    }

    if (t.relationshipVersion != 2 && t.relationshipVersion != 3) {
        throw std::runtime_error("XOBJ: relationship block version not supported");
    }
    w.i32(t.relationshipVersion);
    w.u32(static_cast<std::uint32_t>(t.relationshipIds.size()));
    for (const std::uint32_t id : t.relationshipIds) {
        w.u32(id);
    }

    writeSlots(w, t.slots);

    w.i16(t.effectFlag);
    if (t.effectFlag != 0) {
        w.u16(t.effectVersion);
        w.i32(static_cast<std::int32_t>(t.effects.size()));
        for (const Effect& e : t.effects) {
            w.text(e.name);
            w.text(e.slot);
            w.u32(e.value1);
            w.u32(e.value2);
            if (t.effectVersion >= 2) {
                w.u8(e.value3);
            }
        }
    }

    w.count16(t.materials.size());
    for (const MaterialOverride& m : t.materials) {
        w.text(m.resource);
        w.text(m.subset);
        w.text(m.material);
    }

    w.u32(static_cast<std::uint32_t>(t.timers.size()));
    for (const TimerEntry& e : t.timers) {
        w.u32(e.value1);
        w.u32(e.value2);
        w.u16(e.value3);
        w.u32(e.value4);
        w.i32(e.value5);
        w.i32(e.value6);
        w.shorts(e.values);
    }

    if (kind == RecordKind::ObjectWithMaterial) {
        if (t.materialBlock.size() != kMaterialBlockSize) {
            throw std::runtime_error("XOBJ: material block of the wrong size");
        }
        w.out.insert(w.out.end(), t.materialBlock.begin(), t.materialBlock.end());
    }
}

void writeInteraction(Writer& w, const SavedInteraction& i) {
    w.i32(i.value1);
    w.i32(i.number);
    w.i16(i.actor);
    w.i16(i.target);
    w.i16(i.iconObject);
    w.u16(i.value2);
    w.i32(i.index);
    for (const std::uint16_t v : i.values) {
        w.u16(v);
    }
    w.i32(i.priority);
    w.u16(i.tree);
    w.f32(i.value5);
    w.i32(i.flags);
    w.u16(i.lineFlag3);
    w.u16(i.lineFlag1);
    w.u16(i.lineFlag2);
    w.i16(i.id);
    w.i16(i.linkId);
    w.text(i.name);
    w.i32(i.iconGuid);
    w.i32(i.iconIndex);
    w.u16(i.value6);
}

void writeInteractions(Writer& w, const std::vector<SavedInteraction>& list) {
    w.count32(list.size());
    for (const auto& i : list) {
        writeInteraction(w, i);
    }
}

void writePersonState(Writer& w, const PersonState& s) {
    w.i32(0);
    writeInteraction(w, s.running);
    writeInteraction(w, s.interrupted);
    writeInteractions(w, s.queue);
    w.count32(s.runs.size());
    for (const auto& run : s.runs) {
        w.i16(run.object);
        w.i32(run.depth);
        w.u8(run.flag ? 1 : 0);
    }
    w.count32(s.entries.size());
    for (const auto& e : s.entries) {
        w.i32(e.key);
        w.f32(e.value1);
        w.f32(e.value2);
    }
    w.u16(s.subStartCount);
    w.u16(s.value1);
    w.i32(s.runningSub ? 1 : 0);
    if (s.runningSub) {
        writeInteraction(w, *s.runningSub);
    }
    writeInteractions(w, s.subQueue);
    w.text(s.text);
    w.i32(s.pickedIconIndex);
    w.i32(s.pickedIconGuid);
    w.i32(s.pickedIconObject);
    for (const std::uint16_t v : s.values) {
        w.u16(v);
    }
    w.i32(s.value2);
    writeInteractions(w, s.list);
    w.u32(s.value3);
    w.u32(s.value4);
}

void writeBody(Writer& w, const SavedObject& o, RecordKind kind) {
    if (o.version != kWrittenVersion || !o.tail || (kind == RecordKind::Person) != o.person.has_value()) {
        throw std::runtime_error("XOBJ: only a whole record of the version the game writes can be written");
    }
    const Header& h = o.header;
    for (const float f : h.reserved) {
        w.f32(f);
    }
    for (const float f : h.location) {
        w.f32(f);
    }
    w.i32(h.value1);
    w.i16(h.value2);
    w.f32(h.value3);
    w.i32(h.value4);
    w.i16(h.value5);
    w.f32(h.value6);
    w.countedShorts(o.attributes);
    w.countedShorts(o.semiAttributes);
    w.shorts(o.temps);
    w.shorts(o.data);
    w.shorts(o.tokenSource);
    writeTail(w, *o.tail, kind);
    if (kind == RecordKind::Person) {
        const PersonPart& p = *o.person;
        if (p.personData.size() != personDataCountForVersion(o.version)) {
            throw std::runtime_error("XOBJ: person data of the wrong size");
        }
        w.u32(kPersonMarker);
        w.i32(p.version);
        for (const float m : p.motives) {
            w.f32(m);
        }
        for (const float m : p.motives2) {
            w.f32(m);
        }
        w.shorts(p.personData);
        if (!p.stateUnread) {
            writePersonState(w, p.state);
        }
    }
    w.out.insert(w.out.end(), o.rest.begin(), o.rest.end());
}

} // namespace

std::vector<std::uint8_t> write(const SavedObject& o, RecordKind kind) {
    Writer w;
    w.out.assign(o.name.begin(), o.name.end());
    writeBody(w, o, kind);
    return w.out;
}

SavedObject parseArchived(std::span<const std::uint8_t> data, RecordKind kind, std::size_t& consumed) {
    if (kind == RecordKind::Person) {
        throw std::runtime_error("XOBJ: archived record of unknown length");
    }
    BinaryReader r(data);
    SavedObject o = readObject(r, kWrittenVersion, kind, false, true);
    consumed = data.size() - r.remaining();
    return o;
}

std::vector<std::uint8_t> writeArchived(const SavedObject& o, RecordKind kind) {
    Writer w;
    w.fullLengths = true;
    writeBody(w, o, kind);
    return w.out;
}

std::vector<std::uint8_t> writeToken(const Token& token) {
    Writer w;
    writeToken(w, token);
    return std::move(w.out);
}

} // namespace ts2::xobj
