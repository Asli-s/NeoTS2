#include "sim/route_search.hpp"

#include "sim/path_passes.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <tuple>

namespace ts2::sim {

double RouteRandom::next() {
    state_ *= 0x278DDE6Du;
    return static_cast<std::int32_t>(state_) * (1.0 / 4294967296.0) + 0.5;
}

namespace {

using Cell = RouteGrid::Cell;

// Children touching each side of a cell, low coordinate first: +x, +y, -x, -y.
constexpr int kSideChildren[4][2] = {{0, 2}, {0, 1}, {1, 3}, {2, 3}};

void link(RouteLinks& links, int a, int b) {
    auto& la = links.of[static_cast<std::size_t>(a)];
    if (std::find(la.begin(), la.end(), b) == la.end()) {
        la.push_back(b);
    }
    auto& lb = links.of[static_cast<std::size_t>(b)];
    if (std::find(lb.begin(), lb.end(), a) == lb.end()) {
        lb.push_back(a);
    }
}

// Links `leaf` with every leaf of `side` along the edge they share.
void linkAlong(const RouteGrid& grid, RouteLinks& links, int leaf, int side, int which) {
    if (side < 0) {
        return;
    }
    const Cell& s = grid.cells()[static_cast<std::size_t>(side)];
    if (s.isLeaf()) {
        link(links, leaf, side);
        return;
    }
    for (const int child : kSideChildren[which]) {
        linkAlong(grid, links, leaf, s.children[static_cast<std::size_t>(child)], which);
    }
}

// The neighbour's child touching a cell from one side, or the neighbour itself when it is a leaf.
int touching(const RouteGrid& grid, int neighbour, int child) {
    if (neighbour < 0) {
        return -1;
    }
    const Cell& n = grid.cells()[static_cast<std::size_t>(neighbour)];
    return n.isLeaf() ? neighbour : n.children[static_cast<std::size_t>(child)];
}

// Depth first, children in order. `sides` are the cells next to `cell` (+x, +y, -x, -y), each of its
// size or a larger leaf.
void linkCell(const RouteGrid& grid, RouteLinks& links, int cell, const std::array<int, 4>& sides) {
    const Cell& c = grid.cells()[static_cast<std::size_t>(cell)];
    if (c.isLeaf()) {
        for (int which = 0; which < 4; ++which) {
            linkAlong(grid, links, cell, sides[static_cast<std::size_t>(which)], which);
        }
        return;
    }
    const auto& k = c.children;
    const auto [px, py, mx, my] = sides;
    linkCell(grid, links, k[0], {k[1], k[2], touching(grid, mx, 1), touching(grid, my, 2)});
    linkCell(grid, links, k[1], {touching(grid, px, 0), k[3], k[0], touching(grid, my, 3)});
    linkCell(grid, links, k[2], {k[3], touching(grid, py, 0), touching(grid, mx, 3), k[0]});
    linkCell(grid, links, k[3], {touching(grid, px, 2), touching(grid, py, 1), k[2], k[1]});
}

constexpr float kImpassable = 1.7014117e38f;  // a step at least this costly can't be taken
constexpr float kInset = 0.01f;               // crossing points lie this far inside the cell entered
constexpr float kGoalBand = 0.02f;            // a goal this close to a blocked cell's edge moves across it
constexpr float kGoalNudge = 0.001f;
constexpr double kShorterBy = -9.9999997e-6;  // a shortcut must save more than this
constexpr int kShorteningPasses = 10;
constexpr int kShorteningSteps = 4000;

float distance(float ax, float ay, float bx, float by) {
    return static_cast<float>(std::hypot(static_cast<double>(ax) - bx, static_cast<double>(ay) - by));
}

class Search {
public:
    Search(std::span<const RouteLevel> levels, std::span<const RoutePortal> portals, float aimX, float aimY,
           std::span<const RouteDoorState> doors, RouteRandom& jitter, RouteRandom& heuristic, const RouteIntersects& intersects)
        : levels_(levels.begin(), levels.end()), portals_(portals.begin(), portals.end()), aimX_(aimX), aimY_(aimY), jitter_(jitter),
          heuristic_(heuristic), intersects_(intersects) {
        // The levels' cells, one after another.
        for (std::size_t l = 0; l < levels_.size(); ++l) {
            offsets_.push_back(cells_);
            if (levels_[l].grid == nullptr) {
                continue;
            }
            for (const Cell& c : levels_[l].grid->cells()) {
                const RouteRect r = c.rect();
                Node n;
                n.x = (r.x0 + r.x1) / 2;
                n.y = (r.y0 + r.y1) / 2;
                n.level = static_cast<int>(l);
                nodes_.push_back(n);
                cellLevel_.push_back(static_cast<int>(l));
            }
            cells_ += static_cast<int>(levels_[l].grid->cells().size());
        }
        extra_.resize(static_cast<std::size_t>(cells_));
        base_.resize(static_cast<std::size_t>(cells_));
        for (const RouteDoorState& d : doors) {
            RouteDoorState& state = doors_[d.owner];
            state = d;
            // A broken extra cost (not a finite cost of zero or more) closes the door.
            if (!(std::isfinite(d.extra) && d.extra >= 0)) {
                state.refused[0] = state.refused[1] = true;
            }
        }
    }

