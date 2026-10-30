#include "sim/animation.hpp"

#include <algorithm>

namespace ts2::sim {

AnimateOperands animateOperands(std::span<const std::uint8_t> o) {
    AnimateOperands a;
    if (o.size() < 14) {
        return a;
    }
    a.entry = static_cast<std::uint16_t>(o[0] | o[1] << 8);
    a.flipped = (o[2] & 0x01) != 0;
    a.speedFromTemp = (o[2] & 0x02) != 0;
    a.entryFromParam = (o[2] & 0x04) != 0;
    a.speed = static_cast<std::int8_t>(o[3]);
    a.eventTree = static_cast<std::uint16_t>(o[4] | o[5] << 8);
    a.list = o[6];
    a.eventTreeScope = o[7];
    a.flipFromTemp = (o[8] & 0x01) != 0;
    a.ikOwner = o[9];
    a.ikData = static_cast<std::uint16_t>(o[10] | o[11] << 8);
    a.priority = std::min<std::uint8_t>(o[12], 3);
    a.blend = static_cast<std::int8_t>(o[13]);
    return a;
}

AnimateFades animateFades(float lengthMs, bool fadesIn, bool fadesOut) {
    AnimateFades f{fadesIn ? kAnimateFadeMs : 0.0f, fadesOut ? kAnimateFadeMs : 0.0f};
    const float both = f.inMs + f.outMs;
    const float length = std::max(lengthMs, 0.0f);
    if (both > length + 1e-6f) {
        // Just under the length, so the clip still holds its full weight for an instant.
        const float scale = length / both * 0.9999f;
        f.inMs *= scale;
        f.outMs *= scale;
    }
    // A fade of a millisecond or less is none.
    if (f.inMs <= 1) {
        f.inMs = 0;
    }
    if (f.outMs <= 1) {
        f.outMs = 0;
    }
    return f;
}

} // namespace ts2::sim
