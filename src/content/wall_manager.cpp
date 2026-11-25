#include "content/wall_manager.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>
#include <stack>
#include <tuple>
#include <unordered_map>

namespace ts2::content {

namespace {

constexpr std::uint8_t kMinusX = 1, kPlusY = 2, kPlusX = 4, kMinusY = 8;

// Toward zero, as the game converts.
std::int32_t toInt(float v) {
    return static_cast<std::int32_t>(v);
}

// The game takes floats no more than 32 representable values apart for the same corner.
bool sameCoordinate(float stored, float wanted) {
    const auto ordered = [](float v) {
        const auto bits = std::bit_cast<std::int32_t>(v);
        const auto sign = static_cast<std::uint32_t>(bits >> 31);
        return static_cast<std::int32_t>(((sign >> 1) ^ static_cast<std::uint32_t>(bits)) - sign);
    };
    const std::int64_t d = static_cast<std::int64_t>(ordered(stored)) - ordered(wanted);
    return d >= -32 && d <= 31;
}

// The quarter or half across a quarter's edge.
std::uint8_t opposite(std::uint8_t q) {
    switch (q) {
    case kMinusX: return kPlusX;
    case kPlusX: return kMinusX;
    case kPlusY: return kMinusY;
    case kMinusY: return kPlusY;
    case kMinusX | kPlusY: return kPlusX | kMinusY;
    case kPlusX | kMinusY: return kMinusX | kPlusY;
    case kPlusY | kPlusX: return kMinusX | kMinusY;
    case kMinusX | kMinusY: return kPlusY | kPlusX;
    default: return q;
    }
}

TileQuarter step(const TileQuarter& t, std::uint8_t q) {
    TileQuarter n = t;
    n.x += (q & kPlusX) ? 1 : 0;
    n.x -= (q & kMinusX) ? 1 : 0;
    n.y += (q & kPlusY) ? 1 : 0;
    n.y -= (q & kMinusY) ? 1 : 0;
    n.mask = opposite(q);
    return n;
}

// The quarters a room spreads to from a quarter: two in the same tile and one across its edge for a
// single quarter, the two across its edges and the other half of its tile for a half.
TileQuarter neighbour(const TileQuarter& t, int i) {
    const auto inTile = [&](std::uint8_t q) { return TileQuarter{t.x, t.y, t.level, q}; };
    switch (t.mask) {
    case kMinusX: return i == 0 ? inTile(kPlusY) : i == 1 ? inTile(kMinusY) : step(t, kMinusX);
    case kPlusY: return i == 0 ? inTile(kPlusX) : i == 1 ? inTile(kMinusX) : step(t, kPlusY);
    case kPlusX: return i == 0 ? inTile(kMinusY) : i == 1 ? inTile(kPlusY) : step(t, kPlusX);
    case kMinusY: return i == 0 ? inTile(kMinusX) : i == 1 ? inTile(kPlusX) : step(t, kMinusY);
    case kMinusX | kPlusY: return i == 0 ? step(t, kMinusX) : i == 1 ? step(t, kPlusY) : inTile(kPlusX | kMinusY);
    case kPlusY | kPlusX: return i == 0 ? step(t, kPlusY) : i == 1 ? step(t, kPlusX) : inTile(kMinusX | kMinusY);
    case kMinusX | kMinusY: return i == 0 ? step(t, kMinusY) : i == 1 ? step(t, kMinusX) : inTile(kPlusY | kPlusX);
    case kPlusX | kMinusY: return i == 0 ? step(t, kPlusX) : i == 1 ? step(t, kMinusY) : inTile(kMinusX | kPlusY);
    default: {
        const auto q = static_cast<std::uint8_t>(1u << i);
        TileQuarter n = step(t, q);
        n.mask = opposite(q);
        return n;
    }
    }
}

// A third of a tile from the tile's middle toward the quarter's sides.
std::array<float, 2> middleOf(const TileQuarter& t) {
    constexpr float kThird = 0.33333334f;
    float ox = 0, oy = 0;
    if (t.mask & kMinusX) {
        ox = -kThird;
    }
    if (t.mask & kPlusY) {
        oy = kThird;
    }
    if (t.mask & kPlusX) {
        ox += kThird;
    }
    if (t.mask & kMinusY) {
        oy -= kThird;
    }
    return {static_cast<float>(t.x) + 0.5f + ox, static_cast<float>(t.y) + 0.5f + oy};
}

} // namespace

TileQuarter leftQuarter(const WallSegment& s) {
    const std::int32_t ax = toInt(s.ax), ay = toInt(s.ay), bx = toInt(s.bx), by = toInt(s.by);
    if (s.bx > s.ax) {
        if (s.by > s.ay) {
            return {ax, ay, s.level, kPlusX | kMinusY};
        }
        if (s.by < s.ay) {
            return {ax, by, s.level, kMinusX | kMinusY};
        }
        return {ax, ay - 1, s.level, kPlusY};
    }
    if (s.bx < s.ax) {
        if (s.by < s.ay) {
            return {bx, by, s.level, kMinusX | kPlusY};
        }
        if (s.by > s.ay) {
            return {bx, ay, s.level, kPlusY | kPlusX};
        }
        return {bx, by, s.level, kMinusY};
    }
    if (s.by > s.ay) {
        return {ax, ay, s.level, kMinusX};
    }
    return {bx - 1, by, s.level, kPlusX};
}

TileQuarter quarterAt(float x, float y, std::int32_t level) {
    const float tileX = std::floor(x), tileY = std::floor(y);
    const float fx = x - tileX, fy = y - tileY;
    std::uint8_t q = 0;
    if (fx <= fy) {
        q = 1.0f - fx <= fy ? kPlusY : kMinusX;
    } else {
        q = 1.0f - fx <= fy ? kPlusX : kMinusY;
    }
    return {static_cast<std::int32_t>(tileX), static_cast<std::int32_t>(tileY), level, q};
}

RoomGrid::RoomGrid(array3d::Array3D& grid, std::int32_t firstLevel, const WallBounds& bounds)
    : grid_(grid), firstLevel_(firstLevel), bounds_(bounds) {}

std::uint16_t* RoomGrid::cell(std::int32_t level, std::int32_t x, std::int32_t y) {
    const std::int32_t index = level - firstLevel_;
    if (grid_.elementSize != 8 || !grid_.contains(index, x, y)) {
        return nullptr;
    }
    const std::size_t at = ((static_cast<std::size_t>(index) * grid_.width + static_cast<std::size_t>(x)) * grid_.height + static_cast<std::size_t>(y)) * 8;
    if (at + 8 > grid_.elements.size()) {
        return nullptr;
    }
    return reinterpret_cast<std::uint16_t*>(grid_.elements.data() + at);
}

std::uint16_t RoomGrid::roomAt(const TileQuarter& q) const {
    auto* self = const_cast<RoomGrid*>(this);
    const std::uint16_t* c = self->cell(q.level, q.x, q.y);
    if (c == nullptr) {
        return 0xFFFF;
    }
    for (int i = 0; i < 4; ++i) {
        if (q.mask & (1u << i)) {
            std::uint16_t v = 0;
            std::memcpy(&v, c + i, sizeof v);
            return v;
        }
    }
    return 0xFFFF;
}

void RoomGrid::set(std::uint16_t id, const TileQuarter& q) {
    std::uint16_t* c = cell(q.level, q.x, q.y);
    if (c == nullptr) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (q.mask & (1u << i)) {
            std::memcpy(c + i, &id, sizeof id);
        }
    }
}

