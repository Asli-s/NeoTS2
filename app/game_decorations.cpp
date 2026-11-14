// Decorations: the neighborhood's Decorations catalog (the puck's Decorations button, F3). An entry
// chosen in the catalog follows the mouse over the ground and is placed with a click; the Hand Tool
// picks a placed one up to move it (Delete deletes it), the Eyedropper chooses the entry of a placed
// one. What changed is written into the neighborhood's package when it is saved.
#include "game.hpp"

#include "content/neighborhood.hpp"
#include "content/package_rewrite.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace app {

namespace {

namespace pf = ts2::platform;

constexpr float kDegrees = 3.14159265358979f / 180.0f;
// How far the hand turns what it holds with comma and period.
constexpr float kTurnStep = 45.0f;

// Plants go in the plants list, everything else in the decorations list, as the game saves them.
bool inDecorationList(const ts2::content::NeighborhoodObjectType& type) { return type.sort != "flora"; }

} // namespace

float Game::hoodHeightAt(float x, float y) const {
    const auto& terrain = hood_.scene.terrain;
    if (!terrain || terrain->columns < 2 || terrain->rows < 2 || terrain->heights.size() < static_cast<std::size_t>(terrain->columns) * terrain->rows) {
        return 0;
    }
    // A vertex per tile corner, row by row: between the four around the point.
    const float fx = std::clamp(x / terrain->tileSize, 0.0f, static_cast<float>(terrain->columns - 1));
    const float fy = std::clamp(y / terrain->tileSize, 0.0f, static_cast<float>(terrain->rows - 1));
    const auto c = std::min(static_cast<std::uint32_t>(fx), terrain->columns - 2), r = std::min(static_cast<std::uint32_t>(fy), terrain->rows - 2);
    const float tx = fx - static_cast<float>(c), ty = fy - static_cast<float>(r);
    auto h = [&](std::uint32_t cc, std::uint32_t rr) { return terrain->heights[static_cast<std::size_t>(rr) * terrain->columns + cc]; };
    const float top = h(c, r) + (h(c + 1, r) - h(c, r)) * tx;
    const float bottom = h(c, r + 1) + (h(c + 1, r + 1) - h(c, r + 1)) * tx;
    return top + (bottom - top) * ty;
}

std::optional<std::array<float, 3>> Game::hoodGroundAt(float mx, float my) const {
    const auto& terrain = hood_.scene.terrain;
    if (!terrain || terrain->columns < 2 || terrain->rows < 2) {
        return std::nullopt;
    }
    const auto camera = orbit_.camera();
    const ts2::render::Vec3 d = ts2::render::normalize(screenRay(camera, width_, height_, mx, my));
    const float width = terrain->tileSize * static_cast<float>(terrain->columns - 1), depth = terrain->tileSize * static_cast<float>(terrain->rows - 1);
    // Along the ray in steps of half a tile until it goes under the ground, then halved down to it.
    const float step = terrain->tileSize / 2;
    auto at = [&](float t) { return std::array<float, 3>{camera.eye[0] + d[0] * t, camera.eye[1] + d[1] * t, camera.eye[2] + d[2] * t}; };
    auto above = [&](float t) {
        const auto p = at(t);
        return p[2] > hoodHeightAt(p[0], p[1]);
    };
    if (!above(0)) {
        return std::nullopt;
    }
    constexpr float kFar = 20000;
    for (float t = step; t < kFar; t += step) {
        const auto p = at(t);
        if (p[0] < -width || p[0] > 2 * width || p[1] < -depth || p[1] > 2 * depth) {
            break;
        }
        if (!above(t)) {
            float lo = t - step, hi = t;
            for (int i = 0; i < 20; ++i) {
                const float mid = (lo + hi) / 2;
                (above(mid) ? lo : hi) = mid;
            }
            auto ground = at(hi);
            if (ground[0] < 0 || ground[0] > width || ground[1] < 0 || ground[1] > depth) {
                return std::nullopt;  // off the neighborhood's ground
            }
            ground[2] = hoodHeightAt(ground[0], ground[1]);
            return ground;
        }
    }
    return std::nullopt;
}

