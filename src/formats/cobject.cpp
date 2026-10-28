#include "formats/cobject.hpp"

#include "formats/binary_reader.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ts2::cobject {

namespace {

constexpr std::size_t kLeadingBytes = 64;
constexpr std::size_t kMaxClasses = 8;
constexpr std::uint32_t kObjectTag = 0xFA1C39F7, kAnimatableTag = 0x7D4F2BB8, kLocomotableTag = 0xFD4F2EC7,
                        kPersonTag = 0xFA34F3EB;
constexpr std::uint32_t kRequestTag = 0x09D0CE69, kRequestSubTag = 0x6C9EAF29, kInstanceTag = 0xC9DE69B6,
                        kInstanceBaseTag = 0x3AFF30DB, kCurveTag = 0x8B969D64, kAttachedTag = 0x2A6F2E97;
constexpr std::size_t kTransformFloats = 7;  // position, then rotation as a quaternion

[[noreturn]] void fail(const char* what) {
    throw std::runtime_error(std::string("cObject: ") + what);
}

void expect(std::uint32_t value, std::uint32_t wanted, const char* what) {
    if (value != wanted) {
        fail(what);
    }
}

void skipFloats(BinaryReader& r, std::size_t n) {
    r.skip(4 * n);
}

void objectData(BinaryReader& r, std::uint32_t version, std::vector<SavedJoint>& joints) {
    if (version < 0xD) {
        r.varString();
        r.i32();
    }
    if (version >= 0xE) {
        r.varString();  // model
    }
    for (std::size_t g = r.i32Count(5); g > 0; --g) {
        r.varString();
        if (version >= 0x11) {
            r.u32();
        }
        for (std::size_t p = r.i32Count(2); p > 0; --p) {
            r.varString();
            r.varString();
        }
    }
    if (version >= 0xF) {
        skipFloats(r, kTransformFloats);
    }
    for (std::size_t j = r.i32Count(1 + 4 * kTransformFloats); j > 0; --j) {
        SavedJoint joint;
        joint.name = r.varString();
        if (version <= 0xE) {
            skipFloats(r, kTransformFloats + 1);
            continue;
        }
        for (auto& v : joint.translation) {
            v = r.f32();
        }
        for (auto& v : joint.rotation) {
            v = r.f32();
        }
        joints.push_back(std::move(joint));
    }
    if (version >= 0x10) {
        for (std::size_t b = r.i32Count(6); b > 0; --b) {
            r.varString();
            r.varString();
            r.f32();
        }
    }
}

// The request an animation was started with: its name and type, and whose event tree it tells.
void request(BinaryReader& r, SavedAnimation& a) {
    expect(r.u32(), kRequestTag, "bad request");
    const std::uint32_t version = r.u32();
    a.flipped = (r.u32() & 1) != 0;  // flags: bit 0 played mirrored, the top 3 bits who played it
    a.name = r.varString();
    if (version >= 3) {
        a.type = r.varString();
    }
    r.i32();
    if (version >= 7) {
        a.category = r.i32();
    }
    a.eventObject = r.i16();
    a.eventTree = r.u16();
    r.i32();
    if (version >= 9) {
        a.rate = r.f32();
    }
    skipFloats(r, 2);
    r.skip(4 * 4);
    if (version >= 4) {
        skipFloats(r, kTransformFloats);
    }
    r.skip(2 * 4);
    if (version >= 2) {
        a.eventObjectGuid = r.u32();
    }
    if (version >= 5) {
        r.skip(2 * 4);
    }
    if (version >= 6) {
        r.varString();
    }
    if (version >= 8 && r.i32() != 0) {
        expect(r.u32(), kRequestSubTag, "bad request part");
        r.skip(3 * 4);
    }
    if (version >= 0xA) {
        r.skip(2 * 4);
    }
    if (version >= 0xB) {
        r.i32();
    }
}

// How the animation was playing: state, layer, plays, weight, its blend curve and how far it got.
void instance(BinaryReader& r, SavedAnimation& a) {
    expect(r.u32(), kInstanceTag, "bad animation instance");
    const std::uint32_t version = r.u32();
    expect(r.u32(), kInstanceBaseTag, "bad animation instance");
    r.u32();
    a.state = r.i32();
    r.i32();
    const std::int32_t started = r.i32();
    r.u32();  // stop tick
    skipFloats(r, 2);
    r.i32();
    a.layer = r.i32();
    a.weight = r.f32();
    if (r.i32() != 0) {
        expect(r.u32(), kCurveTag, "bad blend curve");
        r.skip(3 * 4);
        // Segments of a kind each: 0 rising to the clip's weight, 1 holding it, 2 falling from it.
        float total = 0;
        for (std::size_t k = r.i32Count(16); k > 0; --k) {
            const std::int32_t kind = r.i32();
            skipFloats(r, 2);
            const float length = r.f32();
            total += length;
            if (std::isfinite(length) && length > 0) {
                if (kind == 0) {
                    a.fadeInMs += length;
                } else if (kind == 2) {
                    a.fadeOutMs += length;
                }
            }
        }
        // A broken save could sum to infinity, which would make the fades' arithmetic NaN.
        if (std::isfinite(total)) {
            a.lengthMs = total;
            a.fadeInMs = std::min(a.fadeInMs, std::max(total, 0.0f));
            a.fadeOutMs = std::min(a.fadeOutMs, std::max(total, 0.0f));
        } else {
            a.fadeInMs = a.fadeOutMs = 0;
        }
    }
    r.i32();
    a.plays = r.i32();
    skipFloats(r, 2);
    skipFloats(r, 3 * (kTransformFloats + 1));
    const std::int32_t saved = r.i32();
    a.startedMsAgo = static_cast<std::int32_t>(std::max<std::int64_t>(0, std::int64_t{saved} - started));
    r.skip(3 * 4);
    r.f32();
    r.i32();
    r.f32();  // time into the clip
    if (version > 1) {
        r.f32();
    }
}

void clipInstance(BinaryReader& r, SavedAnimation& a) {
    instance(r, a);
    r.varString();  // the clip's resource name
    r.u32();
}

std::optional<SavedAnimation> entry(BinaryReader& r) {
    const std::uint32_t kind = r.u32();
    if (kind == 0) {
        return std::nullopt;
    }
    SavedAnimation a;
    request(r, a);
    if (kind == 1) {
        a.kind = SavedAnimation::Kind::Clip;
        clipInstance(r, a);
    } else if (kind == 2) {
        a.kind = SavedAnimation::Kind::Attached;
        expect(r.u32(), kAttachedTag, "bad attached animation");
        r.u32();
        instance(r, a);
        r.varString();  // what it is attached to
        r.skip(6 * 4);
        r.u32();
        skipFloats(r, 3);
        r.i32();
        r.f32();
    } else {
        fail("bad animation kind");
    }
    return a;
}

void animatableData(BinaryReader& r, std::uint32_t version, std::vector<SavedAnimation>& out) {
    r.f32();  // scale
    if (r.i32() != 0) {
        skipFloats(r, 2 * kTransformFloats);
    }
    if (version < 0xF) {
        for (int i = 0; i < 2; ++i) {
            if (r.i32() != 0) {
                SavedAnimation ignored;
                clipInstance(r, ignored);
            }
        }
        r.skip(2 * 4);
    }
    r.u32();
    if (version >= 0xC) {
        r.u32();
    }
    if (version < 0xA) {
        r.u32();
    }
    if (version >= 9) {
        r.i32();
    }
    if (version >= 0xB) {
        r.i32();
    }
    if (version >= 0xD) {
        r.f32();
    }
    if (version >= 0xE) {
        r.skip(version < 0x10 ? 4 : 8);
    }
    skipFloats(r, kTransformFloats);
    for (std::size_t n = r.i32Count(4); n > 0; --n) {
        if (auto a = entry(r)) {
            out.push_back(std::move(*a));
        }
    }
}

void locomotableData(BinaryReader& r) {
    r.u32();
    for (std::size_t s = r.i32Count(8); s > 0; --s) {
        r.u32();
        for (std::size_t c = r.i32Count(5); c > 0; --c) {
            r.varString();
            r.f32();
        }
    }
}

struct ClassHeader {
    std::uint32_t tag;
    std::uint32_t version;
};

} // namespace