bool RoomGrid::inBounds(float x, float y) const {
    return static_cast<float>(bounds_.minX) <= x && x <= static_cast<float>(bounds_.maxX) + 1.0f && static_cast<float>(bounds_.minY) <= y &&
           y <= static_cast<float>(bounds_.maxY) + 1.0f;
}

void RoomGrid::queue(const WallSegment& segment, std::uint16_t id) {
    const TileQuarter t = leftQuarter(segment);
    const auto key = [&](std::uint8_t q) { return Key{t.level, t.x, t.y, static_cast<std::int8_t>(q)}; };
    bool noSeed = false;
    if (std::has_single_bit(t.mask)) {
        // A half queued beside a single quarter keeps it from spreading.
        std::uint8_t a = 0, b = 0;
        switch (t.mask) {
        case kMinusX: a = kMinusX | kPlusY, b = kMinusX | kMinusY; break;
        case kPlusY: a = kPlusY | kPlusX, b = kMinusX | kPlusY; break;
        case kPlusX: a = kPlusX | kMinusY, b = kPlusY | kPlusX; break;
        default: a = kMinusX | kMinusY, b = kPlusX | kMinusY; break;
        }
        const auto first = queued_.find(key(a));
        if (first != queued_.end()) {
            first->second.noSeed = true;
        }
        if (const auto second = queued_.find(key(b)); second != queued_.end()) {
            second->second.noSeed = true;
            return;
        }
        if (first != queued_.end()) {
            return;
        }
    } else {
        // A half takes the place of its two quarters queued before it.
        for (int i = 0; i < 4; ++i) {
            const auto q = static_cast<std::uint8_t>(1u << i);
            if ((t.mask & q) != 0) {
                if (const auto it = queued_.find(key(q)); it != queued_.end()) {
                    queued_.erase(it);
                    noSeed = true;
                }
            }
        }
    }
    queued_.try_emplace(key(t.mask), Queued{id, noSeed});
}

void RoomGrid::flush() {
    if (queued_.empty()) {
        return;
    }
    const auto quarterOf = [](const Key& k) {
        return TileQuarter{std::get<1>(k), std::get<2>(k), std::get<0>(k), static_cast<std::uint8_t>(std::get<3>(k))};
    };
    const std::uint16_t oldId = roomAt(quarterOf(queued_.begin()->first));
    for (const auto& [k, q] : queued_) {
        set(q.id, quarterOf(k));
    }
    for (const auto& [k, q] : queued_) {
        if (!q.noSeed && oldId != q.id) {
            spread(oldId, q.id, neighbour(quarterOf(k), 0));
            spread(oldId, q.id, neighbour(quarterOf(k), 1));
        }
    }
    queued_.clear();
}

// The game spreads by recursion. A stack fills the same quarters, as the order doesn't matter.
void RoomGrid::spread(std::uint16_t oldId, std::uint16_t id, const TileQuarter& from) {
    std::stack<TileQuarter> next;
    next.push(from);
    while (!next.empty()) {
        const TileQuarter q = next.top();
        next.pop();
        if (roomAt(q) != oldId) {
            continue;
        }
        set(id, q);
        for (int i = 0; i < 3; ++i) {
            const TileQuarter n = neighbour(q, i);
            if (inBounds(static_cast<float>(n.x), static_cast<float>(n.y))) {
                next.push(n);
            }
        }
    }
}

struct WallLayer::Vertex {
    std::int32_t id = 0;
    float x = 0, y = 0;
    std::int32_t level = 0;
    std::optional<Ref> first;  // where the round starts
    int count = 0;
};

struct WallLayer::Face {
    std::int32_t id = 0;
};

