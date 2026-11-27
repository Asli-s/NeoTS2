#include "game.hpp"

#include "floor_tool.hpp"
#include "lot_wall_rebuild.hpp"
#include "placement_errors.hpp"

#include "diag/timing.hpp"
#include "scene/builder.hpp"
#include "scene/lot_parts.hpp"
#include "formats/scenegraph.hpp"
#include "scene/lot_scene.hpp"
#include "scene/lot_shadow.hpp"
#include "sim/placement_walls.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <limits>
#include <optional>
#include <set>

namespace app {

namespace {

// Whether a wall style is a half wall's GUID.
bool isHalfWall(const ts2::scene::Assets& assets, std::uint32_t style) {
    const auto* fence = assets.fence(style);
    return fence != nullptr && fence->halfWall;
}

// A style's kind and rules for the wall checks. Fences and half walls never stand in pond water,
// greenhouse fences never go diagonally, half walls and fences that bear load stand level, and fences
// Sims may walk over don't block walking.
ts2::content::WallRules wallRules(const ts2::scene::Assets& assets, const ts2::content::BuildScripts& scripts, std::uint32_t style) {
    using Kind = ts2::content::WallRules::Kind;
    if (const auto* fence = assets.fence(style)) {
        return fence->halfWall ? ts2::content::WallRules{Kind::HalfWall, true, false, true}
                               : ts2::content::WallRules{Kind::Fence, !fence->greenhouse, false, fence->bearsLoad, true, !fence->mayWalkOver};
    }
    if (scripts.fenceArches.count(style) != 0) {
        return {Kind::FenceArch, false, false, true};
    }
    constexpr std::uint32_t kWall300 = 300, kWall301 = 301;
    const Kind kind = style == kWall300 ? Kind::Wall300 : style == kWall301 ? Kind::Wall301 : Kind::Wall;
    const auto found = scripts.wallStyles.find(style);
    return found == scripts.wallStyles.end() ? ts2::content::WallRules{kind} : ts2::content::WallRules{kind, found->second.mayPlaceDiagonally, found->second.submersible, found->second.flatBottom,
                                                                                          found->second.supportUnderneath, found->second.blocksLocomotion};
}

// The marker effect at the corner under the mouse, by mode and by whether the style is a half wall's,
// while the button is up and while it is held.
const char* markerName(WallTool::Mode mode, bool halfWall, bool active) {
    switch (mode) {
    case WallTool::Mode::Demolish:
        if (halfWall) {
            return active ? "halfwalldrawdemolishactive" : "halfwalldrawdemolish";
        }
        return active ? "walldrawdemolishactive" : "walldrawdemolish";
    case WallTool::Mode::Room:
    case WallTool::Mode::DiagonalRoom:
    case WallTool::Mode::OctagonRoom: return active ? "roomdrawcreateactive" : "roomdrawcreate";
    case WallTool::Mode::Wall: break;
    }
    if (halfWall) {
        return active ? "halfwalldrawcreateactive" : "halfwalldrawcreate";
    }
    return active ? "walldrawcreateactive" : "walldrawcreate";
}

// A section in the wall graph's terms: the tool's x and y are the lot's, the graph's the other way
// round, and its levels count the ground storey as 0.
ts2::content::WallSegment segmentOf(const WallTool::Section& s) {
    return {static_cast<float>(s[0].y), static_cast<float>(s[0].x), static_cast<float>(s[1].y), static_cast<float>(s[1].x), s[0].storey - 1};
}

bool sameSegments(const std::vector<ts2::content::WallSegment>& a, const std::vector<ts2::content::WallSegment>& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const auto& l, const auto& r) {
        return l.ax == r.ax && l.ay == r.ay && l.bx == r.bx && l.by == r.by && l.level == r.level;
    });
}

ts2::scene::EmitterPlace placeAt(const std::array<float, 3>& point) {
    ts2::scene::EmitterPlace where;
    where.position = point;
    return where;
}

} // namespace

void Game::showWallMarker(const std::string& name) {
    if (!lotEffectPlayer_ || name == wallMarkerName_) {
        return;
    }
    if (wallMarker_) {
        wallMarker_->stop(true);
    }
    wallMarkerName_ = name;
    wallMarker_ = lotEffectPlayer_->create(name);
    if (wallMarker_) {
        wallMarker_->start(false);
    }
}

void Game::updateWallTool(std::span<const std::uint8_t> shown, const std::optional<std::array<float, 3>>& ray) {
    using Tool = app::LotUi::Tool;
    std::erase_if(wallPoofs_, [](const auto& p) { return !p->running(); });
    if (!lotLife_ || buyTool_ != Tool::Wall || lotUi_.mode() != app::LotUi::Mode::Build) {
        stopWallTool();
        return;
    }
    // The mode follows the keys, with the button up or held. With no keys held the catalog entry
    // chosen gives it.
    const auto choice = lotUi_.wallToolChoice();
    const auto base = app::WallTool::baseMode(choice ? choice->clone : 0);
    wallMode_ = app::WallTool::modeFor(ts2::platform::ctrlHeld(), ts2::platform::keyHeld(ts2::platform::Key::Shift), ts2::platform::altHeld(), base);
    if (ray && !freeCam_) {
        if (const auto floor = app::BuyHand::floorPoint(lot_, shown, lotPicker_, orbit_.camera().eye, *ray, lot_.groundLevel, lotContent_.get(), true)) {
            wallPoint_ = lotLife_->routePoint(floor->point);
            const int storey = wallStart_ ? wallStart_->storey : floor->storey;
            const auto corner = app::WallTool::snap(wallPoint_[0], wallPoint_[1], storey);
            // Each corner the drag reaches says so, while it builds. Demolishing says so only at a
            // corner a wall meets.
            if (wallStart_ && wallCorner_ && corner != *wallCorner_ && audio_) {
                if (wallMode_ != app::WallTool::Mode::Demolish) {
                    (void)audio_->playUi("ui_build_wall_section");
                } else if (wallManager_ && !wallManager_->wallsAt(static_cast<float>(corner.y), static_cast<float>(corner.x), corner.storey - 1).empty()) {
                    (void)audio_->playUi("ui_build_walldelete_section");
                }
            }
            wallCorner_ = corner;
        }
    }
    if (!wallCorner_) {
        return;
    }
    const bool halfWall = choice && isHalfWall(*assets_.get(), choice->style);
    showWallMarker(markerName(wallMode_, halfWall, wallStart_.has_value()));
    if (wallMarker_) {
        wallMarker_->place(placeAt(lotLife_->scenePointAt(static_cast<float>(wallCorner_->x), static_cast<float>(wallCorner_->y), wallCorner_->storey, lot_)));
    }
    // While dragging, the price and size of the walls planned.
    if (!wallStart_) {
        wallLabels_.hide(screen_);
        return;
    }
    // Demolishing takes out the walls along a line as the drag goes, shows the money they give back
    // and no size, and its marker goes to the line's end.
    if (wallMode_ == app::WallTool::Mode::Demolish) {
        const auto line = app::WallTool::line(*wallStart_, wallPoint_[0], wallPoint_[1]);
        if (wallMarker_) {
            wallMarker_->place(placeAt(lotLife_->scenePointAt(static_cast<float>(line.end.x), static_cast<float>(line.end.y), line.end.storey, lot_)));
        }
        wallPlanMode_ = wallMode_;
        showPlannedWalls({});
        showDemolishPlan(line.sections);
        wallLabels_.show(screen_, -wallRefund_, false, std::string(), lotMouseUi_[0], lotMouseUi_[1]);
        return;
    }
    showDemolishPlan({});
    // An octagonal room is planned once, as the button goes down, and the drag doesn't change it.
    using Mode = app::WallTool::Mode;
    if (wallMode_ == Mode::OctagonRoom && !wallOctagon_) {
        wallLabels_.hide(screen_);
        return;
    }
    const auto plan = wallPlan(wallMode_);
    if (wallMarker_) {
        wallMarker_->place(placeAt(lotLife_->scenePointAt(static_cast<float>(plan.end.x), static_cast<float>(plan.end.y), plan.end.storey, lot_)));
    }
    // The walls planned stand on the lot while the drag lasts, as the game puts them in. The price is
    // for the walls it could put in.
    wallPlanMode_ = wallMode_;
    showPlannedWalls(plan);
    const std::int32_t cost = (choice ? choice->price : 0) * static_cast<std::int32_t>(wallPlanned_.size());
    // An octagonal room shows no size.
    wallLabels_.show(screen_, cost, lotLife_->funds() < cost,
                     wallMode_ == Mode::OctagonRoom ? std::string() : app::WallTool::sizeText(*wallStart_, plan), lotMouseUi_[0], lotMouseUi_[1]);
}