std::array<float, 4> Game::decorationBounds(std::size_t instance, const std::array<float, 3>& position, float angle) const {
    // The model's own extent across the ground, turned with it.
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool any = false;
    if (instance < hood_.scene.instances.size()) {
        const int mesh = hood_.scene.instances[instance].mesh;
        if (mesh >= 0 && static_cast<std::size_t>(mesh) < hood_.scene.meshes.size()) {
            for (const auto& p : hood_.scene.meshes[static_cast<std::size_t>(mesh)].primitives) {
                for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                    const float x = p.positions[i], y = p.positions[i + 1];
                    x0 = any ? std::min(x0, x) : x;
                    y0 = any ? std::min(y0, y) : y;
                    x1 = any ? std::max(x1, x) : x;
                    y1 = any ? std::max(y1, y) : y;
                    any = true;
                }
            }
        }
    }
    const float c = std::cos(angle * kDegrees), s = std::sin(angle * kDegrees);
    std::array<float, 4> out{position[0], position[1], position[0], position[1]};
    for (const auto& [x, y] : {std::pair{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}}) {
        const float wx = position[0] + x * c - y * s, wy = position[1] + x * s + y * c;
        out[0] = std::min(out[0], wx);
        out[1] = std::min(out[1], wy);
        out[2] = std::max(out[2], wx);
        out[3] = std::max(out[3], wy);
    }
    return out;
}

void Game::setHoodHidden(std::size_t instance, bool hidden) {
    if (hoodHidden_.size() < hood_.scene.instances.size()) {
        hoodHidden_.resize(hood_.scene.instances.size(), 0);
    }
    if (instance >= hoodHidden_.size()) {
        return;
    }
    hoodHidden_[instance] = hidden ? 1 : 0;
    // With effect models, their placing passes this on each frame; without, it goes now.
    if (hood_.stats.effectModels.empty()) {
        std::vector<std::uint8_t> shown(hood_.scene.instances.size(), 1);
        for (std::size_t i = 0; i < shown.size() && i < hoodHidden_.size(); ++i) {
            shown[i] = hoodHidden_[i] ? 0 : 1;
        }
        renderer_.show(std::move(shown));
    }
}

std::optional<std::size_t> Game::addDecorationInstance(std::uint32_t type) {
    if (hoodObjectTypes_.empty()) {
        hoodObjectTypes_ = ts2::content::neighborhoodObjectTypes(ts2::content::findProducts(install_));
    }
    const auto entry = hoodObjectTypes_.find(type);
    if (entry == hoodObjectTypes_.end() || entry->second.model.empty()) {
        return std::nullopt;
    }
    const std::string& model = entry->second.model;
    const std::size_t index = hood_.scene.instances.size();
    // A model the neighborhood already shows is drawn again from the same mesh; another is read and
    // added to the scene.
    const auto same = std::find_if(hood_.scene.instances.begin(), hood_.scene.instances.end(), [&](const ts2::scene::Instance& i) {
        return i.name == model && i.mesh >= 0 && i.ground.has_value();
    });
    if (same != hood_.scene.instances.end()) {
        ts2::scene::Instance copy = *same;
        hood_.scene.instances.push_back(std::move(copy));
    } else {
        ts2::scene::SceneBuilder builder(*assets_.get());
        builder.setDrawShadows(true);
        const int mesh = builder.model(model, {});
        ts2::scene::Instance* placed = mesh >= 0 ? builder.place(mesh, model, ts2::scene::Transform{}) : nullptr;
        if (placed == nullptr) {
            ts2::diag::warnOnce("decorations: the model of a catalog entry is missing: " + model);
            return std::nullopt;
        }
        placed->litOutdoors = true;
        placed->seaReflected = true;
        placed->ground = std::array<float, 4>{};
        appendScene(hood_.scene, std::move(builder.scene()), ts2::scene::Transform{});
        renderer_.addMaterials(hood_.scene);
        renderer_.addMeshes(hood_.scene);
    }
    renderer_.setInstance(hood_.scene, index);
    setHoodHidden(index, true);
    return index;
}

std::optional<std::size_t> Game::hoodDecorationAt(const std::array<float, 3>& point) const {
    // Of the plants and decorations whose ground holds the point, the smallest.
    std::optional<std::size_t> best;
    float bestArea = std::numeric_limits<float>::infinity();
    auto consider = [&](std::size_t instance) {
        if (instance >= hood_.scene.instances.size() || (instance < hoodHidden_.size() && hoodHidden_[instance]) ||
            (heldDecoration_ && heldDecoration_->instance == instance)) {
            return;
        }
        const auto& g = hood_.scene.instances[instance].ground;
        if (!g || !(point[0] >= (*g)[0] && point[0] <= (*g)[2] && point[1] >= (*g)[1] && point[1] <= (*g)[3])) {
            return;
        }
        const float area = ((*g)[2] - (*g)[0]) * ((*g)[3] - (*g)[1]);
        if (area < bestArea) {
            bestArea = area;
            best = instance;
        }
    };
    for (const auto& r : hood_.stats.placedRecords) {
        if (!hoodEdits_.removed.contains({r.decoration, r.record})) {
            consider(r.instance);
        }
    }
    for (const auto& a : hoodEdits_.added) {
        consider(a.instance);
    }
    return best;
}

