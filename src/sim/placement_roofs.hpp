#pragma once

#include "content/lot.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace ts2::scene {
struct RoofGrid;
}

namespace ts2::sim {

// The placement test's roof check (code 0x2F): no room under a roof. When a roof covers the spot,
// the highest roof over it among those on its storey, the one below and the two above must not lie
// more than 0.1 but less than a storey (3.0) above the floor. A roof seen from a storey below its
// own counts only within its walls (not its overhang). A roof of type 15 counts as 0.2 above the
// floor wherever it is. Only grid roofs (types 1 to 6) have heights yet. Other roofs are not known.
class PlacementRoofs {
public:
    explicit PlacementRoofs(const content::Lot& lot);
    ~PlacementRoofs();
    PlacementRoofs(PlacementRoofs&&) noexcept;
    PlacementRoofs& operator=(PlacementRoofs&&) noexcept;

    // Whether a roof leaves no room at (x, y) on `level`, in the game's axes and levels (0 = ground).
    [[nodiscard]] bool tooLow(float x, float y, std::int32_t level) const;

private:
    struct Roof;
    float floorHeight(float x, float y, std::int32_t level) const;

    std::vector<Roof> roofs_;
    std::int32_t firstLevel_ = 0;
    std::shared_ptr<const array3d::Array3D> heights_;
};

} // namespace ts2::sim