void Game::wallButton(bool down) {
    if (!down) {
        placeWalls();
        wallStart_.reset();
        wallOctagon_.reset();
        wallLabels_.hide(screen_);
        return;
    }
    if (!wallCorner_ || !lotLife_) {
        return;
    }
    wallStart_ = wallCorner_;
    wallOctagon_.reset();
    if (wallMode_ == app::WallTool::Mode::OctagonRoom) {
        wallOctagon_ = app::WallTool::octagonRoom(*wallStart_, wallOctagonSize_, wallOctagonTurn_);
    }
    // A press puffs dust at its corner and says so.
    if (lotEffectPlayer_) {
        if (auto poof = lotEffectPlayer_->create("build_mode_tool_poof")) {
            poof->start(false);
            poof->place(placeAt(lotLife_->scenePointAt(static_cast<float>(wallStart_->x), static_cast<float>(wallStart_->y), wallStart_->storey, lot_)));
            wallPoofs_.push_back(std::move(poof));
        }
    }
    if (audio_) {
        (void)audio_->playUi(wallMode_ == app::WallTool::Mode::Demolish ? "ui_build_walldelete_mdown" : "ui_build_wall_mdown");
    }
}

void Game::stopWallTool() {
    if (wallMarker_) {
        wallMarker_->stop(true);
        wallMarker_.reset();
    }
    wallMarkerName_.clear();
    showPlannedWalls({});
    showDemolishPlan({});
    wallStart_.reset();
    wallCorner_.reset();
    wallOctagon_.reset();
    wallLabels_.hide(screen_);
}

void Game::startWallManager() {
    if (wallManager_ || !lotContent_) {
        return;
    }
    auto& lot = *lotContent_;
    const ts2::content::WorldBounds b = lot.worldBounds.value_or(ts2::content::WorldBounds{});
    wallManager_ = std::make_unique<ts2::content::WallManager>(lot.wallLayers, lot.grids, lot.savedWallCoverings, lot.wallPatterns,
                                                               ts2::content::WallBounds{b.minX, b.minY, b.minLevel, b.maxX, b.maxY, b.maxLevel},
                                                               assets_.get()->buildScripts());
    fenceHeights_.clear();
    wallManager_->setFences([this, assets = assets_.get()](std::uint32_t style) -> std::optional<ts2::content::WallManager::Fence> {
        const auto* fence = assets->fence(style);
        if (fence == nullptr) {
            return std::nullopt;
        }
        return ts2::content::WallManager::Fence{fence->halfWall, fence->postSpacing, fenceHeight(style, *fence), fence->bearsLoad};
    });
    wallManager_->setPosts(&lot.fencePosts);
    // Posts a plan put up or took down on an earlier lot are not this lot's.
    wallPlanPosts_.clear();
    wallDemolishPosts_.clear();
    wallDemolishPostsRemoved_.clear();
    wallDemolishPostsChanged_.clear();
    wallPlanPostsChanged_.clear();
}

float Game::fenceHeight(std::uint32_t style, const ts2::scene::Assets::Fence& fence) {
    if (const auto known = fenceHeights_.find(style); known != fenceHeights_.end()) {
        return known->second;
    }
    if (!fenceModels_) {
        fenceModels_ = std::make_unique<ts2::scene::SceneBuilder>(*assets_.get());
    }
    const std::string& model = fence.post.empty() ? fence.rail : fence.post;
    if (!model.empty()) {
        (void)fenceModels_->model(model, {});
    }
    const float height = ts2::scene::lotshadow::fenceTop(fenceModels_->scene(), fence.post, fence.rail);
    fenceHeights_.emplace(style, height);
    return height;
}

