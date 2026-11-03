#include "formats/effects.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace ts2::effects {

namespace {

// Longer than any name or texture in the game's files. The game sets no limit of its own.
constexpr std::uint32_t kLongestString = 64 * 1024;

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) : r_(data) {}

    std::uint8_t u8() { return r_.u8(); }
    std::uint16_t u16() { return r_.u16(); }
    std::uint32_t u32() { return r_.u32(); }
    std::int32_t i32() { return r_.i32(); }
    float f32() { return r_.f32(); }
    void floats(std::size_t n) { r_.skip(4 * n); }
    std::size_t remaining() const { return r_.remaining(); }

    std::size_t count(std::size_t minBytes) { return r_.u32Count(minBytes); }

    std::string string() {
        const std::uint32_t length = u32();
        if (length > kLongestString) {
            throw std::runtime_error("effects: string too long");
        }
        return r_.text(length);
    }

    // The top byte says the form: 0 a list of samples, 0x81 a packed curve of at most four
    // values, anything else an empty curve with nothing more stored.
    Curve curve() {
        const std::uint32_t header = u32();
        const std::uint32_t n = header & 0xFFFFFF;
        Curve c;
        if ((header & 0xFF000000) == 0) {
            c.values.resize(r_.checkedCount(n, 4));
        } else if ((header & 0xFF000000) == 0x81000000) {
            c.packed = true;
            c.values.resize(std::min<std::uint32_t>(n, 4));
        }
        for (float& v : c.values) {
            v = f32();
        }
        return c;
    }

    void vectors() { floats(3 * count(12)); }  // a list of 3-float vectors
    std::vector<std::array<float, 3>> vectorList() {
        std::vector<std::array<float, 3>> out(count(12));
        for (auto& v : out) {
            v = {f32(), f32(), f32()};
        }
        return out;
    }

    std::uint64_t flags64() {
        const std::uint64_t high = u32();
        return high << 32 | u32();
    }

private:
    BinaryReader r_;
};

std::uint16_t version(Reader& r, std::uint16_t lowest, std::uint16_t highest) {
    const std::uint16_t v = r.u16();
    if (v < lowest || v > highest) {
        // The game reads nothing more of such a block, which leaves the rest of the file unreadable.
        throw std::runtime_error("effects: block version " + std::to_string(v));
    }
    return v;
}

void pathPoints(Reader& r) {
    const std::size_t n = r.count(28);
    r.floats(7 * n);
}

void models(Reader& r) {
    const std::size_t n = r.count(4);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint32_t flags = r.u32() & 3;
        const std::uint8_t v = (flags & 2) ? r.u8() : 0;
        r.string();
        r.floats(11);
        if (v >= 1) {
            r.f32();
        }
        r.floats(2);
        r.string();
        r.string();
    }
}

void wiggles(Reader& r, bool listed) {
    if (!listed) {
        r.floats(3);
        r.curve();
        r.f32();
        return;
    }
    r.u16();
    const std::size_t n = r.count(23);
    for (std::size_t i = 0; i < n; ++i) {
        r.u16();
        r.u8();
        r.floats(3);
        r.curve();
        r.floats(2);
    }
}

void emitList(Reader& r) {
    const std::size_t n = r.count(28);
    r.floats(7 * n);
}

void emitter(Reader& r) {
    r.u32();
    emitList(r);
    r.u32();
}

std::array<float, 3> vec3(Reader& r) { return {r.f32(), r.f32(), r.f32()}; }

