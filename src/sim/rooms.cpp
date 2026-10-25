#include "sim/rooms.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ts2::sim {

namespace {

// Bounds well beyond any real lot. A grid larger than this is bad data.
constexpr std::int32_t kMaxSide = 256;
constexpr std::int32_t kMaxLevels = 16;

constexpr int kTop = 0;
constexpr int kRight = 1;
constexpr int kBottom = 2;
constexpr int kLeft = 3;

struct Step {
    std::int32_t x0, y0, x1, y1;
};

// Splits a straight wall into one-tile steps (along an edge or across a tile's diagonal).
std::vector<Step> unitSteps(float fx0, float fy0, float fx1, float fy1) {
    for (const float v : {fx0, fy0, fx1, fy1}) {
        if (!std::isfinite(v) || std::abs(v) > static_cast<float>(kMaxSide) * 2) {
            diag::warnOnce("rooms: a wall past 512 tiles or with broken numbers divides no rooms");
            return {};
        }
    }
    const auto x0 = static_cast<std::int32_t>(std::lround(fx0));
    const auto y0 = static_cast<std::int32_t>(std::lround(fy0));
    const auto x1 = static_cast<std::int32_t>(std::lround(fx1));
    const auto y1 = static_cast<std::int32_t>(std::lround(fy1));
    const std::int32_t dx = x1 - x0;
    const std::int32_t dy = y1 - y0;
    std::vector<Step> steps;
    if (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy)) {
        return steps;
    }
    const std::int32_t n = std::max(std::abs(dx), std::abs(dy));
    const std::int32_t sx = dx > 0 ? 1 : dx < 0 ? -1 : 0;
    const std::int32_t sy = dy > 0 ? 1 : dy < 0 ? -1 : 0;
    for (std::int32_t i = 0; i < n; ++i) {
        steps.push_back({x0 + sx * i, y0 + sy * i, x0 + sx * (i + 1), y0 + sy * (i + 1)});
    }
    return steps;
}

} // namespace