    std::optional<RoutePlan> run(std::span<const RouteStart> starts, std::span<const RouteGoal> goals);

private:
    enum class Kind : std::uint8_t { Cell, Start, Goal, Portal };
    struct Node {
        Kind kind = Kind::Cell;
        float x = 0, y = 0;  // entry point
        double g = 0, f = 0;
        int parent = -1;
        int cell = -1;       // a start's or goal's cell
        int goal = -1;       // a goal's index
        float score = 0;
        std::uint8_t state = 0;  // 0 new, 1 open, 2 closed
        bool keep = false;       // the entry point never changes (starts, portal ends)
        double order = 0;        // place among nodes of equal f in the open list
        int level = 0;
        int portal = -1;  // a portal end's portal, its side (0 or 1) and the other end's node
        int side = 0;
        int partner = -1;
    };
    struct Step {
        double cost = 0;
        float x = 0, y = 0;
        std::uint32_t door = 0;  // the door whose line the step crosses
        int enters = -1;         // and the side it enters (-1: none)
    };
    struct Passage {
        int type = 0;
        bool special = false;
        double cost = 0;
        bool passable() const { return special || type < 2; }
    };

    bool isCell(int n) const { return n < cells_; }
    const RouteGrid& gridOf(int cell) const { return *levels_[static_cast<std::size_t>(cellLevel_[static_cast<std::size_t>(cell)])].grid; }
    std::size_t localOf(int cell) const {
        return static_cast<std::size_t>(cell - offsets_[static_cast<std::size_t>(cellLevel_[static_cast<std::size_t>(cell)])]);
    }
    const Cell& cellOf(int cell) const { return gridOf(cell).cells()[localOf(cell)]; }
    // The leaf under a point of a level, -1 for none.
    int leafAt(int level, float x, float y) const {
        if (level < 0 || static_cast<std::size_t>(level) >= levels_.size() || levels_[static_cast<std::size_t>(level)].grid == nullptr ||
            !std::isfinite(x) || !std::isfinite(y)) {
            return -1;
        }
        const int leaf = levels_[static_cast<std::size_t>(level)].grid->leafAt(x, y);
        return leaf < 0 ? -1 : leaf + offsets_[static_cast<std::size_t>(level)];
    }
    void addPortals();
    bool startLike(int n) const { return nodes_[static_cast<std::size_t>(n)].kind != Kind::Cell; }
    Node& node(int n) { return nodes_[static_cast<std::size_t>(n)]; }

    Passage passage(int cell);
    double nodeCost(int n);
    void forEachLink(int n, auto&& f) const;
    bool linked(int a, int b) const;
    void attach(int n, int cell);
    RouteGrid::Passage basePassage(int cell);
    const RouteItem* doorway(int cell, bool& any) const;
    bool crossing(int cell, float fx, float fy, float tx, float ty, double& extra, Step* out);
    double legCost(int n, int parent, float px, float py, float ex, float ey, Step* out = nullptr);
    Step step(int n, int parent, float px, float py);
    Step cellStep(int c, int parent, float px, float py);
    double heuristicFor(float x, float y, double g);
    void relax(int n, int parent);
    void enqueue(int n, bool improved);
    void shorten(std::vector<int>& path);
    bool tryChain(std::vector<int>& path, int from, std::span<const int> chain, int c);