void Game::showPlannedWalls(const WallTool::Plan& plan) {
    std::vector<ts2::content::WallSegment> wanted;
    for (const auto& s : plan.sections) {
        wanted.push_back(segmentOf(s));
    }
    if (sameSegments(wanted, wallPlanWanted_) || !lotContent_ || !lot_.lotWalls || !lot_.lightMaps) {
        return;
    }
    wallPlanWanted_ = wanted;
    auto& lot = *lotContent_;
    const auto& scripts = assets_.get()->buildScripts();
    startWallManager();
    // The walls planned and their posts come out and the ground they levelled goes back, as the game
    // clears its plans.
    takeOutPlannedWalls();
    const bool groundBefore = !wallPlanGround_.empty();
    wallPlanGround_.toggle(lot);
    wallPlanGround_ = {};
    wallPlanOk_ = true;
    const auto choice = lotUi_.wallToolChoice();
    const std::uint32_t style = choice ? choice->style : 1;
    // Both sides get the style's default pattern, and a half wall's is the blank one, as the game saves it.
    const auto found = scripts.wallStyles.find(style);
    const std::string pattern = isHalfWall(*assets_.get(), style)          ? std::string("blank")
                                : found != scripts.wallStyles.end() ? found->second.defaultPattern
                                                                    : std::string();
    std::vector<ts2::content::TerrainRefusal> refusals;
    if (!wanted.empty() && wallStart_) {
        // Each section levels its corners to the ground at the drag's start before it goes in, and a
        // refused corner fails the whole plan: its walls come out and the ground levelled so far stays.
        ts2::content::TerrainEdit edit(lot, *wallManager_, scripts,
                                       [this](std::int32_t x, std::int32_t y, std::int32_t level) { return terrainObjects(x, y, level); },
                                       wallPlanGround_, lotLife_ ? lotLife_->simulation().world().lotShape().beachSide : std::nullopt);
        // A quarter is held up from below as the floor tool finds it.
        const auto support = app::floorSupport(lot, *assets_.get());
        const auto bounds = lot.worldBounds.value_or(ts2::content::WorldBounds{});
        edit.setHeldUp([&](const ts2::content::TileQuarter& q) {
            return support.heldUp(ts2::content::gridIndex(lot, q.level), q.x - bounds.minX, q.y - bounds.minY, std::countr_zero(q.mask));
        });
        const ts2::content::TerrainCorner start{wallStart_->y, wallStart_->x, wallStart_->storey - 1};
        const float h = edit.height(start);
        // The plan goes in run by run: each straight run of sections, as each side of a room, then for
        // a fence or half wall the run's posts.
        const bool fenceKind = assets_.get()->fence(style) != nullptr;
        const auto rules = wallRules(*assets_.get(), scripts, style);
        std::size_t runStart = 0;
        for (std::size_t i = 0; i < wanted.size() && wallPlanOk_; ++i) {
            const auto& s = wanted[i];
            // A section whose first refusal is a wall already on its edge is left out and the plan goes
            // on. Any other refusal fails the plan. The checks for objects other than Sims in the way are
            // not done yet.
            constexpr int kSharesEdge = 0x41;
            std::vector<ts2::content::TerrainRefusal> reasons;
            const bool levelled = edit.flattenSection(s, h, style, reasons);
            const bool passes = rules.kind != ts2::content::WallRules::Kind::FenceArch ? edit.checkWall(s, rules, reasons) && levelled : levelled;
            if (!reasons.empty() && reasons.front().code != kSharesEdge) {
                refusals = std::move(reasons);
                wallPlanOk_ = false;
                break;
            }
            if (passes && !wallManager_->has(s) && wallManager_->add(style, pattern, pattern, s)) {
                wallPlanned_.push_back({s, style, pattern, pattern});
            }
            const bool runEnds = i + 1 == wanted.size() || wanted[i + 1].ax != s.bx || wanted[i + 1].ay != s.by ||
                                 wanted[i + 1].bx - wanted[i + 1].ax != s.bx - s.ax || wanted[i + 1].by - wanted[i + 1].ay != s.by - s.ay;
            if (!runEnds) {
                continue;
            }
            if (fenceKind) {
                // Only the ends of this run's sections that went in get posts.
                const auto built = [&](float x, float y) {
                    return std::any_of(wallPlanned_.begin(), wallPlanned_.end(), [&](const auto& w) {
                        const auto& g = w.segment;
                        const bool inRun = std::any_of(wanted.begin() + static_cast<std::ptrdiff_t>(runStart), wanted.begin() + static_cast<std::ptrdiff_t>(i) + 1,
                                                       [&](const auto& r) { return r.ax == g.ax && r.ay == g.ay && r.bx == g.bx && r.by == g.by; });
                        return inRun && ((g.ax == x && g.ay == y) || (g.bx == x && g.by == y));
                    });
                };
                const auto& first = wanted[runStart];
                auto changes = wallManager_->addRunPosts(style, first.ax, first.ay, s.bx, s.by, s.level, static_cast<int>(i + 1 - runStart), built);
                wallPlanPosts_.insert(wallPlanPosts_.end(), changes.added.begin(), changes.added.end());
                for (const auto& was : changes.replaced) {
                    wallPlanPostsChanged_.push_back({was, *wallManager_->postAt(was.x, was.y, was.level)});
                }
            }
            runStart = i + 1;
        }
        if (!wallPlanOk_) {
            takeOutPlannedWalls();
        } else if (plan.shape == WallTool::Plan::Shape::Rectangle || plan.shape == WallTool::Plan::Shape::Diamond) {
            // A room's ground is levelled inside too.
            const ts2::content::TerrainCorner end{plan.end.y, plan.end.x, plan.end.storey - 1};
            wallPlanOk_ = plan.shape == WallTool::Plan::Shape::Rectangle ? edit.flattenBox(start, end, h, style, refusals)
                                                                         : edit.flattenDiamond(start, end, h, style, refusals);
        }
    }
    // The game says why, at the place it names, for its first reason.
    if (!refusals.empty() && lotLife_) {
        const auto& r = refusals.front();
        constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
        const std::string text = app::placementErrorText(
            [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, r.code, 0, 0);
        if (!text.empty()) {
            showWorldTip(text, lotLife_->scenePointAt(r.y, r.x, r.level + 1, lot_));
        }
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
    }
    wallsChanged();
    if (groundBefore || !wallPlanGround_.empty()) {
        groundChanged();
    }
}

std::vector<ts2::content::TerrainObject> Game::terrainObjects(std::int32_t x, std::int32_t y, std::int32_t level) {
    std::vector<ts2::content::TerrainObject> out;
    if (!lotLife_) {
        return out;
    }
    constexpr std::size_t kFacingField = 1, kPlacementField = 0x2A, kHiddenField = 0x22, kWallFlagsField = 0x0D;
    constexpr double kUnit = 65536;
    // Helper objects that never stop the storey under them getting lower: Come And See (and its
    // deleted copy and one more), Destination, Swim Here and the grave marker.
    constexpr std::uint32_t kHelpers[] = {0x4C859106, 0xACC53DFF, 0xEC85F0F3, 0x000007C4, 0xECFEDB37, 0x6CD4A8BC};
    static const std::map<std::uint32_t, std::string> kNone;
    const auto& saved = lotContent_ ? lotContent_->objectModels : kNone;
    lotLife_->simulation().world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const ts2::sim::SimObject& o) {
        // The simulation's tiles and positions run the other way round from the wall graph.
        if (o.removed || !o.tile || o.tile->y != x || o.tile->x != y || o.tile->level - 1 != level) {
            return false;
        }
        ts2::content::TerrainObject t;
        t.sim = o.isPerson();
        t.placement = static_cast<std::uint16_t>(o.data[kPlacementField]);
        t.hidden = static_cast<std::uint16_t>(o.data[kHiddenField]);
        t.wallFlags = static_cast<std::uint16_t>(o.data[kWallFlagsField]);
        // The simulation's footprint runs the other way round too.
        for (const auto& r : lotLife_->simulation().standingFootprint(o)) {
            t.footprint.push_back({r.y0, r.x0, r.y1, r.x1});
        }
        constexpr std::size_t kTestByBoxField = 0x4E;  // OBJD
        constexpr int kDoorType = 8;
        constexpr std::uint16_t kDoorWallFlags = 0x50;
        t.byBox = o.definition.field(ts2::objd::field::kMultiTileMasterId) == 0 &&
                  (o.definition.field(kTestByBoxField) != 0 || (o.definition.field(ts2::objd::field::kType) == kDoorType && (t.wallFlags & kDoorWallFlags) != 0));
        t.type = o.definition.field(ts2::objd::field::kType);
        t.x = static_cast<float>(o.y / kUnit);
        t.y = static_cast<float>(o.x / kUnit);
        t.direction = (2 - o.data[kFacingField]) & 7;
        // Its base where it stands: the ground under it as the lot was drawn.
        t.elevation = lot_.objectHeight ? lot_.objectHeight(level + 1, t.x, t.y, o.data[kFacingField], 0, static_cast<std::int16_t>(t.placement)) : 0.0f;
        const std::uint32_t guid = o.definition.guid();
        std::string model = o.model;
        if (model.empty()) {
            if (const auto m = saved.find(static_cast<std::uint32_t>(o.id)); m != saved.end()) {
                model = m->second;
            }
        }
        t.belowExempt = std::find(std::begin(kHelpers), std::end(kHelpers), guid) != std::end(kHelpers) || (!model.empty() && modelHasPostTrans(model));
        out.push_back(t);
        return false;
    });
    return out;
}

bool Game::modelHasPostTrans(const std::string& model) {
    if (const auto known = postTransModels_.find(model); known != postTransModels_.end()) {
        return known->second;
    }
    constexpr std::uint32_t kCres = 0xE519C933;  // resource node
    constexpr int kDeepest = 8;                  // models drawn inside models, a guard against loops
    bool found = false;
    std::vector<std::pair<std::string, int>> todo{{model, 0}};
    while (!todo.empty() && !found) {
        const auto [name, depth] = todo.back();
        todo.pop_back();
        const auto data = depth > kDeepest ? std::nullopt : assets_.get()->byName(kCres, name + "_cres");
        if (!data) {
            continue;
        }
        try {
            for (const auto& n : ts2::scenegraph::parseResourceNode(*data).nodes) {
                std::string lower = n.name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower == "post_trans") {
                    found = true;
                    break;
                }
                if (n.kind == ts2::scenegraph::SceneNode::Kind::ModelRef && !n.name.empty()) {
                    todo.emplace_back(n.name.ends_with("_cres") ? n.name.substr(0, n.name.size() - 5) : n.name, depth + 1);
                }
            }
        } catch (const std::exception&) {
            // A broken model has no such node.
        }
    }
    postTransModels_.emplace(model, found);
    return found;
}

