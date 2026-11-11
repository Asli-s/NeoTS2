#include "scene/roof_meeting.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace ts2::scene {

namespace {

// How far another roof's top must rise above a point to cover it.
constexpr float kCoverMargin = 0.01f;

// Whether two floats are at most 32 representable values apart (31 when a is above b), the game's
// test for equal slopes and equal rectangle sides.
bool withinSteps(float a, float b) {
    auto ordered = [](float f) {
        const auto i = std::bit_cast<std::uint32_t>(f);
        return (i & 0x80000000u) != 0 ? 0u - (i & 0x7fffffffu) : i;
    };
    const auto d = static_cast<std::int32_t>(ordered(a) - ordered(b));
    return d >= -32 && d <= 31;
}

bool slopesOrGables(int type) { return type == 1 || type == 3 || type == 4; }

// Whether another roof's overhang over `asked`, both on one level, covers it at a point. It does only
// past the ends of asked's rectangle along the way asked lies, and only where the other's rectangle
// ends on the same line. Single slopes and gables (and the game's type 15) are asked this way.
bool overhangCovers(const RoofGrid& asked, const RoofGrid& other, float x, float y) {
    if (!slopesOrGables(asked.type) && asked.type != 15) {
        return false;
    }
    if (asked.lengthwise != other.lengthwise) {
        return false;
    }
    if (!asked.lengthwise) {
        if (y < asked.rectMinY && withinSteps(other.rectMinY, asked.rectMinY)) {
            return true;
        }
        return y > asked.rectMaxY && withinSteps(asked.rectMaxY, other.rectMaxY);
    }
    if (x < asked.rectMinX && withinSteps(asked.rectMinX, other.rectMinX)) {
        return true;
    }
    return x > asked.rectMaxX && withinSteps(asked.rectMaxX, other.rectMaxX);
}

bool meets(const std::vector<CornerRoof>& at, const RoofGrid* roof) {
    return std::any_of(at.begin(), at.end(), [&](const CornerRoof& c) { return c.roof == roof; });
}

const CornerRoof* find(const std::vector<CornerRoof>& at, const RoofGrid* roof) {
    const auto it = std::find_if(at.begin(), at.end(), [&](const CornerRoof& c) { return c.roof == roof; });
    return it == at.end() ? nullptr : &*it;
}

int junctions(const TriangleCorners& t) {
    return static_cast<int>(std::count_if(t.flags.begin(), t.flags.end(), [](int f) { return (f & kCornerJunction) != 0; }));
}

int cornersMet(const TriangleCorners& t, const RoofGrid* roof) {
    return static_cast<int>(std::count_if(t.meeting.begin(), t.meeting.end(), [&](const auto& at) { return meets(at, roof); }));
}

// How many corners another roof meets on a hip (hip true) or on its ridge.
int cornersOn(const TriangleCorners& t, const RoofGrid* roof, bool hip) {
    int n = 0;
    for (const auto& at : t.meeting) {
        const CornerRoof* c = find(at, roof);
        n += c && (hip ? c->hip : c->ridge) != 0 ? 1 : 0;
    }
    return n;
}

// The same for the triangle's own roof, from its corner flags.
int ownCornersOn(const TriangleCorners& t, bool hip) {
    const int bit = hip ? kCornerHip : kCornerRidge;
    return static_cast<int>(std::count_if(t.flags.begin(), t.flags.end(), [&](int f) { return (f & bit) != 0; }));
}

// The other roofs at the corners, by saved id.
std::vector<const RoofGrid*> touchingRoofs(const TriangleCorners& t) {
    std::vector<const RoofGrid*> roofs;
    for (const auto& at : t.meeting) {
        for (const CornerRoof& c : at) {
            if (std::find(roofs.begin(), roofs.end(), c.roof) == roofs.end()) {
                roofs.push_back(c.roof);
            }
        }
    }
    std::stable_sort(roofs.begin(), roofs.end(), [](const RoofGrid* a, const RoofGrid* b) { return a->id < b->id; });
    return roofs;
}

// Where own and another roof's ridges or hips meet over the whole triangle: whether the triangle is
// left out. hips and ridges: the corners the deciding other roof has on a hip and on its ridge.
bool ridgesLeaveOut(int hips, int ridges, int ownHips, int ownRidges) {
    const bool first = hips == 1 && (ownHips >= 2 || ownRidges >= 1);
    if (!first && (ridges != 1 || ownHips < 2)) {
        return false;
    }
    if (ridges == 1 && ownRidges == 1 && ownHips == 2 && hips == 2) {
        return false;
    }
    if ((ridges == 1 || ridges == 2) && ownRidges == ridges) {
        return ownHips != 1 || hips != 1;
    }
    return ownRidges != 3;
}

// The junction bits again from the roofs still joining: a corner joins where one of them is at the
// same height, unless that point is on its ridge or a hip bordering the way the triangle faces.
void rejoin(TriangleCorners& t, const std::vector<const RoofGrid*>& joining, int face) {
    for (std::size_t k = 0; k < 3; ++k) {
        t.flags[k] &= ~kCornerJunction;
        for (const RoofGrid* o : joining) {
            const CornerRoof* c = find(t.meeting[k], o);
            if (c && ((c->ridge | c->hip) & face) == 0) {
                t.flags[k] |= kCornerJunction;
            }
        }
    }
}

} // namespace