void Game::dropHeldDecoration(bool putBack) {
    if (!heldDecoration_) {
        return;
    }
    const HeldDecoration held = *heldDecoration_;
    heldDecoration_.reset();
    if (!held.moving) {
        setHoodHidden(held.instance, true);  // a catalog entry not placed goes away
        return;
    }
    if (!putBack) {
        setHoodHidden(held.instance, true);  // deleted
        return;
    }
    // Back where it stood, as the record it was or as placed this visit.
    const float turn = held.was.angle * kDegrees;
    const auto place = ts2::scene::Transform::at(held.was.position[0], held.was.position[1], held.was.position[2], turn);
    hood_.scene.instances[held.instance].transform = place;
    hood_.scene.instances[held.instance].ground = held.was.bounds;
    renderer_.setPlace(held.instance, place);
    setHoodHidden(held.instance, false);
    if (held.wasRecord) {
        hoodEdits_.removed.erase(*held.wasRecord);
    } else {
        const auto entry = hoodObjectTypes_.find(held.type);
        hoodEdits_.added.push_back({held.instance, entry != hoodObjectTypes_.end() && inDecorationList(entry->second), held.was});
    }
}

void Game::decorationToolChosen() {
    // The hand lets go of what it held: a picked-up one goes back.
    dropHeldDecoration(true);
    if (hoodUi_.decorationTool() == app::NeighborhoodUi::DecorationTool::Place) {
        const std::uint32_t type = hoodUi_.chosenDecoration();
        if (const auto instance = addDecorationInstance(type)) {
            heldDecoration_ = HeldDecoration{type, *instance};
        } else {
            hoodUi_.setDecorationTool(app::NeighborhoodUi::DecorationTool::None);
        }
    }
}

void Game::updateHeldDecoration() {
    if (heldDecoration_ && hoodUi_.bin() != app::NeighborhoodUi::Bin::Decorations) {
        dropHeldDecoration(true);  // the catalog closed
    }
    if (!heldDecoration_) {
        return;
    }
    float x = 0, y = 0;
    pf::mousePosition(x, y);
    const auto [ux, uy] = toUi(x, y);
    const auto ground = screen_.over(ux, uy) ? std::nullopt : hoodGroundAt(x, y);
    HeldDecoration& held = *heldDecoration_;
    if (!ground) {
        if (held.shown) {
            setHoodHidden(held.instance, true);
            held.shown = false;
        }
        return;
    }
    const auto place = ts2::scene::Transform::at((*ground)[0], (*ground)[1], (*ground)[2], held.angle * kDegrees);
    hood_.scene.instances[held.instance].transform = place;
    renderer_.setPlace(held.instance, place);
    if (!held.shown) {
        setHoodHidden(held.instance, false);
        held.shown = true;
    }
}

