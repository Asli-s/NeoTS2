#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

// Build mode's wall tool. It works on tile corners in the lot's x and y, in tiles, and only makes the
// plan of a drag.
class WallTool {
public:
    // Ctrl demolishes, Shift draws rooms, Ctrl with Alt diagonal rooms, and Ctrl, Shift and Alt octagonal rooms.
    enum class Mode { Wall, Demolish, Room, DiagonalRoom, OctagonRoom };
    // In tiles across, also the size the tool starts with.
    static constexpr int kSmallestOctagon = 3;

    struct Corner {
        int x = 0, y = 0;
        int storey = 1;
        bool operator==(const Corner&) const = default;
    };

    // One tile's length of wall between two corners.
    using Section = std::array<Corner, 2>;
    struct Plan {
        Corner end;
        std::vector<Section> sections;
        bool diagonal = false;
        // Rooms also level the ground inside them.
        enum class Shape { Line, Rectangle, Diamond, Octagon };
        Shape shape = Shape::Line;
    };

    // With no keys held, and for Alt alone or Shift with Alt (not known yet), the mode is `base`.
    static Mode modeFor(bool ctrl, bool shift, bool alt, Mode base);
    // By the catalog entry's clone: 0 Wall Tool, 1 Room Tool, 2 Diagonal Room Tool, 3 Octagonal Room Tool.
    static Mode baseMode(int clone);
    static Corner snap(float x, float y, int storey);
    // A line in the nearest of the eight directions, as many tiles long as the mouse is from `start`
    // along the longer axis.
    static Plan line(const Corner& start, float x, float y);
    // One side of no length makes it a line.
    static Plan room(const Corner& start, float x, float y);
    // A diamond room to the corner of the mouse's tile edge that a diagonal from `start` can reach.
    // Corners on one diagonal make it a line instead.
    static Plan diagonalRoom(const Corner& start, float x, float y);
    // Diagonal sides are a third of `size`, rounded down. `turn` is in eighths (0, 2, 4 or 6).
    static Plan octagonRoom(const Corner& centre, int size, int turn);
    // The size shown beside the mouse, such as "4 x 0". Diagonal lines and diamond rooms count diagonal
    // steps and add " (diagonal)".
    static std::string sizeText(const Corner& start, const Plan& plan);
};

} // namespace app
