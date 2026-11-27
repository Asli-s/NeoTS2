#include "game.hpp"
#include "platform/platform.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <utility>

namespace app {

namespace {

// The game compares pattern names ignoring case.
bool sameName(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char l, char r) {
        return std::tolower(static_cast<unsigned char>(l)) == std::tolower(static_cast<unsigned char>(r));
    });
}

ts2::content::WallSegment segmentOf(const ts2::sim::WallPiece& p) {
    return {p.from.x, p.from.y, p.to.x, p.to.y, p.from.level};
}

bool sameSegment(const ts2::content::WallSegment& a, const ts2::content::WallSegment& b) {
    return a.ax == b.ax && a.ay == b.ay && a.bx == b.bx && a.by == b.by && a.level == b.level;
}

// The pattern a half wall is built with, which taking its paint off puts back.
constexpr const char* kBlankPattern = "blank";
// Invisible light blockers and stage sides stand outside the room outline.
constexpr std::uint32_t kInvisibleWallStyle = 300, kStageWallStyle = 301;

} // namespace

bool Game::planPaint(const ts2::sim::WallPiece& side, const std::string& pattern) {
    const auto& scripts = assets_.get()->buildScripts();
    const auto* fence = assets_.get()->fence(side.type);
    const bool halfWall = fence != nullptr && fence->halfWall;
    const auto style = scripts.wallStyles.find(side.type);
    // Walls and half walls take paint, fences and the other kinds don't, nor a style without
    // mayChangeSurface.
    const bool wall = style != scripts.wallStyles.end() && style->second.kind == ts2::content::WallStyle::Kind::Plain;
    if (!halfWall && (!wall || !style->second.mayChangeSurface)) {
        return false;
    }
    const auto segment = segmentOf(side);
    const auto current = wallManager_->sidePattern(segment);
    if (!current || sameName(*current, pattern)) {
        return false;
    }
    const auto price = [&](const std::string& name) { return lotUi_.wallCoveringPrice(name).value_or(0); };
    // Putting a wall style's own pattern back sells the covering painted over for four fifths of its
    // price, rounded toward zero. A side planned already sells nothing again.
    constexpr float kSellBack = 0.8f;
    std::int32_t refund = 0;
    const auto planned = std::find_if(paintPlan_.begin(), paintPlan_.end(), [&](const PaintedSide& p) { return sameSegment(p.side, segment); });
    if (planned == paintPlan_.end()) {
        paintPlan_.push_back({segment, *current, pattern});
        refund = static_cast<std::int32_t>(static_cast<float>(price(*current)) * kSellBack);
    } else {
        planned->after = pattern;
    }
    // Whether taking a half wall's paint off gives money back is not known yet.
    const bool ownPattern = wall && !style->second.defaultPattern.empty() && sameName(pattern, style->second.defaultPattern);
    paintPlanCost_ += price(pattern) - (ownPattern ? refund : 0);
    wallManager_->setSidePattern(segment, pattern);
    return true;
}

