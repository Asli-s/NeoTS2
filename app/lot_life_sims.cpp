// Sims drawn during play: those the loader builds, newborns, made Sims, new ages and outfit changes.
#include "lot_life.hpp"

#include "diag/log.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "scene/newborn_face.hpp"

#include <climits>
#include <ctime>
#include <format>

namespace app {

namespace {

// The rig's node for each of `bones` by name, -1 for an empty name or one the rig doesn't have.
std::vector<int> boneNodes(const ts2::scene::Rig* rig, const std::vector<std::string>& bones) {
    std::vector<int> nodes;
    nodes.reserve(bones.size());
    for (const auto& bone : bones) {
        nodes.push_back(rig != nullptr && !bone.empty() ? rig->find(bone) : -1);
    }
    return nodes;
}

} // namespace

void LotLife::freeSimInstance(ts2::scene::Scene& scene, std::size_t index) {
    scene.instances[index] = ts2::scene::Instance{};
    changedInstances_.push_back(index);
    freeSimInstances_.push_back(index);
}

void LotLife::keepSimParts(std::int16_t id, const ts2::scene::Actor& actor, bool drawnNow) {
    const auto created = drawnNow ? createdSims_.try_emplace(id).first : createdSims_.find(id);
    if (!drawnNow && created != createdSims_.end()) {
        created->second.clear();
    }
    for (const auto& s : actor.skinned) {
        changedInstances_.push_back(s.instance);
        if (created != createdSims_.end()) {
            created->second.push_back(s.instance);
        }
    }
}

void LotLife::loadSimsInBackground(const std::filesystem::path& install) {
    simLoader_ = std::make_unique<SimLoader>(install);
    sim_->world().setCharacterLoad([this](std::int16_t neighbor) -> std::optional<bool> {
        const auto file = sim_->characterFile(neighbor);
        if (!file) {
            return std::nullopt;
        }
        // A Sim made from the loaded character since it was last asked for takes it.
        auto& world = sim_->world();
        std::erase_if(claimingSims_, [&](std::int16_t id) {
            const auto* o = world.find(id);
            return o == nullptr || o->removed || !o->isPerson();
        });
        world.nextAfter(0, [&](const ts2::sim::SimObject& o) {
            if (o.isPerson() && !o.removed && world.neighborIdOf(o) == neighbor && claimingSims_.insert(o.id).second) {
                simLoader_->claim(neighbor);
            }
            return false;
        });
        return simLoader_->load(neighbor, *file, lotFrame_);
    });
}

bool LotLife::simLoading(const ts2::sim::SimObject& sim) const {
    const std::int16_t neighbor = simLoader_ ? sim_->world().neighborIdOf(sim) : std::int16_t{0};
    return neighbor != 0 && simLoader_->loading(neighbor);
}

bool LotLife::addLoadedSim(ts2::scene::Scene& scene, const ts2::sim::SimObject& sim, SimLoader::Built built) {
    for (const auto& problem : built.problems) {
        ts2::diag::warnOnce(std::format("Sim {}: {}", sim.id, problem));
    }
    if (!built.drawn) {
        return false;
    }
    const auto mapped = [](const std::map<int, int>& map, int index) {
        const auto it = map.find(index);
        return it != map.end() ? it->second : -1;
    };
    for (auto& [index, image] : built.images) {
        loadedImages_[index] = static_cast<int>(scene.images.size());
        scene.images.push_back(std::move(image));
    }
    for (auto& [index, cube] : built.envCubes) {
        loadedCubes_[index] = static_cast<int>(scene.envCubes.size());
        scene.envCubes.push_back(std::move(cube));
    }
    for (auto& [index, material] : built.materials) {
        material.image = mapped(loadedImages_, material.image);
        material.normalMap = mapped(loadedImages_, material.normalMap);
        material.mask = mapped(loadedImages_, material.mask);
        material.detail = mapped(loadedImages_, material.detail);
        material.envCube = mapped(loadedCubes_, material.envCube);
        loadedMaterials_[index] = static_cast<int>(scene.materials.size());
        scene.materials.push_back(std::move(material));
    }
    for (auto& [index, mesh] : built.meshes) {
        for (auto& p : mesh.primitives) {
            p.material = p.material >= 0 ? mapped(loadedMaterials_, p.material) : p.material;
        }
        loadedMeshes_[index] = static_cast<int>(scene.meshes.size());
        scene.meshes.push_back(std::move(mesh));
    }
    // Into the scene's instances, taking the places removed Sims left first.
    std::vector<std::size_t> placed;
    for (auto& instance : built.instances) {
        instance.mesh = mapped(loadedMeshes_, instance.mesh);
        instance.level = scene.groundLevel + sim.level - 1;
        if (!freeSimInstances_.empty() && freeSimInstances_.back() < scene.instances.size()) {
            placed.push_back(freeSimInstances_.back());
            freeSimInstances_.pop_back();
            scene.instances[placed.back()] = std::move(instance);
        } else {
            placed.push_back(scene.instances.size());
            scene.instances.push_back(std::move(instance));
        }
    }
    ts2::scene::Actor actor = std::move(built.actor);
    for (auto& s : actor.skinned) {
        s.instance = placed.at(s.instance);
    }
    for (auto& h : actor.held) {
        h.instance = placed.at(h.instance);
    }
    actor.object = static_cast<std::uint32_t>(sim.id);
    actor.lotFrame = lotFrame_;
    actor.floorHeight = scene.floorHeight;
    actor.slotNodes = boneNodes(actor.rig.get(), built.slotBones);
    actor.targetNodes = boneNodes(actor.rig.get(), built.targetBones);
    keepSimParts(sim.id, actor, true);
    scene.actors.push_back(std::move(actor));
    return true;
}

// The looks drawn for aging Sims' outfits and for Sims Make New Character makes come from the game's
// own generator, seeded once a run from the time since the run started, as the game seeds it.
namespace {
ts2::scene::OutfitRandom& lookRandom() {
    static ts2::scene::OutfitRandom random(static_cast<std::uint32_t>(std::clock()));
    return random;
}
} // namespace

// The look of a Sim Make New Character made, drawn once for its neighbor and kept, as the game
// writes it into the Sim's new character file. Its clothes come from the outfit object's age data
// of the Sim's age and gender (or of none), one of them drawn by a generator seeded from the clock
// when there are several.
const ts2::scene::MadeLook* LotLife::madeLook(std::int16_t neighbor, const ts2::sim::SavedNeighbor::MadeAs& as) {
    if (const auto it = madeLooks_.find(neighbor); it != madeLooks_.end()) {
        return &it->second;
    }
    std::vector<ts2::scene::AgeData> outfits;
    if (as.outfit != 0) {
        std::vector<ts2::scene::AgeData> fitting;
        for (auto& data : outfitAgeData(as.outfit)) {
            const std::uint32_t age = ts2::propset::number(data.properties, "age");
            const std::uint32_t gender = ts2::propset::number(data.properties, "gender");
            if ((age == 0 || (age & static_cast<std::uint32_t>(as.age))) && (gender == 0 || (gender & static_cast<std::uint32_t>(as.gender)))) {
                fitting.push_back(std::move(data));
            }
        }
        if (fitting.size() > 1) {
            ts2::scene::OutfitRandom draw(static_cast<std::uint32_t>(std::clock()));
            outfits.push_back(std::move(fitting[draw.below(static_cast<std::uint32_t>(fitting.size()))]));
        } else {
            outfits = std::move(fitting);
        }
    }
    const ts2::scene::NewCharacter who{static_cast<std::uint32_t>(as.age), static_cast<std::uint32_t>(as.gender), as.skin};
    return &madeLooks_.emplace(neighbor, ts2::scene::makeLook(who, outfits, outfitCatalog_.get(), lookRandom())).first->second;
}

// A Sim created during play (a visitor) is drawn in what its character file has it wear,
// standing idle as the lot's Sims start. It is placed with the others on the next update. A newborn
// has no character file yet: it wears what the game picks for its genes, and has no slots. A Sim the
// loader built is taken from it. A Sim Make New Character made wears the look drawn for it.
bool LotLife::addCreatedSim(ts2::scene::Scene& scene, const ts2::sim::SimObject& sim) {
    constexpr std::int16_t kBabyLifeStage = 1;
    constexpr std::size_t kSpeciesPersonData = 0xBA;  // 0 human
    const std::int16_t neighbor = sim_->world().neighborIdOf(sim);
    const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
    if (!scene.floorHeight) {
        return false;
    }
    // A Sim the loader couldn't build is built here, in case its trouble was the loader's own.
    if (auto built = simLoader_ && file ? simLoader_->take(neighbor, *file) : std::nullopt; built && built->drawn) {
        return addLoadedSim(scene, sim, std::move(*built));
    }
    std::optional<ts2::scene::Assets::CharacterInUse> inUse;
    ts2::scene::SimLook look;
    if (file) {
        inUse.emplace(assets_, *file);
        look = ts2::scene::simLook(assets_, *file);
    } else if (const auto* made = neighbor != 0 ? sim_->world().neighbor(neighbor) : nullptr;
               made != nullptr && sim.personData[ts2::sim::kLifeStagePersonData] == kBabyLifeStage) {
        // A newborn has no character file yet: it wears what the game picks for its skin gene.
        const auto skin = made->genes.find(ts2::sim::kSkinGene);
        if (skin == made->genes.end() || skin->second.dominant.empty()) {
            return false;
        }
        ts2::scene::Newborn baby;
        baby.gender = sim.personData[ts2::sim::kGenderPersonData] == 1 ? 1 : 2;
        baby.skinTone = skin->second.dominant;
        const auto gene = [&](std::uint32_t number) {
            const auto it = made->genes.find(number);
            return it != made->genes.end() ? it->second.dominant : std::string();
        };
        baby.hairTone = gene(ts2::sim::kHairGene);
        baby.eyeColour = gene(ts2::sim::kEyeGene);
        baby.browDraw = made->browDraw;
        const auto shape = [&](std::int16_t parent) -> std::optional<ts2::faceshape::FaceShape> {
            if (const auto parentFile = parent != 0 ? sim_->characterFile(parent) : std::nullopt) {
                return ts2::scene::faceShapeOf(*parentFile);
            }
            // A Sim with no character file (an NPC such as the alien) keeps its face shape with its object.
            const auto* saved = sim_->world().neighbor(parent);
            const auto* type = saved != nullptr ? sim_->code().objectType(saved->guid) : nullptr;
            const auto bytes = type != nullptr ? sim_->code().characterResource(ts2::faceshape::kType, type->group) : std::nullopt;
            try {
                return bytes ? std::optional(ts2::faceshape::parse(*bytes)) : std::nullopt;
            } catch (const std::exception& ex) {
                ts2::diag::warnOnce(std::string("newborn face: a parent's face shape can't be read: ") + ex.what());
                return std::nullopt;
            }
        };
        // The blend depends only on the parents and the picks, so babies alike share one face mesh
        // and a neighbor id given again later can't bring back an earlier baby's face.
        std::string key = std::format("newborn face {} {}", made->faceParentA, made->faceParentB);
        for (const auto& [region, fromA] : made->facePicks) {
            key += std::format(" {}={}", region, fromA ? 'a' : 'b');
        }
        baby.face = std::make_shared<const ts2::scene::MeshEdit>(ts2::scene::newbornFaceEdit(
            assets_, newbornFaces_, std::move(key), shape(made->faceParentA), shape(made->faceParentB), made->facePicks));
        look = ts2::scene::newbornLook(assets_, babyCatalog_.get(), baby);
    } else if (const auto* fromTemplate = neighbor != 0 ? sim_->world().neighbor(neighbor) : nullptr;
               fromTemplate != nullptr && fromTemplate->madeAs && fromTemplate->personData.size() > kSpeciesPersonData &&
               fromTemplate->personData[kSpeciesPersonData] == 0) {
        // A human made from a template during play: its look drawn as Make New Character draws it.
        constexpr std::uint32_t kEveryday = 1;
        look = ts2::scene::madeSimLook(assets_, outfitCatalog_.get(), *madeLook(neighbor, *fromTemplate->madeAs), kEveryday);
    }
    for (const auto& problem : look.problems) {
        ts2::diag::warnOnce(std::format("Sim {}: {}", sim.id, problem));
    }
    if (look.parts.empty()) {
        return false;
    }
    ts2::scene::Actor actor = objectMaterials_.sim(look, std::to_string(sim.id), lotFrame_, scene.groundLevel + sim.level - 1,
                                                   ts2::scene::standingIdle(assets_, look.age), scene, freeSimInstances_);
    actor.object = static_cast<std::uint32_t>(sim.id);
    actor.basePose = ts2::scene::standingPose(assets_, look.age);
    actor.lotFrame = lotFrame_;
    actor.floorHeight = scene.floorHeight;
    if (file) {
        actor.slotNodes = boneNodes(actor.rig.get(), ts2::scene::simSlotBones(assets_, *file));
        actor.targetNodes = boneNodes(actor.rig.get(), ts2::scene::simSlotBones(assets_, *file, true));
    }
    keepSimParts(sim.id, actor, true);
    scene.actors.push_back(std::move(actor));
    return true;
}

// Builds the age data of `sim` for a new age as Age runs, and keeps it for the new body and the
// Sim's later outfit changes. Gives the keys of its part lists that hold a part.
std::optional<std::set<std::uint32_t>> LotLife::buildAgeData(const ts2::sim::SimObject& sim, std::uint32_t age) {
    const std::int16_t neighbor = sim_->world().neighborIdOf(sim);
    const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
    if (!file) {
        ts2::diag::warnOnce("Age: a Sim without a character file keeps its body");
        return std::nullopt;
    }
    try {
        const ts2::scene::Assets::CharacterInUse inUse(assets_, *file);
        const auto aged = agedData_.find(neighbor);
        const auto old = aged != agedData_.end() ? std::optional(aged->second) : ts2::scene::characterAgeData(assets_, *file);
        if (!old) {
            return std::nullopt;
        }
        auto made = ts2::scene::ageUp(*old, age, ts2::scene::characterParts(*file), outfitCatalog_.get(), lookRandom());
        if (!made) {
            ts2::diag::warnOnce("Age: a baby's, a pet's or a new young adult's body isn't built yet (PR-62)");
            return std::nullopt;
        }
        std::set<std::uint32_t> filled;
        for (const auto& [key, parts] : made->lists) {
            if (!parts.empty()) {
                filled.insert(key);
            }
        }
        agedData_[neighbor] = std::move(*made);
        // The working copy is dropped, as Age drops it, and made again from the new age data.
        outfitData_.erase(neighbor);
        return filled;
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::format("Sim {}: its new age can't be built: {}", sim.id, ex.what()));
        return std::nullopt;
    }
}