    std::vector<RouteLevel> levels_;
    std::vector<RoutePortal> portals_;
    std::vector<int> offsets_;    // each level's first cell
    std::vector<int> cellLevel_;  // each cell's level
    std::map<int, std::vector<int>> portalCells_;  // the cells each portal end links with
    float aimX_, aimY_;
    RouteRandom& jitter_;
    RouteRandom& heuristic_;
    int cells_ = 0;
    std::vector<Node> nodes_;
    std::vector<std::vector<int>> extra_;  // starts and goals linked to each cell, after its leaf links
    std::set<std::tuple<double, double, int>> open_;  // f, order, node
    double front_ = 0, back_ = 0;
    std::map<std::uint32_t, RouteDoorState> doors_;
    const RouteIntersects& intersects_;
    std::vector<std::optional<RouteGrid::Passage>> base_;  // each cell's type and cost, taken once per search
    int escape_ = 0;  // while 2 or more: the Sim stands in a blocked cell and may cross cells of up to this type
    bool plainHeuristic_ = false;
};

// The type and cost a cell has for this Sim. A Sim standing inside an obstacle may walk out of it
// through cells of no higher type, at a fixed cost by type. Doing so lowers what it may cross next.
Search::Passage Search::passage(int cell) {
    const auto p = basePassage(cell);
    Passage out{p.type, false, p.cost};
    if (escape_ >= 2 && escape_ >= p.type) {
        escape_ = p.type;
        out.special = p.type > 1;
    }
    if (out.special) {
        out.cost = p.type <= 5 ? 1.0 : p.type <= 7 ? 2.0 : p.type <= 11 ? 3.0 : 4.0;
    }
    return out;
}

// A cell's type and cost from its items, with door swing areas on the sides counted active. The game
// takes it once per search, when the cell is first costed: a side made active later only counts in
// cells costed after that.
RouteGrid::Passage Search::basePassage(int cell) {
    auto& known = base_[static_cast<std::size_t>(cell)];
    if (known) {
        return *known;
    }
    const Cell& c = cellOf(cell);
    RouteGrid::Passage p = gridOf(cell).passage(c, &intersects_);
    if (p.walkable()) {
        const RouteRect r = c.rect();
        for (const int i : c.items) {
            const RouteItem& item = gridOf(cell).items()[static_cast<std::size_t>(i)];
            if (item.kind != RouteItemKind::Area || !item.door) {
                continue;
            }
            const auto d = doors_.find(item.owner);
            if (d != doors_.end() && d->second.active[item.door->side((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)]) {
                p.type = std::max(p.type, item.type);
                p.cost = std::max(p.cost, item.cost);
            }
        }
    }
    known = p;
    return p;
}

// The doorway of a cell: its first solid square, when that belongs to a door. `any` tells whether
// the cell has a solid square at all.
const RouteItem* Search::doorway(int cell, bool& any) const {
    any = false;
    for (const int i : cellOf(cell).items) {
        const RouteItem& item = gridOf(cell).items()[static_cast<std::size_t>(i)];
        if (item.kind == RouteItemKind::Solid) {
            any = true;
            return item.door ? &item : nullptr;
        }
    }
    return nullptr;
}

// Whether a leg from (fx, fy) to (tx, ty) crosses the line of the door in `cell`, and if so what that
// adds: the door's extra cost, or a wall's when this Sim may not go that way.
bool Search::crossing(int cell, float fx, float fy, float tx, float ty, double& extra, Step* out) {
    bool any = false;
    const RouteItem* d = doorway(cell, any);
    if (d == nullptr) {
        return false;
    }
    const int from = d->door->side(fx, fy);
    if (from == d->door->side(tx, ty)) {
        return false;
    }
    RouteDoorState& state = doors_[d->owner];
    extra = state.refused[from] ? std::numeric_limits<double>::max() : state.extra;
    if (out != nullptr) {
        out->door = d->owner;
        out->enters = 1 - from;
    }
    return true;
}

double Search::nodeCost(int n) { return isCell(n) ? passage(n).cost : 1.0; }

void Search::forEachLink(int n, auto&& f) const {
    if (isCell(n)) {
        const auto level = static_cast<std::size_t>(cellLevel_[static_cast<std::size_t>(n)]);
        for (const int m : levels_[level].links->of[localOf(n)]) {
            f(m + offsets_[level]);
        }
        for (const int m : extra_[static_cast<std::size_t>(n)]) {
            f(m);
        }
        return;
    }
    const Node& v = nodes_[static_cast<std::size_t>(n)];
    if (v.kind == Kind::Portal) {
        // A portal end: the cells around it, then the other end when the portal may be crossed this way.
        if (const auto cells = portalCells_.find(n); cells != portalCells_.end()) {
            for (const int c : cells->second) {
                f(c);
            }
        }
        const RoutePortal& p = portals_[static_cast<std::size_t>(v.portal)];
        if ((v.side == 0 || p.twoWay) && !p.refused) {
            f(v.partner);
        }
    } else if (v.cell >= 0) {
        f(v.cell);
    }
}

bool Search::linked(int a, int b) const {
    bool found = false;
    forEachLink(a, [&](int m) { found |= m == b; });
    return found;
}

void Search::attach(int n, int cell) {
    node(n).cell = cell;
    extra_[static_cast<std::size_t>(cell)].push_back(n);
}

// The cost of the leg from `parent`'s entry point (px, py) to (ex, ey) where `n` is entered. On a
// leg from one cell into the next, the part inside the cell entered costs that cell's cost and the
// rest the parent's.
double Search::legCost(int n, int parent, float px, float py, float ex, float ey, Step* out) {
    if (parent < 0) {
        return 0;
    }
    const Node& to = node(n);
    if (to.kind == Kind::Start) {
        return distance(ex, ey, px, py);
    }
    if (to.kind == Kind::Goal && to.cell < 0) {
        return 0;
    }
    if (to.kind == Kind::Portal) {
        // Across the portal: its flat cost. Onto it from a cell: the distance at that cell's cost.
        if (node(parent).kind == Kind::Portal) {
            const RoutePortal& p = portals_[static_cast<std::size_t>(to.portal)];
            return p.refused || !(std::isfinite(p.cost) && p.cost >= 0) ? std::numeric_limits<double>::max() : p.cost;
        }
        double extra = 0;
        const double cost = nodeCost(parent);
        crossing(parent, px, py, ex, ey, extra, out);
        if (cost > kImpassable || extra > kImpassable) {
            return std::numeric_limits<double>::max();
        }
        return distance(ex, ey, px, py) * cost + extra;
    }
    double extra = 0;
    const auto withDoor = [&](double cost) {
        return cost > kImpassable || extra > kImpassable ? std::numeric_limits<double>::max() : cost + extra;
    };
    if (to.kind == Kind::Goal || startLike(parent)) {
        const int cell = to.kind == Kind::Goal ? to.cell : n;
        const double cost = nodeCost(cell);
        crossing(cell, px, py, ex, ey, extra, out);
        return withDoor(cost > kImpassable ? cost : distance(ex, ey, px, py) * cost);
    }
    // The parent first: stepping out of an obstacle lowers what may be crossed (see passage).
    const double parentCost = nodeCost(parent);
    const double cost = nodeCost(n);
    if (parentCost > kImpassable || cost > kImpassable) {
        return std::numeric_limits<double>::max();
    }
    const RouteRect p = cellOf(parent).rect();
    double t = 0;
    if (ex > p.x1 && ex != px) {
        t = (ex - p.x1) / (static_cast<double>(ex) - px);
    } else if (ex < p.x0 && ex != px) {
        t = (ex - p.x0) / (static_cast<double>(ex) - px);
    } else if (ey > p.y1 && ey != py) {
        t = (ey - p.y1) / (static_cast<double>(ey) - py);
    } else if (ey < p.y0 && ey != py) {
        t = (ey - p.y0) / (static_cast<double>(ey) - py);
    }
    t = std::clamp(t, 0.0, 1.0);
    // The door of the cell entered first, then the parent's.
    if (!crossing(n, px, py, ex, ey, extra, out)) {
        crossing(parent, px, py, ex, ey, extra, out);
    }
    return withDoor(distance(ex, ey, px, py) * ((1 - t) * parentCost + t * cost));
}

// Where cell `c` is entered from `parent`, whose entry point is (px, py), and at what cost.
Search::Step Search::cellStep(int c, int parent, float px, float py) {
    const Node& n = node(c);
    if (parent < 0) {
        return {0, n.x, n.y};
    }
    float x = n.x, y = n.y;
    const RouteRect r = cellOf(c).rect();
    if (n.keep) {
    } else if (startLike(parent)) {
        x = std::clamp(px, r.x0, r.x1);
        y = std::clamp(py, r.y0, r.y1);
    } else {
        const RouteRect p = cellOf(parent).rect();
        const float pcx = (p.x0 + p.x1) / 2, pcy = (p.y0 + p.y1) / 2;
        const float ySep = std::max(p.y0 - r.y1, r.y0 - p.y1), xSep = std::max(p.x0 - r.x1, r.x0 - p.x1);
        // The crossing along the shared edge: where the line from the parent's entry point to the aim
        // crosses it, kept off the corners. It is the aim's own coordinate when the line misses the edge.
        const auto cross = [&](float edge, float pc, float aimA, float aimB, float entryA, float entryB, float lo, float hi) {
            float b = aimB;
            const bool between = (edge <= aimA && pc <= edge) || (aimA <= edge && edge <= pc);
            if (between && std::abs(aimA - entryA) > 1e-4f) {
                b = entryB + (edge - entryA) * (aimB - entryB) / (aimA - entryA);
            }
            return b <= lo ? lo : b >= hi ? hi : b;
        };
        if (ySep < xSep) {
            x = r.x0 <= pcx ? r.x1 - kInset : r.x0 + kInset;
            y = cross(x, pcx, aimX_, aimY_, px, py, std::max(p.y0, r.y0) + kInset, std::min(p.y1, r.y1) - kInset);
        } else {
            y = r.y0 <= pcy ? r.y1 - kInset : r.y0 + kInset;
            x = cross(y, pcy, aimY_, aimX_, py, px, std::max(p.x0, r.x0) + kInset, std::min(p.x1, r.x1) - kInset);
        }
    }
    Step s{0, x, y};
    s.cost = legCost(c, parent, px, py, x, y, &s);
    return s;
}

Search::Step Search::step(int n, int parent, float px, float py) {
    const Node& to = node(n);
    if (to.kind == Kind::Cell) {
        return cellStep(n, parent, px, py);
    }
    Step s{0, to.x, to.y};
    s.cost = legCost(n, parent, px, py, to.x, to.y, &s);
    return s;
}

// The game's estimate of the cost left: the distance to the aim, plus a random amount that grows
// with the cost so far once the aim is 3 tiles away or more.
double Search::heuristicFor(float x, float y, double g) {
    const double d = distance(x, y, aimX_, aimY_);
    if (plainHeuristic_ || d < 3) {
        return d;
    }
    const double most = std::min(0.1 * g, 3.0);
    if (d < 6) {
        return d + (d - 3) * heuristic_.uniform(0, most) * 0.33333334;
    }
    return d + heuristic_.uniform(0, most);
}

void Search::enqueue(int n, bool improved) {
    Node& v = node(n);
    if (v.state == 1) {
        open_.erase({v.f, v.order, n});
    }
    // A node reached for the first time goes before others of equal f, an improved one after them.
    v.order = improved ? ++back_ : --front_;
    v.state = 1;
    open_.insert({v.f, v.order, n});
}

void Search::relax(int n, int parent) {
    Node& v = node(n);
    if (v.state == 2) {
        return;
    }
    const float px = parent >= 0 ? node(parent).x : 0, py = parent >= 0 ? node(parent).y : 0;
    Step s = step(n, parent, px, py);
    if (s.cost >= kImpassable) {
        return;
    }
    const double parentG = parent >= 0 ? node(parent).g : 0;
    // A step through a door makes the side it enters count that door's swing areas.
    const auto enterDoor = [&] {
        if (s.enters >= 0) {
            doors_[s.door].active[s.enters] = true;
        }
    };
    if (v.state == 0) {
        if (parent >= 0) {
            s.cost += jitter_.uniform(0.0, 0.01);
        }
        enterDoor();
        v.parent = parent;
        if (!v.keep) {
            v.x = s.x;
            v.y = s.y;
        }
        v.g = parentG + s.cost;
        v.f = v.g + heuristicFor(v.x, v.y, v.g) * nodeCost(n);
        enqueue(n, false);
        return;
    }
    const double g = parentG + s.cost;
    const float x = v.keep ? v.x : s.x, y = v.keep ? v.y : s.y;
    const double f = g + heuristicFor(x, y, g) * nodeCost(n);
    if (f < v.f) {
        enterDoor();
        v.parent = parent;
        v.x = x;
        v.y = y;
        v.g = g;
        open_.erase({v.f, v.order, n});
        v.state = 0;
        v.f = f;
        enqueue(n, true);
    }
}

// Tries replacing the nodes between path[from] and the node `c` (path[from + removed + 1], or none at
// the goal end) by `chain`, and keeps it when the cost from path[from] to the end of that stretch drops.
bool Search::tryChain(std::vector<int>& path, int from, std::span<const int> chain, int c) {
    const int head = path[static_cast<std::size_t>(from)];
    const auto cIt = c >= 0 ? std::find(path.begin(), path.end(), c) : path.end();
    const int lastOld = static_cast<int>((cIt == path.end() ? path.end() : cIt) - path.begin()) - 1;
    const int tail = cIt == path.end() ? path[static_cast<std::size_t>(lastOld)] : c;
    std::vector<int> nodes(chain.begin(), chain.end());
    if (cIt != path.end()) {
        nodes.push_back(c);
    }
    // While shortening, crossings aim at the entry point of the node that ends the stretch.
    struct Aim {
        Search& s;
        float x, y;
        ~Aim() { s.aimX_ = x; s.aimY_ = y; }
    } restore{*this, aimX_, aimY_};
    aimX_ = node(tail).x;
    aimY_ = node(tail).y;
    std::vector<Step> steps;
    float px = node(head).x, py = node(head).y;
    int prev = head;
    double total = 0;
    for (const int n : nodes) {
        Step s = step(n, prev, px, py);
        if (s.cost >= kImpassable) {
            return false;
        }
        if (node(n).keep) {
            s.x = node(n).x;
            s.y = node(n).y;
        }
        total += s.cost;
        steps.push_back(s);
        px = s.x;
        py = s.y;
        prev = n;
    }
    // What the stretch costs as it is now, with its entry points as they are.
    double old = 0;
    prev = head;
    px = node(head).x;
    py = node(head).y;
    for (auto it = path.begin() + from + 1; it != path.end(); ++it) {
        Step s = step(*it, prev, px, py);
        if (node(*it).keep) {
            s.x = node(*it).x;
            s.y = node(*it).y;
        }
        old += s.cost;
        px = s.x;
        py = s.y;
        prev = *it;
        if (*it == tail) {
            break;
        }
    }
    if (!(total - old < kShorterBy)) {
        return false;
    }
    double g = node(head).g;
    prev = head;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        Node& v = node(nodes[i]);
        g += steps[i].cost;
        v.parent = prev;
        v.x = steps[i].x;
        v.y = steps[i].y;
        v.g = g;
        prev = nodes[i];
    }
    const double gain = total - old;
    for (auto it = cIt == path.end() ? path.end() : cIt + 1; it != path.end(); ++it) {
        node(*it).g += gain;
    }
    path.erase(path.begin() + from + 1, path.begin() + lastOld + 1 + (cIt != path.end() ? 1 : 0));
    path.insert(path.begin() + from + 1, nodes.begin(), nodes.end());
    return true;
}

// The game's shortening: from the goal back to the start, tries a neighbour in place of (or
// beside) the node before, skipping one or two nodes, and a fresh crossing point.
void Search::shorten(std::vector<int>& path) {
    for (int pass = 0; pass < kShorteningPasses; ++pass) {
        bool changed = false;
        int steps = 0;
        int c = -1;
        int n = path.back();
        while (!startLike(n) || node(n).kind == Kind::Goal) {
            if (++steps > kShorteningSteps) {
                break;
            }
            auto at = [&](int node) { return static_cast<int>(std::find(path.begin(), path.end(), node) - path.begin()); };
            int i = at(n);
            if (i == 0) {
                break;
            }
            const int p = path[static_cast<std::size_t>(i - 1)];
            const int g = i >= 2 ? path[static_cast<std::size_t>(i - 2)] : -1;
            std::vector<int> candidates;
            forEachLink(n, [&](int m) { candidates.push_back(m); });
            for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) {
                const int m = *it;
                if (m == c || m == p || startLike(m) || std::find(path.begin(), path.end(), m) != path.end()) {
                    continue;
                }
                const int chainA[] = {m, n};
                if (g >= 0 && !startLike(p) && linked(m, g) && tryChain(path, i - 2, chainA, c)) {
                    changed = true;
                    break;
                }
                if (linked(m, p) && tryChain(path, i - 1, chainA, c)) {
                    changed = true;
                    break;
                }
            }
            i = at(n);
            if (i >= 2) {
                const int p2 = path[static_cast<std::size_t>(i - 1)];
                const int g2 = path[static_cast<std::size_t>(i - 2)];
                const int direct[] = {n};
                if (!startLike(p2) && linked(n, g2) && tryChain(path, i - 2, direct, c)) {
                    changed = true;
                }
            }
            i = at(n);
            if (i >= 3) {
                const int p3 = path[static_cast<std::size_t>(i - 1)];
                const int g3 = path[static_cast<std::size_t>(i - 2)];
                const int gg = path[static_cast<std::size_t>(i - 3)];
                const int direct[] = {n};
                if (!startLike(p3) && !startLike(g3) && linked(n, gg) && tryChain(path, i - 3, direct, c)) {
                    changed = true;
                }
            }
            i = at(n);
            if (!startLike(n) && !node(n).keep && i >= 1) {
                const int again[] = {n};
                changed |= tryChain(path, i - 1, again, c);
            }
            i = at(n);
            c = n;
            n = path[static_cast<std::size_t>(i - 1)];
        }
        if (!changed) {
            break;
        }
    }
}

std::optional<RoutePlan> Search::run(std::span<const RouteStart> starts, std::span<const RouteGoal> goals) {
    const auto finite = [](float a, float b) { return std::isfinite(a) && std::isfinite(b); };
    // Starts: each linked with the cell it stands in, which is entered at the start point.
    std::vector<int> startNodes;
    int blockedStarts = 0;
    for (const RouteStart& s : starts) {
        const int cell = finite(s.x, s.y) ? leafAt(s.level, s.x, s.y) : -1;
        if (cell < 0) {
            continue;
        }
        node(cell).x = s.x;
        node(cell).y = s.y;
        const Passage p = passage(cell);
        if (!p.passable()) {
            ++blockedStarts;
        }
        const int n = static_cast<int>(nodes_.size());
        nodes_.push_back({Kind::Start, s.x, s.y});
        node(n).level = s.level;
        node(n).keep = true;
        attach(n, cell);
        startNodes.push_back(n);
    }
    // Goals: each linked with the cell under it, or, when that cell blocks and the goal lies within
    // 0.02 of its edge, the first open cell just across the nearer side.
    for (std::size_t gi = 0; gi < goals.size(); ++gi) {
        const RouteGoal& goal = goals[gi];
        const int n = static_cast<int>(nodes_.size());
        nodes_.push_back({Kind::Goal, goal.x, goal.y});
        node(n).level = goal.level;
        node(n).goal = static_cast<int>(gi);
        node(n).score = goal.score;
        const int cell = finite(goal.x, goal.y) ? leafAt(goal.level, goal.x, goal.y) : -1;
        if (cell < 0) {
            continue;
        }
        int chosen = cell;
        if (!passage(cell).passable()) {
            const RouteRect r = cellOf(cell).rect();
            const auto band = [](float v, float lo, float hi) {
                return v >= lo - kGoalBand && v <= hi + kGoalBand && !(v > lo + kGoalBand && v < hi - kGoalBand);
            };
            const bool nearX = band(goal.x, r.x0, r.x1), nearY = band(goal.y, r.y0, r.y1);
            const float sx = goal.x < (r.x0 + r.x1) * 0.5f ? std::min(goal.x, r.x0 - kGoalNudge) : std::max(goal.x, r.x1 + kGoalNudge);
            const float sy = goal.y < (r.y0 + r.y1) * 0.5f ? std::min(goal.y, r.y0 - kGoalNudge) : std::max(goal.y, r.y1 + kGoalNudge);
            std::vector<std::pair<float, float>> tries;
            if (nearX && nearY) {
                tries = {{sx, sy}, {sx, goal.y}};  // the game never tries (x, snapped y) here
            } else if (nearX) {
                tries = {{sx, goal.y}};
            } else if (nearY) {
                tries = {{goal.x, sy}};
            }
            for (const auto& [x, y] : tries) {
                const int other = leafAt(goal.level, x, y);
                if (other >= 0 && passage(other).passable()) {
                    chosen = other;
                    node(n).x = x;
                    node(n).y = y;
                    break;
                }
            }
        }
        attach(n, chosen);
    }
    if (startNodes.empty()) {
        return std::nullopt;
    }
    addPortals();
    // A Sim standing inside an obstacle: it may leave through cells of up to the obstacle's type.
    if (blockedStarts >= static_cast<int>(startNodes.size())) {
        const int cell = node(startNodes[0]).cell;
        const Passage p = passage(cell);
        escape_ = p.special ? 0 : p.type;
        plainHeuristic_ = true;
    }

    for (const int s : startNodes) {
        relax(s, -1);
    }
    std::vector<bool> reached(goals.size());
    std::size_t found = 0;
    int best = -1;
    while (!open_.empty() && found < goals.size()) {
        const int n = std::get<2>(*open_.begin());
        open_.erase(open_.begin());
        Node& v = node(n);
        v.state = 2;
        if (v.kind == Kind::Goal && !reached[static_cast<std::size_t>(v.goal)]) {
            reached[static_cast<std::size_t>(v.goal)] = true;
            ++found;
            if (best < 0 || v.score > node(best).score || (v.score == node(best).score && v.g < node(best).g)) {
                best = n;
            }
            if (found == goals.size()) {
                break;
            }
        }
        forEachLink(n, [&](int m) {
            if (m != v.parent) {
                relax(m, n);
            }
        });
    }
    if (best < 0) {
        return std::nullopt;
    }
    std::vector<int> path;
    for (int n = best; n >= 0 && path.size() <= nodes_.size(); n = node(n).parent) {
        path.push_back(n);
        if (node(n).kind == Kind::Start) {
            break;
        }
    }
    std::reverse(path.begin(), path.end());
    if (node(path.front()).kind != Kind::Start) {
        return std::nullopt;
    }
    shorten(path);

    RoutePlan plan;
    plan.goal = node(best).goal;
    // Where the path passes a door's line the game marks the first point past it (door entry) and the
    // point halfway to the next (door exit) in place of a plain point. It follows the doorway cells'
    // sides along the path. A cell whose first solid square isn't a door's forgets the door.
    std::uint32_t lastOwner = 0;
    const RouteItem* lastDoor = nullptr;
    int lastSide = -1;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const Node& v = node(path[i]);
        const auto g = static_cast<float>(v.g);
        const auto add = [&](float x, float y, WaypointKind kind) {
            RouteWaypoint w{x, y, g, kind};
            w.level = v.level;
            w.portal = v.kind == Kind::Portal ? v.portal : -1;
            plan.waypoints.push_back(w);
        };
        if (v.kind == Kind::Portal) {
            // The end the path crosses from is the stair's entry, the other its exit.
            const bool entry = i + 1 < path.size() && path[i + 1] == v.partner;
            add(v.x, v.y, entry ? WaypointKind::LevelEnd : WaypointKind::LevelStart);
            continue;
        }
        if (v.kind != Kind::Cell) {
            add(v.x, v.y, v.kind == Kind::Start ? WaypointKind::Start : WaypointKind::Goal);
            continue;
        }
        bool any = false;
        const RouteItem* d = doorway(path[i], any);
        if (any && d == nullptr) {
            lastOwner = 0;
        } else if (d != nullptr) {
            const int side = d->door->side(v.x, v.y);
            if (d->owner != lastOwner || side != lastSide) {
                const bool through = lastDoor != nullptr && lastDoor->owner == d->owner && side != lastSide && i + 1 < path.size();
                lastOwner = d->owner;
                lastDoor = d;
                lastSide = side;
                if (through) {
                    const Node& next = node(path[i + 1]);
                    add(v.x, v.y, WaypointKind::DoorEntry);
                    add((next.x + v.x) * 0.5f, (next.y + v.y) * 0.5f, WaypointKind::DoorExit);
                    continue;
                }
            }
        }
        add(v.x, v.y, WaypointKind::Plain);
    }
    // The passes see the grid as the search did, each stretch between changes of level on its own
    // level's grid.
    std::size_t begin = 0;
    for (std::size_t i = 1; i <= plan.waypoints.size(); ++i) {
        if (i < plan.waypoints.size() && plan.waypoints[i].kind != WaypointKind::LevelStart) {
            continue;
        }
        std::vector<RouteWaypoint> stretch(plan.waypoints.begin() + static_cast<std::ptrdiff_t>(begin),
                                           plan.waypoints.begin() + static_cast<std::ptrdiff_t>(i));
        const int level = stretch.front().level;
        const int first = stretch.front().portal, last = stretch.back().portal;
        const int offset = offsets_[static_cast<std::size_t>(level)];
        const RouteLines lines(*levels_[static_cast<std::size_t>(level)].grid, [this, offset](int cell) {
            const Passage p = passage(cell + offset);
            return LeafPassage{p.passable(), p.cost};
        });
        smoothPath(stretch, lines);
        for (RouteWaypoint& w : stretch) {
            w.level = level;
            w.portal = w.kind == WaypointKind::LevelStart ? first : w.kind == WaypointKind::LevelEnd ? last : -1;
        }
        plan.walk.insert(plan.walk.end(), stretch.begin(), stretch.end());
        begin = i;
    }
    return plan;
}