std::optional<ts2::sim::WallPiece> Game::wallFaceAt(const std::array<float, 3>& ray) {
    const auto& eye = orbit_.camera().eye;
    // The ray passes through objects and Sims to the walls, as the game's does.
    const auto hit = lotPicker_.pickAmong(lot_, lotVisible_, eye, ray, [](const ts2::scene::Instance& i) {
        return i.part == ts2::scene::Part::Wall && !i.walls.empty();
    });
    if (!hit || hit->instance >= lot_.instances.size() || !lot_.lotWalls) {
        return std::nullopt;
    }
    const auto& instance = lot_.instances[hit->instance];
    const int first = lot_.lotWalls->firstWall;
    if (instance.part != ts2::scene::Part::Wall || instance.walls.empty() || instance.walls.front() < first ||
        static_cast<std::size_t>(instance.walls.front() - first) >= lot_.lotWalls->walls.size()) {
        return std::nullopt;
    }
    const auto& built = lot_.lotWalls->walls[static_cast<std::size_t>(instance.walls.front() - first)];
    const auto& graph = *lotContent_->roomGraph;
    const auto section = std::find_if(graph.walls.begin(), graph.walls.end(),
                                      [&](const auto& w) { return w.from == built.points[0] && w.to == built.points[1]; });
    const auto* a = graph.point(built.points[0]);
    const auto* b = graph.point(built.points[1]);
    if (section == graph.walls.end() || a == nullptr || b == nullptr) {
        return std::nullopt;
    }
    const auto entry = std::find_if(lotContent_->wallCoverings.begin(), lotContent_->wallCoverings.end(),
                                    [&](const auto& e) { return e.wall == static_cast<std::uint32_t>(section->id); });
    if (entry == lotContent_->wallCoverings.end()) {
        return std::nullopt;
    }
    // As the eyedropper finds it: the wall graph's x and y are the route's y and x, and a point nearer
    // the wall's middle than its faces is on the wall's top, which has no side.
    const std::array<float, 3> point{eye[0] + ray[0] * hit->distance, eye[1] + ray[1] * hit->distance, eye[2] + ray[2] * hit->distance};
    const auto at = lotLife_->routePoint(point);
    const float dx = b->x - a->x, dy = b->y - a->y, length = std::sqrt(dx * dx + dy * dy);
    const float cross = dx * (at[0] - a->y) - dy * (at[1] - a->x);
    constexpr float kOnFace = 0.8f;
    if (length <= 0 || std::abs(cross) / length < built.halfThickness * kOnFace) {
        return std::nullopt;
    }
    // The side painted is the one on the right of from and to, where the point is.
    return cross < 0 ? ts2::sim::WallPiece{*a, *b, entry->type} : ts2::sim::WallPiece{*b, *a, entry->type};
}

void Game::updatePaintTool(const std::optional<std::array<float, 3>>& ray) {
    using Tool = app::LotUi::Tool;
    const auto choice = lotUi_.wallCoveringChoice();
    if (!lotLife_ || !lotContent_ || !lotContent_->roomGraph || buyTool_ != Tool::WallPaint || lotUi_.mode() != app::LotUi::Mode::Build ||
        !choice) {
        if (paintHover_ || !paintPlan_.empty()) {
            stopPaintTool();
        }
        return;
    }
    startWallManager();
    if (!paintOutline_) {
        // The pieces the room outline holds: every wall and fence but fence arches.
        const auto& graph = *lotContent_->roomGraph;
        const auto& arches = assets_.get()->buildScripts().fenceArches;
        std::map<std::int32_t, const ts2::wallgraph::Wall*> walls;
        for (const auto& w : graph.walls) {
            walls[w.id] = &w;
        }
        std::vector<ts2::sim::WallPiece> pieces;
        for (const auto& e : lotContent_->wallCoverings) {
            const auto w = walls.find(static_cast<std::int32_t>(e.wall));
            if (w == walls.end() || arches.contains(e.type) || e.type == kInvisibleWallStyle || e.type == kStageWallStyle) {
                continue;
            }
            const auto* from = graph.point(w->second->from);
            const auto* to = graph.point(w->second->to);
            if (from != nullptr && to != nullptr) {
                pieces.push_back({*from, *to, e.type});
            }
        }
        paintOutline_.emplace(pieces);
    }
    // The side of the wall face under the mouse. Stage sides are not done yet.
    std::optional<ts2::sim::WallPiece> picked;
    if (ray && !freeCam_) {
        picked = wallFaceAt(*ray);
    }
    const bool remove = ts2::platform::ctrlHeld();
    const bool room = ts2::platform::keyHeld(ts2::platform::Key::Shift);
    const std::string side = picked ? std::format("{} {} {} {} {}", picked->from.x, picked->from.y, picked->to.x, picked->to.y, picked->from.level)
                                    : std::string();
    const std::string mode = std::format("{} {} {}", remove, room, choice->pattern);
    const std::string key = std::format("{}|{}|{}", side, mode, paintHeld_);
    paintHover_ = picked;
    if (key != paintPlanKey_) {
        const bool modeChanged = mode != paintPlanMode_;
        paintPlanKey_ = key;
        paintPlanMode_ = mode;
        bool changed = false;
        // Without the button held, and on a change of Ctrl or Shift, the plan starts again.
        if (!paintHeld_ || modeChanged) {
            changed = !paintPlan_.empty();
            for (auto it = paintPlan_.rbegin(); it != paintPlan_.rend(); ++it) {
                wallManager_->setSidePattern(it->side, it->before);
            }
            paintPlan_.clear();
            paintPlanCost_ = 0;
        }
        if (picked) {
            // Ctrl puts the wall style's own pattern back, the blank one where it has none.
            const auto& scripts = assets_.get()->buildScripts();
            const auto style = scripts.wallStyles.find(picked->type);
            const std::string own = style != scripts.wallStyles.end() && !style->second.defaultPattern.empty() ? style->second.defaultPattern
                                                                                                           : std::string(kBlankPattern);
            const std::string pattern = remove ? own : choice->pattern;
            if (!room) {
                changed = planPaint(*picked, pattern) || changed;
            } else {
                // Shift goes round the room from the side both ways, as far as the sides have the covering
                // the side had.
                const auto around = paintOutline_->around(*picked);
                const std::string was = wallManager_->sidePattern(segmentOf(*picked)).value_or(std::string());
                const auto sameAsWas = [&](const ts2::sim::WallPiece& p) {
                    return sameName(wallManager_->sidePattern(segmentOf(p)).value_or(std::string()), was);
                };
                std::size_t forward = 0;
                for (; forward < around.size() && sameAsWas(around[forward]); ++forward) {
                    changed = planPaint(around[forward], pattern) || changed;
                }
                for (std::size_t back = around.size(); back > forward + 1 && sameAsWas(around[back - 1]); --back) {
                    changed = planPaint(around[back - 1], pattern) || changed;
                }
            }
        }
        if (changed) {
            wallManager_->save();
            ts2::content::refreshRoomWalls(*lotContent_);
            redrawLotWalls();
        }
    }
    if (paintPlan_.empty()) {
        wallLabels_.hide(screen_);
    } else {
        wallLabels_.show(screen_, paintPlanCost_, lotLife_->funds() < paintPlanCost_, std::format("{}", paintPlan_.size()), lotMouseUi_[0],
                         lotMouseUi_[1]);
    }
}