ParticleBlock particles(Reader& r) {
    const std::uint16_t v = version(r, 1, 11);
    ParticleBlock b;
    b.flags = v >= 5 ? r.flags64() : r.u32();
    // The shared head: life, prewarm, two delay ranges, the direction box, the speed range, then
    // the spawn box.
    b.life = {r.f32(), r.f32()};
    b.prewarm = r.f32();
    r.floats(12);
    b.spawnLow = vec3(r);
    b.spawnHigh = vec3(r);
    if (v > 2) {
        r.f32();
    }
    b.count = r.curve();
    b.interval = r.f32();
    b.burstLimit = r.u16();
    r.f32();
    b.size = r.curve();
    r.f32();
    b.aspect = r.curve();
    r.f32();
    b.rotationAxis = vec3(r);
    r.floats(3);
    r.u8();
    // Before version 6 each multiplier is one value: above 0 it is (-x, x), otherwise (1, 1).
    if (v >= 6) {
        b.rotationScale = {r.f32(), r.f32()};
        r.floats(2);
    } else {
        const float x = r.f32();
        r.f32();
        b.rotationScale = x > 0 ? std::array<float, 2>{-x, x} : std::array<float, 2>{1, 1};
    }
    b.rotationOffset = r.f32();
    r.f32();
    b.rotation = r.curve();
    r.curve();
    if (v >= 5) {
        r.vectors();
        r.floats(3);
    }
    b.alpha = r.curve();
    r.f32();
    b.colours = r.vectorList();
    r.floats(3);
    b.texture = r.string();
    b.atlasColumns = r.u8();
    b.atlasRows = r.u8();
    b.alignment = r.u8();
    b.blend = r.u8();
    b.layer = r.f32();
    r.f32();
    r.u8();
    r.u8();
    r.floats(11);
    pathPoints(r);
    r.floats(1);  // four bytes whose use is not known yet
    models(r);
    r.string();
    if (v >= 11) {
        r.floats(3);
    }
    r.floats(16);
    wiggles(r, v >= 8);
    if (v >= 7) {
        emitter(r);
    }
    if (v >= 4) {
        r.string();
    }
    if (v >= 9) {
        r.f32();
    }
    if (v >= 11) {
        r.curve();
        r.u8();
        r.floats(2);
        r.curve();
        r.u8();
        r.f32();
    }
    return b;
}

MetaParticleBlock metaParticles(Reader& r) {
    const std::uint16_t v = version(r, 1, 4);
    MetaParticleBlock b;
    b.flags = r.flags64();
    b.life = {r.f32(), r.f32()};
    r.floats(13);
    b.spawnLow = vec3(r);
    b.spawnHigh = vec3(r);
    if (v > 1) {
        r.f32();
    }
    r.curve();
    r.f32();
    r.u32();
    b.size = r.curve();
    r.f32();
    r.curve();
    r.f32();
    r.curve();
    r.floats(2);
    r.vectors();
    r.floats(3);
    r.curve();
    r.f32();
    b.effect = r.string();
    r.u8();
    r.floats(8);
    b.damping = r.f32();
    r.f32();
    pathPoints(r);
    r.floats(1);  // four bytes whose use is not known yet
    models(r);
    r.floats(10);
    for (auto& w : b.wander) {
        w.retime = {r.f32(), r.f32()};
        w.speed = {r.f32(), r.f32()};
        w.turn = r.f32();
        w.leastTurn = r.f32();
        w.upward = r.f32();
    }
    r.floats(6);
    wiggles(r, v >= 4);
    if (v >= 3) {
        emitter(r);
    } else {
        emitList(r);
    }
    r.f32();
    return b;
}

DecalBlock decal(Reader& r) {
    version(r, 1, 1);
    DecalBlock b;
    b.flags = r.u32() & 0x7F;
    b.texture = r.string();
    b.draw = static_cast<DecalBlock::Draw>(r.u8());
    b.lifeMode = static_cast<DecalBlock::Life>(r.u8());
    b.life = r.f32();
    b.rotation = r.curve();
    b.size = r.curve();
    b.alpha = r.curve();
    b.colours.resize(r.count(12));
    for (auto& c : b.colours) {
        c = {r.f32(), r.f32(), r.f32()};
    }
    b.aspect = r.curve();
    b.alphaVary = r.f32();
    b.sizeVary = r.f32();
    b.rotationVary = r.f32();
    b.repeat = r.f32();
    b.textureOffset = {r.f32(), r.f32()};
    return b;
}

SequenceBlock sequence(Reader& r) {
    version(r, 1, 1);
    SequenceBlock b;
    b.items.resize(r.count(12));
    for (auto& item : b.items) {
        item.range = {r.f32(), r.f32()};
        item.effect = r.string();
    }
    b.flags = r.u32() & 0xF;
    return b;
}

CameraBlock camera(Reader& r) {
    version(r, 1, 1);
    CameraBlock b;
    b.flags = r.u32() & 0x3FFFF;
    b.length = r.f32();
    r.f32();
    b.curves[0] = r.curve();
    b.curves[1] = r.curve();
    r.f32();
    r.u8();
    for (std::size_t k = 2; k < b.curves.size(); ++k) {
        b.curves[k] = r.curve();
    }
    r.u8();
    r.u8();
    r.f32();
    b.name = r.string();
    return b;
}

ScreenBlock screen(Reader& r) {
    version(r, 1, 1);
    ScreenBlock b;
    b.mode = r.u8();
    b.flags = r.u32() & 3;
    b.colours.resize(r.count(12));
    for (auto& c : b.colours) {
        c = {r.f32(), r.f32(), r.f32()};
    }
    b.strength = r.curve();
    b.length = r.f32();
    b.delay = r.f32();
    b.texture = r.string();
    return b;
}