bool Game::decorationClick(float x, float y) {
    using Tool = app::NeighborhoodUi::DecorationTool;
    if (hoodUi_.bin() != app::NeighborhoodUi::Bin::Decorations || hoodUi_.decorationTool() == Tool::None) {
        return false;
    }
    const auto ground = hoodGroundAt(x, y);
    if (!ground) {
        return true;  // the tool's click, on nothing
    }
    if (heldDecoration_) {
        // Placed where the mouse is: a new record, or a picked-up one's new place.
        HeldDecoration held = *heldDecoration_;
        const auto entry = hoodObjectTypes_.find(held.type);
        if (entry == hoodObjectTypes_.end()) {
            return true;
        }
        ts2::nhood::PlacedObject object;
        object.position = *ground;
        object.angle = held.angle;
        object.type = held.type;
        object.bounds = decorationBounds(held.instance, *ground, held.angle);
        // Undo goes back to before it was picked up, or to the catalog entry not placed.
        const HoodEdits editsBefore = held.moving && pickedUp_ ? pickedUp_->first : hoodEdits_;
        const DecorationState stateBefore = held.moving && pickedUp_ ? pickedUp_->second : DecorationState{true, {}, {}};
        const auto place = ts2::scene::Transform::at((*ground)[0], (*ground)[1], (*ground)[2], held.angle * kDegrees);
        hood_.scene.instances[held.instance].transform = place;
        hood_.scene.instances[held.instance].ground = object.bounds;
        renderer_.setPlace(held.instance, place);
        setHoodHidden(held.instance, false);
        hoodEdits_.added.push_back({held.instance, inDecorationList(entry->second), object});
        recordDecorationStep(editsBefore, held.instance, stateBefore);
        heldDecoration_.reset();
        ts2::diag::step(std::format("decorations: placed {} at ({:.1f}, {:.1f})", entry->second.model, (*ground)[0], (*ground)[1]));
        // A catalog entry stays chosen: another one follows the mouse.
        if (!held.moving && hoodUi_.decorationTool() == Tool::Place) {
            if (const auto next = addDecorationInstance(held.type)) {
                heldDecoration_ = HeldDecoration{held.type, *next, held.angle};
            }
        }
        return true;
    }
    const auto under = hoodDecorationAt(*ground);
    if (!under) {
        return true;
    }
    // The saved record or the one placed this visit that the instance draws: its type and place.
    std::optional<std::pair<bool, std::size_t>> record;
    ts2::nhood::PlacedObject was;
    for (const auto& r : hood_.stats.placedRecords) {
        if (r.instance == *under) {
            record = std::pair{r.decoration, r.record};
            was = r.object;
        }
    }
    if (!record) {
        const auto a = std::find_if(hoodEdits_.added.begin(), hoodEdits_.added.end(), [&](const HoodEdits::Added& e) { return e.instance == *under; });
        if (a == hoodEdits_.added.end()) {
            return true;
        }
        was = a->object;
    }
    if (hoodObjectTypes_.empty()) {
        hoodObjectTypes_ = ts2::content::neighborhoodObjectTypes(ts2::content::findProducts(install_));
    }
    if (!hoodObjectTypes_.contains(was.type)) {
        return true;  // not an entry of the catalog
    }
    if (hoodUi_.decorationTool() == Tool::Eyedropper) {
        hoodUi_.setDecorationTool(Tool::Place, was.type);
        decorationToolChosen();
        return true;
    }
    if (hoodUi_.decorationTool() != Tool::Hand) {
        return true;
    }
    // Picked up: it follows the mouse until placed, deleted or put back.
    pickedUp_ = std::pair{hoodEdits_, decorationState(*under)};
    if (record) {
        hoodEdits_.removed.insert(*record);
    } else {
        std::erase_if(hoodEdits_.added, [&](const HoodEdits::Added& e) { return e.instance == *under; });
    }
    heldDecoration_ = HeldDecoration{was.type, *under, was.angle, true, true, was, record};
    return true;
}

bool Game::decorationKey(pf::Key key) {
    if (!heldDecoration_) {
        return false;
    }
    if (key == pf::Key::Escape) {
        dropHeldDecoration(true);
        if (hoodUi_.decorationTool() == app::NeighborhoodUi::DecorationTool::Place) {
            hoodUi_.setDecorationTool(app::NeighborhoodUi::DecorationTool::None);
        }
        return true;
    }
    if ((key == pf::Key::Delete || key == pf::Key::Backspace) && heldDecoration_->moving) {
        ts2::diag::step("decorations: deleted one");
        const std::size_t instance = heldDecoration_->instance;
        dropHeldDecoration(false);
        if (pickedUp_) {
            recordDecorationStep(pickedUp_->first, instance, pickedUp_->second);
        }
        return true;
    }
    if (key == pf::Key::Comma || key == pf::Key::Period) {
        float& angle = heldDecoration_->angle;
        angle = std::fmod(angle + (key == pf::Key::Comma ? kTurnStep : -kTurnStep) + 360.0f, 360.0f);
        return true;
    }
    return false;
}

Game::DecorationState Game::decorationState(std::size_t instance) const {
    const auto& i = hood_.scene.instances[instance];
    return {instance < hoodHidden_.size() && hoodHidden_[instance] != 0, i.transform, i.ground};
}

void Game::applyDecorationState(std::size_t instance, const DecorationState& state) {
    auto& i = hood_.scene.instances[instance];
    i.transform = state.transform;
    i.ground = state.ground;
    renderer_.setPlace(instance, state.transform);
    setHoodHidden(instance, state.hidden);
}

void Game::recordDecorationStep(const HoodEdits& before, std::size_t instance, const DecorationState& was) {
    decorationUndo_.push_back({before, hoodEdits_, instance, was, decorationState(instance)});
    decorationRedo_.clear();
    pickedUp_.reset();
    hoodUi_.setDecorationUndo(true, false);
}