std::size_t RoomMap::index(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const {
    return ((static_cast<std::size_t>(level - firstLevel_) * height_ + y) * width_ + x) * 4 + quarter;
}

RoomMap::RoomMap(const wallgraph::WallGraph& graph) : width_(graph.width), height_(graph.height) {
    if (graph.points.empty() || width_ <= 0 || height_ <= 0 || width_ > kMaxSide || height_ > kMaxSide) {
        if (width_ > kMaxSide || height_ > kMaxSide) {
            diag::warnOnce("rooms: a lot larger than 256 tiles has no rooms");
        }
        return;
    }
    std::unordered_map<std::int32_t, const wallgraph::Point*> points;
    std::int32_t lowest = graph.points.front().level;
    std::int32_t highest = lowest;
    for (const auto& p : graph.points) {
        points[p.id] = &p;
        lowest = std::min(lowest, p.level);
        highest = std::max(highest, p.level);
    }
    if (lowest < -kMaxLevels || highest > kMaxLevels || highest - lowest >= kMaxLevels) {
        diag::warnOnce("rooms: walls spread over more than 16 levels leave the lot without rooms");
        return;
    }
    firstLevel_ = lowest;
    levelCount_ = highest - lowest + 1;
    const std::size_t count = static_cast<std::size_t>(levelCount_) * width_ * height_ * 4;
    std::vector<std::uint8_t> blocked(count, 0);  // bit q: the side towards neighbour q is closed
    std::unordered_map<std::size_t, std::unordered_map<std::int32_t, int>> votes;
    quarterRoom_.assign(count, kNoRoom);

    auto inside = [&](std::int32_t x, std::int32_t y) { return x >= 0 && y >= 0 && x < width_ && y < height_; };
    auto vote = [&](std::int32_t level, std::int32_t x, std::int32_t y, int q, std::int32_t room) {
        if (inside(x, y)) {
            ++votes[index(level, x, y, q)][room];
        }
    };
    // Bits 0-3 close a triangle's outer edge. Bits 4-7 close its edge to the next triangle.
    auto closeOuter = [&](std::int32_t level, std::int32_t x, std::int32_t y, int q) {
        if (inside(x, y)) {
            blocked[index(level, x, y, q)] |= static_cast<std::uint8_t>(1u << q);
        }
    };
    auto closeInner = [&](std::int32_t level, std::int32_t x, std::int32_t y, int a, int b) {
        if (inside(x, y)) {
            blocked[index(level, x, y, a)] |= static_cast<std::uint8_t>(0x10u << b);
            blocked[index(level, x, y, b)] |= static_cast<std::uint8_t>(0x10u << a);
        }
    };

    for (const auto& wall : graph.walls) {
        const auto from = points.find(wall.from);
        const auto to = points.find(wall.to);
        if (from == points.end() || to == points.end()) {
            continue;
        }
        const std::int32_t level = from->second->level;
        for (const Step& s : unitSteps(from->second->x, from->second->y, to->second->x, to->second->y)) {
            if (s.x0 == s.x1) {
                const std::int32_t y = std::min(s.y0, s.y1);
                const bool down = s.y1 > s.y0;
                closeOuter(level, s.x0 - 1, y, kRight);
                closeOuter(level, s.x0, y, kLeft);
                vote(level, s.x0, y, kLeft, down ? wall.leftRoom : wall.rightRoom);
                vote(level, s.x0 - 1, y, kRight, down ? wall.rightRoom : wall.leftRoom);
            } else if (s.y0 == s.y1) {
                const std::int32_t x = std::min(s.x0, s.x1);
                const bool rightward = s.x1 > s.x0;
                closeOuter(level, x, s.y0 - 1, kBottom);
                closeOuter(level, x, s.y0, kTop);
                vote(level, x, s.y0 - 1, kBottom, rightward ? wall.leftRoom : wall.rightRoom);
                vote(level, x, s.y0, kTop, rightward ? wall.rightRoom : wall.leftRoom);
            } else {
                const std::int32_t x = std::min(s.x0, s.x1);
                const std::int32_t y = std::min(s.y0, s.y1);
                const bool mainDiagonal = (s.x1 - s.x0) == (s.y1 - s.y0);
                if (mainDiagonal) {
                    // Top-left to bottom-right: {top, right} | {bottom, left}.
                    closeInner(level, x, y, kTop, kLeft);
                    closeInner(level, x, y, kRight, kBottom);
                    const bool downRight = s.x1 > s.x0;
                    const std::int32_t upper = downRight ? wall.leftRoom : wall.rightRoom;
                    const std::int32_t lower = downRight ? wall.rightRoom : wall.leftRoom;
                    vote(level, x, y, kTop, upper);
                    vote(level, x, y, kRight, upper);
                    vote(level, x, y, kBottom, lower);
                    vote(level, x, y, kLeft, lower);
                } else {
                    // Top-right to bottom-left: {top, left} | {right, bottom}.
                    closeInner(level, x, y, kTop, kRight);
                    closeInner(level, x, y, kBottom, kLeft);
                    const bool downLeft = s.y1 > s.y0;
                    const std::int32_t lowerRight = downLeft ? wall.leftRoom : wall.rightRoom;
                    const std::int32_t upperLeft = downLeft ? wall.rightRoom : wall.leftRoom;
                    vote(level, x, y, kRight, lowerRight);
                    vote(level, x, y, kBottom, lowerRight);
                    vote(level, x, y, kTop, upperLeft);
                    vote(level, x, y, kLeft, upperLeft);
                }
            }
        }
    }

    std::vector<bool> seen(count, false);
    std::vector<std::size_t> region;
    std::vector<std::size_t> stack;
    for (std::int32_t level = firstLevel_; level < firstLevel_ + levelCount_; ++level) {
        for (std::int32_t y = 0; y < height_; ++y) {
            for (std::int32_t x = 0; x < width_; ++x) {
                for (int q = 0; q < 4; ++q) {
                    const std::size_t start = index(level, x, y, q);
                    if (seen[start]) {
                        continue;
                    }
                    region.clear();
                    bool edge = false;
                    seen[start] = true;
                    stack.push_back(start);
                    while (!stack.empty()) {
                        const std::size_t at = stack.back();
                        stack.pop_back();
                        region.push_back(at);
                        const int cq = static_cast<int>(at % 4);
                        const std::size_t tile = at / 4;
                        const auto tx = static_cast<std::int32_t>(tile % width_);
                        const auto ty = static_cast<std::int32_t>((tile / width_) % height_);
                        auto visit = [&](std::size_t next) {
                            if (!seen[next]) {
                                seen[next] = true;
                                stack.push_back(next);
                            }
                        };
                        for (const int other : {(cq + 1) % 4, (cq + 3) % 4}) {
                            if ((blocked[at] & (0x10u << other)) == 0) {
                                visit(tile * 4 + other);
                            }
                        }
                        if (blocked[at] & (1u << cq)) {
                            continue;
                        }
                        std::int32_t nx = tx;
                        std::int32_t ny = ty;
                        switch (cq) {
                        case kTop: --ny; break;
                        case kRight: ++nx; break;
                        case kBottom: ++ny; break;
                        default: --nx; break;
                        }
                        if (!inside(nx, ny)) {
                            edge = true;
                            continue;
                        }
                        visit(index(level, nx, ny, (cq + 2) % 4));
                    }

                    std::unordered_map<std::int32_t, int> total;
                    for (const std::size_t at : region) {
                        if (const auto it = votes.find(at); it != votes.end()) {
                            for (const auto& [room, n] : it->second) {
                                total[room] += n;
                            }
                        }
                    }
                    std::int32_t best = kNoRoom;
                    int bestVotes = 0;
                    for (const auto& [room, n] : total) {
                        if (n > bestVotes || (n == bestVotes && room < best)) {
                            best = room;
                            bestVotes = n;
                        }
                    }
                    if (best == kNoRoom) {
                        continue;
                    }
                    RoomInfo& info = rooms_[best];
                    if (info.quarterTiles == 0) {
                        info.id = best;
                        info.level = level;
                    }
                    info.quarterTiles += static_cast<std::int32_t>(region.size());
                    info.touchesLotEdge = info.touchesLotEdge || edge;
                    for (const std::size_t at : region) {
                        quarterRoom_[at] = best;
                    }
                }
            }
        }
    }
}

RoomMap::RoomMap(const array3d::Array3D& savedRooms, std::int32_t firstLevel) {
    if (savedRooms.elementSize != 8 || savedRooms.width > kMaxSide || savedRooms.height > kMaxSide ||
        savedRooms.levels > kMaxLevels) {
        if (savedRooms.width > kMaxSide || savedRooms.height > kMaxSide || savedRooms.levels > kMaxLevels) {
            diag::warnOnce("rooms: saved rooms larger than 256 tiles or 16 levels are dropped");
        }
        return;
    }
    width_ = static_cast<std::int32_t>(savedRooms.width);
    height_ = static_cast<std::int32_t>(savedRooms.height);
    levelCount_ = static_cast<std::int32_t>(savedRooms.levels);
    if (firstLevel < -kMaxLevels || firstLevel > kMaxLevels) {
        diag::warnOnce("rooms: saved rooms starting past level 16 are dropped");
        return;
    }
    firstLevel_ = firstLevel;
    quarterRoom_.assign(static_cast<std::size_t>(levelCount_) * width_ * height_ * 4, kNoRoom);
    for (std::int32_t l = 0; l < levelCount_; ++l) {
        const std::int32_t level = l + firstLevel_;
        for (std::int32_t x = 0; x < width_; ++x) {
            for (std::int32_t y = 0; y < height_; ++y) {
                for (int q = 0; q < 4; ++q) {
                    const std::int32_t room = savedRooms.i16(l, x, y, static_cast<std::size_t>(3 - q));
                    quarterRoom_[index(level, x, y, q)] = room;
                    RoomInfo& info = rooms_[room];
                    if (info.quarterTiles == 0) {
                        info.id = room;
                        info.level = level;
                    }
                    ++info.quarterTiles;
                    if (x == 0 || y == 0 || x == width_ - 1 || y == height_ - 1) {
                        info.touchesLotEdge = true;
                    }
                }
            }
        }
    }
}

void RoomMap::assignLayer(int layer, const RoomMap& regions) {
    if (layer < 0 || layer > 1) {
        return;
    }
    std::map<std::int32_t, std::map<std::int32_t, std::int32_t>> overlap;  // room -> region -> quarters
    for (std::int32_t level = firstLevel_; level < firstLevel_ + levelCount_; ++level) {
        for (std::int32_t y = 0; y < height_; ++y) {
            for (std::int32_t x = 0; x < width_; ++x) {
                for (int q = 0; q < 4; ++q) {
                    // Where the layer's walls enclose nothing (e.g. a level without walls) the game
                    // holds region 0, the one outdoors.
                    const std::int32_t region = regions.quarterRoom(level, x, y, q);
                    ++overlap[quarterRoom_[index(level, x, y, q)]][region == kNoRoom ? 0 : region];
                }
            }
        }
    }
    for (auto& [id, info] : rooms_) {
        const auto it = overlap.find(id);
        if (it == overlap.end()) {
            info.layerRegion[layer] = kNoRoom;
            continue;
        }
        const auto best = std::max_element(it->second.begin(), it->second.end(),
                                           [](const auto& a, const auto& b) { return a.second < b.second; });
        info.layerRegion[layer] = best->first;
    }
}

std::int32_t RoomMap::roomAt(std::int32_t level, float x, float y) const {
    if (quarterRoom_.empty() || level < firstLevel_ || level >= firstLevel_ + levelCount_ ||
        !(x >= 0 && y >= 0 && x < static_cast<float>(width_) && y < static_cast<float>(height_))) {
        return kNoRoom;
    }
    const auto tx = static_cast<std::int32_t>(x);
    const auto ty = static_cast<std::int32_t>(y);
    const float dx = x - static_cast<float>(tx) - 0.5f;
    const float dy = y - static_cast<float>(ty) - 0.5f;
    int quarter = kTop;
    if (std::abs(dx) > std::abs(dy)) {
        quarter = dx > 0 ? kRight : kLeft;
    } else if (dy > 0) {
        quarter = kBottom;
    }
    return quarterRoom_[index(level, tx, ty, quarter)];
}

std::int32_t RoomMap::quarterRoom(std::int32_t level, std::int32_t x, std::int32_t y, int quarter) const {
    if (quarterRoom_.empty() || level < firstLevel_ || level >= firstLevel_ + levelCount_ || x < 0 ||
        y < 0 || x >= width_ || y >= height_ || quarter < 0 || quarter > 3) {
        return kNoRoom;
    }
    return quarterRoom_[index(level, x, y, quarter)];
}

const RoomInfo* RoomMap::room(std::int32_t id) const {
    const auto it = rooms_.find(id);
    return it != rooms_.end() ? &it->second : nullptr;
}

} // namespace ts2::sim

