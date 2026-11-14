// Create-a-Sim from the neighborhood (Create New Family): the screen, and the Sim being made built
// into the neighborhood's scene out of sight and drawn as a picture for the screen. The Sim is made as
// Make New Character makes one; its hair and clothes are then changed in its age data, as a saved
// Sim keeps them.
#include "game.hpp"

#include "content/ui_meters.hpp"
#include "diag/log.hpp"
#include "scene/age_data.hpp"
#include "scene/portrait.hpp"
#include "scene/sim_builder.hpp"
#include "scene/sims.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <set>

namespace app {

namespace {

using ts2::scene::CatalogPart;
using ts2::scenegraph::Link;

// Where the Sim being made stands in the neighborhood's world: anywhere, as it is only drawn alone.
constexpr float kStandX = 0, kStandY = 0, kStandZ = 0;
// The picture's camera: this much of the Sim's height in view, from a little above its middle.
constexpr float kFovY = 0.5f, kHeightShown = 1.32f, kLookAt = 0.56f;
constexpr std::uint32_t kEveryday = 1, kHuman = 1;
// Parts of these flags are left out of what Create-a-Sim offers, as Make New Character leaves them out
// of what it draws (career uniforms, costumes, hats).
constexpr std::uint32_t kNotOffered = 1 | 8;
// The kinds of part (a property set's "parts") and an age data list's key (category << 16 | part).
constexpr std::uint32_t kHairPart = 1, kFacePart = 2, kTopPart = 4, kBodyPart = 8, kBottomPart = 16;
std::uint32_t listKey(std::uint32_t category, std::uint32_t part) { return category << 16 | part; }

bool sameLink(const Link& a, const Link& b) {
    return a.type == b.type && a.group == b.group && a.instance == b.instance && a.instanceHigh == b.instanceHigh;
}
std::string linkText(const Link& l) { return std::format("{:08x}{:08x}{:08x}{:08x}", l.type, l.group, l.instance, l.instanceHigh); }

// A hair colour (1 black, 2 brown, 3 blonde, 4 red) as the catalog writes tones (hex digits only) and
// as age data keeps it.
std::string toneDigits(int colour) { return std::format("{:08x}000000000000000000000000", colour); }
std::string toneGuid(int colour) { return std::format("{:08x}-0000-0000-0000-000000000000", colour); }

// The part of `family` in hair colour `tone` that fits outfit `category`, else any of the family in
// that colour. Null when the family has none in it.
const CatalogPart* hairOf(const ts2::scene::OutfitCatalog& catalog, const std::string& family, const std::string& tone, std::uint32_t category,
                          std::uint32_t age, std::uint32_t gender) {
    const CatalogPart* any = nullptr;
    for (const auto& p : catalog.parts) {
        if ((p.parts & kHairPart) && !(p.flags & kNotOffered) && p.family == family && p.hairTone == tone && (p.age & age) && (p.gender & gender)) {
            if (p.category == 0 || (p.category & category)) {
                return &p;
            }
            any = any ? any : &p;
        }
    }
    return any;
}

// The first entry of the list the Sim wears for `category` and `part`, if any.
const Link* worn(const ts2::scene::MadeLook& made, std::uint32_t category, std::uint32_t part) {
    const auto it = made.data.lists.find(listKey(category, part));
    return it == made.data.lists.end() || it->second.empty() ? nullptr : &it->second.front();
}

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
    casMade_.reset();
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
    casMade_.reset();
    casChoices_.clear();
    hoodUi_.hideAll(false);
    ts2::diag::step("create-a-sim: closed");
}

bool Game::casCatalogReady() const {
    return casCatalog_.valid() && casCatalog_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

void Game::casClicked(int window) {
    switch (cas_.clicked(window)) {
    case CasScreen::Action::Cancel: closeCas(); break;
    case CasScreen::Action::Remake:
        casMade_.reset();
        casRebuild_ = true;
        casListChoices_ = cas_.step() != CasScreen::Step::Genetics;
        break;
    case CasScreen::Action::Recolour:
        if (casMade_) {
            recolourHair(*casMade_, cas_.sim().hairColour);
        }
        casRebuild_ = true;
        casListChoices_ = true;
        break;
    case CasScreen::Action::ChoicesNeeded: casListChoices_ = true; break;
    case CasScreen::Action::Category:
        casListChoices_ = true;
        casRebuild_ = true;  // shown in the category's outfit
        break;
    case CasScreen::Action::Chosen:
        if (casMade_ && cas_.chosen() < casChoices_.size()) {
            wearChoice(*casMade_, casChoices_[cas_.chosen()]);
            casRebuild_ = true;
        }
        break;
    case CasScreen::Action::Turned: casRedraw_ = true; break;
    case CasScreen::Action::None: break;
    }
}

void Game::updateCas() {
    if (!cas_.isOpen() || !casCatalogReady()) {
        return;  // the catalog is read once, in the background; the Sim waits for it
    }
    if (casRebuild_) {
        casRebuild_ = false;
        buildCasSim();
        casRedraw_ = true;
    }
    if (casListChoices_ && casMade_) {
        casListChoices_ = false;
        listCasChoices();
    }
    if (casRedraw_ && casActor_) {
        casRedraw_ = false;
        drawCasSim();
        return;
    }
    // One choice's picture a frame, for the page shown.
    const auto missing = cas_.shownChoices();
    if (!missing.empty() && casMade_) {
        drawCasChoice(missing.front());
    }
}

bool Game::casPicturesPending() const { return cas_.isOpen() && !cas_.shownChoices().empty(); }

void Game::recolourHair(ts2::scene::MadeLook& made, int colour) {
    const auto& catalog = casCatalog_.get();
    const auto& sim = cas_.sim();
    made.data.properties["haircolor"] = toneGuid(colour);
    for (auto& [key, parts] : made.data.lists) {
        if ((key & 0xFFFF) != kHairPart) {
            continue;
        }
        for (auto& link : parts) {
            const CatalogPart* now = ts2::scene::findPart(catalog, link);
            if (const CatalogPart* other = now ? hairOf(catalog, now->family, toneDigits(colour), key >> 16, sim.age, sim.gender) : nullptr) {
                link = other->key;
            }
        }
    }
}

void Game::wearChoice(ts2::scene::MadeLook& made, const Link& choice) {
    const auto& catalog = casCatalog_.get();
    const auto& sim = cas_.sim();
    const CatalogPart* part = ts2::scene::findPart(catalog, choice);
    if (part == nullptr) {
        return;
    }
    if (part->parts & kHairPart) {
        // Set for all outfits: each outfit with a hair gets this style, in the version fitting it.
        for (auto& [key, parts] : made.data.lists) {
            if ((key & 0xFFFF) == kHairPart) {
                const CatalogPart* fit = hairOf(catalog, part->family, part->hairTone, key >> 16, sim.age, sim.gender);
                parts = {fit ? fit->key : choice};
            }
        }
        return;
    }
    const std::uint32_t c = sim.category;
    auto& lists = made.data.lists;
    if (part->parts & kBodyPart) {
        // A whole outfit takes the place of a top and a bottom.
        lists[listKey(c, kBodyPart)] = {choice};
        lists.erase(listKey(c, kTopPart));
        lists.erase(listKey(c, kBottomPart));
        return;
    }
    const std::uint32_t kind = (part->parts & kTopPart) ? kTopPart : kBottomPart, other = kind == kTopPart ? kBottomPart : kTopPart;
    lists[listKey(c, kind)] = {choice};
    lists.erase(listKey(c, kBodyPart));
    // A top needs a bottom with it, and a bottom a top: the first of the catalog's when there is none.
    if (!worn(made, c, other)) {
        ts2::scene::PartQuery q;
        q.age = sim.age;
        q.gender = sim.gender;
        q.species = kHuman;
        q.part = other;
        q.category = c;
        const auto found = ts2::scene::findParts(catalog, q);
        if (!found.empty()) {
            lists[listKey(c, other)] = {found.front()->key};
        }
    }
}

void Game::listCasChoices() {
    const auto& catalog = casCatalog_.get();
    const auto& sim = cas_.sim();
    casChoices_.clear();
    std::optional<std::size_t> wornChoice;
    ts2::scene::PartQuery q;
    q.age = sim.age;
    q.gender = sim.gender;
    q.species = kHuman;
    if (cas_.step() == CasScreen::Step::Hair) {
        // One of each style in the chosen colour, as everyday hair.
        q.part = kHairPart;
        q.category = kEveryday;
        q.hairTone = toneDigits(sim.hairColour);
        const Link* now = worn(*casMade_, kEveryday, kHairPart);
        const CatalogPart* nowPart = now ? ts2::scene::findPart(catalog, *now) : nullptr;
        std::set<std::string> families;
        for (const CatalogPart* p : ts2::scene::findParts(catalog, q)) {
            if ((p->flags & kNotOffered) || p->hairTone != *q.hairTone || !families.insert(p->family).second) {
                continue;
            }
            if (nowPart && p->family == nowPart->family) {
                wornChoice = casChoices_.size();
            }
            casChoices_.push_back(p->key);
        }
    } else if (cas_.step() == CasScreen::Step::Clothing) {
        q.category = sim.category;
        q.part = sim.clothes == CasScreen::Clothes::Tops ? kTopPart : sim.clothes == CasScreen::Clothes::Bottoms ? kBottomPart : kBodyPart;
        const Link* now = worn(*casMade_, sim.category, q.part);
        std::set<std::string> seen;
        for (const CatalogPart* p : ts2::scene::findParts(catalog, q)) {
            if ((p->flags & kNotOffered) || !seen.insert(linkText(p->key)).second) {
                continue;
            }
            if (now && sameLink(*now, p->key)) {
                wornChoice = casChoices_.size();
            }
            casChoices_.push_back(p->key);
        }
    }
    cas_.setChoices(casChoices_.size(), wornChoice);
}

std::optional<std::size_t> Game::buildCasLook(const ts2::scene::MadeLook& made, std::uint32_t category, bool hairOnly) {
    using namespace ts2::scene;
    auto& assets = *assets_.get();
    SimLook look = madeSimLook(assets, casCatalog_.get(), made, category);
    for (const auto& problem : look.problems) {
        ts2::diag::warnOnce("create-a-sim: " + problem);
    }
    if (hairOnly) {
        // A hair's picture: the head and the hair.
        std::erase_if(look.parts, [](const SimPart& p) { return p.part != kHairPart && p.part != kFacePart; });
    }
    if (look.parts.empty()) {
        return std::nullopt;
    }
    SceneBuilder builder(assets);
    std::vector<Transform> world;
    Actor actor = buildSim(builder, *casRigs_, look, Transform::at(kStandX, kStandY, kStandZ, 0), 0, "create-a-sim",
                           standingIdle(assets, look.age), world);
    builder.scene().actors.push_back(std::move(actor));
    const std::size_t first = hood_.scene.instances.size();
    appendScene(hood_.scene, std::move(builder.scene()), Transform{});
    renderer_.addMaterials(hood_.scene);
    renderer_.addMeshes(hood_.scene);
    for (std::size_t i = first; i < hood_.scene.instances.size(); ++i) {
        renderer_.setInstance(hood_.scene, i);
        setHoodHidden(i, true);  // drawn only into pictures
    }
    return hood_.scene.actors.size() - 1;
}

void Game::buildCasSim() {
    using namespace ts2::scene;
    const auto& sim = cas_.sim();
    try {
        if (!casMade_) {
            // The Sim as Make New Character makes one of this age, gender and skin, drawn by its seed,
            // in the hair colour chosen.
            OutfitRandom random(sim.seed);
            casMade_ = makeLook(NewCharacter{sim.age, sim.gender, sim.skin}, {}, casCatalog_.get(), random);
            recolourHair(*casMade_, sim.hairColour);
        }
        const std::size_t firstInstance = hood_.scene.instances.size();
        const auto actor = buildCasLook(*casMade_, cas_.step() == CasScreen::Step::Clothing ? sim.category : kEveryday, false);
        if (!actor) {
            ts2::diag::warning("create-a-sim: the Sim has nothing to wear, so it isn't shown");
            return;
        }
        casInstances_.clear();
        for (std::size_t i = firstInstance; i < hood_.scene.instances.size(); ++i) {
            casInstances_.push_back(i);
        }
        casActor_ = actor;
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::string("create-a-sim: the Sim can't be built: ") + ex.what());
    }
}

std::optional<ts2::tga::Pixels> Game::drawCasPicture(std::size_t actorIndex, int width, int height, bool face, float turnDegrees) {
    using namespace ts2::scene;
    if (actorIndex >= hood_.scene.actors.size() || width <= 0 || height <= 0) {
        return std::nullopt;
    }
    const Actor& actor = hood_.scene.actors[actorIndex];
    if (!actor.rig) {
        return std::nullopt;
    }
    auto& assets = *assets_.get();
    const auto pose = casAnimations_->bound(actor.rig, standingIdle(assets, actor.age));
    PortraitShot shot;
    if (!portraitShot(hood_.scene, actor, pose.get(), shot)) {
        return std::nullopt;
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
    ts2::render::Camera camera;
    if (face) {
        // The face as a portrait shows it.
        camera.eye = shot.camera.eye;
        camera.target = shot.camera.target;
        camera.fovY = shot.camera.fovY;
        camera.nearPlane = shot.camera.nearPlane;
        camera.farPlane = shot.camera.farPlane;
    } else {
        // The whole Sim from in front (where the portrait camera looks at its face from), turned.
        const auto& head = shot.camera.target;
        const float simHeight = std::max(0.2f, head[2] - kStandZ);
        float dx = shot.camera.eye[0] - head[0], dy = shot.camera.eye[1] - head[1];
        const float length = std::hypot(dx, dy);
        if (length > 1e-4f) {
            dx /= length;
            dy /= length;
        } else {
            dx = 0, dy = -1;
        }
        const float turn = turnDegrees * std::numbers::pi_v<float> / 180;
        const float tx = dx * std::cos(turn) - dy * std::sin(turn), ty = dx * std::sin(turn) + dy * std::cos(turn);
        // As much of the Sim's height as fits the picture's shape.
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const float shownHeight = simHeight * kHeightShown * std::max(1.0f, 0.55f / aspect);
        const float distance = shownHeight / (2 * std::tan(kFovY / 2));
        camera.target = {kStandX, kStandY, kStandZ + simHeight * kLookAt};
        camera.eye = {kStandX + tx * distance, kStandY + ty * distance, kStandZ + simHeight * kLookAt + simHeight * 0.08f};
        camera.fovY = kFovY;
        camera.nearPlane = 0.05f;
        camera.farPlane = distance * 4;
    }
    auto image = renderer_.drawInstancesToImage(camera, static_cast<unsigned>(width), static_cast<unsigned>(height), shot.lighting);
    // The neighborhood's own instances shown again for its next frame.
    std::vector<std::uint8_t> shown(hood_.scene.instances.size(), 1);
    for (std::size_t i = 0; i < shown.size() && i < hoodHidden_.size(); ++i) {
        shown[i] = hoodHidden_[i] ? 0 : 1;
    }
    renderer_.show(std::move(shown));
    if (!image) {
        return std::nullopt;
    }
    return ts2::content::unpremultiplied(ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)});
}

