#pragma once

#include "scene/room_light.hpp"
#include "scene/wall_runs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::content {
struct Lot;
}

namespace ts2::scene::roomsamples {

// What each room's six light samples gather, as the game's lighting manager gathers them: every
// floor quarter (its covering's colour lit by the floor light below and the same light above),
// every wall face (its covering's colour lit by the wall light, toward the way it faces) and
// every portal (the light it lets in, toward the way it shines).

struct Settings {
    float saturation = 0.5f;  // roomSaturation: how much of a covering's colour shows
    float ceiling = 1;        // roomCeilingStrength
    float portal = 0.75f;     // roomPortalStrength
};

using Color = std::array<float, 3>;

// A covering's colour for room light. The game uses 0.5 grey for coverings it has no thumbnail of.
inline constexpr Color kNoCoveringColor{0.5f, 0.5f, 0.5f};

// One quarter of a floor tile: its covering's colour and the floor light at its middle (the floor
// light grid's sample there).
void addFloor(roomlight::Samples& samples, const Color& covering, const Color& floorLight, const Settings& settings);

// One wall face, walked from (x0, y0) to (x1, y1) so that it looks to its right: its covering's
// colour and the wall light the game reads at it.
void addWall(roomlight::Samples& samples, float x0, float y0, float x1, float y1, const Color& covering, const Color& wallLight,
             const Settings& settings);

// A portal shining into the room along (nx, ny) with the light it lets in.
void addPortal(roomlight::Samples& samples, float nx, float ny, const Color& light, const Settings& settings);

// What gathering a lot's room samples needs: rooms, coverings and the light on floors and walls.
struct LotInputs {
    // The room of a tile quarter (bit 0 -x, 1 +y, 2 +x, 3 -y), 0 or less: outdoors or none.
    std::function<std::int16_t(int level, int x, int y, int quarter)> quarterRoom;
    // The room in front of a wall face, 0 or less: outdoors or none.
    std::function<std::int16_t(const wallruns::Key& face)> faceRoom;
    // A covering's colour by its name. Nullopt where the game has none (it then uses kNoCoveringColor).
    std::function<std::optional<Color>(bool floor, const std::string& name)> covering;
    std::function<Color(int level, float x, float y)> floorLight;  // the floor light grid's sample
    std::function<Color(const wallruns::Key& face)> wallLight;    // the wall light the game reads at a face
};

// A portal's light toward the room it shines into.
struct PortalInput {
    std::int16_t into = 0;
    float nx = 0, ny = 0;
    Color light{0, 0, 0};
};

// Every room's samples, gathered in the game's order: floor quarters tile by tile (x outer, then
// y, then quarter), each wall of the room wall graph once with both faces (the face walked from
// its first point to its second shows the left covering), then the portals.
std::map<std::int16_t, roomlight::Samples> gather(const content::Lot& lot, const LotInputs& in, const std::vector<PortalInput>& portals,
                                                  const Settings& settings);

} // namespace ts2::scene::roomsamples