struct WallLayer::Edge {
    std::int32_t id = 0;
    std::array<Ref, 4> next{};             // each record's next going round its origin
    std::array<Vertex*, 2> ends{};         // records 0 and 2
    std::array<Face*, 2> rooms{};          // records 1 and 3
};

WallLayer::Ref& WallLayer::onext(Ref e) {
    return e.edge->next[static_cast<std::size_t>(e.r)];
}

WallLayer::Vertex* WallLayer::org(Ref e) {
    return e.edge->ends[static_cast<std::size_t>(e.r / 2)];
}

WallLayer::Face*& WallLayer::face(Ref e) {
    return e.edge->rooms[static_cast<std::size_t>(e.r / 2)];
}

void WallLayer::splice(Ref a, Ref b) {
    const Ref alpha = rot(onext(a)), beta = rot(onext(b));
    std::swap(onext(a), onext(b));
    std::swap(onext(alpha), onext(beta));
}

float WallLayer::angle(Ref e) {
    const Vertex* a = org(e);
    const Vertex* b = dest(e);
    const float dx = b->x - a->x, dy = b->y - a->y;
    constexpr float kPi = std::numbers::pi_v<float>;
    if (dy == 0) {
        return dx > 0 ? 0.0f : kPi;
    }
    if (dx == 0) {
        return dy > 0 ? kPi / 2 : -kPi / 2;
    }
    if (dx == dy) {
        return dx > 0 ? 0.25f * kPi : -0.75f * kPi;
    }
    if (dx == -dy) {
        return dx > 0 ? -0.25f * kPi : 0.75f * kPi;
    }
    return static_cast<float>(std::atan2(static_cast<double>(dy), static_cast<double>(dx)));
}

WallSegment WallLayer::segmentOf(Ref e) {
    const Vertex* a = org(e);
    const Vertex* b = dest(e);
    return {a->x, a->y, b->x, b->y, a->level};
}

WallLayer::WallLayer(const wallgraph::WallGraph& saved) : header_(saved) {
    header_.points.clear();
    header_.rooms.clear();
    header_.walls.clear();
    std::unordered_map<std::int32_t, Vertex*> vertexById;
    for (const auto& p : saved.points) {
        auto v = std::make_unique<Vertex>();
        v->id = p.id;
        v->x = p.x;
        v->y = p.y;
        v->level = p.level;
        vertexById.try_emplace(p.id, v.get());
        vertices_.push_back(std::move(v));
    }
    for (const std::int32_t id : saved.rooms) {
        auto f = std::make_unique<Face>();
        f->id = id;
        faces_.push_back(std::move(f));
    }
    for (const auto& w : saved.walls) {
        const auto a = vertexById.find(w.from), b = vertexById.find(w.to);
        if (a == vertexById.end() || b == vertexById.end() || a->second == b->second) {
            continue;  // a broken file's section without two corners
        }
        auto edge = std::make_unique<Edge>();
        Edge* e = edge.get();
        e->id = w.id;
        e->next = {Ref{e, 0}, Ref{e, 3}, Ref{e, 2}, Ref{e, 1}};
        e->ends = {a->second, b->second};
        e->rooms = {faceById(w.leftRoom), faceById(w.rightRoom)};
        edges_.push_back(std::move(edge));
        link(e->ends[0], Ref{e, 0});
        link(e->ends[1], Ref{e, 2});
    }
}

WallLayer::~WallLayer() = default;

WallLayer::Vertex* WallLayer::findVertex(float x, float y, std::int32_t level) const {
    const auto less = [](const std::unique_ptr<Vertex>& v, const std::tuple<std::int32_t, float, float>& p) {
        const auto& [pl, px, py] = p;
        if (v->level != pl) {
            return v->level < pl;
        }
        return v->x == px ? v->y < py : v->x < px;
    };
    const auto it = std::lower_bound(vertices_.begin(), vertices_.end(), std::tuple{level, x, y}, less);
    if (it != vertices_.end() && (*it)->level == level && sameCoordinate((*it)->x, x) && sameCoordinate((*it)->y, y)) {
        return it->get();
    }
    return nullptr;
}

WallLayer::Vertex* WallLayer::findOrMake(float x, float y, std::int32_t level) {
    if (Vertex* v = findVertex(x, y, level)) {
        return v;
    }
    const auto less = [](const std::unique_ptr<Vertex>& v, const std::tuple<std::int32_t, float, float>& p) {
        const auto& [pl, px, py] = p;
        if (v->level != pl) {
            return v->level < pl;
        }
        return v->x == px ? v->y < py : v->x < px;
    };
    const auto at = std::lower_bound(vertices_.begin(), vertices_.end(), std::tuple{level, x, y}, less);
    auto v = std::make_unique<Vertex>();
    v->id = header_.counters[0]++;
    v->x = x;
    v->y = y;
    v->level = level;
    return vertices_.insert(at, std::move(v))->get();
}

// The ring round a corner goes by growing angle from its first section.
void WallLayer::link(Vertex* v, Ref e) {
    if (v->count > 0) {
        Ref slot = *v->first;
        if (v->count > 1) {
            const float a = angle(e);
            Ref f = *v->first;
            bool found = false;
            do {
                if (angle(f) > a) {
                    found = true;
                    break;
                }
                f = onext(f);
            } while (!(f == *v->first));
            slot = oprev(found ? f : *v->first);
        }
        splice(slot, e);
    }
    if (!v->first || angle(*v->first) > angle(e)) {
        v->first = e;
    }
    ++v->count;
}

void WallLayer::unlink(Vertex* v, Ref e) {
    const Ref n = onext(e);
    if (n == e) {
        v->first.reset();
    } else if (v->first && *v->first == e) {
        v->first = n;
    }
    --v->count;
}

