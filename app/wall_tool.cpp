#include "wall_tool.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numbers>

namespace app {

namespace {

// Rounds a half to the even neighbour, as the game does.
int roundEven(float v) {
    return static_cast<int>(std::nearbyint(v));
}

// By direction: 0 is -x, 2 is -y, 4 is +x, 6 is +y, and the odd ones lie between.
constexpr std::array<std::array<int, 2>, 8> kSteps{{{-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}}};

int sign(int v) {
    return (v > 0) - (v < 0);
}

void addRun(std::vector<WallTool::Section>& out, const WallTool::Corner& a, const WallTool::Corner& b) {
    const int dx = b.x - a.x, dy = b.y - a.y;
    const int count = std::max(std::abs(dx), std::abs(dy));
    WallTool::Corner at = a;
    for (int i = 0; i < count; ++i) {
        const WallTool::Corner next{at.x + sign(dx), at.y + sign(dy), a.storey};
        out.push_back({at, next});
        at = next;
    }
}

std::vector<WallTool::Section> loop(const std::vector<WallTool::Corner>& corners) {
    std::vector<WallTool::Section> out;
    for (std::size_t i = 0; i < corners.size(); ++i) {
        addRun(out, corners[i], corners[(i + 1) % corners.size()]);
    }
    return out;
}

} // namespace

WallTool::Mode WallTool::modeFor(bool ctrl, bool shift, bool alt, Mode base) {
    if (ctrl && !alt) {
        return Mode::Demolish;
    }
    if (ctrl && alt) {
        return shift ? Mode::OctagonRoom : Mode::DiagonalRoom;
    }
    return shift && !alt ? Mode::Room : base;
}

WallTool::Mode WallTool::baseMode(int clone) {
    switch (clone) {
    case 1: return Mode::Room;
    case 2: return Mode::DiagonalRoom;
    case 3: return Mode::OctagonRoom;
    default: return Mode::Wall;
    }
}

WallTool::Corner WallTool::snap(float x, float y, int storey) {
    return {static_cast<int>(std::floor(x + 0.5f)), static_cast<int>(std::floor(y + 0.5f)), storey};
}

WallTool::Plan WallTool::line(const Corner& start, float x, float y) {
    const float dx = x - static_cast<float>(start.x), dy = y - static_cast<float>(start.y);
    // In eighths of a turn from -x.
    const float eighths = (std::atan2(dy, dx) + std::numbers::pi_v<float>) * 4.0f / std::numbers::pi_v<float>;
    const int direction = ((roundEven(eighths) % 8) + 8) % 8;
    const int count = std::max(std::abs(roundEven(dx)), std::abs(roundEven(dy)));
    const auto& step = kSteps[static_cast<std::size_t>(direction)];
    Plan plan{{start.x + step[0] * count, start.y + step[1] * count, start.storey}, {}, direction % 2 == 1 && count > 0};
    addRun(plan.sections, start, plan.end);
    return plan;
}

WallTool::Plan WallTool::room(const Corner& start, float x, float y) {
    const float dx = x - static_cast<float>(start.x), dy = y - static_cast<float>(start.y);
    const int width = std::abs(roundEven(dx)), depth = std::abs(roundEven(dy));
    const Corner end{start.x + (dx > 0 ? width : -width), start.y + (dy > 0 ? depth : -depth), start.storey};
    Plan plan{end, {}, false};
    if (width == 0 || depth == 0) {
        addRun(plan.sections, start, end);
    } else {
        plan.sections = loop({start, {end.x, start.y, start.storey}, end, {start.x, end.y, start.storey}});
        plan.shape = Plan::Shape::Rectangle;
    }
    return plan;
}

WallTool::Plan WallTool::diagonalRoom(const Corner& start, float x, float y) {
    // The quarter of its tile the mouse is in names the edge.
    const int tileX = static_cast<int>(std::floor(x)), tileY = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(tileX), fy = y - static_cast<float>(tileY);
    std::array<std::array<int, 2>, 2> edge{};
    if (fx <= fy) {
        edge = fx + fy < 1.0f ? std::array<std::array<int, 2>, 2>{{{tileX, tileY}, {tileX, tileY + 1}}}
                              : std::array<std::array<int, 2>, 2>{{{tileX, tileY + 1}, {tileX + 1, tileY + 1}}};
    } else {
        edge = fx + fy >= 1.0f ? std::array<std::array<int, 2>, 2>{{{tileX + 1, tileY}, {tileX + 1, tileY + 1}}}
                               : std::array<std::array<int, 2>, 2>{{{tileX, tileY}, {tileX + 1, tileY}}};
    }
    // Diagonals keep x + y even or odd, so the end is the edge's corner that matches the start.
    const auto odd = [](int a, int b) { return ((a + b) % 2 + 2) % 2; };
    const auto& end = odd(edge[0][0], edge[0][1]) == odd(start.x, start.y) ? edge[0] : edge[1];
    const int dx = end[0] - start.x, dy = end[1] - start.y;
    if (std::abs(dx) == std::abs(dy)) {
        return line(start, x, y);
    }
    // The other two corners are where diagonals from the start and the end meet.
    const Corner far{end[0], end[1], start.storey};
    const Corner p{(start.x - start.y + far.x + far.y) / 2, (start.y - start.x + far.x + far.y) / 2, start.storey};
    const Corner q{(start.x + far.x + start.y - far.y) / 2, (start.y + far.y + start.x - far.x) / 2, start.storey};
    return {far, loop({start, p, far, q}), true, Plan::Shape::Diamond};
}

WallTool::Plan WallTool::octagonRoom(const Corner& centre, int size, int turn) {
    // An odd size can't be centred, and the turn picks the side of its extra tile. Doubled coordinates
    // keep the halves whole.
    const int half = size / 2, rest = size - half;
    std::array<int, 2> near{1, -1}, far{-1, 1};
    switch (turn) {
    case 2: near = {1, 1}, far = {-1, -1}; break;
    case 4: near = {-1, 1}, far = {1, -1}; break;
    case 6: near = {-1, -1}, far = {1, 1}; break;
    default: break;
    }
    const int middleX = 2 * centre.x + near[0] * half + far[0] * rest, middleY = 2 * centre.y + near[1] * half + far[1] * rest;
    const int diagonal = size / 3;
    const int a = size - 2 * diagonal, b = size;
    const auto at = [&](int x, int y) { return Corner{(middleX + x) / 2, (middleY + y) / 2, centre.storey}; };
    const Corner end{centre.x + far[0] * rest, centre.y + far[1] * rest, centre.storey};
    return {end, loop({at(a, -b), at(b, -a), at(b, a), at(a, b), at(-a, b), at(-b, a), at(-b, -a), at(-a, -b)}), false, Plan::Shape::Octagon};
}

std::string WallTool::sizeText(const Corner& start, const Plan& plan) {
    const int alongX = start.x - plan.end.x;
    if (!plan.diagonal) {
        return std::format("{} x {}", std::abs(alongX), std::abs(start.y - plan.end.y));
    }
    // One side counts the diagonal steps, the other what is left of the x run.
    const int half = (alongX - plan.end.y + start.y) / 2;
    return std::format("{} x {} (diagonal)", std::abs(half), std::abs(half - alongX));
}

} // namespace app