std::optional<bool> Game::instanceStands(std::size_t instance) {
    if (!lotLife_) {
        return true;
    }
    for (const auto& placed : lot_.placedObjects) {
        if (placed.instance != instance) {
            continue;
        }
        if (placed.object > static_cast<std::uint32_t>(INT16_MAX)) {
            return std::nullopt;
        }
        auto& world = lotLife_->simulation().world();
        const auto id = static_cast<std::int16_t>(placed.object);
        if (world.find(id) != nullptr) {
            return true;
        }
        return world.findIncludingRemoved(id) != nullptr ? std::optional<bool>(false) : std::nullopt;
    }
    return true;
}

void Game::lotShapeChanged() {
    ts2::diag::Timer timer("walls: simulation takes the lot again");
    ts2::scene::setLotHeights(lot_, *lotContent_);
    for (auto& actor : lot_.actors) {
        actor.floorHeight = lot_.floorHeight;
    }
    if (lotLife_) {
        lotLife_->simulation().lotChanged(*lotContent_);
    }
    // The lot's light and the buildings' sun shadow are worked out again.
    if (lotDaylight_) {
        lotDaylight_->lotChanged(*assets_.get(), lot_, *lotContent_);
    }
}

void Game::groundChanged() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    auto meshes = ts2::scene::moveLotGround(lot_, *lotContent_, &runs);
    // Only the points that moved go to the renderer again: the whole ground takes a while to upload.
    for (const auto m : meshes) {
        renderer_.updateMeshVertices(lot_, m, runs);
    }
}

void Game::showDemolishPlan(const std::vector<WallTool::Section>& sections) {
    std::vector<ts2::content::WallSegment> wanted;
    for (const auto& s : sections) {
        wanted.push_back(segmentOf(s));
    }
    if (sameSegments(wanted, wallDemolishWanted_) || !lotContent_ || !lot_.lotWalls || !lot_.lightMaps) {
        return;
    }
    wallDemolishWanted_ = wanted;
    startWallManager();
    // The walls taken out by the last plan go back with their patterns, last first, and the objects
    // that went with them come back, and so do the posts it took down. Those it put up go.
    for (auto it = wallDemolishPosts_.rbegin(); it != wallDemolishPosts_.rend(); ++it) {
        wallManager_->removePost(it->x, it->y, it->level);
    }
    for (auto it = wallDemolishPostsRemoved_.rbegin(); it != wallDemolishPostsRemoved_.rend(); ++it) {
        wallManager_->addPost(*it);
    }
    for (auto it = wallDemolishPostsChanged_.rbegin(); it != wallDemolishPostsChanged_.rend(); ++it) {
        wallManager_->removePost(it->first.x, it->first.y, it->first.level);
        wallManager_->addPost(it->first);
    }
    wallDemolishPosts_.clear();
    wallDemolishPostsRemoved_.clear();
    wallDemolishPostsChanged_.clear();
    for (auto it = wallDemolished_.rbegin(); it != wallDemolished_.rend(); ++it) {
        wallManager_->add(it->style, it->left, it->right, it->segment);
    }
    for (auto it = wallDemolishSold_.rbegin(); it != wallDemolishSold_.rend(); ++it) {
        if (lotLife_) {
            lotLife_->handRestore(it->first, 0);
        }
    }
    wallDemolished_.clear();
    wallDemolishSold_.clear();
    wallRefund_ = 0;
    wallDemolishOk_ = true;
    // Then each wall standing on a section comes out in turn. Each gives back four fifths of its catalog
    // price, rounded down, and the objects hung on it are sold with it. A section without a wall adds a
    // refusal that is never shown. The plan fails at a refusal while the smallest code so far is not a
    // missing wall. Fences, arches, stage sides and the platform pieces reshaped where a wall goes are
    // not done yet.
    constexpr int kNoWall = 0x42, kNoWallEither = 0x41;
    constexpr float kRefund = 0.8f;
    std::set<int> refusals;
    const auto passes = [&] { return refusals.empty() || *refusals.begin() == kNoWall || *refusals.begin() == kNoWallEither; };
    for (const auto& s : wanted) {
        const auto kept = wallManager_->keptOn(s);
        if (!kept) {
            refusals.insert(kNoWall);
            continue;
        }
        // A style the wall tool may not delete, as a foundation's or deck's side, stays without a word.
        const auto& styles = assets_.get()->buildScripts().wallStyles;
        if (const auto style = styles.find(kept->style);
            style != styles.end() && (style->second.deleteTools & ts2::content::WallStyle::kDeletePartition) == 0) {
            continue;
        }
        std::vector<std::uint32_t> hung;
        const auto refused = demolishRefusals(kept->segment, hung);
        refusals.insert(refused.begin(), refused.end());
        if (!refused.empty()) {
            if (!passes()) {
                wallDemolishOk_ = false;
                break;
            }
            continue;
        }
        if (!wallManager_->remove(kept->segment)) {
            continue;
        }
        wallDemolished_.push_back({kept->segment, kept->style, kept->left, kept->right});
        wallRefund_ += static_cast<std::int32_t>(static_cast<float>(lotUi_.wallStylePrice(kept->style)) * kRefund);
        for (const std::uint32_t object : hung) {
            if (std::ranges::any_of(wallDemolishSold_, [&](const auto& s) { return s.first == object; })) {
                continue;
            }
            if (const auto value = sellWithWall(object)) {
                wallDemolishSold_.push_back({object, *value});
                wallRefund_ += *value;
            }
        }
    }
    // Then each corner of the line, both ends too: a fence or half wall's post no longer needed comes
    // down, and a corner that needs a post and has none gets the post of its tallest fence.
    if (wallDemolishOk_ && !wanted.empty()) {
        std::vector<std::array<float, 2>> corners{{wanted.front().ax, wanted.front().ay}};
        for (const auto& s : wanted) {
            corners.push_back({s.bx, s.by});
        }
        const std::int32_t level = wanted.front().level;
        const auto fence = [this](std::uint32_t style) { return assets_.get()->fence(style) != nullptr; };
        for (const auto& [x, y] : corners) {
            const auto post = wallManager_->postAt(x, y, level);
            const bool need = wallManager_->needsPost(x, y, level);
            if (!post && need) {
                if (const auto style = wallManager_->postStyleAt(x, y, level)) {
                    const ts2::fencepostlayer::Post p{level, x, y, *style};
                    if (wallManager_->addPost(p)) {
                        wallDemolishPosts_.push_back(p);
                    }
                }
            } else if (post && !need && fence(post->fence) && wallManager_->removePost(x, y, level)) {
                wallDemolishPostsRemoved_.push_back(*post);
            } else if (post && need) {
                // A post that stays becomes the post of the corner's tallest fence.
                if (const auto was = wallManager_->refreshPost(x, y, level)) {
                    wallDemolishPostsChanged_.push_back({*was, *wallManager_->postAt(x, y, level)});
                }
            }
        }
    }
    // The game says why, for its first reason that isn't a missing wall.
    for (const int code : refusals) {
        if (code == kNoWall || code == kNoWallEither || !lotLife_) {
            continue;
        }
        constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
        const std::string text = app::placementErrorText(
            [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, code, 0, 0);
        if (!text.empty()) {
            showWorldTip(text, lotLife_->scenePointAt(wallPoint_[0], wallPoint_[1], wallStart_ ? wallStart_->storey : 1, lot_));
        }
        if (audio_) {
            (void)audio_->playUi("ui_error");
        }
        break;
    }
    wallsChanged();
}

std::optional<std::int32_t> Game::sellWithWall(std::uint32_t object) {
    if (!lotLife_) {
        return std::nullopt;
    }
    const auto sale = lotLife_->handSale(object);
    std::int32_t value = sale ? sale->value : 0;
    // Aspiration and career rewards sell for nothing.
    constexpr std::size_t kCatalogFlagsField = 0x28;
    if (const auto* o = lotLife_->simulation().world().find(static_cast<std::int16_t>(object));
        o != nullptr && (o->definition.field(kCatalogFlagsField) & 0xC00) != 0) {
        value = 0;
    }
    return lotLife_->handKeep(object, 0) ? std::optional(value) : std::nullopt;
}

std::vector<int> Game::demolishRefusals(const ts2::content::WallSegment& section, std::vector<std::uint32_t>& hung) {
    std::vector<int> out;
    if (!lotLife_) {
        return out;
    }
    constexpr std::size_t kFacingField = 1, kContainerField = 2, kWallSidesField = 0x0D, kHiddenField = 0x22, kUseCountField = 0x3E;
    constexpr int kSimBeside = 0x4B, kHungInUse = 0x1B;
    // The tiles beside the section: either side of a tile edge, or the tile a diagonal crosses.
    const auto x0 = static_cast<std::int32_t>(std::floor(std::min(section.ax, section.bx)));
    const auto y0 = static_cast<std::int32_t>(std::floor(std::min(section.ay, section.by)));
    std::vector<std::array<std::int32_t, 2>> beside;
    if (section.ax == section.bx) {
        beside = {{x0 - 1, y0}, {x0, y0}};
    } else if (section.ay == section.by) {
        beside = {{x0, y0 - 1}, {x0, y0}};
    } else {
        beside = {{x0, y0}};
    }
    const auto same = [&](const std::array<std::int32_t, 4>& e) {
        const auto ax = static_cast<std::int32_t>(section.ax), ay = static_cast<std::int32_t>(section.ay);
        const auto bx = static_cast<std::int32_t>(section.bx), by = static_cast<std::int32_t>(section.by);
        return (e[0] == ax && e[1] == ay && e[2] == bx && e[3] == by) || (e[0] == bx && e[1] == by && e[2] == ax && e[3] == ay);
    };
    std::set<int> codes;
    lotLife_->simulation().world().nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const ts2::sim::SimObject& o) {
        // The simulation's tiles run the other way round from the wall graph.
        if (o.removed || !o.tile || o.tile->level - 1 != section.level) {
            return false;
        }
        const std::array<std::int32_t, 2> tile{o.tile->y, o.tile->x};
        if (std::ranges::find(beside, tile) == beside.end()) {
            return false;
        }
        const bool hidden = o.data[kHiddenField] != 0;
        if (o.isPerson()) {
            // A Sim held by an object, as one sitting in a chair, is not on the tile and doesn't refuse it.
            if (!hidden && o.data[kContainerField] <= 0) {
                codes.insert(kSimBeside);
            }
            return false;
        }
        // An object hangs on the section when one of the edges its wall sides need is the section.
        const auto edges = ts2::sim::PlacementWalls::neededEdges(static_cast<std::uint16_t>(o.data[kWallSidesField]), tile[0], tile[1],
                                                                 static_cast<std::int16_t>((2 - o.data[kFacingField]) & 7));
        if (!std::ranges::any_of(edges, same)) {
            return false;
        }
        if (!hidden && o.data[kUseCountField] > 0) {
            codes.insert(kHungInUse);
        }
        hung.push_back(lotLife_->handLead(static_cast<std::uint32_t>(o.id)));
        return false;
    });
    out.assign(codes.begin(), codes.end());
    return out;
}