std::optional<WallLayer::Ref> WallLayer::find(const WallSegment& s) const {
    Vertex* a = findVertex(s.ax, s.ay, s.level);
    Vertex* b = findVertex(s.bx, s.by, s.level);
    if (a == nullptr || b == nullptr || !a->first) {
        return std::nullopt;
    }
    Ref f = *a->first;
    do {
        if (dest(f) == b) {
            return f;
        }
        f = onext(f);
    } while (!(f == *a->first));
    return std::nullopt;
}

bool WallLayer::contains(const WallSegment& segment) const {
    return find(segment).has_value();
}

std::optional<WallSegment> WallLayer::storedOf(const WallSegment& segment) const {
    const auto e = find(segment);
    if (!e) {
        return std::nullopt;
    }
    return segmentOf(Ref{e->edge, 0});
}

std::vector<WallSegment> WallLayer::segmentsAt(float x, float y, std::int32_t level) const {
    std::vector<WallSegment> out;
    const Vertex* v = findVertex(x, y, level);
    if (v == nullptr || !v->first) {
        return out;
    }
    Ref e = *v->first;
    do {
        out.push_back(segmentOf(e));
        e = onext(e);
    } while (!(e == *v->first) && out.size() < static_cast<std::size_t>(v->count));
    return out;
}

std::int32_t WallLayer::idOf(const WallSegment& segment) const {
    const auto e = find(segment);
    return e ? e->edge->id : -1;
}

bool WallLayer::inOrbit(Ref start, Ref e) const {
    Ref f = start;
    do {
        if (f == e) {
            return true;
        }
        f = faceNext(f);
    } while (!(f == start));
    return false;
}

// Whether the loop crosses a line from the section's left quarter an even number of times, so its left
// side is outside the loop.
bool WallLayer::evenCrossings(Ref e) const {
    const TileQuarter q = leftQuarter(segmentOf(e));
    int crossings = 0;
    for (Ref f = faceNext(e); !(f == e); f = faceNext(f)) {
        const TileQuarter o = leftQuarter(segmentOf(f));
        bool counts = false;
        switch (q.mask) {
        case 3:
        case 6:
        case 8:
            counts = (o.mask & 0xA) && o.x == q.x && (o.y > q.y || (o.y == q.y && (q.mask == 8 || (q.mask & o.mask))));
            break;
        case 2:
        case 9:
        case 12:
            counts = (o.mask & 0xA) && o.x == q.x && (o.y < q.y || (o.y == q.y && (q.mask == 2 || (q.mask & o.mask))));
            break;
        case 4: counts = (o.mask & 5) && o.y == q.y && o.x <= q.x; break;
        case 1: counts = (o.mask & 5) && o.y == q.y && o.x >= q.x; break;
        default: break;
        }
        crossings += counts ? 1 : 0;
    }
    return crossings % 2 == 0;
}

WallLayer::Face* WallLayer::faceById(std::int32_t id) const {
    const auto it = std::lower_bound(faces_.begin(), faces_.end(), id, [](const std::unique_ptr<Face>& f, std::int32_t v) { return f->id < v; });
    return it != faces_.end() && (*it)->id == id ? it->get() : nullptr;
}

// A new room takes the smallest id not in use.
WallLayer::Face* WallLayer::newFace() {
    std::size_t i = 0;
    while (i < faces_.size() && faces_[i]->id <= static_cast<std::int32_t>(i)) {
        ++i;
    }
    auto f = std::make_unique<Face>();
    f->id = static_cast<std::int32_t>(i);
    return faces_.insert(faces_.begin() + static_cast<std::ptrdiff_t>(i), std::move(f))->get();
}

void WallLayer::reassign(Face* f, const std::function<std::int32_t(const WallSegment&)>& roomOf) {
    for (const auto& edge : edges_) {
        const Ref e{edge.get(), 0};
        if (roomOf(segmentOf(e)) == f->id) {
            face(rot(e)) = f;
        }
        if (roomOf(segmentOf(sym(e))) == f->id) {
            face(invRot(e)) = f;
        }
    }
}

// A section closing a loop splits the room, the new room going on the side the loop encloses.
void WallLayer::addRooms(Ref e, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf) {
    const bool open = inOrbit(e, sym(e));
    if (grid == nullptr) {
        Face* outside = faces_.empty() ? nullptr : faces_.front().get();
        face(rot(e)) = face(invRot(e)) = outside;
        return;
    }
    Face* old = faceById(grid->roomAt(leftQuarter(segmentOf(e))));
    if (open) {
        face(rot(e)) = face(invRot(e)) = old;
        return;
    }
    Face* added = newFace();
    Ref start = e;
    if (old != nullptr && old->id == 0 && evenCrossings(e)) {
        start = sym(e);
        face(rot(e)) = old;
    } else {
        face(invRot(e)) = old;
    }
    Ref f = start;
    do {
        face(rot(f)) = added;
        grid->queue(segmentOf(f), static_cast<std::uint16_t>(added->id));
        f = faceNext(f);
    } while (!(f == start));
    grid->flush();
    reassign(added, roomOf);
}