void Game::paintButton(bool down) {
    if (!lotLife_ || !lotContent_) {
        return;
    }
    paintHeld_ = down;
    paintPlanKey_.clear();
    if (down || paintPlan_.empty()) {
        return;
    }
    const std::int32_t cost = paintPlanCost_;
    if (cost > 0 && cost > lotLife_->funds()) {
        for (auto it = paintPlan_.rbegin(); it != paintPlan_.rend(); ++it) {
            wallManager_->setSidePattern(it->side, it->before);
        }
        paintPlan_.clear();
        paintPlanCost_ = 0;
        wallsChanged();
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
        return;
    }
    // The plan shown stays painted. A lot with no household and no Sim selected builds free.
    const std::int32_t charged = buildFree() ? 0 : cost;
    paintsDone_.push_back(std::exchange(paintPlan_, {}));
    paintPlanCost_ = 0;
    buyHand_.recordPaint(static_cast<std::uint32_t>(paintsDone_.size() - 1), charged);
    lotLife_->handPay(charged);
    // A change kept in the history plays the buy or the sell sound by what it cost.
    if (charged != 0) {
        moneyText_.show(screen_, charged, lotMouseUi_[0], lotMouseUi_[1]);
        if (audio_) {
            (void)audio_->playUi(charged > 0 ? "ui_object_buy" : "ui_object_sell");
        }
    }
    wallLabels_.hide(screen_);
}

void Game::stopPaintTool() {
    if (wallManager_ && !paintPlan_.empty()) {
        for (auto it = paintPlan_.rbegin(); it != paintPlan_.rend(); ++it) {
            wallManager_->setSidePattern(it->side, it->before);
        }
        paintPlan_.clear();
        wallsChanged();
    }
    paintPlan_.clear();
    paintPlanCost_ = 0;
    paintPlanKey_.clear();
    paintPlanMode_.clear();
    paintHover_.reset();
    paintHeld_ = false;
    paintOutline_.reset();
    wallLabels_.hide(screen_);
}

void Game::togglePaint(std::uint32_t id, bool in) {
    if (!lotContent_ || id >= paintsDone_.size()) {
        return;
    }
    // A plan on show goes first, so it doesn't stay painted over what comes back.
    stopPaintTool();
    startWallManager();
    const auto& done = paintsDone_[id];
    if (in) {
        for (const auto& p : done) {
            wallManager_->setSidePattern(p.side, p.after);
        }
    } else {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            wallManager_->setSidePattern(it->side, it->before);
        }
    }
    wallsChanged();
}

} // namespace app