app::WallTool::Plan Game::wallPlan(app::WallTool::Mode mode) const {
    using Mode = app::WallTool::Mode;
    if (!wallStart_) {
        return {};
    }
    switch (mode) {
    case Mode::OctagonRoom: return wallOctagon_.value_or(app::WallTool::Plan{});
    case Mode::Room: return app::WallTool::room(*wallStart_, wallPoint_[0], wallPoint_[1]);
    case Mode::DiagonalRoom: return app::WallTool::diagonalRoom(*wallStart_, wallPoint_[0], wallPoint_[1]);
    case Mode::Wall:
    case Mode::Demolish: break;
    }
    return app::WallTool::line(*wallStart_, wallPoint_[0], wallPoint_[1]);
}

bool Game::keepDemolished() {
    // The walls taken out stay out and give their money back, as one change in the undo history.
    if (wallDemolished_.empty()) {
        return false;
    }
    if (!wallDemolishOk_) {
        showDemolishPlan({});
        return false;
    }
    const std::int32_t refund = wallRefund_;
    const auto id = static_cast<std::uint32_t>(wallsBuilt_.size());
    wallsBuilt_.push_back({std::move(wallDemolished_), {}, true, std::move(wallDemolishSold_), std::move(wallDemolishPosts_),
                           std::move(wallDemolishPostsRemoved_), std::move(wallDemolishPostsChanged_)});
    wallDemolishPosts_.clear();
    wallDemolishPostsRemoved_.clear();
    wallDemolishPostsChanged_.clear();
    wallDemolished_.clear();
    wallDemolishSold_.clear();
    wallDemolishWanted_.clear();
    wallRefund_ = 0;
    lotLife_->handPay(-refund);
    buyHand_.recordWalls(id, -refund);
    lotShapeChanged();
    if (refund > 0) {
        moneyText_.show(screen_, -refund, lotMouseUi_[0], lotMouseUi_[1]);
        if (audio_) {
            (void)audio_->playUi("ui_object_sell");
        }
    }
    return true;
}

void Game::smashWall(const ts2::scene::WallInfo& wall, const std::array<float, 2>& at) {
    // A foundation's or deck's side takes out that piece of the platform in the game, which is not
    // done yet.
    if (!lotLife_ || !lotContent_ || !wall.cuttable) {
        return;
    }
    // The wall's ends are in the scene, so they go into the lot's frame like the mouse. The game takes
    // out the one tile's length of it under the mouse.
    const auto a = lotLife_->routePoint({wall.x0, wall.y0, 0});
    const auto b = lotLife_->routePoint({wall.x1, wall.y1, 0});
    const float dx = b[0] - a[0], dy = b[1] - a[1];
    const int tiles = static_cast<int>(std::lround(std::max(std::abs(dx), std::abs(dy))));
    if (tiles <= 0) {
        return;
    }
    const float sx = dx / static_cast<float>(tiles), sy = dy / static_cast<float>(tiles);
    const float along = ((at[0] - a[0]) * sx + (at[1] - a[1]) * sy) / (sx * sx + sy * sy);
    const int step = std::clamp(static_cast<int>(std::floor(along)), 0, tiles - 1);
    const int storey = wall.level - ts2::content::groundGridLevel(*lotContent_) + 1;
    const auto corner = [&](int i) {
        return app::WallTool::Corner{static_cast<int>(std::lround(a[0] + sx * static_cast<float>(i))),
                                     static_cast<int>(std::lround(a[1] + sy * static_cast<float>(i))), storey};
    };
    const app::WallTool::Section section{corner(step), corner(step + 1)};
    const auto mode = wallPlanMode_;
    wallPlanMode_ = app::WallTool::Mode::Demolish;
    wallStart_ = section[0];
    wallPoint_ = at;
    showDemolishPlan({section});
    if (!keepDemolished()) {
        showDemolishPlan({});
    }
    wallStart_.reset();
    wallPlanMode_ = mode;
}