std::vector<ts2::scene::AgeData> LotLife::outfitAgeData(std::uint32_t guid) {
    constexpr std::uint32_t kAgeData = 0xAC598EAC;
    std::vector<ts2::scene::AgeData> outfits;
    const auto* type = sim_->code().objectType(guid);
    if (type == nullptr) {
        return outfits;
    }
    // Each age data's parts are named by the reference list of the same instance.
    std::map<std::uint32_t, std::vector<std::uint8_t>> references;
    for (auto& [instance, bytes] : sim_->code().groupResources(ts2::reflist::kType, type->group)) {
        references[instance] = std::move(bytes);
    }
    for (const auto& [instance, bytes] : sim_->code().groupResources(kAgeData, type->group)) {
        try {
            const auto refs = references.find(instance);
            outfits.push_back(ts2::scene::readAgeData(ts2::propset::parse(bytes), refs != references.end()
                                                                                     ? ts2::reflist::parse(refs->second)
                                                                                     : std::vector<ts2::scenegraph::Link>{}));
        } catch (const std::exception& ex) {
            ts2::diag::warnOnce(std::format("an outfit of 0x{:08X} can't be read: {}", guid, ex.what()));
        }
    }
    return outfits;
}

// Change Outfit's work on a Sim's working copy of its age data. The copy is made from the Sim's own
// age data at the Sim's first outfit change (the game makes it as the Sim loads) or when asked anew.
// A source's age data is listed in instance order, though the game lists it package by package in
// each package's own order (PR-63).
std::optional<ts2::sim::World::BuiltOutfit> LotLife::buildOutfit(const ts2::sim::SimObject& sim,
                                                                 const ts2::sim::World::OutfitBuildRequest& request) {
    const std::int16_t neighbor = sim_->world().neighborIdOf(sim);
    const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
    if (!file) {
        return std::nullopt;
    }
    try {
        auto working = outfitData_.find(neighbor);
        if (working == outfitData_.end() || request.renew) {
            const ts2::scene::Assets::CharacterInUse inUse(assets_, *file);
            const auto aged = agedData_.find(neighbor);
            auto own = aged != agedData_.end() ? std::optional(aged->second) : ts2::scene::characterAgeData(assets_, *file);
            if (!own) {
                return std::nullopt;
            }
            working = outfitData_.insert_or_assign(neighbor, std::move(*own)).first;
        }
        ts2::scene::AgeData& data = working->second;
        ts2::sim::World::BuiltOutfit built;
        if (request.sourceGuid != 0 && (request.sourceGuid & 0x7FFF) == request.sourceGuid) {
            ts2::diag::warnOnce("Change Outfit: an outfit numbered in the neighborhood's age data isn't worn yet (PR-63)");
        } else if (request.sourceGuid != 0) {
            const auto outfits = outfitAgeData(request.sourceGuid);
            const std::uint32_t age = ts2::propset::number(data.properties, "age");
            const std::uint32_t gender = ts2::propset::number(data.properties, "gender");
            if (const auto* outfit = ts2::scene::pickOutfit(outfits, age, gender, sim.definition.guid())) {
                for (const auto& problem : ts2::scene::putOn(data, *outfit)) {
                    ts2::diag::warnOnce(std::format("Change Outfit: {}", problem));
                }
                built.sourceFound = true;
            }
        }
        if (request.commit) {
            agedData_[neighbor] = data;
        }
        for (const auto& [key, parts] : data.lists) {
            if (!parts.empty()) {
                built.lists.insert(key);
            }
        }
        return built;
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::format("Sim {}: its outfit can't be built: {}", sim.id, ex.what()));
        return std::nullopt;
    }
}