bool roofCoveredAt(const RoofGrid& asked, float x, float y, int askLevel, const std::vector<const RoofGrid*>& roofs) {
    if (!asked.inGrid(x, y)) {
        return false;
    }
    const float above = asked.heightFrom(x, y, askLevel) + kCoverMargin;
    for (const RoofGrid* other : roofs) {
        if (other == &asked) {
            continue;
        }
        float h = other->heightFrom(x, y, askLevel);
        // A roof of another slope counts only within its rectangle, and one grid step lower.
        if (!withinSteps(other->scale, asked.scale)) {
            if (other->inOverhang(x, y)) {
                continue;
            }
            h -= asked.scale;
        }
        if (!(h > above)) {
            continue;
        }
        if (slopesOrGables(other->type) && other->level == asked.level && other->inOverhang(x, y) &&
            !overhangCovers(asked, *other, x, y)) {
            continue;
        }
        // The game asks a roof on another level to rise a grid step higher only after the roof
        // slope cheat has changed the slope of new roofs, which loading a lot undoes.
        return true;
    }
    return false;
}

bool settleMeeting(TriangleCorners& t, const RoofGrid& roof, int face, std::array<float, 2> centre,
                   const std::vector<const RoofGrid*>& roofs) {
    // Ids at or above this are never taken as the lowest plain roof, as in the game.
    constexpr std::uint32_t kNoRoof = 10'000'000;
    std::vector<const RoofGrid*> touching = touchingRoofs(t);
    std::vector<const RoofGrid*> dropped;
    bool leaveOut = false;     // a roof meeting all three corners has no ridge or hip there
    bool ridgedAll = false;    // one that does
    std::uint32_t lowestId = kNoRoof;
    const RoofGrid* lowestPlain = nullptr;
    int hips = 3, ridges = 3;  // the deciding ridged roof's corners on a hip and on its ridge
    if (junctions(t) == 3) {
        // Roofs meeting all three corners never join them. Which of two such roofs keeps its top
        // is settled below.
        for (const RoofGrid* o : touching) {
            if (cornersMet(t, o) != 3) {
                continue;
            }
            const bool plain = cornersOn(t, o, true) == 0 && cornersOn(t, o, false) == 0;
            leaveOut = leaveOut || plain;
            if (!plain) {
                ridgedAll = true;
                const int u = cornersOn(t, o, true), i = cornersOn(t, o, false);
                if ((u == 1 && (hips >= 2 || ridges != 0)) || (i == 1 && hips >= 2)) {
                    hips = u;
                    ridges = i;
                }
            } else if (o->id < lowestId) {
                lowestId = o->id;
                lowestPlain = o;
            }
            for (std::size_t k = 0; k < 3; ++k) {
                auto& at = t.meeting[k];
                at.erase(std::remove_if(at.begin(), at.end(), [&](const CornerRoof& c) { return c.roof == o; }), at.end());
                if (at.empty()) {
                    t.flags[k] &= ~kCornerJunction;
                }
            }
            dropped.push_back(o);
        }
    } else {
        // A roof touching a corner that is on its ridge or a hip bordering the way the triangle
        // faces joins none of them.
        for (const RoofGrid* o : touching) {
            for (const auto& at : t.meeting) {
                const CornerRoof* c = find(at, o);
                if (c && ((c->ridge | c->hip) & face) != 0) {
                    dropped.push_back(o);
                    break;
                }
            }
        }
    }
    std::erase_if(touching, [&](const RoofGrid* o) { return std::find(dropped.begin(), dropped.end(), o) != dropped.end(); });
    // Corners joined by different roofs, none meeting all of them: only the first roof joins.
    if (const int left = junctions(t); left > 1 && dropped.empty() && !touching.empty() &&
                                       std::none_of(touching.begin(), touching.end(), [&](const RoofGrid* o) { return cornersMet(t, o) == left; })) {
        for (std::size_t k = 0; k < 3; ++k) {
            t.flags[k] = (t.flags[k] & ~kCornerJunction) | (meets(t.meeting[k], touching.front()) ? kCornerJunction : 0);
        }
    }
    const bool ownPlain = ownCornersOn(t, true) == 0 && ownCornersOn(t, false) == 0;
    bool out = false;
    if (leaveOut) {
        // Two plain tops on one another: the lower id keeps its triangle, the other keeps it only
        // where a third roof covers the lower one. A triangle on its own roof's ridge or hip keeps
        // it only there too.
        const bool lowerCovered = lowestPlain && roofCoveredAt(*lowestPlain, centre[0], centre[1], roof.level, roofs);
        out = ownPlain && roof.id <= lowestId ? false : !lowerCovered;
    } else if (ridgedAll && !ownPlain) {
        out = ridgesLeaveOut(hips, ridges, ownCornersOn(t, true), ownCornersOn(t, false));
    }
    if (!out && !dropped.empty()) {
        rejoin(t, touching, face);
    }
    return out;
}

} // namespace ts2::scene