void Game::smashBuilt(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point) {
    if (!lotLife_ || hit.instance >= lot_.instances.size()) {
        return;
    }
    const auto& instance = lot_.instances[hit.instance];
    const auto at = lotLife_->routePoint(point);
    if (instance.part == ts2::scene::Part::Wall && !instance.walls.empty()) {
        const auto w = static_cast<std::size_t>(instance.walls.front());
        if (w < lot_.walls.size()) {
            smashWall(lot_.walls[w], at);
        }
    } else if (instance.part == ts2::scene::Part::Floor) {
        smashFloor(instance.level, at);
    }
}

const ts2::content::Covering* Game::eyedropCovering(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point) {
    if (!lotLife_ || !lotContent_ || hit.object != 0 || hit.instance >= lot_.instances.size()) {
        return nullptr;
    }
    const auto& instance = lot_.instances[hit.instance];
    if (instance.part == ts2::scene::Part::Wall && !instance.walls.empty() && lot_.lotWalls && lotContent_->roomGraph) {
        const int first = lot_.lotWalls->firstWall;
        const auto thick = static_cast<std::size_t>(instance.walls.front() - first);
        if (instance.walls.front() < first || thick >= lot_.lotWalls->walls.size()) {
            return nullptr;
        }
        const auto& built = lot_.lotWalls->walls[thick];
        const auto& points = built.points;
        const auto& graph = *lotContent_->roomGraph;
        const auto section = std::find_if(graph.walls.begin(), graph.walls.end(),
                                          [&](const auto& w) { return w.from == points[0] && w.to == points[1]; });
        const auto* a = graph.point(points[0]);
        const auto* b = graph.point(points[1]);
        if (section == graph.walls.end() || a == nullptr || b == nullptr) {
            return nullptr;
        }
        const auto entry = std::find_if(lotContent_->wallCoverings.begin(), lotContent_->wallCoverings.end(),
                                        [&](const auto& e) { return e.wall == static_cast<std::uint32_t>(section->id); });
        if (entry == lotContent_->wallCoverings.end()) {
            return nullptr;
        }
        // The wall graph's x and y are the route's y and x. The left side is the one its room graph's
        // leftRoom names, where the point turns left from a to b. A side's face is half the wall's
        // thickness from its middle, and a point nearer is on the wall's top, which has no covering.
        const auto at = lotLife_->routePoint(point);
        const float dx = b->x - a->x, dy = b->y - a->y, length = std::sqrt(dx * dx + dy * dy);
        const float cross = dx * (at[0] - a->y) - dy * (at[1] - a->x);
        constexpr float kOnFace = 0.8f;
        if (length <= 0 || std::abs(cross) / length < built.halfThickness * kOnFace) {
            return nullptr;
        }
        const auto pattern = lotContent_->wallPatterns.find(cross < 0 ? entry->left : entry->right);
        return pattern == lotContent_->wallPatterns.end() || pattern->second.name.empty()
                   ? nullptr
                   : lotUi_.covering(pattern->second.name, ts2::content::CoveringKind::Wall);
    }
    if (instance.part != ts2::scene::Part::Floor || instance.underside != ts2::scene::Underside::None) {
        return nullptr;
    }
    const auto at = lotLife_->routePoint(point);
    // The floor grid's x and y are the route's y and x.
    const auto spot = app::floorSpotAt(at[1], at[0], instance.level);
    const std::string pattern = ts2::content::floorPatternAt(*lotContent_, spot.level, spot.x, spot.y, spot.quarter);
    return pattern.empty() ? nullptr : lotUi_.covering(pattern, ts2::content::CoveringKind::Floor);
}

void Game::showEyedropTip(const std::string& text, const std::array<float, 3>& point) {
    if (text.empty()) {
        if (!eyedropTip_.empty()) {
            eyedropTip_.clear();
            hideWorldTip();
        }
        return;
    }
    if (text != eyedropTip_) {
        eyedropTip_ = text;
        showWorldTip(text, point);
    }
    // It stays while the mouse is over what it names, following the mouse.
    worldTipPoint_ = point;
    worldTipShown_ = ts2::platform::ticksNs();
}

int Game::selectionImage(const std::string& texture) {
    if (const auto known = selectionImages_.find(texture); known != selectionImages_.end()) {
        return known->second;
    }
    ts2::scene::SceneBuilder builder(*assets_.get());
    const int material = builder.textureMaterial(texture);
    const auto& built = builder.scene();
    int at = -1;
    if (material >= 0 && static_cast<std::size_t>(material) < built.materials.size()) {
        const int image = built.materials[static_cast<std::size_t>(material)].image;
        if (image >= 0 && static_cast<std::size_t>(image) < built.images.size()) {
            at = static_cast<int>(lot_.images.size());
            lot_.images.push_back(built.images[static_cast<std::size_t>(image)]);
            renderer_.addMaterials(lot_);
        }
    }
    selectionImages_.emplace(texture, at);
    return at;
}

std::optional<ts2::render::TileHighlight> Game::smashHighlight(const ts2::scene::Picker::Hit& hit, const std::array<float, 3>& point) {
    if (!lotLife_ || !lotContent_ || hit.instance >= lot_.instances.size()) {
        return std::nullopt;
    }
    const auto& instance = lot_.instances[hit.instance];
    const std::int32_t ground = ts2::content::groundGridLevel(*lotContent_);
    const auto at = lotLife_->routePoint(point);
    ts2::render::TileHighlight h;
    if (instance.part == ts2::scene::Part::Wall && !instance.walls.empty()) {
        const auto w = static_cast<std::size_t>(instance.walls.front());
        if (w >= lot_.walls.size() || !lot_.lotWalls) {
            return std::nullopt;
        }
        const auto& wall = lot_.walls[w];
        const int first = lot_.lotWalls->firstWall;
        const auto thick = static_cast<std::size_t>(instance.walls.front() - first);
        if (instance.walls.front() < first || thick >= lot_.lotWalls->walls.size()) {
            return std::nullopt;
        }
        const auto& q = lot_.lotWalls->walls[thick].q;
        const float height = q.top0 - q.bottom0;
        // The tile's length of the wall under the mouse, as the sledgehammer takes it.
        const auto a = lotLife_->routePoint({wall.x0, wall.y0, 0});
        const auto b = lotLife_->routePoint({wall.x1, wall.y1, 0});
        const float dx = b[0] - a[0], dy = b[1] - a[1];
        const int tiles = static_cast<int>(std::lround(std::max(std::abs(dx), std::abs(dy))));
        if (tiles <= 0 || height <= 0) {
            return std::nullopt;
        }
        const float sx = dx / static_cast<float>(tiles), sy = dy / static_cast<float>(tiles);
        const float along = ((at[0] - a[0]) * sx + (at[1] - a[1]) * sy) / (sx * sx + sy * sy);
        const int step = std::clamp(static_cast<int>(std::floor(along)), 0, tiles - 1);
        const int storey = wall.level - ground + 1;
        const auto start = lotLife_->scenePointAt(a[0] + sx * static_cast<float>(step), a[1] + sy * static_cast<float>(step), storey, lot_);
        const auto end = lotLife_->scenePointAt(a[0] + sx * static_cast<float>(step + 1), a[1] + sy * static_cast<float>(step + 1), storey, lot_);
        // u along the tile from its start, and v up the wall from the floor under it.
        const float ex = end[0] - start[0], ey = end[1] - start[1], length = ex * ex + ey * ey;
        if (length <= 0) {
            return std::nullopt;
        }
        const float ux = ex / length, uy = ey / length, u0 = -(start[0] * ux + start[1] * uy);
        const float rise = end[2] - start[2];
        h.kind = ts2::render::TileHighlight::Kind::Wall;
        h.instance = hit.instance;
        h.image = selectionImage("wall_selection_colors");
        h.toTile = {ux, uy, 0, u0, -rise * ux / height, -rise * uy / height, 1 / height, (-start[2] - rise * u0) / height};
    } else if (instance.part == ts2::scene::Part::Floor && instance.underside == ts2::scene::Underside::None) {
        const auto spot = app::floorSpotAt(at[1], at[0], instance.level);
        bool floored = false;
        for (int quarter = 0; quarter < 4; ++quarter) {
            floored = floored || !ts2::content::floorPatternAt(*lotContent_, spot.level, spot.x, spot.y, quarter).empty();
        }
        if (!floored) {
            return std::nullopt;
        }
        // The floor grid's x and y are the route's y and x.
        const int storey = spot.level - ground + 1;
        const auto fx = static_cast<float>(spot.x), fy = static_cast<float>(spot.y);
        const auto o = lotLife_->scenePointAt(fy, fx, storey, lot_);
        const auto px = lotLife_->scenePointAt(fy, fx + 1, storey, lot_);
        const auto py = lotLife_->scenePointAt(fy + 1, fx, storey, lot_);
        const float ax = px[0] - o[0], ay = px[1] - o[1], bx = py[0] - o[0], by = py[1] - o[1];
        const float det = ax * by - ay * bx;
        if (std::abs(det) < 1e-6f) {
            return std::nullopt;
        }
        // The inverse of the tile's two edges takes a point's offset from its corner to u and v.
        const float r0x = by / det, r0y = -bx / det, r1x = -ay / det, r1y = ax / det;
        h.kind = ts2::render::TileHighlight::Kind::Floor;
        h.level = instance.level;
        h.image = selectionImage("floor_selection_colors");
        h.toTile = {r0x, r0y, 0, -(r0x * o[0] + r0y * o[1]), r1x, r1y, 0, -(r1x * o[0] + r1y * o[1])};
    } else {
        return std::nullopt;
    }
    if (h.image < 0) {
        return std::nullopt;
    }
    return h;
}