void LotLife::dressFromSource(std::int16_t id) {
    constexpr std::size_t kCurrentOutfit = 0x08, kDefaultSource = 0x69, kSourceGuidLow = 0x77, kSourceGuidHigh = 0x78;
    auto* sim = sim_->world().find(id);
    if (sim == nullptr || !sim->isPerson() || sim->personData.size() <= kSourceGuidHigh) {
        return;
    }
    const std::uint32_t guid = static_cast<std::uint16_t>(sim->personData[kSourceGuidLow]) |
                               static_cast<std::uint32_t>(static_cast<std::uint16_t>(sim->personData[kSourceGuidHigh])) << 16;
    if (guid == 0) {
        if (sim->personData[kDefaultSource] != 0) {
            ts2::diag::warnOnce("a Sim's default outfit source (person data 0x69) isn't worn yet (PR-63)");
        }
        return;
    }
    if (const auto built = buildOutfit(*sim, {guid, false, false}); built && built->sourceFound) {
        sim->outfitChange = ts2::sim::SimObject::OutfitChange{sim->personData[kCurrentOutfit], guid};
    }
}

// A Sim that Age made older gets the new age's body at once, built from the age data Age made, on
// the new age's skeleton, wearing its current outfit. Its animations stop, as the game stops them.
void LotLife::buildNewAges(ts2::scene::Scene& scene) {
    constexpr std::size_t kCurrentOutfit = 0x08;
    auto& world = sim_->world();
    for (auto& actor : scene.actors) {
        auto* sim = actor.object != 0 && actor.object <= static_cast<std::uint32_t>(INT16_MAX)
                        ? world.find(static_cast<std::int16_t>(actor.object))
                        : nullptr;
        if (sim == nullptr || !sim->newAge) {
            continue;
        }
        sim->newAge.reset();
        const std::int16_t neighbor = world.neighborIdOf(*sim);
        const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
        const auto made = agedData_.find(neighbor);
        if (!file || !actor.rig || made == agedData_.end()) {
            continue;
        }
        try {
            const ts2::scene::Assets::CharacterInUse inUse(assets_, *file);
            const auto category = ts2::sim::SimObject::OutfitChange{sim->personData[kCurrentOutfit], std::nullopt}.categoryFlag();
            const ts2::scene::SimLook look = ts2::scene::simLook(assets_, made->second, category);
            for (const auto& problem : look.problems) {
                ts2::diag::warnOnce(std::format("Sim {}: {}", sim->id, problem));
            }
            if (look.parts.empty()) {
                continue;
            }
            ts2::scene::Actor built = objectMaterials_.sim(look, std::to_string(sim->id), actor.place, scene.groundLevel + sim->level - 1,
                                                           ts2::scene::standingIdle(assets_, look.age), scene, freeSimInstances_);
            if (!built.rig) {
                for (const auto& s : built.skinned) {
                    freeSimInstance(scene, s.instance);
                }
                continue;
            }
            for (const auto& s : actor.skinned) {
                freeSimInstance(scene, s.instance);
            }
            // What the Sim holds stays at the same bone of the new skeleton.
            for (auto& h : actor.held) {
                const auto& nodes = actor.rig->nodes;
                h.node = h.node >= 0 && static_cast<std::size_t>(h.node) < nodes.size() ? built.rig->find(nodes[static_cast<std::size_t>(h.node)].name)
                                                                                         : -1;
            }
            actor.age = built.age;
            actor.gender = built.gender;
            actor.rig = built.rig;
            actor.animation = built.animation;
            actor.clips.clear();
            actor.skinned = std::move(built.skinned);
            actor.basePose = ts2::scene::standingPose(assets_, look.age);
            actor.slotNodes = boneNodes(actor.rig.get(), ts2::scene::simSlotBones(assets_, *file));
            actor.targetNodes = boneNodes(actor.rig.get(), ts2::scene::simSlotBones(assets_, *file, true));
            keepSimParts(sim->id, actor, false);
            ownAnimations_.erase(actor.object);
        } catch (const std::exception& ex) {
            ts2::diag::warning(std::format("Sim {} keeps its body: its new age can't be built: {}", sim->id, ex.what()));
        }
    }
}

