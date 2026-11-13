#include "formats/cobject_record.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace ts2::cobject {

namespace {

constexpr std::size_t kNameSize = 64;

struct ClassHeader {
    std::uint32_t tag;
    std::uint32_t version;
    const char* name;
};

// The versions the game writes now.
constexpr ClassHeader kPerson{0xFA34F3EB, 3, "cPerson"};
constexpr ClassHeader kLocomotable{0xFD4F2EC7, 4, "cLocomotable"};
constexpr ClassHeader kAnimatable{0x7D4F2BB8, 0x10, "cAnimatable"};
constexpr ClassHeader kObject{kType, 0x11, "cObject"};
constexpr std::uint32_t kRequestTag = 0x09D0CE69, kRequestVersion = 0xB;
constexpr std::uint32_t kRequestPartTag = 0x6C9EAF29, kRequestPartVersion = 1;
constexpr std::uint32_t kInstanceTag = 0xC9DE69B6, kInstanceVersion = 2;
constexpr std::uint32_t kInstanceBaseTag = 0x3AFF30DB, kInstanceBaseVersion = 7;
constexpr std::uint32_t kCurveTag = 0x8B969D64, kCurveVersion = 1;
constexpr std::uint32_t kAttachedTag = 0x2A6F2E97, kAttachedVersion = 5;

// Any part that does not match what the game writes now.
struct NotWritten {};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : r(data) {}

    Word word() { return r.u32(); }
    std::int32_t i32() { return r.i32(); }

    template <std::size_t N>
    void words(std::array<Word, N>& out) {
        for (Word& w : out) {
            w = r.u32();
        }
    }

    // The game writes a length in as few 7-bit groups as it needs, so a longer one would not write back.
    std::string text() {
        if (fullLengths) {
            const auto bytes = r.bytes(r.u32());
            return std::string(bytes.begin(), bytes.end());
        }
        const std::size_t start = r.position();
        std::string t = r.varString();
        if (r.position() - start != lengthBytes(t.size()) + t.size()) {
            throw NotWritten{};
        }
        return t;
    }

    std::size_t count(std::size_t minBytes) { return r.i32Count(minBytes); }

    void expect(std::uint32_t value) {
        if (r.u32() != value) {
            throw NotWritten{};
        }
    }

    // A flag the game writes as 0 or 1.
    bool flag() {
        const std::int32_t v = r.i32();
        if (v != 0 && v != 1) {
            throw NotWritten{};
        }
        return v == 1;
    }

    static std::size_t lengthBytes(std::size_t length) {
        std::size_t n = 1;
        while (length >= 0x80) {
            length >>= 7;
            ++n;
        }
        return n;
    }

    BinaryReader r;
    // Texts with 32-bit lengths, as inside an object archive.
    bool fullLengths = false;
};