SavedObject parse(std::span<const std::uint8_t> data) {
    BinaryReader r(data);
    r.skip(kLeadingBytes);
    SavedObject saved;
    std::vector<ClassHeader> headers;
    while (true) {
        if (headers.size() == kMaxClasses) {
            fail("no cObject class");
        }
        const ClassHeader h{r.u32(), r.u32()};
        saved.classes.push_back(r.varString());
        headers.push_back(h);
        if (h.tag == kObjectTag) {
            break;
        }
    }
    const std::uint32_t objectVersion = headers.back().version;
    if (objectVersion < 0xC || objectVersion > 0x11) {
        fail("unknown cObject version");
    }
    objectData(r, objectVersion, saved.joints);
    for (auto h = headers.rbegin() + 1; h != headers.rend(); ++h) {
        switch (h->tag) {
        case kAnimatableTag:
            if (h->version < 8 || h->version > 16) {
                fail("unknown cAnimatable version");
            }
            animatableData(r, h->version, saved.animations);
            break;
        case kLocomotableTag:
            expect(h->version, 4, "unknown cLocomotable version");
            locomotableData(r);
            break;
        case kPersonTag:
            expect(h->version, 3, "unknown cPerson version");
            break;
        default:
            fail("unknown class");
        }
    }
    if (r.remaining() != 0) {
        fail("data after the end");
    }
    return saved;
}

} // namespace ts2::cobject