// A Sim that Change Outfit dressed wears the new outfit at once. Only its parts change: its rig,
// clips and place stay, as the game keeps the skeleton and the animations playing. The outfit comes
// from the Sim's working copy of its age data, else its character file, so a Sim without one keeps
// what it wears.
void LotLife::wearChangedOutfits(ts2::scene::Scene& scene) {
    auto& world = sim_->world();
    const auto sameRig = [](const ts2::scene::Rig& a, const ts2::scene::Rig& b) {
        return a.stretch == b.stretch && std::ranges::equal(a.nodes, b.nodes, [](const auto& x, const auto& y) {
                   return x.name == y.name && x.parent == y.parent;
               });
    };
    for (auto& actor : scene.actors) {
        auto* sim = actor.object != 0 && actor.object <= static_cast<std::uint32_t>(INT16_MAX)
                        ? world.find(static_cast<std::int16_t>(actor.object))
                        : nullptr;
        if (sim == nullptr || !sim->outfitChange) {
            continue;
        }
        const std::uint32_t category = sim->outfitChange->categoryFlag();
        sim->outfitChange.reset();
        const std::int16_t neighbor = world.neighborIdOf(*sim);
        const auto file = neighbor != 0 ? sim_->characterFile(neighbor) : std::nullopt;
        if (!file || !actor.rig) {
            ts2::diag::warnOnce("Change Outfit: a Sim without a character file keeps what it wears");
            continue;
        }
        try {
            const ts2::scene::Assets::CharacterInUse inUse(assets_, *file);
            const auto working = outfitData_.find(neighbor);
            const auto aged = agedData_.find(neighbor);
            const ts2::scene::SimLook look = working != outfitData_.end() ? ts2::scene::simLook(assets_, working->second, category)
                                             : aged != agedData_.end()    ? ts2::scene::simLook(assets_, aged->second, category)
                                                                          : ts2::scene::simLook(assets_, *file, category);
            for (const auto& problem : look.problems) {
                ts2::diag::warnOnce(std::format("Sim {}: {}", sim->id, problem));
            }
            if (look.parts.empty()) {
                continue;
            }
            ts2::scene::Actor dressed = objectMaterials_.sim(look, std::to_string(sim->id), actor.place, scene.groundLevel + sim->level - 1,
                                                             std::string(), scene, freeSimInstances_);
            // The parts' skin follows the new rig's nodes, so they fit only a rig made the same way.
            const bool fits = dressed.rig && sameRig(*dressed.rig, *actor.rig);
            auto& gone = fits ? actor.skinned : dressed.skinned;
            for (const auto& s : gone) {
                freeSimInstance(scene, s.instance);
            }
            if (!fits) {
                ts2::diag::warnOnce("Change Outfit: an outfit on another skeleton isn't worn yet (PR-62)");
                continue;
            }
            actor.skinned = std::move(dressed.skinned);
            // The new parts set their joints on the skeleton as it is. Joints they don't name keep
            // what the outfit before gave them, as the game keeps its skeleton through a change.
            auto rig = std::make_shared<ts2::scene::Rig>(*actor.rig);
            ts2::scene::wearPartJoints(*rig, look);
            actor.rig = std::move(rig);
            keepSimParts(sim->id, actor, false);
            // The new parts are lit where the Sim stands as it is posed next.
            ownAnimations_[actor.object].litLevel = -1;
        } catch (const std::exception& ex) {
            ts2::diag::warning(std::format("Sim {} keeps what it wears: the outfit can't be read: {}", sim->id, ex.what()));
        }
    }
}

} // namespace app