namespace ts2::sim {

std::int32_t roomOnSideOfWall(const RoomMap& rooms, const wallgraph::Point& from, const wallgraph::Point& to) {
    // The tile beside the wall and where in it to look, a third of a tile from its centre toward the
    // wall's left side: straight across for a wall along a tile edge, toward the corner for a diagonal.
    constexpr float kThird = 1.0f / 3.0f;
    auto look = [&](float x, float y, float dx, float dy) {
        // A broken lot file's points may be far off or not numbers, and can't be made whole tiles.
        constexpr float kFar = 1e6f;
        if (!(std::abs(x) < kFar && std::abs(y) < kFar)) {
            return RoomMap::kNoRoom;
        }
        const auto tileX = static_cast<float>(static_cast<std::int32_t>(x));
        const auto tileY = static_cast<float>(static_cast<std::int32_t>(y));
        return rooms.roomAt(from.level, tileX + 0.5f + dx, tileY + 0.5f + dy);
    };
    if (from.x < to.x) {
        if (from.y < to.y) {
            return look(from.x, from.y, kThird, -kThird);
        }
        if (from.y > to.y) {
            return look(from.x, to.y, -kThird, -kThird);
        }
        return look(from.x, from.y - 1, 0, kThird);
    }
    if (from.x > to.x) {
        if (from.y > to.y) {
            return look(to.x, to.y, -kThird, kThird);
        }
        if (from.y < to.y) {
            return look(to.x, from.y, kThird, kThird);
        }
        return look(to.x, to.y, 0, -kThird);
    }
    if (from.y < to.y) {
        return look(from.x, from.y, -kThird, 0);
    }
    return look(to.x - 1, to.y, kThird, 0);
}

} // namespace ts2::sim