// Reads one block of an unnamed or unused type, keeping nothing.
void skipBlock(Reader& r, int type) {
    version(r, 1, 1);
    switch (type) {
    case 4:
        r.floats(3);
        r.u8();
        r.floats(6);
        break;
    case 5:
        r.floats(13);
        break;
    case 7:
        r.floats(5);
        break;
    case 9:
        r.floats(4);
        break;
    case 10:  // a model, by name
        r.string();
        r.floats(5);
        break;
    case 12:
        if (r.u32() & 1) {
            r.u8();
            r.u8();
            r.u16();
            r.floats(5);
        } else {
            r.floats(3);
        }
        break;
    case 13:
        r.u32();
        r.vectors();
        r.curve();
        r.curve();
        r.f32();
        break;
    default:
        break;
    }
}

Component component(Reader& r, std::uint16_t effectVersion) {
    Component c;
    c.name = r.string();
    c.type = r.u8();
    r.u32();
    r.u8();
    c.scale = r.f32();
    for (float& v : c.orientation) {
        v = r.f32();
    }
    c.offset = {r.f32(), r.f32(), r.f32()};
    r.u8();
    r.u8();
    r.floats(6);
    if (effectVersion >= 3) {
        c.state = r.u32();
        c.stateMask = r.u32();
    } else {
        c.state = r.u16();
        c.stateMask = r.u16();
    }
    c.group = r.u16();
    c.weight = r.u16();
    r.f32();
    c.index = r.i32();
    return c;
}

VisualEffect visualEffect(Reader& r, std::uint16_t effectVersion) {
    VisualEffect e;
    e.flags = r.u32() & 0x1FFFF;
    r.u16();
    if (effectVersion >= 3) {
        r.u32();
    } else {
        r.u16();
    }
    r.floats(4);
    e.components.resize(r.count(104));
    for (auto& c : e.components) {
        c = component(r, effectVersion);
    }
    return e;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

} // namespace

float Curve::at(float t) const {
    if (values.empty()) {
        return 0;
    }
    if (packed || values.size() == 1 || !(t > 0)) {
        return values.front();
    }
    if (t >= 1) {
        return values.back();
    }
    const float x = t * static_cast<float>(values.size() - 1);
    const auto i = static_cast<std::size_t>(x);
    const float f = x - static_cast<float>(i);
    return i + 1 < values.size() ? values[i] + (values[i + 1] - values[i]) * f : values.back();
}

const VisualEffect* File::find(std::string_view name) const {
    const auto it = names.find(lower(name));
    return it != names.end() && it->second < effects.size() ? &effects[it->second] : nullptr;
}

File parse(std::span<const std::uint8_t> data) {
    Reader r(data);
    if (r.u16() != 0) {
        throw std::runtime_error("effects: unknown file version");
    }
    File f;
    for (int type = 1; type <= kBlockTypes; ++type) {
        // Every block takes at least its two version bytes.
        const std::size_t n = r.count(2);
        f.blockCounts[static_cast<std::size_t>(type - 1)] = static_cast<std::uint32_t>(n);
        for (std::size_t i = 0; i < n; ++i) {
            switch (type) {
            case 1:
                f.particles.push_back(particles(r));
                break;
            case 2:
                f.metaParticles.push_back(metaParticles(r));
                break;
            case 3:
                f.decals.push_back(decal(r));
                break;
            case 6:
                f.sequences.push_back(sequence(r));
                break;
            case 8:
                f.cameras.push_back(camera(r));
                break;
            case 11:
                f.screens.push_back(screen(r));
                break;
            default:
                skipBlock(r, type);
                break;
            }
        }
    }
    const std::uint16_t effectVersion = r.u16();
    f.effects.resize(r.count(28));
    for (auto& e : f.effects) {
        e = visualEffect(r, effectVersion);
    }
    for (;;) {
        std::string name = r.string();
        const std::uint32_t index = r.u32();
        if (name == "end" && index == 0xFFFFFFFF) {
            break;
        }
        f.names.emplace(lower(name), index);
    }
    // Two more lists follow in every file the game reads, both empty in the shipped files.
    if (r.remaining() > 0) {
        const std::size_t n = r.count(12);
        for (std::size_t i = 0; i < n; ++i) {
            r.string();
            r.u32();
            r.u32();
        }
        r.u16();
        const std::size_t m = r.count(8);
        for (std::size_t i = 0; i < m; ++i) {
            r.u32();
            r.string();
        }
    }
    return f;
}

} // namespace ts2::effects
