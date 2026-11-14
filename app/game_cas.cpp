// Create-a-Sim from the neighborhood (Create New Family): the screen, and the Sim being made built
// into the neighborhood's scene out of sight and drawn as a picture for the screen.
#include "game.hpp"

#include "content/ui_meters.hpp"
#include "diag/log.hpp"
#include "scene/age_data.hpp"
#include "scene/portrait.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"

#include <cmath>
#include <format>
#include <numbers>

namespace app {

namespace {

// Where the Sim being made stands in the neighborhood's world: anywhere, as it is only drawn alone.
constexpr float kStandX = 0, kStandY = 0, kStandZ = 0;
// The picture's camera: this much of the Sim's height in view, from a little above its middle.
constexpr float kFovY = 0.5f, kHeightShown = 1.32f, kLookAt = 0.56f;
constexpr std::uint32_t kEveryday = 1;

} // namespace

void Game::openCas() {
    if (cas_.isOpen()) {
        return;
    }
    hoodUi_.hideAll(true);
    cas_.open(uiWidth_, uiHeight_, static_cast<std::uint32_t>(ts2::platform::ticksNs() & 0x7FFFFFFF));
    if (!casCatalog_.valid()) {
        casCatalog_ = std::async(std::launch::async, [packages = assets_.get()->skinCatalog()] { return ts2::scene::readOutfitCatalog(packages); })
                          .share();
    }
    if (!casRigs_) {
        casRigs_ = std::make_unique<ts2::scene::RigLibrary>(*assets_.get());
        casAnimations_ = std::make_unique<ts2::scene::AnimationLibrary>(*assets_.get());
    }
    casRebuild_ = true;
    ts2::diag::step("create-a-sim: opened");
}

void Game::closeCas() {
    if (!cas_.isOpen()) {
        return;
    }
    cas_.close();
    for (const std::size_t i : casInstances_) {
        setHoodHidden(i, true);
    }
    casInstances_.clear();
    casActor_.reset();
    hoodUi_.hideAll(false);
    ts2::diag::step("create-a-sim: closed");
}

void Game::casClicked(int window) {
    switch (cas_.clicked(window)) {
    case CasScreen::Action::Cancel: closeCas(); break;
    case CasScreen::Action::LookChanged: casRebuild_ = true; break;
    case CasScreen::Action::Turned: casRedraw_ = true; break;
    case CasScreen::Action::None: break;
    }
}

void Game::updateCas() {
    if (!cas_.isOpen()) {
        return;
    }
    // The catalog is read once, in the background; the Sim waits for it.
    if (casRebuild_ && casCatalog_.valid() && casCatalog_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        casRebuild_ = false;
        buildCasSim();
        casRedraw_ = true;
    }
    if (casRedraw_ && casActor_) {
        casRedraw_ = false;
        drawCasSim();
    }
}

void Game::buildCasSim() {
    using namespace ts2::scene;
    auto& assets = *assets_.get();
    const auto& sim = cas_.sim();
    try {
        // The Sim as Make New Character makes one of this age, gender and skin, drawn by its seed.
        OutfitRandom random(sim.seed);
        const MadeLook made = makeLook(NewCharacter{sim.age, sim.gender, sim.skin}, {}, casCatalog_.get(), random);
        const SimLook look = madeSimLook(assets, casCatalog_.get(), made, kEveryday);
        for (const auto& problem : look.problems) {
            ts2::diag::warnOnce("create-a-sim: " + problem);
        }
        if (look.parts.empty()) {
            ts2::diag::warning("create-a-sim: the Sim has nothing to wear, so it isn't shown");
            return;
        }
        SceneBuilder builder(assets);
        std::vector<Transform> world;
        Actor actor = buildSim(builder, *casRigs_, look, Transform::at(kStandX, kStandY, kStandZ, 0), 0, "create-a-sim",
                               standingIdle(assets, look.age), world);
        builder.scene().actors.push_back(std::move(actor));
        // The last Sim shown goes; the new one is put into the neighborhood's scene, hidden from it.
        for (const std::size_t i : casInstances_) {
            setHoodHidden(i, true);
        }
        casInstances_.clear();
        const std::size_t first = hood_.scene.instances.size();
        appendScene(hood_.scene, std::move(builder.scene()), Transform{});
        renderer_.addMaterials(hood_.scene);
        renderer_.addMeshes(hood_.scene);
        for (std::size_t i = first; i < hood_.scene.instances.size(); ++i) {
            renderer_.setInstance(hood_.scene, i);
            casInstances_.push_back(i);
            setHoodHidden(i, true);
        }
        casActor_ = hood_.scene.actors.size() - 1;
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::string("create-a-sim: the Sim can't be built: ") + ex.what());
    }
}

void Game::drawCasSim() {
    using namespace ts2::scene;
    if (!casActor_ || *casActor_ >= hood_.scene.actors.size()) {
        return;
    }
    const Actor& actor = hood_.scene.actors[*casActor_];
    if (!actor.rig) {
        return;
    }
    auto& assets = *assets_.get();
    const auto pose = casAnimations_->bound(actor.rig, standingIdle(assets, actor.age));
    PortraitShot shot;
    if (!portraitShot(hood_.scene, actor, pose.get(), shot)) {
        return;
    }
    // Only the Sim, posed as the shot has it.
    std::vector<std::uint8_t> visible(hood_.scene.instances.size(), 0);
    for (const auto& [instance, joints] : shot.joints) {
        visible[instance] = 1;
        renderer_.setJoints(instance, joints);
    }
    for (const auto& [instance, weights] : shot.morphs) {
        renderer_.setMorphWeights(instance, weights);
    }
    for (const auto& [instance, eyes] : shot.eyes) {
        renderer_.setEyes(instance, eyes);
    }
    renderer_.show(std::move(visible));
    // The whole Sim from in front (where the portrait camera looks at its face from), turned.
    const auto& head = shot.camera.target;
    const float height = std::max(0.2f, head[2] - kStandZ);
    float dx = shot.camera.eye[0] - head[0], dy = shot.camera.eye[1] - head[1];
    const float length = std::hypot(dx, dy);
    if (length > 1e-4f) {
        dx /= length;
        dy /= length;
    } else {
        dx = 0, dy = -1;
    }
    const float turn = cas_.turn() * std::numbers::pi_v<float> / 180;
    const float tx = dx * std::cos(turn) - dy * std::sin(turn), ty = dx * std::sin(turn) + dy * std::cos(turn);
    const float distance = height * kHeightShown / (2 * std::tan(kFovY / 2));
    ts2::render::Camera camera;
    camera.target = {kStandX, kStandY, kStandZ + height * kLookAt};
    camera.eye = {kStandX + tx * distance, kStandY + ty * distance, kStandZ + height * kLookAt + height * 0.08f};
    camera.fovY = kFovY;
    camera.nearPlane = 0.05f;
    camera.farPlane = distance * 4;
    const int w = cas_.previewWidth(), h = cas_.previewHeight();
    auto image = w > 0 && h > 0 ? renderer_.drawInstancesToImage(camera, static_cast<unsigned>(w), static_cast<unsigned>(h), shot.lighting)
                                : std::nullopt;
    // The neighborhood's own instances shown again for its next frame.
    std::vector<std::uint8_t> shown(hood_.scene.instances.size(), 1);
    for (std::size_t i = 0; i < shown.size() && i < hoodHidden_.size(); ++i) {
        shown[i] = hoodHidden_[i] ? 0 : 1;
    }
    renderer_.show(std::move(shown));
    if (image) {
        cas_.setPreview(ts2::content::unpremultiplied(ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)}));
    }
}

} // namespace app