// The right room's id stays, unless the left one is the outdoors (room 0).
void WallLayer::joinRooms(Ref e, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf) {
    if (grid == nullptr || inOrbit(e, sym(e))) {
        return;
    }
    Face* left = face(rot(e));
    Face* right = face(invRot(e));
    if (left == nullptr || right == nullptr || left->id == right->id) {
        return;
    }
    Face* kept = right;
    Face* gone = left;
    Ref start = e;
    if (left->id == 0) {
        kept = left;
        gone = right;
        start = sym(e);
    }
    Ref f = start;
    do {
        face(rot(f)) = kept;
        grid->queue(segmentOf(f), static_cast<std::uint16_t>(kept->id));
        f = faceNext(f);
    } while (!(f == start));
    grid->flush();
    reassign(kept, roomOf);
    for (const auto& edge : edges_) {
        for (auto& room : edge->rooms) {
            if (room == gone) {
                room = nullptr;
            }
        }
    }
    std::erase_if(faces_, [&](const std::unique_ptr<Face>& p) { return p.get() == gone; });
}

void WallLayer::add(const WallSegment& s, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf) {
    Vertex* a = findOrMake(s.ax, s.ay, s.level);
    Vertex* b = findOrMake(s.bx, s.by, s.level);
    auto edge = std::make_unique<Edge>();
    Edge* e = edge.get();
    e->id = header_.counters[1]++;
    e->next = {Ref{e, 0}, Ref{e, 3}, Ref{e, 2}, Ref{e, 1}};
    e->ends = {a, b};
    link(a, Ref{e, 0});
    link(b, Ref{e, 2});
    addRooms(Ref{e, 0}, grid, roomOf);
    edges_.push_back(std::move(edge));
}

bool WallLayer::remove(const WallSegment& s, RoomGrid* grid, const std::function<std::int32_t(const WallSegment&)>& roomOf) {
    const auto found = find(s);
    if (!found) {
        return false;
    }
    const Ref e{found->edge, 0};
    Vertex* a = org(e);
    Vertex* b = dest(e);
    joinRooms(e, grid, roomOf);
    unlink(a, e);
    if (a->count > 0) {
        splice(e, oprev(e));
    }
    unlink(b, sym(e));
    if (b->count > 0) {
        splice(sym(e), oprev(sym(e)));
    }
    for (Vertex* v : {a, b}) {
        if (v->count == 0) {
            std::erase_if(vertices_, [&](const std::unique_ptr<Vertex>& p) { return p.get() == v; });
        }
    }
    std::erase_if(edges_, [&](const std::unique_ptr<Edge>& p) { return p.get() == e.edge; });
    return true;
}

wallgraph::WallGraph WallLayer::saved() const {
    wallgraph::WallGraph g = header_;
    for (const auto& v : vertices_) {
        g.points.push_back({v->id, v->x, v->y, v->level});
    }
    for (const auto& f : faces_) {
        g.rooms.push_back(f->id);
    }
    for (const auto& e : edges_) {
        const auto roomId = [](const Face* f) { return f != nullptr ? f->id : 0; };
        g.walls.push_back({e->id, e->ends[0]->id, roomId(e->rooms[0]), e->ends[1]->id, roomId(e->rooms[1])});
    }
    return g;
}

namespace {

// The number by each is the wall graph instance it is saved as.
enum LayerIndex : std::size_t {
    kSpanFloorToFloor,       // 22: drawn walls a storey high
    kSpanBasedCeiling,       // 8: drawn walls up to the floor above's thickness
    kSpanFloorToRoof,        // 23: drawn walls up to the roof
    kArches,                 // 19
    kLightBlocking,          // 7: walls that stop light, with grid 11
    kLoadBearing,            // 6: walls that hold up the storey above, with grid 10
    kPlacementBlocking,      // 5: the rooms, with grid 9
    kAllWalls,               // 24: every wall, the one the coverings name
    kHalfWalls,              // 25: half walls
    kInvisibleLightBlocking, // 26
    kStageSides,             // 28
    kStageHalfWalls,         // 31: half walls with both ends beside a stage
    kLayerCount
};
constexpr std::array<std::uint32_t, kLayerCount> kInstances{22, 8, 23, 19, 7, 6, 5, 24, 25, 26, 28, 31};
// By room grid kind: light blocking, load bearing and placement blocking.
constexpr std::array<std::uint32_t, 3> kGridInstances{11, 10, 9};
constexpr std::array<LayerIndex, 3> kGridLayers{kLightBlocking, kLoadBearing, kPlacementBlocking};
constexpr std::uint32_t kAtticStyle = 3, kStageWallStyle = 301, kInvisibleWallStyle = 300;
// A stage's quarters in the platform region grid (instance 0x14).
constexpr std::int32_t kStageRegion = 4;
constexpr std::uint32_t kPlatformRegionGrid = 0x14;

} // namespace

struct WallManager::Layers {
    std::array<std::unique_ptr<WallLayer>, kLayerCount> layers;
    std::array<std::optional<RoomGrid>, 3> grids;
};

WallManager::WallManager(std::map<std::uint32_t, wallgraph::WallGraph>& graphs, std::map<std::uint32_t, array3d::Array3D>& grids,
                         std::vector<walllayer::Entry>& coverings, std::map<std::uint16_t, stringmap::Entry>& patterns,
                         const WallBounds& bounds, const BuildScripts& scripts)
    : graphs_(graphs), coverings_(coverings), patterns_(patterns), bounds_(bounds), scripts_(scripts), layers_(std::make_unique<Layers>()) {
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        if (const auto it = graphs.find(kInstances[i]); it != graphs.end()) {
            layers_->layers[i] = std::make_unique<WallLayer>(it->second);
        }
    }
    const auto rooms = graphs.find(5);
    const std::int32_t firstLevel = rooms != graphs.end() ? rooms->second.firstLevel : 0;
    firstLevel_ = firstLevel;
    if (const auto it = grids.find(kPlatformRegionGrid); it != grids.end()) {
        regions_ = &it->second;
    }
    for (std::size_t k = 0; k < 3; ++k) {
        if (const auto it = grids.find(kGridInstances[k]); it != grids.end()) {
            layers_->grids[k].emplace(it->second, firstLevel, bounds);
        }
    }
}

