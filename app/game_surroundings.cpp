// What shows around the lot: the neighbors' lots, as imposters or real houses, the ground and the
// neighborhood's decorations, at each View Distance.
#include "game.hpp"

#include "diag/log.hpp"
#include "scene/instance_merge.hpp"
#include "scene/lot_scene.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace {

// For each of `count` images by their keys (see SceneBuilder::imageKeys), the shared image it can use
// instead, -1 for none.
std::vector<int> sharedImageIndexes(const std::vector<std::string>& keys, const std::map<std::string, int>& shared, std::size_t count) {
    std::vector<int> at(count, -1);
    for (std::size_t i = 0; i < count && i < keys.size(); ++i) {
        if (const auto known = shared.find(keys[i]); known != shared.end()) {
            at[i] = known->second;
        }
    }
    return at;
}

// Builds a neighbor's lot as it looks from outside. It is lit once by day, as a played lot is by day so that
// its ground meets the ground around it, keeps what shows with its walls up and its roofs on, and takes no
// part in the lot's views.
ts2::scene::Scene neighborHouse(ts2::scene::Assets& assets, const std::filesystem::path& file, std::vector<std::string>& imageKeys) {
    using namespace ts2::scene;
    SceneBuilder builder(assets);
    const auto lot = ts2::content::loadLot(file);
    addLot(builder, lot, file);
    app::lightLot(assets, builder.scene(), lot, file, "day");
    imageKeys = builder.imageKeys();
    Scene scene = std::move(builder.scene());
    View view;
    view.level = topLevel(scene);
    view.walls = WallView::Up;
    const auto visible = visibleInstances(scene, view);
    std::vector<Instance> kept;
    kept.reserve(scene.instances.size());
    for (std::size_t i = 0; i < scene.instances.size(); ++i) {
        if (visible[i]) {
            Instance& instance = scene.instances[i];
            instance.part = Part::Other;
            instance.underside = Instance{}.underside;
            instance.walls.clear();
            instance.neighborHouse = true;
            // Objects' pictures from the sun fall on the played lot alone.
            instance.shadowMesh = -1;
            kept.push_back(std::move(instance));
        }
    }
    scene.instances = std::move(kept);
    scene.walls.clear();
    scene.actors.clear();
    scene.placedObjects.clear();
    scene.clip.reset();
    // Seen only from outside, so whatever is drawn alike is drawn together, in squares of 16 tiles
    // so that what is out of view is still left out.
    constexpr float kMergeCell = 16;
    mergeAlike(scene, kMergeCell);
    return scene;
}

} // namespace