class Writer {
public:
    void word(Word v) {
        for (int shift = 0; shift < 32; shift += 8) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void i32(std::int32_t v) { word(static_cast<Word>(v)); }

    template <std::size_t N>
    void words(const std::array<Word, N>& values) {
        for (const Word w : values) {
            word(w);
        }
    }

    void text(const std::string& t) {
        if (fullLengths) {
            count(t.size());
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

    void count(std::size_t n) {
        if (n > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::runtime_error("cObject: a list is too long to write");
        }
        i32(static_cast<std::int32_t>(n));
    }

    void header(const ClassHeader& h) {
        word(h.tag);
        word(h.version);
        text(h.name);
    }

    std::vector<std::uint8_t> out;
    bool fullLengths = false;
};

void readHeader(Reader& r, const ClassHeader& h) {
    r.expect(h.tag);
    r.expect(h.version);
    if (r.text() != h.name) {
        throw NotWritten{};
    }
}

ObjectPart readObject(Reader& r) {
    ObjectPart o;
    o.model = r.text();
    for (std::size_t n = r.count(9); n > 0; --n) {
        NodeMaterials m;
        m.node = r.text();
        m.value = r.word();
        for (std::size_t p = r.count(2); p > 0; --p) {
            std::string subset = r.text();
            m.subsets.emplace_back(std::move(subset), r.text());
        }
        o.materials.push_back(std::move(m));
    }
    r.words(o.place);
    for (std::size_t n = r.count(1 + sizeof(Place)); n > 0; --n) {
        PosedJoint j;
        j.name = r.text();
        r.words(j.place);
        o.joints.push_back(std::move(j));
    }
    for (std::size_t n = r.count(6); n > 0; --n) {
        NamedWeight b;
        b.first = r.text();
        b.second = r.text();
        b.weight = r.word();
        o.blends.push_back(std::move(b));
    }
    return o;
}

void writeObject(Writer& w, const ObjectPart& o) {
    w.text(o.model);
    w.count(o.materials.size());
    for (const NodeMaterials& m : o.materials) {
        w.text(m.node);
        w.word(m.value);
        w.count(m.subsets.size());
        for (const auto& [subset, material] : m.subsets) {
            w.text(subset);
            w.text(material);
        }
    }
    w.words(o.place);
    w.count(o.joints.size());
    for (const PosedJoint& j : o.joints) {
        w.text(j.name);
        w.words(j.place);
    }
    w.count(o.blends.size());
    for (const NamedWeight& b : o.blends) {
        w.text(b.first);
        w.text(b.second);
        w.word(b.weight);
    }
}

Request readRequest(Reader& r) {
    r.expect(kRequestTag);
    r.expect(kRequestVersion);
    Request q;
    q.flags = r.word();
    q.name = r.text();
    q.type = r.text();
    q.value1 = r.i32();
    q.category = r.i32();
    q.eventObject = r.r.i16();
    q.eventTree = r.r.u16();
    q.value2 = r.i32();
    q.rate = r.word();
    r.words(q.words1);
    q.eventObjectGuid = r.word();
    r.words(q.words2);
    q.text = r.text();
    if (r.flag()) {
        r.expect(kRequestPartTag);
        r.expect(kRequestPartVersion);
        const std::int32_t value = r.i32();
        // The game keeps this value in 16 bits and writes it widened.
        if (value != static_cast<std::int16_t>(value)) {
            throw NotWritten{};
        }
        q.part.emplace(value, r.word());
    }
    r.words(q.words3);
    q.value3 = r.i32();
    return q;
}

void writeRequest(Writer& w, const Request& q) {
    w.word(kRequestTag);
    w.word(kRequestVersion);
    w.word(q.flags);
    w.text(q.name);
    w.text(q.type);
    w.i32(q.value1);
    w.i32(q.category);
    w.word(static_cast<std::uint16_t>(q.eventObject) | static_cast<Word>(q.eventTree) << 16);
    w.i32(q.value2);
    w.word(q.rate);
    w.words(q.words1);
    w.word(q.eventObjectGuid);
    w.words(q.words2);
    w.text(q.text);
    w.i32(q.part ? 1 : 0);
    if (q.part) {
        w.word(kRequestPartTag);
        w.word(kRequestPartVersion);
        w.i32(static_cast<std::int16_t>(q.part->first));
        w.word(q.part->second);
    }
    w.words(q.words3);
    w.i32(q.value3);
}

Instance readInstance(Reader& r) {
    r.expect(kInstanceTag);
    r.expect(kInstanceVersion);
    r.expect(kInstanceBaseTag);
    r.expect(kInstanceBaseVersion);
    Instance s;
    s.state = r.i32();
    s.word1 = r.word();
    s.startTick = r.i32();
    s.stopTick = r.word();
    r.words(s.words2);
    s.layer = r.i32();
    s.weight = r.word();
    if (r.flag()) {
        r.expect(kCurveTag);
        r.expect(kCurveVersion);
        Curve c;
        c.value = r.i32();
        c.word = r.word();
        for (std::size_t n = r.count(16); n > 0; --n) {
            CurveSegment seg;
            seg.kind = r.i32();
            r.words(seg.values);
            seg.length = r.word();
            c.segments.push_back(seg);
        }
        s.curve = std::move(c);
    }
    s.word3 = r.word();
    s.plays = r.i32();
    r.words(s.words4);
    s.savedTick = r.i32();
    r.words(s.words5);
    s.time = r.word();
    s.word6 = r.word();
    return s;
}

void writeInstance(Writer& w, const Instance& s) {
    w.word(kInstanceTag);
    w.word(kInstanceVersion);
    w.word(kInstanceBaseTag);
    w.word(kInstanceBaseVersion);
    w.i32(s.state);
    w.word(s.word1);
    w.i32(s.startTick);
    w.word(s.stopTick);
    w.words(s.words2);
    w.i32(s.layer);
    w.word(s.weight);
    w.i32(s.curve ? 1 : 0);
    if (s.curve) {
        w.word(kCurveTag);
        w.word(kCurveVersion);
        w.i32(s.curve->value);
        w.word(s.curve->word);
        w.count(s.curve->segments.size());
        for (const CurveSegment& seg : s.curve->segments) {
            w.i32(seg.kind);
            w.words(seg.values);
            w.word(seg.length);
        }
    }
    w.word(s.word3);
    w.i32(s.plays);
    w.words(s.words4);
    w.i32(s.savedTick);
    w.words(s.words5);
    w.word(s.time);
    w.word(s.word6);
}

Animation readAnimation(Reader& r) {
    Animation a;
    const Word kind = r.word();
    if (kind == 0) {
        return a;
    }
    if (kind != 1 && kind != 2) {
        throw NotWritten{};
    }
    a.kind = static_cast<Animation::Kind>(kind);
    a.request = readRequest(r);
    if (a.kind == Animation::Kind::Clip) {
        a.instance = readInstance(r);
        a.resource = r.text();
        a.word = r.word();
    } else {
        r.expect(kAttachedTag);
        r.expect(kAttachedVersion);
        a.instance = readInstance(r);
        a.resource = r.text();
        r.words(a.attached);
    }
    return a;
}

void writeAnimation(Writer& w, const Animation& a) {
    w.word(static_cast<Word>(a.kind));
    if (a.kind == Animation::Kind::None) {
        return;
    }
    writeRequest(w, a.request);
    if (a.kind == Animation::Kind::Clip) {
        writeInstance(w, a.instance);
        w.text(a.resource);
        w.word(a.word);
    } else {
        w.word(kAttachedTag);
        w.word(kAttachedVersion);
        writeInstance(w, a.instance);
        w.text(a.resource);
        w.words(a.attached);
    }
}

AnimatablePart readAnimatable(Reader& r) {
    AnimatablePart p;
    p.scale = r.word();
    if (r.flag()) {
        p.places.emplace();
        r.words(*p.places);
    }
    r.words(p.settings);
    r.words(p.place);
    for (std::size_t n = r.count(4); n > 0; --n) {
        p.animations.push_back(readAnimation(r));
    }
    return p;
}

void writeAnimatable(Writer& w, const AnimatablePart& p) {
    w.word(p.scale);
    w.i32(p.places ? 1 : 0);
    if (p.places) {
        w.words(*p.places);
    }
    w.words(p.settings);
    w.words(p.place);
    w.count(p.animations.size());
    for (const Animation& a : p.animations) {
        writeAnimation(w, a);
    }
}

LocomotablePart readLocomotable(Reader& r) {
    LocomotablePart p;
    p.value = r.word();
    for (std::size_t n = r.count(8); n > 0; --n) {
        LocomotionSet s;
        s.id = r.word();
        for (std::size_t c = r.count(5); c > 0; --c) {
            std::string name = r.text();
            s.weights.emplace_back(std::move(name), r.word());
        }
        p.sets.push_back(std::move(s));
    }
    return p;
}

void writeLocomotable(Writer& w, const LocomotablePart& p) {
    w.word(p.value);
    w.count(p.sets.size());
    for (const LocomotionSet& s : p.sets) {
        w.word(s.id);
        w.count(s.weights.size());
        for (const auto& [name, weight] : s.weights) {
            w.text(name);
            w.word(weight);
        }
    }
}

} // namespace

namespace {

// The class headers, then each class's data.
Record readChain(Reader& r, std::span<const std::uint8_t> data) {
    Record record;
    const std::uint32_t outer = BinaryReader(data.subspan(r.r.position())).u32();
    record.chain = outer == kPerson.tag ? Chain::Person : outer == kAnimatable.tag ? Chain::Animatable : Chain::Object;
    if (record.chain == Chain::Person) {
        readHeader(r, kPerson);
        readHeader(r, kLocomotable);
    }
    if (record.chain != Chain::Object) {
        readHeader(r, kAnimatable);
    }
    readHeader(r, kObject);
    record.object = readObject(r);
    if (record.chain != Chain::Object) {
        record.animatable = readAnimatable(r);
    }
    if (record.chain == Chain::Person) {
        record.locomotable = readLocomotable(r);
    }
    return record;
}

void writeChain(Writer& w, const Record& record) {
    if (record.chain == Chain::Person) {
        w.header(kPerson);
        w.header(kLocomotable);
    }
    if (record.chain != Chain::Object) {
        w.header(kAnimatable);
    }
    w.header(kObject);
    writeObject(w, record.object);
    if (record.chain != Chain::Object) {
        writeAnimatable(w, record.animatable);
    }
    if (record.chain == Chain::Person) {
        writeLocomotable(w, record.locomotable);
    }
}

} // namespace

std::optional<Record> readRecord(std::span<const std::uint8_t> data) {
    try {
        Reader r(data);
        const auto name = r.r.bytes(kNameSize);
        if (std::any_of(name.begin(), name.end(), [](std::uint8_t b) { return b != 0; })) {
            return std::nullopt;
        }
        Record record = readChain(r, data);
        if (r.r.remaining() != 0) {
            return std::nullopt;
        }
        return record;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeRecord(const Record& record) {
    Writer w;
    w.out.assign(kNameSize, 0);
    writeChain(w, record);
    return std::move(w.out);
}

std::optional<Record> readArchived(std::span<const std::uint8_t> data, std::size_t& consumed) {
    try {
        Reader r(data);
        r.fullLengths = true;
        Record record = readChain(r, data);
        consumed = r.r.position();
        return record;
    } catch (const NotWritten&) {
        return std::nullopt;
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

std::vector<std::uint8_t> writeArchived(const Record& record) {
    Writer w;
    w.fullLengths = true;
    writeChain(w, record);
    return std::move(w.out);
}

} // namespace ts2::cobject