void Game::placeWalls() {
    if (!wallStart_ || !lotLife_) {
        showPlannedWalls({});
        showDemolishPlan({});
        return;
    }
    if (wallPlanMode_ == app::WallTool::Mode::Demolish) {
        if (keepDemolished()) {
            finishRelease();
        }
        return;
    }
    const auto choice = lotUi_.wallToolChoice();
    const std::int32_t price = choice ? choice->price : 0;
    const std::int32_t planned = price * static_cast<std::int32_t>(wallPlanned_.size());
    // A refused plan comes out. Short of money, a balloon at the mouse says so too.
    if (!wallPlanOk_) {
        showPlannedWalls({});
        return;
    }
    if (planned > 0 && planned > lotLife_->funds()) {
        constexpr std::uint32_t kUiText = 0x7FA7DC7C;  // string id, leakcheck: ok
        constexpr int kCantAfford = 0x16;  // placement error code
        const std::string text = app::placementErrorText(
            [&](std::uint32_t table, std::uint32_t line) { return screen_.data().text(kUiText, table << 16 | line); }, kCantAfford, 0, 0);
        if (!text.empty()) {
            showWorldTip(text, lotLife_->scenePointAt(wallPoint_[0], wallPoint_[1], wallStart_->storey, lot_));
        }
        showPlannedWalls({});
        return;
    }
    // For walls, the game plans once more to the corner let go at, in the mode of the last plan,
    // taking the walls out and putting them in again. Fences and half walls stay as the drag left them.
    if (!(choice && assets_.get()->fence(choice->style) != nullptr)) {
        wallPlanWanted_.clear();
        showPlannedWalls(wallPlan(wallPlanMode_));
    }
    if (wallPlanned_.empty()) {
        return;
    }
    // The walls stay and are paid for, and the undo history takes them as one change.
    const std::int32_t cost = buildFree() ? 0 : price * static_cast<std::int32_t>(wallPlanned_.size());
    const auto id = static_cast<std::uint32_t>(wallsBuilt_.size());
    wallsBuilt_.push_back({std::move(wallPlanned_), std::move(wallPlanGround_)});
    wallsBuilt_.back().posts = std::move(wallPlanPosts_);
    wallsBuilt_.back().postsChanged = std::move(wallPlanPostsChanged_);
    wallPlanned_.clear();
    wallPlanPosts_.clear();
    wallPlanPostsChanged_.clear();
    wallPlanGround_ = {};
    wallPlanWanted_.clear();
    lotLife_->handPay(cost);
    buyHand_.recordWalls(id, cost);
    lotShapeChanged();
    if (cost > 0) {
        moneyText_.show(screen_, cost, lotMouseUi_[0], lotMouseUi_[1]);
        if (audio_) {
            (void)audio_->playUi("ui_object_buy");
        }
    }
    finishRelease();
}

void Game::finishRelease() {
    if (lotEffectPlayer_) {
        if (auto poof = lotEffectPlayer_->create("build_mode_tool_poof")) {
            poof->start(false);
            poof->place(placeAt(lotLife_->scenePointAt(static_cast<float>(wallStart_->x), static_cast<float>(wallStart_->y), wallStart_->storey, lot_)));
            wallPoofs_.push_back(std::move(poof));
        }
    }
    if (audio_) {
        (void)audio_->playUi("ui_build_wall_mup");
    }
}

void Game::toggleBuiltWalls(std::uint32_t id, bool in) {
    if (id >= wallsBuilt_.size() || !wallManager_) {
        return;
    }
    // The ground they levelled first, then the walls, both ways going from the last wall built to the
    // first, as the game's undo does.
    auto& built = wallsBuilt_[id];
    built.ground.toggle(*lotContent_);
    // The objects a demolish sold come back with an undo and go again with a redo. Their money moves
    // with the walls' in the history.
    for (const auto& [object, value] : built.sold) {
        if (lotLife_) {
            if (in) {
                lotLife_->handKeep(object, 0);
            } else {
                lotLife_->handRestore(object, 0);
            }
        }
    }
    for (auto it = built.walls.rbegin(); it != built.walls.rend(); ++it) {
        // Undoing a demolish puts its walls back, redoing takes them out again.
        if (in != built.removed) {
            wallManager_->add(it->style, it->left, it->right, it->segment);
        } else {
            wallManager_->remove(it->segment);
        }
    }
    // A post the change made another fence's is as it was again after an undo, before the posts it
    // put up go, as one of those may be the changed one.
    if (!in) {
        for (auto it = built.postsChanged.rbegin(); it != built.postsChanged.rend(); ++it) {
            wallManager_->removePost(it->first.x, it->first.y, it->first.level);
            wallManager_->addPost(it->first);
        }
    }
    // The posts the change put up and took down go and come back with it.
    for (const auto& p : built.posts) {
        if (in) {
            wallManager_->addPost(p);
        } else {
            wallManager_->removePost(p.x, p.y, p.level);
        }
    }
    for (const auto& p : built.postsRemoved) {
        if (in) {
            wallManager_->removePost(p.x, p.y, p.level);
        } else {
            wallManager_->addPost(p);
        }
    }
    // And changed again after a redo, once the posts it put up stand.
    if (in) {
        for (const auto& [before, after] : built.postsChanged) {
            wallManager_->removePost(after.x, after.y, after.level);
            wallManager_->addPost(after);
        }
    }
    wallsChanged();
    if (!built.ground.empty()) {
        groundChanged();
    }
    lotShapeChanged();
}

bool Game::buildFree() {
    return lotLife_ && lotLife_->household().empty() && lotLife_->selected() <= 0;
}