WallManager::~WallManager() = default;

// Its covering's style, or with none the stage side style when the stage layer has the section.
std::uint32_t WallManager::styleOf(const WallSegment& segment) const {
    if (const auto& all = layers_->layers[kAllWalls]) {
        const std::int32_t id = all->idOf(segment);
        if (id >= 0) {
            for (const auto& c : coverings_) {
                if (static_cast<std::int32_t>(c.wall) == id) {
                    return c.type;
                }
            }
        }
    }
    const auto& stage = layers_->layers[kStageSides];
    return stage && stage->contains(segment) ? kStageWallStyle : 0;
}

// Read at the middle of the left quarter. Next to an attic wall, light passes only where the
// rooms grid (grid 9) has a room.
std::int32_t WallManager::roomByPoint(const WallSegment& segment, int kind) const {
    const TileQuarter side = leftQuarter(segment);
    const auto middle = middleOf(side);
    const TileQuarter at = quarterAt(middle[0], middle[1], side.level);
    const auto region = [&](int k) -> std::int32_t {
        const auto& grid = layers_->grids[static_cast<std::size_t>(k)];
        return grid ? grid->roomAt(at) : 0xFFFF;
    };
    if (kind == 0 && styleOf(segment) == kAtticStyle && region(2) == 0) {
        return 0;
    }
    return region(kind);
}

// Whether a quarter touching the corner is in a stage's platform region.
bool WallManager::cornerOnStage(float x, float y, std::int32_t level) const {
    if (regions_ == nullptr || regions_->elementSize != 16) {
        return false;
    }
    struct Near {
        std::int32_t dx, dy;
        std::uint8_t quarters;
    };
    constexpr std::array<Near, 4> kNear{{{-1, -1, kPlusY | kPlusX}, {0, -1, kMinusX | kPlusY}, {0, 0, kMinusX | kMinusY}, {-1, 0, kPlusX | kMinusY}}};
    const auto cx = static_cast<std::int32_t>(std::nearbyint(x)), cy = static_cast<std::int32_t>(std::nearbyint(y));
    const std::int32_t index = level - firstLevel_;
    for (const auto& n : kNear) {
        const std::int32_t tx = cx + n.dx, ty = cy + n.dy;
        if (tx < bounds_.minX || tx > bounds_.maxX || ty < bounds_.minY || ty > bounds_.maxY || level < bounds_.minLevel ||
            level > bounds_.maxLevel || !regions_->contains(index, tx, ty)) {
            continue;
        }
        const auto cell = regions_->at(index, tx, ty);
        for (std::size_t q = 0; q < 4; ++q) {
            std::int32_t region = 0;
            if ((n.quarters & (1u << q)) != 0 && cell.size() >= (q + 1) * sizeof region) {
                std::memcpy(&region, cell.data() + q * sizeof region, sizeof region);
                if (region == kStageRegion) {
                    return true;
                }
            }
        }
    }
    return false;
}

bool WallManager::isHalfWall(std::uint32_t style) const {
    const auto fence = fenceOf_ ? fenceOf_(style) : std::nullopt;
    return fence && fence->halfWall;
}

namespace {

// Within 32 representable floats of zero, as the game compares lengths.
bool nearZero(float v) {
    const auto ordered = [](float f) {
        const auto bits = std::bit_cast<std::int32_t>(f);
        return bits < 0 ? static_cast<std::int32_t>(0x80000000u - static_cast<std::uint32_t>(bits)) : bits;
    };
    const std::int64_t d = static_cast<std::int64_t>(ordered(v)) - ordered(0.0f);
    return d > -32 && d < 32;
}

bool samePlace(const fencepostlayer::Post& p, float x, float y, std::int32_t level) {
    return p.level == level && p.x == x && p.y == y;
}

} // namespace

bool WallManager::needsPost(float x, float y, std::int32_t level) const {
    std::vector<WallSegment> fences;
    std::set<std::uint32_t> spaced;
    for (const auto& w : wallsAt(x, y, level)) {
        if (const auto fence = fenceOf_ ? fenceOf_(w.style) : std::nullopt) {
            fences.push_back(w.segment);
            if (fence->postSpacing > 0) {
                spaced.insert(w.style);
            }
        }
    }
    const std::size_t n = fences.size();
    if (n == 1 || n > 2 || spaced.size() > 1) {
        return true;
    }
    if (n == 2) {
        // Two in a line through the corner need none.
        const float ux = fences[0].bx - x, uy = fences[0].by - y, vx = fences[1].bx - x, vy = fences[1].by - y;
        if (!nearZero(std::fabs(ux * vy - uy * vx))) {
            return true;
        }
    }
    const auto& arches = layers_->layers[kArches];
    if (arches && !arches->segmentsAt(x, y, level).empty()) {
        return true;
    }
    const bool hasPost = posts_ != nullptr && std::any_of(posts_->begin(), posts_->end(), [&](const auto& p) { return samePlace(p, x, y, level); });
    return hasPost && !spaced.empty();
}