void Game::undoDecoration(bool redo) {
    auto& from = redo ? decorationRedo_ : decorationUndo_;
    auto& to = redo ? decorationUndo_ : decorationRedo_;
    if (from.empty()) {
        return;
    }
    // What the hand holds goes back first, so the step meets the neighborhood as it left it.
    dropHeldDecoration(true);
    pickedUp_.reset();
    const DecorationStep step = from.back();
    from.pop_back();
    hoodEdits_ = redo ? step.after : step.before;
    applyDecorationState(step.instance, redo ? step.now : step.was);
    to.push_back(step);
    hoodUi_.setDecorationUndo(!decorationUndo_.empty(), !decorationRedo_.empty());
    ts2::diag::step(redo ? "decorations: redone" : "decorations: undone");
    // A catalog entry still chosen gets a new one to follow the mouse.
    if (hoodUi_.decorationTool() == app::NeighborhoodUi::DecorationTool::Place) {
        decorationToolChosen();
    }
}

void Game::clearDecorationSteps() {
    decorationUndo_.clear();
    decorationRedo_.clear();
    pickedUp_.reset();
    hoodUi_.setDecorationUndo(false, false);
}

bool Game::saveNeighborhood() {
    if (!hoodEdits_.changed()) {
        return true;
    }
    const auto file = ts2::content::neighborhoodPackage(openFolder_);
    std::array<std::size_t, 2> listSizes{};  // the plants' and decorations' records as saved before
    try {
        std::vector<ts2::content::ReplacedResource> changes;
        {
            ts2::dbpf::Package package{file};
            const auto entry = std::find_if(package.entries().begin(), package.entries().end(),
                                            [](const auto& e) { return e.key.type == ts2::nhood::kObjectsType; });
            if (entry == package.entries().end()) {
                throw std::runtime_error("the neighborhood has no objects resource");
            }
            const auto data = package.read(*entry);
            ts2::nhood::ObjectsLayout layout;
            ts2::nhood::parseObjects(data, &layout);
            listSizes = {layout.flora.records.size(), layout.decorations.records.size()};
            std::vector<bool> keepFlora(layout.flora.records.size(), true), keepDecorations(layout.decorations.records.size(), true);
            for (const auto& [decoration, record] : hoodEdits_.removed) {
                auto& keep = decoration ? keepDecorations : keepFlora;
                if (record < keep.size()) {
                    keep[record] = false;
                }
            }
            std::vector<ts2::nhood::PlacedObject> addFlora, addDecorations;
            for (const auto& a : hoodEdits_.added) {
                (a.decoration ? addDecorations : addFlora).push_back(a.object);
            }
            changes.push_back({entry->key, ts2::nhood::rewritePlaced(data, layout, keepFlora, addFlora, keepDecorations, addDecorations)});
        }
        const auto report = ts2::content::rewritePackage(file, changes);
        ts2::diag::step(std::format("saved the neighborhood's decorations: {} placed, {} taken away{}", hoodEdits_.added.size(), hoodEdits_.removed.size(),
                                    report.madeBackup ? ", the old package kept as " + report.backup.filename().string() : ""));
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("the neighborhood's decorations couldn't be saved: ") + ex.what());
        return false;
    }
    // The records now as saved: in each list the ones kept, in order (those the scene doesn't draw
    // too), then the ones placed. Later changes count from these.
    std::array<std::size_t, 2> next{};  // plants, decorations: the next record's place
    for (const bool decoration : {false, true}) {
        const std::size_t taken = static_cast<std::size_t>(std::count_if(hoodEdits_.removed.begin(), hoodEdits_.removed.end(),
                                                                         [&](const auto& r) { return r.first == decoration; }));
        next[decoration ? 1 : 0] = listSizes[decoration ? 1 : 0] - std::min(taken, listSizes[decoration ? 1 : 0]);
    }
    std::vector<ts2::scene::NeighborhoodSceneStats::PlacedRecord> records;
    for (const auto& r : hood_.stats.placedRecords) {
        if (hoodEdits_.removed.contains({r.decoration, r.record})) {
            continue;
        }
        auto now = r;
        now.record -= static_cast<std::size_t>(std::count_if(hoodEdits_.removed.begin(), hoodEdits_.removed.end(), [&](const auto& taken) {
            return taken.first == r.decoration && taken.second < r.record;
        }));
        records.push_back(now);
    }
    for (const auto& a : hoodEdits_.added) {
        records.push_back({a.instance, a.decoration, next[a.decoration ? 1 : 0]++, a.object});
    }
    hood_.stats.placedRecords = std::move(records);
    hoodEdits_ = {};
    clearDecorationSteps();  // the steps name records as they were before the save
    return true;
}

void Game::saveDecorationsOnLeaving() {
    if (!inNeighborhood_) {
        return;
    }
    dropHeldDecoration(true);
    hoodUi_.setDecorationTool(app::NeighborhoodUi::DecorationTool::None);
    if (hoodEdits_.changed()) {
        saveNeighborhood();
    }
}

} // namespace app