namespace app {

std::pair<ts2::scene::Scene, ts2::scene::SkirtArea> Game::makeSkirt(const SkirtSource& s, int margin) {
    const auto area = ts2::scene::lotSkirtArea(*s.hood->terrain, s.footprint, margin);
    ts2::scene::SceneBuilder builder(*s.assets);
    builder.setPoolStyleSea(s.poolStyleSea);
    builder.setSmoothLotSkirts(s.smoothLotSkirts);
    ts2::scene::addLotSkirt(builder, *s.hood->terrain, s.footprint, margin, s.terrainType, s.roadTextures, s.lotHeights.get(), s.groundLevel,
                            s.worldBounds ? &*s.worldBounds : nullptr, &s.groundLook);
    return {std::move(builder.scene()), area};
}

std::vector<std::uint8_t> Game::lotShown() const {
    auto shown = ts2::scene::visibleInstances(lot_, lotView_.view);
    const auto hide = [&](std::size_t first, std::size_t count) {
        for (std::size_t i = first; i < first + count && i < shown.size(); ++i) {
            shown[i] = 0;
        }
    };
    for (const auto& [margin, around] : surroundings_) {
        if (margin != margin_) {
            hide(around.first, around.count);
        } else if (!lotViewOptions_.decorations) {
            hide(around.first, around.decorations);
        }
    }
    if (!lotViewOptions_.realHouses) {
        for (const NeighborHouse& house : neighborHouses_) {
            hide(house.first, house.count);
        }
    }
    // The neighbors' imposter objects hide with their imposters: all with Neighbors off, and a lot's
    // while its real house shows.
    const auto around = surroundings_.find(margin_);
    if (around != surroundings_.end()) {
        for (std::size_t i = around->second.first; i < around->second.first + around->second.decorations && i < shown.size(); ++i) {
            const auto& lot = lot_.instances[i].imposterLot;
            const bool built = lot && lotViewOptions_.realHouses &&
                               std::any_of(neighborHouses_.begin(), neighborHouses_.end(), [&](const NeighborHouse& h) { return h.id == *lot; });
            if (lot && (!lotViewOptions_.neighbors || built)) {
                shown[i] = 0;
            }
        }
    }
    return shown;
}

// The folder a neighborhood's High lot imposter pictures are kept in, out of its saves.
std::filesystem::path Game::imposterCache(const std::filesystem::path& hood) const {
    return options_.user / "NeoTS2 Cache" / "Imposters" / hood.filename();
}

// Lets the imposters' baker go on a thread of its own, as it waits for its work in flight and its asset
// reader takes a while to let go. Bakers let go earlier that are done are forgotten.
void Game::freeImposterBaker() {
    std::erase_if(imposterBakersFreeing_, [](const std::future<void>& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; });
    if (imposterBaker_) {
        imposterBakersFreeing_.push_back(std::async(std::launch::async, [baker = std::move(imposterBaker_)]() mutable { baker.reset(); }));
    }
    renderer_.releasePictureTargets();
}

// Starts drawing the open neighborhood's High lot imposters (or putting the game's own pictures back)
// for the lots whose imposters aren't so yet, and makes `high` what they are becoming.
void Game::startImposterBaker(bool high) {
    freeImposterBaker();
    highImposters_ = high;
    std::vector<app::ImposterBaker::Lot> lots;
    for (const auto& lot : hood_.stats.imposterLots) {
        if (lot.high != high) {
            lots.push_back({lot.lot, lot.file});
        }
    }
    if (!lots.empty()) {
        imposterBaker_ = std::make_unique<app::ImposterBaker>(install_, renderer_, imposterCache(openFolder_), std::move(lots), hood_.stats.imposterZOffset, high);
    }
}

// Moves the imposters' baking on between neighborhood frames, showing each lot as it is done.
void Game::stepImposterBaker() {
    if (!imposterBaker_ || !hood_.scene.terrain) {
        return;
    }
    auto baked = imposterBaker_->step(app::ImposterBaker::kStepBudget);
    bool renumbered = false;
    for (auto& done : baked) {
        // A lot's textures come uploaded ahead, so only its parts go up here, unless the textures
        // had to be numbered anew (all go up again then).
        renumbered = ts2::scene::replaceImposter(*hood_.scene.terrain, done.lot, std::move(done.imposter)) || renumbered;
        if (!renumbered) {
            renderer_.addImposters(hood_.scene, std::move(done.textures));
        }
        for (auto& lot : hood_.stats.imposterLots) {
            if (lot.lot.id == done.lot) {
                lot.high = highImposters_;
            }
        }
    }
    if (renumbered) {
        renderer_.setSurroundings(hood_.scene);
    }
    if (imposterBaker_->done()) {
        freeImposterBaker();
    }
}

void Game::applyLotViewOptions() {
    lotViewOptions_ = lotUi_.gameOptions().lotView();
    renderer_.setImpostersShown(lotViewOptions_.neighbors);
    if (skirtSource_) {
        showMargin(lotViewOptions_.skirtMargin.value_or(skirtSource_->defaultMargin));
    }
}

// With Real Houses, the neighbors within the View Distance start to build, the nearest first and one at
// a time. Their imposters show until each is ready. Each costs about 30 MB of video memory and 65 MB of
// memory. A house already built stays, ready to show again.
void Game::startNeighborHouses() {
    // No more than the renderer can leave out of the ground around the lot.
    constexpr std::size_t kMostHouses = ts2::render::kMaxNeighborGrounds;
    neighborsToBuild_.clear();
    const auto around = surroundings_.find(margin_);
    if (around == surroundings_.end() || !lotViewOptions_.realHouses) {
        return;
    }
    const float x = lotPlace_.m[3], y = lotPlace_.m[7];
    std::set<std::uint32_t> ids;
    for (const auto& part : around->second.imposters) {
        ids.insert(part.lot);
    }
    for (const auto& footprint : hood_.stats.lotFootprints) {
        const bool built = std::any_of(neighborHouses_.begin(), neighborHouses_.end(), [&](const NeighborHouse& house) { return house.id == footprint.id; });
        if (ids.contains(footprint.id) && !built) {
            neighborsToBuild_.push_back(footprint);
        }
    }
    const auto away = [&](const ts2::scene::LotFootprint& f) {
        const float dx = (f.x0 + f.x1) / 2 - x, dy = (f.y0 + f.y1) / 2 - y;
        return dx * dx + dy * dy;
    };
    std::sort(neighborsToBuild_.begin(), neighborsToBuild_.end(), [&](const auto& a, const auto& b) { return away(a) < away(b); });
    const std::size_t room = kMostHouses - std::min(kMostHouses, neighborHouses_.size());
    if (neighborsToBuild_.size() > room) {
        neighborsToBuild_.resize(room);
    }
    std::reverse(neighborsToBuild_.begin(), neighborsToBuild_.end());
    ts2::diag::info(std::format("lot view: {} neighbors to build as real houses", neighborsToBuild_.size()));
}

// The neighbors' imposters of a View Distance, less those with a real house showing.
// Real Houses in force: any left to build start, and what is built shows.
void Game::applyNeighborHouses() {
    showNeighborHouses();
    startNeighborHouses();
}

// The houses or their imposters show, and the ground around the lot makes room for the houses.
// The built houses in place of their imposters, with Real Houses on. The imposters stay uploaded.
void Game::showNeighborHouses() {
    std::set<std::uint32_t> leftOut;
    neighborGrounds_.clear();
    for (const NeighborHouse& house : neighborHouses_) {
        if (lotViewOptions_.realHouses) {
            leftOut.insert(house.id);
            neighborGrounds_.push_back(house.ground);
        }
    }
    renderer_.setImpostersLeftOut(std::move(leftOut));
    renderer_.setNeighborGrounds(neighborGrounds_);
}

void Game::buildNextNeighborHouse() {
    if (!inLot_ || neighborsToBuild_.empty() || neighborBuilding_.valid()) {
        return;
    }
    const std::uint32_t id = neighborsToBuild_.back().id;
    const auto file = openFolder_ / "Lots" / (openFolder_.filename().u8string() + u8"_Lot" + std::u8string(reinterpret_cast<const char8_t*>(std::to_string(id).c_str())) + u8".package");
    // Houses are built one at a time and the shared images change only as one is added, so they stay
    // as they are now until this one is added.
    const ts2::render::IRenderer* uploader = renderer_.capabilities().threadedPrepare ? &renderer_ : nullptr;
    neighborBuilding_ = std::async(std::launch::async, [install = install_, assets = neighborAssets_, file, id, shared = sharedImages_, uploader]() mutable {
        NeighborBuilt built;
        built.id = id;
        try {
            built.assets = assets ? std::move(assets) : std::make_shared<ts2::scene::Assets>(install);
            built.scene = neighborHouse(*built.assets, file, built.imageKeys);
            built.imageAt = sharedImageIndexes(built.imageKeys, shared, built.scene.images.size());
            if (uploader != nullptr) {
                built.gpu = uploader->prepareAddition(built.scene, built.imageAt);
            }
        } catch (const std::exception& ex) {
            ts2::diag::error(std::format("couldn't build neighbor's house, lot {}: {}", id, ex.what()));
            built.failed = true;
        }
        return built;
    });
}

void Game::addNeighborHouse(NeighborBuilt built) {
    neighborAssets_ = std::move(built.assets);
    const auto where = std::find_if(neighborsToBuild_.begin(), neighborsToBuild_.end(), [&](const ts2::scene::LotFootprint& f) { return f.id == built.id; });
    if (!inLot_ || where == neighborsToBuild_.end()) {
        return;
    }
    const ts2::scene::LotFootprint footprint = *where;
    neighborsToBuild_.erase(where);
    if (built.failed) {
        return;
    }
    NeighborHouse house;
    house.id = footprint.id;
    house.first = lot_.instances.size();
    house.count = built.scene.instances.size();
    // Anything the lot gained before goes up first, so the house's prepared upload lines up.
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    const std::size_t firstImage = lot_.images.size(), firstMesh = lot_.meshes.size();
    const std::vector<int> imageAt = sharedImageIndexes(built.imageKeys, sharedImages_, built.scene.images.size());
    if (imageAt != built.imageAt) {
        built.gpu.reset();
    }
    const std::size_t shared = static_cast<std::size_t>(std::count_if(imageAt.begin(), imageAt.end(), [](int i) { return i >= 0; }));
    // The images moved in keep their order, so the ones not shared are numbered from firstImage on.
    for (std::size_t i = 0, next = firstImage; i < imageAt.size(); ++i) {
        if (imageAt[i] < 0) {
            if (i < built.imageKeys.size() && !built.imageKeys[i].empty()) {
                sharedImages_.emplace(built.imageKeys[i], static_cast<int>(next));
            }
            ++next;
        }
    }
    app::appendScene(lot_, std::move(built.scene), footprint.place, &imageAt);
    renderer_.addPrepared(lot_, std::move(built.gpu));
    for (std::size_t i = house.first; i < house.first + house.count; ++i) {
        renderer_.setInstance(lot_, i);
    }
    // The pictures and meshes now on the GPU are never read again, unlike the lot's own light maps and
    // objects. Each primitive stays, empty, so the instances' lists by primitive still line up.
    for (std::size_t i = firstImage; i < lot_.images.size(); ++i) {
        lot_.images[i].rgba = {};
        lot_.images[i].smallerLevels = {};
        lot_.images[i].blockLevels = {};
    }
    for (std::size_t i = firstMesh; i < lot_.meshes.size(); ++i) {
        for (auto& p : lot_.meshes[i].primitives) {
            ts2::scene::Primitive empty;
            empty.material = p.material;
            empty.group = std::move(p.group);
            empty.alphaSort = p.alphaSort;
            empty.origin = p.origin;
            p = std::move(empty);
        }
    }
    house.ground = {footprint.x0, footprint.x1, footprint.y0, footprint.y1};
    const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
    float low[3] = {footprint.x0, footprint.y0, footprint.z}, high[3] = {footprint.x1, footprint.y1, footprint.top};
    toLot.apply(low, true);
    toLot.apply(high, true);
    for (std::size_t c = 0; c < 3; ++c) {
        house.box.low[c] = std::min(low[c], high[c]);
        house.box.high[c] = std::max(low[c], high[c]);
    }
    neighborHouses_.push_back(house);
    showNeighborHouses();
    renderer_.show(lotShown());
    ts2::diag::info(std::format("lot view: lot {} shows as a real house ({} instances, {} images shared)", footprint.id, house.count, shared));
}

// A house between the camera and the lot fades as its imposter would.
void Game::fadeNeighborHouses(const std::array<float, 3>& eye) {
    namespace propfade = ts2::scene::propfade;
    const float fade = lotViewOptions_.fadeSquared;
    const auto reach = fade > 0 ? propfade::reach(eye, lot_.lotSize) : std::nullopt;
    for (NeighborHouse& house : neighborHouses_) {
        const float alpha = reach ? propfade::imposterAlpha(house.box, eye, *reach, fade) : 1.0f;
        if (alpha == house.alpha) {
            continue;
        }
        house.alpha = alpha;
        const std::optional<std::array<float, 4>> color = alpha == 1.0f ? std::nullopt : std::optional(std::array<float, 4>{1, 1, 1, alpha});
        for (std::size_t i = house.first; i < house.first + house.count && i < lot_.instances.size(); ++i) {
            lot_.instances[i].shapeColor = color;
            renderer_.setShapeColor(i, color);
        }
    }
}

void Game::showMargin(int margin) {
    if (margin == margin_ || !skirtSource_) {
        return;
    }
    const auto built = surroundings_.find(margin);
    if (built == surroundings_.end()) {
        // One build at a time. The one that finishes looks for the margin wanted then.
        if (!surroundingsBuilding_.valid()) {
            surroundingsBuilding_ = std::async(std::launch::async, [source = skirtSource_, margin] {
                auto [skirt, area] = makeSkirt(*source, margin);
                SurroundingsBuilt out;
                out.margin = margin;
                out.scene = app::lotInNeighborhood(*source->hood, ts2::scene::Scene{}, std::move(skirt), source->footprint, area, &out.decorations);
                return out;
            });
        }
        return;
    }
    margin_ = margin;
    if (lot_.terrain) {
        lot_.terrain->imposters = built->second.imposters;
    }
    lot_.clip = built->second.clip;
    renderer_.setSurroundings(lot_);
}

void Game::addSurroundings(SurroundingsBuilt built) {
    if (!inLot_ || !skirtSource_ || surroundings_.contains(built.margin)) {
        return;
    }
    Surroundings around;
    around.first = lot_.instances.size();
    around.count = built.scene.instances.size();
    around.decorations = built.decorations;
    if (built.scene.terrain) {
        around.imposters = std::move(built.scene.terrain->imposters);
    }
    around.clip = built.scene.clip;
    app::appendScene(lot_, std::move(built.scene), ts2::scene::Transform{});
    // Lit for the time of day the lot is lit for now, as the rest of its skirt.
    const bool day = !lotDaylight_ || ts2::content::stateTime(lotDaylight_->state()) != ts2::content::StateTime::Night;
    ts2::scene::lightLotSkirt(lot_, day);
    ts2::scene::poolwater::lightPoolWater(lot_, day);
    renderer_.addMaterials(lot_);
    renderer_.addMeshes(lot_);
    for (std::size_t i = around.first; i < around.first + around.count; ++i) {
        renderer_.setInstance(lot_, i);
        lotPicker_.update(lot_, i);
    }
    ts2::diag::info(std::format("lot view: the ground {} tiles around the lot built ({} instances)", built.margin, around.count));
    surroundings_.emplace(built.margin, std::move(around));
    showMargin(lotViewOptions_.skirtMargin.value_or(skirtSource_->defaultMargin));
}

void Game::fadeProps(const std::array<float, 3>& eye) {
    namespace propfade = ts2::scene::propfade;
    fadeNeighborHouses(eye);
    const auto around = surroundings_.find(margin_);
    if (around == surroundings_.end()) {
        return;
    }
    Surroundings& shown = around->second;
    if (shown.propBoxes.size() != shown.decorations) {
        const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
        // The lot's tiles in the neighborhood. Its record places it on whole tiles.
        std::array<int, 4> lotTiles{};
        if (skirtSource_) {
            const auto& f = skirtSource_->footprint;
            lotTiles = {static_cast<int>(std::lround(f.x0 / 10)), static_cast<int>(std::lround(f.y0 / 10)), static_cast<int>(std::lround(f.x1 / 10)),
                        static_cast<int>(std::lround(f.y1 / 10))};
        }
        // Only the plants and decorations the game puts on its fade list get a box (see
        // fadesInLotView), and the rest are drawn as they are. Whether the game fades bridges is not
        // known yet, so they don't fade.
        shown.propBoxes.clear();
        for (std::size_t i = shown.first; i < shown.first + shown.decorations; ++i) {
            const auto* instance = i < lot_.instances.size() ? &lot_.instances[i] : nullptr;
            std::optional<propfade::Box> box;
            if (skirtSource_ && instance && instance->ground && instance->mesh >= 0 && static_cast<std::size_t>(instance->mesh) < lot_.meshes.size()) {
                box = propfade::placedBox(lot_.meshes[static_cast<std::size_t>(instance->mesh)], toLot * instance->transform);
                if (box && !propfade::fadesInLotView(*box, propfade::groundTiles(*instance->ground), lotTiles)) {
                    box.reset();
                }
            }
            shown.propBoxes.push_back(box);
        }
    }
    // With the Fade Distance off, or the camera over the lot, every prop is drawn as it is.
    const float fade = lotViewOptions_.fadeSquared;
    const auto reach = fade > 0 ? propfade::reach(eye, lot_.lotSize) : std::nullopt;
    for (std::size_t k = 0; k < shown.propBoxes.size(); ++k) {
        std::optional<float> alpha = 1.0f;
        if (reach && shown.propBoxes[k]) {
            alpha = propfade::alpha(*shown.propBoxes[k], eye, *reach, fade);
        }
        if (!alpha) {
            continue;
        }
        const std::optional<std::array<float, 4>> color =
            *alpha == 1.0f ? std::nullopt : std::optional(std::array<float, 4>{1, 1, 1, *alpha});
        const std::size_t i = shown.first + k;
        if (i < lot_.instances.size() && lot_.instances[i].shapeColor != color) {
            lot_.instances[i].shapeColor = color;
            renderer_.setShapeColor(i, color);
        }
    }

    // The neighbors' imposters: each a box around all of its parts, its ground among them.
    if (!shown.imposterBoxes) {
        const ts2::scene::Transform toLot = lotPlace_.rigidInverse();
        constexpr std::size_t kVertexFloats = 7;  // x, y, z, u, v, then the second set's u, v
        auto& boxes = shown.imposterBoxes.emplace();
        for (const auto& part : shown.imposters) {
            for (std::size_t v = 0; v + 2 < part.vertices.size(); v += kVertexFloats) {
                float at[3] = {part.vertices[v], part.vertices[v + 1], part.vertices[v + 2]};
                if (!std::isfinite(at[0]) || !std::isfinite(at[1]) || !std::isfinite(at[2])) {
                    continue;
                }
                toLot.apply(at, true);
                const auto [it, added] = boxes.try_emplace(part.lot, propfade::Box{{at[0], at[1], at[2]}, {at[0], at[1], at[2]}});
                for (std::size_t c = 0; c < 3 && !added; ++c) {
                    it->second.low[c] = std::min(it->second.low[c], at[c]);
                    it->second.high[c] = std::max(it->second.high[c], at[c]);
                }
            }
        }
    }
    std::map<std::uint32_t, float> alphas;
    if (reach) {
        for (const auto& [lot, box] : *shown.imposterBoxes) {
            if (const float a = propfade::imposterAlpha(box, eye, *reach, fade); a != 1.0f) {
                alphas.emplace(lot, a);
            }
        }
    }
    for (const auto& [lot, a] : imposterAlphas_) {
        if (!alphas.contains(lot)) {
            renderer_.setImposterAlpha(lot, 1);
        }
    }
    for (const auto& [lot, a] : alphas) {
        const auto was = imposterAlphas_.find(lot);
        if (was == imposterAlphas_.end() || was->second != a) {
            renderer_.setImposterAlpha(lot, a);
        }
    }
    imposterAlphas_ = std::move(alphas);
}

void Game::unfadeImposters() {
    for (const auto& [lot, a] : imposterAlphas_) {
        renderer_.setImposterAlpha(lot, 1);
    }
    imposterAlphas_.clear();
}

} // namespace app