// Each portal's two ends, linked both ways with the leaves under their points and 0.0625 tiles
// around them (up to five).
void Search::addPortals() {
    constexpr float kAround = 0.0625f;
    constexpr float kOffsets[5][2] = {{0, 0}, {kAround, kAround}, {kAround, -kAround}, {-kAround, kAround}, {-kAround, -kAround}};
    for (std::size_t pi = 0; pi < portals_.size(); ++pi) {
        const RoutePortal& p = portals_[pi];
        const int first = static_cast<int>(nodes_.size());
        for (int side = 0; side < 2; ++side) {
            Node end;
            end.kind = Kind::Portal;
            end.x = p.x[side];
            end.y = p.y[side];
            end.level = p.level[side];
            end.keep = true;
            end.portal = static_cast<int>(pi);
            end.side = side;
            end.partner = first + 1 - side;
            nodes_.push_back(end);
        }
        for (int side = 0; side < 2; ++side) {
            const int n = first + side;
            std::vector<int>& cells = portalCells_[n];
            for (const auto& [dx, dy] : kOffsets) {
                const int cell = leafAt(p.level[side], p.x[side] + dx, p.y[side] + dy);
                if (cell >= 0 && std::find(cells.begin(), cells.end(), cell) == cells.end()) {
                    cells.push_back(cell);
                    extra_[static_cast<std::size_t>(cell)].push_back(n);
                }
            }
        }
    }
}

} // namespace