void Game::takeOutPlannedWalls() {
    // Posts the plan changed get back what they were before the ones it added go, as a post it
    // added may have been changed since.
    for (auto it = wallPlanPostsChanged_.rbegin(); it != wallPlanPostsChanged_.rend(); ++it) {
        wallManager_->removePost(it->first.x, it->first.y, it->first.level);
        wallManager_->addPost(it->first);
    }
    for (auto it = wallPlanPosts_.rbegin(); it != wallPlanPosts_.rend(); ++it) {
        wallManager_->removePost(it->x, it->y, it->level);
    }
    wallPlanPosts_.clear();
    wallPlanPostsChanged_.clear();
    for (auto it = wallPlanned_.rbegin(); it != wallPlanned_.rend(); ++it) {
        wallManager_->remove(it->segment);
    }
    wallPlanned_.clear();
}

void Game::wallsChanged() {
    paintOutline_.reset();
    wallManager_->save();
    ts2::content::refreshRoomWalls(*lotContent_);
    redrawLotWalls();
}

void Game::recutWalls() {
    if (!lotLife_ || !lot_.lotWalls) {
        return;
    }
    if (!wallMasks_) {
        wallMasks_ = ts2::scene::wallMasks(assets_.get()->materialNames());
    }
    // Others may hold the lot's walls as they are, so the changes go to a copy.
    auto copy = std::make_shared<ts2::scene::LotWalls>(*lot_.lotWalls);
    auto& walls = copy->walls;
    const int first = copy->firstWall;
    bool changed = false;
    const auto uncut = [&](const std::vector<int>& tied, const std::vector<ts2::scene::WallMask>& masks) {
        for (const int w : tied) {
            if (w < first || static_cast<std::size_t>(w - first) >= walls.size()) {
                continue;
            }
            auto& wall = walls[static_cast<std::size_t>(w - first)];
            for (auto* mask : {&wall.leftMask, &wall.rightMask}) {
                if (std::any_of(masks.begin(), masks.end(), [&](const auto& m) { return m.material == *mask; })) {
                    mask->clear();
                }
            }
        }
    };
    // An object taken out of the scene (a purchase undone, a held catalog item let go of) leaves
    // its wall faces whole.
    for (auto it = cutsOf_.begin(); it != cutsOf_.end();) {
        const auto still = std::find_if(lot_.placedObjects.begin(), lot_.placedObjects.end(), [&](const auto& p) {
            return p.object == it->first && p.instance == it->second.instance && ts2::scene::lower(p.model) == it->second.model;
        });
        const auto masks = wallMasks_->find(it->second.model);
        if (still != lot_.placedObjects.end() || masks == wallMasks_->end()) {
            ++it;
            continue;
        }
        uncut(it->second.walls, masks->second);
        it = cutsOf_.erase(it);
        changed = true;
    }
    for (const auto& placed : lot_.placedObjects) {
        const auto masks = wallMasks_->find(ts2::scene::lower(placed.model));
        if (masks == wallMasks_->end() || placed.instance >= lot_.instances.size() || !instanceStands(placed.instance)) {
            continue;
        }
        auto& instance = lot_.instances[placed.instance];
        // One put aside by a sale cuts nothing until it stands again, nor one held where it can't go.
        const bool refused = buyHand_.carrying() && buyHand_.badSpot() && placed.object == buyHand_.carried();
        const auto cuts = instanceStands(placed.instance) == true && !refused
                              ? ts2::scene::wallCuts(walls, masks->second, placed.lotFrame.rigidInverse() * instance.transform, instance.level)
                              : std::vector<ts2::scene::WallCut>{};
        std::vector<int> tied;
        for (const auto& cut : cuts) {
            tied.push_back(first + static_cast<int>(cut.wall));
        }
        if (tied.empty()) {
            cutsOf_.erase(placed.object);
        } else {
            cutsOf_[placed.object] = {placed.instance, masks->first, tied};
        }
        // A half turn in place keeps its walls but swaps the faces each mask cuts.
        if (tied == instance.walls && std::all_of(cuts.begin(), cuts.end(), [&](const auto& cut) {
                return (cut.left ? walls[cut.wall].leftMask : walls[cut.wall].rightMask) == cut.material;
            })) {
            continue;
        }
        // The faces it cut where it stood are whole again.
        uncut(instance.walls, masks->second);
        for (const auto& cut : cuts) {
            (cut.left ? walls[cut.wall].leftMask : walls[cut.wall].rightMask) = cut.material;
        }
        instance.walls = std::move(tied);
        changed = true;
    }
    if (changed) {
        lot_.lotWalls = std::move(copy);
        redrawLotWalls();
    }
}

void Game::redrawLotWalls() {
    ts2::diag::Timer timer("walls: draw again");
    // The builder is kept for the lot, so the walls' materials and textures are read from the game's
    // files once. Each time it draws only the walls again.
    if (!wallBuilder_) {
        wallBuilder_ = std::make_unique<ts2::scene::SceneBuilder>(*assets_.get());
    }
    auto& built = wallBuilder_->scene();
    // The last time's meshes went into the lot's scene, the fences' rail models among them.
    built.meshes.clear();
    wallBuilder_->forgetModels();
    built.instances.clear();
    built.walls.clear();
    built.lotWalls.reset();
    {
        ts2::diag::Timer build("walls: build again");
        // Walls standing again get back the cuts of the doors and windows still standing in them.
        app::forgetCutsOfObjectsAway(lot_, [this](std::size_t i) { return instanceStands(i); });
        auto before = *lot_.lotWalls;
        before.gone = app::goneWithObjectsStanding(before, [this](std::size_t i) { return instanceStands(i); });
        ts2::scene::rebuildLotWalls(*wallBuilder_, *lotContent_, before, *lot_.lightMaps);
    }
    std::optional<ts2::diag::Timer> put("walls: put in the scene");
    auto up = app::putRebuiltWalls(lot_, built, wallBuilder_->imageKeys(), sharedImages_, [this](std::size_t i) { return instanceStands(i); });
    put.emplace("walls: upload");
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    renderer_.replaceMeshes(lot_, up.meshes);
    std::sort(up.instances.begin(), up.instances.end());
    up.instances.erase(std::unique(up.instances.begin(), up.instances.end()), up.instances.end());
    // The mouse picks the parts where they are drawn now.
    for (const std::size_t i : up.instances) {
        renderer_.setInstance(lot_, i);
        lotPicker_.update(lot_, i);
    }
    if (!up.images.empty()) {
        renderer_.setImages(lot_, up.images);
    }
    put.emplace("walls: show");
    renderer_.show(lotShown());
}

bool Game::wallKey(ts2::platform::Key key) {
    using Key = ts2::platform::Key;
    if (buyTool_ != app::LotUi::Tool::Wall || lotUi_.mode() != app::LotUi::Mode::Build) {
        return false;
    }
    if (key == Key::Minus || key == Key::Equals) {
        // The lot keys "-" and "=" make the octagonal room smaller and bigger. Only the lot's edges
        // limit how big it gets.
        wallOctagonSize_ = std::max(app::WallTool::kSmallestOctagon, wallOctagonSize_ + (key == Key::Equals ? 1 : -1));
    } else if ((key == Key::Comma || key == Key::Period) && wallMode_ == app::WallTool::Mode::OctagonRoom) {
        // "," and "." turn the octagonal room a quarter, as they turn a held object. Whether they turn
        // the camera in the tool's other modes is not known yet.
        wallOctagonTurn_ = (wallOctagonTurn_ + (key == Key::Comma ? 2 : 6)) % 8;
    } else {
        return false;
    }
    // A new size puts the room down again at the corner under the mouse.
    if (wallMode_ == app::WallTool::Mode::OctagonRoom) {
        wallButton(true);
    }
    return true;
}

} // namespace app