WallManager::PostChanges WallManager::addRunPosts(std::uint32_t style, float ax, float ay, float bx, float by, std::int32_t level, int count,
                                                  const std::function<bool(float, float)>& built) {
    PostChanges changes;
    const auto fence = fenceOf_ ? fenceOf_(style) : std::nullopt;
    if (!fence || posts_ == nullptr || count <= 0) {
        return changes;
    }
    const float stepX = (bx - ax) / static_cast<float>(count), stepY = (by - ay) / static_cast<float>(count);
    float x = ax, y = ay;
    for (int i = 0; i <= count; ++i, x += stepX, y += stepY) {
        if (!built(x, y)) {
            continue;
        }
        const bool post = i == 0 || i == count || needsPost(x, y, level) || (fence->postSpacing > 0 && i % fence->postSpacing == 0);
        if (!post) {
            continue;
        }
        const fencepostlayer::Post p{level, x, y, style};
        if (addPost(p)) {
            changes.added.push_back(p);
        } else if (const auto was = refreshPost(x, y, level)) {
            changes.replaced.push_back(*was);
        }
    }
    return changes;
}

std::optional<fencepostlayer::Post> WallManager::refreshPost(float x, float y, std::int32_t level) {
    const auto post = postAt(x, y, level);
    if (!post || !fenceOf_ || !fenceOf_(post->fence)) {
        return std::nullopt;
    }
    const auto style = postStyleAt(x, y, level);
    if (!style || *style == post->fence) {
        return std::nullopt;
    }
    for (auto& p : *posts_) {
        if (samePlace(p, x, y, level)) {
            p.fence = *style;
        }
    }
    return post;
}

bool WallManager::addPost(const fencepostlayer::Post& post) {
    if (posts_ == nullptr) {
        return false;
    }
    const auto key = [](const fencepostlayer::Post& p) { return std::tuple{p.level, p.x, p.y}; };
    const auto at = std::lower_bound(posts_->begin(), posts_->end(), post, [&](const auto& a, const auto& b) { return key(a) < key(b); });
    if (at != posts_->end() && key(*at) == key(post)) {
        return false;
    }
    posts_->insert(at, post);
    return true;
}