RouteLinks RouteLinks::build(const RouteGrid& grid) {
    RouteLinks links;
    links.of.resize(grid.cells().size());
    if (!grid.cells().empty()) {
        linkCell(grid, links, 0, {-1, -1, -1, -1});
    }
    return links;
}

std::optional<RoutePlan> planRoute(const RouteGrid& grid, const RouteLinks& links, std::span<const RouteStart> starts,
                                   std::span<const RouteGoal> goals, float aimX, float aimY, std::span<const RouteDoorState> doors,
                                   RouteRandom& jitter, RouteRandom& heuristic, const RouteIntersects& intersects) {
    const RouteLevel level[] = {{&grid, &links}};
    std::vector<RouteStart> onGrid(starts.begin(), starts.end());
    std::vector<RouteGoal> goalsOnGrid(goals.begin(), goals.end());
    for (RouteStart& s : onGrid) {
        s.level = 0;
    }
    for (RouteGoal& g : goalsOnGrid) {
        g.level = 0;
    }
    return planRoute(level, {}, onGrid, goalsOnGrid, aimX, aimY, doors, jitter, heuristic, intersects);
}

std::optional<RoutePlan> planRoute(std::span<const RouteLevel> levels, std::span<const RoutePortal> portals,
                                   std::span<const RouteStart> starts, std::span<const RouteGoal> goals, float aimX, float aimY,
                                   std::span<const RouteDoorState> doors, RouteRandom& jitter, RouteRandom& heuristic,
                                   const RouteIntersects& intersects) {
    // More than this is a sign of broken data.
    constexpr std::size_t kMaxLevels = 64, kMaxPortals = 4096;
    if (starts.empty() || goals.empty() || starts.size() > kMaxRouteStarts || goals.size() > kMaxRouteGoals || levels.size() > kMaxLevels ||
        portals.size() > kMaxPortals || !std::isfinite(aimX) || !std::isfinite(aimY)) {
        return std::nullopt;
    }
    for (const RouteLevel& l : levels) {
        if (l.grid != nullptr && (l.links == nullptr || l.links->of.size() != l.grid->cells().size())) {
            return std::nullopt;
        }
    }
    // A portal with an end off the levels given, or on a level without a grid, can't be used. It
    // keeps its place, so waypoints name portals as the caller does.
    const auto onLevel = [&](int l) { return l >= 0 && static_cast<std::size_t>(l) < levels.size() && levels[static_cast<std::size_t>(l)].grid; };
    std::vector<RoutePortal> usable(portals.begin(), portals.end());
    for (RoutePortal& p : usable) {
        if (!onLevel(p.level[0]) || !onLevel(p.level[1]) || !std::isfinite(p.x[0]) || !std::isfinite(p.y[0]) || !std::isfinite(p.x[1]) ||
            !std::isfinite(p.y[1])) {
            p.refused = true;
            p.level[0] = p.level[1] = -1;
        }
    }
    Search search(levels, usable, aimX, aimY, doors, jitter, heuristic, intersects);
    return search.run(starts, goals);
}

} // namespace ts2::sim
