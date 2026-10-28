#include "scene/object_shadow.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace ts2::scene::objectshadow {

namespace {

constexpr int kFirstPortalType = 8;  // doors, windows, stairs, modular stairs and their portals
constexpr int kLastPortalType = 12;

bool portalType(int type) {
    return type >= kFirstPortalType && type <= kLastPortalType;
}

// Flags a wall-mounted object sets for itself, which keep its shadow groups outdoors too.
bool keepsShadowGroups(const Facts& f) {
    return (f.data13 & 0x3F) != 0 && (f.data8 & 0x06) != 0;
}

} // namespace

bool isShadowGroup(const std::string& group) {
    static constexpr std::array<std::string_view, 6> kNames{"groundshadow", "wallshadow", "northwallshadow",
                                                          "southwallshadow", "eastwallshadow", "westwallshadow"};
    return std::ranges::any_of(kNames, [&](std::string_view name) {
        return name.size() == group.size() && std::equal(name.begin(), name.end(), group.begin(), [](char a, char b) {
                   return a == std::tolower(static_cast<unsigned char>(b));
               });
    });
}

Kind requested(const Facts& f) {
    // Object data 0x44: 1 for none, 2 for the shadow groups, 3 for a live picture, else the sun's.
    if (f.data68 == 1) {
        return Kind::Model;
    }
    if (portalType(f.type)) {
        return f.hasShadowGroups ? Kind::Quad : Kind::Model;
    }
    if (!f.drawable) {
        return Kind::Model;
    }
    if (keepsShadowGroups(f) || f.data68 == 2) {
        return Kind::Quad;
    }
    return f.data68 == 3 ? Kind::Live : Kind::Sun;
}

Kind resolved(Kind requested, bool outdoors, const Facts& f) {
    switch (requested) {
    case Kind::Sun:
        return outdoors ? Kind::Sun : Kind::Quad;
    case Kind::Quad:
        // Outdoors only doors, windows, stairs and wall-mounted objects keep their shadow groups.
        return outdoors && !portalType(f.type) && !keepsShadowGroups(f) ? Kind::None : Kind::Quad;
    default:
        return requested;
    }
}

} // namespace ts2::scene::objectshadow