std::optional<fencepostlayer::Post> WallManager::postAt(float x, float y, std::int32_t level) const {
    if (posts_ != nullptr) {
        for (const auto& p : *posts_) {
            if (samePlace(p, x, y, level)) {
                return p;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> WallManager::postStyleAt(float x, float y, std::int32_t level) const {
    const auto& arches = layers_->layers[kArches];
    if (arches && !arches->segmentsAt(x, y, level).empty()) {
        return std::nullopt;
    }
    std::optional<std::uint32_t> style;
    float tallest = -1;
    for (const auto& w : wallsAt(x, y, level)) {
        if (const auto fence = fenceOf_ ? fenceOf_(w.style) : std::nullopt; fence && fence->height > tallest) {
            tallest = fence->height;
            style = w.style;
        }
    }
    return style;
}

bool WallManager::removePost(float x, float y, std::int32_t level) {
    if (posts_ == nullptr) {
        return false;
    }
    return std::erase_if(*posts_, [&](const auto& p) { return samePlace(p, x, y, level); }) > 0;
}

// Half walls with both ends beside a stage have a layer of their own.
std::size_t WallManager::halfWallLayer(const WallSegment& segment) const {
    const bool stage = cornerOnStage(segment.ax, segment.ay, segment.level) && cornerOnStage(segment.bx, segment.by, segment.level);
    return stage ? kStageHalfWalls : kHalfWalls;
}

void WallManager::addPattern(const std::string& name) {
    if (name.empty()) {
        return;
    }
    for (auto& [id, p] : patterns_) {
        if (p.name == name) {
            ++p.uses;
            return;
        }
    }
    std::uint16_t id = 0;
    while (patterns_.contains(id)) {
        ++id;
    }
    patterns_[id] = {name, 1};
}

std::uint16_t WallManager::patternId(const std::string& name) const {
    for (const auto& [id, p] : patterns_) {
        if (!name.empty() && p.name == name) {
            return id;
        }
    }
    return 0xFFFF;
}

void WallManager::releasePattern(std::uint16_t id) {
    const auto it = patterns_.find(id);
    if (it == patterns_.end()) {
        return;
    }
    if (it->second.uses <= 1) {
        patterns_.erase(it);
    } else {
        --it->second.uses;
    }
}

bool WallManager::add(std::uint32_t style, const std::string& left, const std::string& right, const WallSegment& segment) {
    const bool halfWall = isHalfWall(style);
    const auto found = scripts_.wallStyles.find(style);
    if (!halfWall && (found == scripts_.wallStyles.end() || style == kInvisibleWallStyle || style == kStageWallStyle ||
                      found->second.kind != WallStyle::Kind::Plain)) {
        return false;
    }
    // A half wall divides rooms but neither holds up the storey above nor stops light.
    WallStyle d = halfWall ? WallStyle{} : found->second;
    if (halfWall) {
        d.bearsLoad = d.blocksLight = d.rendered = false;
    }
    const auto onLot = [&](const TileQuarter& t) {
        return bounds_.minX <= t.x && t.x <= bounds_.maxX && bounds_.minY <= t.y && t.y <= bounds_.maxY && bounds_.minLevel <= t.level &&
               t.level <= bounds_.maxLevel;
    };
    if (!onLot(leftQuarter(segment)) && !onLot(leftQuarter(segment.reversed()))) {
        return false;
    }
    // A section already standing would go into the graphs twice.
    if (has(segment)) {
        return false;
    }
    auto& L = layers_->layers;
    const auto put = [&](LayerIndex i, bool unlessThere) {
        if (!L[i] || (unlessThere && L[i]->contains(segment))) {
            return;
        }
        std::size_t k = 3;
        for (std::size_t g = 0; g < 3; ++g) {
            k = kGridLayers[g] == i ? g : k;
        }
        RoomGrid* grid = k < 3 && layers_->grids[k] ? &*layers_->grids[k] : nullptr;
        const int kind = static_cast<int>(k);
        L[i]->add(segment, grid, [&](const WallSegment& s) { return roomByPoint(s, kind); });
    };
    put(kAllWalls, false);
    if (d.blocksPlacement) {
        put(kPlacementBlocking, true);
    }
    if (halfWall) {
        put(static_cast<LayerIndex>(halfWallLayer(segment)), false);
    }
    if (d.bearsLoad) {
        put(kLoadBearing, false);
    }
    if (d.blocksLight) {
        put(kLightBlocking, true);
    }
    if (d.rendered) {
        switch (d.span) {
        case WallStyle::Span::FloorToFloor: put(kSpanFloorToFloor, false); break;
        case WallStyle::Span::FloorToFloorBasedCeiling: put(kSpanBasedCeiling, false); break;
        case WallStyle::Span::FloorToRoof: put(kSpanFloorToRoof, false); break;
        default: break;
        }
    }
    if (!L[kAllWalls]) {
        return true;
    }
    const std::int32_t id = L[kAllWalls]->idOf(segment);
    addPattern(left);
    addPattern(right);
    walllayer::Entry entry{static_cast<std::uint32_t>(id), style, patternId(left), patternId(right)};
    const auto at = std::lower_bound(coverings_.begin(), coverings_.end(), entry.wall,
                                     [](const walllayer::Entry& c, std::uint32_t w) { return c.wall < w; });
    if (at != coverings_.end() && at->wall == entry.wall) {
        *at = entry;
    } else {
        coverings_.insert(at, entry);
    }
    return true;
}

std::vector<WallManager::WallAt> WallManager::wallsAt(float x, float y, std::int32_t level) const {
    std::vector<WallAt> out;
    if (const auto& all = layers_->layers[kAllWalls]) {
        for (const auto& s : all->segmentsAt(x, y, level)) {
            out.push_back({s, styleOf(s)});
        }
    }
    return out;
}

std::optional<std::uint32_t> WallManager::styleOn(const WallSegment& segment) const {
    if (!has(segment)) {
        return std::nullopt;
    }
    return styleOf(segment);
}

std::optional<WallManager::Kept> WallManager::keptOn(const WallSegment& segment) const {
    const auto& all = layers_->layers[kAllWalls];
    const auto stored = all ? all->storedOf(segment) : std::nullopt;
    if (!stored) {
        return std::nullopt;
    }
    Kept kept{*stored, styleOf(*stored), {}, {}};
    const std::int32_t id = all->idOf(*stored);
    const auto name = [&](std::uint16_t pattern) {
        const auto p = patterns_.find(pattern);
        return p != patterns_.end() ? p->second.name : std::string();
    };
    for (const auto& c : coverings_) {
        if (static_cast<std::int32_t>(c.wall) == id) {
            kept.left = name(c.left);
            kept.right = name(c.right);
            break;
        }
    }
    return kept;
}

bool WallManager::has(const WallSegment& segment) const {
    const auto& all = layers_->layers[kAllWalls];
    return all && all->contains(segment);
}

bool WallManager::remove(const WallSegment& segment) {
    auto& L = layers_->layers;
    if (!L[kAllWalls] || !L[kAllWalls]->contains(segment)) {
        return false;
    }
    const std::uint32_t style = styleOf(segment);
    const bool halfWall = isHalfWall(style);
    const auto found = scripts_.wallStyles.find(style);
    if (!halfWall && (found == scripts_.wallStyles.end() || found->second.kind != WallStyle::Kind::Plain)) {
        return false;  // fences and the other kinds are not done yet
    }
    WallStyle d = halfWall ? WallStyle{} : found->second;
    if (halfWall) {
        d.bearsLoad = d.blocksLight = d.rendered = false;
    }
    const std::int32_t id = L[kAllWalls]->idOf(segment);
    if (const auto c = std::find_if(coverings_.begin(), coverings_.end(), [&](const walllayer::Entry& e) { return static_cast<std::int32_t>(e.wall) == id; });
        c != coverings_.end()) {
        releasePattern(c->left);
        releasePattern(c->right);
        coverings_.erase(c);
    }
    const auto take = [&](LayerIndex i) {
        if (!L[i]) {
            return;
        }
        std::size_t k = 3;
        for (std::size_t g = 0; g < 3; ++g) {
            k = kGridLayers[g] == i ? g : k;
        }
        RoomGrid* grid = k < 3 && layers_->grids[k] ? &*layers_->grids[k] : nullptr;
        const int kind = static_cast<int>(k);
        L[i]->remove(segment, grid, [&](const WallSegment& s) { return roomByPoint(s, kind); });
    };
    take(kAllWalls);
    if (halfWall) {
        take(static_cast<LayerIndex>(halfWallLayer(segment)));
    }
    // The rooms keep a section the stage sides still need.
    if (d.blocksPlacement && !(L[kStageSides] && L[kStageSides]->contains(segment))) {
        take(kPlacementBlocking);
    }
    if (d.bearsLoad) {
        take(kLoadBearing);
    }
    if (d.blocksLight) {
        take(kLightBlocking);
    }
    if (d.rendered) {
        switch (d.span) {
        case WallStyle::Span::FloorToFloor: take(kSpanFloorToFloor); break;
        case WallStyle::Span::FloorToFloorBasedCeiling: take(kSpanBasedCeiling); break;
        case WallStyle::Span::FloorToRoof: take(kSpanFloorToRoof); break;
        default: break;
        }
    }
    return true;
}

void WallManager::save() {
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        if (layers_->layers[i]) {
            graphs_[kInstances[i]] = layers_->layers[i]->saved();
        }
    }
}

} // namespace ts2::content