void Game::drawCasSim() {
    if (casActor_) {
        if (auto picture = drawCasPicture(*casActor_, cas_.previewWidth(), cas_.previewHeight(), false, cas_.turn())) {
            cas_.setPreview(*picture);
        }
    }
}

void Game::drawCasChoice(std::size_t choice) {
    if (choice >= casChoices_.size()) {
        return;
    }
    const bool hair = cas_.step() == CasScreen::Step::Hair;
    const auto& sim = cas_.sim();
    const std::string key = std::format("{} {} {} {}", linkText(casChoices_[choice]), sim.age, sim.gender, sim.skin);
    if (const auto known = casPictures_.find(key); known != casPictures_.end()) {
        cas_.setChoicePicture(choice, known->second);
        return;
    }
    // The Sim wearing it: its head for a hair, the whole Sim for clothes. Each is drawn once.
    ts2::tga::Pixels picture{1, 1, {0, 0, 0, 0}};
    try {
        auto made = *casMade_;
        wearChoice(made, casChoices_[choice]);
        if (const auto actor = buildCasLook(made, hair ? kEveryday : sim.category, hair)) {
            if (auto drawn = drawCasPicture(*actor, cas_.choiceWidth(), cas_.choiceHeight(), hair, 0)) {
                picture = std::move(*drawn);
            }
        }
    } catch (const std::exception& ex) {
        ts2::diag::warnOnce(std::string("create-a-sim: a choice's picture can't be drawn: ") + ex.what());
    }
    casPictures_[key] = picture;
    cas_.setChoicePicture(choice, picture);
}

} // namespace app
