#include "scene/sim_builder.hpp"

#include <algorithm>

namespace ts2::scene {

std::shared_ptr<const Rig> RigLibrary::rig(const std::string& skeleton, float stretch, bool teenArms) {
    if (skeleton.empty()) {
        return nullptr;
    }
    auto& r = rigs_[{skeleton, stretch, teenArms}];
    if (!r) {
        Rig base = loadRig(assets_, skeleton);
        if (teenArms) {
            stretchTeenArms(base);
        }
        auto loaded = std::make_shared<Rig>(stretched(std::move(base), stretch));
        if (!loaded->nodes.empty()) {
            r = std::move(loaded);
        }
    }
    return r;
}

std::shared_ptr<const RigAnimation> RigLibrary::animation(const std::string& skeleton, float stretch, const std::string& name,
                                                          bool teenArms) {
    const auto r = rig(skeleton, stretch, teenArms);
    if (!r || name.empty()) {
        return nullptr;
    }
    auto& a = animations_[{skeleton, stretch, teenArms, name}];
    if (!a) {
        if (auto loaded = loadAnimation(assets_, name)) {
            a = std::make_shared<RigAnimation>(bindAnimation(*r, std::make_shared<anim::Animation>(std::move(*loaded))));
        }
    }
    return a;
}

void wearPartJoints(Rig& rig, const SimLook& look) {
    std::vector<const SimPart*> order;
    for (const auto& part : look.parts) {
        order.push_back(&part);
    }
    std::stable_sort(order.begin(), order.end(), [](const SimPart* a, const SimPart* b) { return a->part < b->part; });
    for (const auto* part : order) {
        adjustJoints(rig, part->jointAdjusts);
    }
}

Actor buildSim(SceneBuilder& builder, RigLibrary& rigs, const SimLook& look, const Transform& at, int level,
               const std::string& name, const std::string& animation, std::vector<Transform>& world) {
    Actor actor;
    actor.place = at;
    actor.age = look.age;
    actor.gender = look.gender;
    // The game gives teen humans' upper arms the body stretch (age 4, species 1).
    const bool teenArms = look.age == kTeenAge && look.species == 1;
    actor.rig = rigs.rig(look.skeleton, look.stretch, teenArms);
    actor.animation = rigs.animation(look.skeleton, look.stretch, animation, teenArms);
    if (actor.rig && std::any_of(look.parts.begin(), look.parts.end(), [](const SimPart& p) { return !p.jointAdjusts.empty(); })) {
        auto own = std::make_shared<Rig>(*actor.rig);
        wearPartJoints(*own, look);
        actor.rig = std::move(own);
    }
    if (actor.rig) {
        poseRig(*actor.rig, actor.animation.get(), 0, world);
    }
    // A top and a bottom worn together share one texture: the skin, the top, then the bottom.
    auto firstMaterial = [&](std::uint32_t kind) {
        for (const auto& part : look.parts) {
            if (part.part == kind && !part.overrides.empty()) {
                return part.overrides.front().material;
            }
        }
        return std::string();
    };
    Assets& assets = builder.assets();
    const std::string top = firstMaterial(kTopPart), bottom = firstMaterial(kBottomPart);
    const std::string separates =
        !top.empty() && !bottom.empty() ? separatesOverSkin(builder, assets, look, top, bottom) : std::string();
    for (const auto& part : look.parts) {
        // Clothes are drawn over the Sim's skin (its uncovered arms, hands, neck).
        auto overrides = part.overrides;
        if (part.part == kHairPart) {
            // A hair's scalp is drawn over the Sim's bald head (its strands' alpha layers are not).
            for (auto& o : overrides) {
                if (lower(o.group) == "hair" && o.material != look.skinHair) {
                    o.material = hairOverScalp(builder, assets, look, o.material);
                }
            }
        } else if (part.part == kFacePart) {
            for (auto& o : overrides) {
                o.material = faceWithOverlays(builder, look, o.material);
            }
        } else if (part.part == kBodyPart) {
            for (auto& o : overrides) {
                o.material = clothesOverSkin(builder, assets, look, o.material);
            }
        } else if (!separates.empty() && (part.part == kTopPart || part.part == kBottomPart)) {
            for (auto& o : overrides) {
                if (o.material == top || o.material == bottom) {
                    o.material = separates;
                }
            }
        }
        const scenegraph::Link* modelKey = part.modelKey ? &*part.modelKey : nullptr;
        const bool hair = part.part == kHairPart;
        const int mesh = actor.rig ? builder.skinnedModel(part.model, overrides, part.meshEdit.get(), modelKey, hair)
                                   : builder.model(part.model, overrides, part.meshEdit.get(), modelKey, hair);
        const std::size_t instance = builder.scene().instances.size();
        auto* placed = builder.place(mesh, name + " " + part.model, at, Part::Object, level);
        if (placed) {
            // A Sim moves, and where it stands isn't tested against the buildings' shadow as it does yet.
            placed->sunTest = SunTest::Never;
            placed->lightGroup = LightGroup::Sims;
        }
        if (placed && actor.rig && !builder.scene().meshes[static_cast<std::size_t>(mesh)].skin.empty()) {
            actor.skinned.push_back({instance, skinNodes(builder.scene().meshes[static_cast<std::size_t>(mesh)], *actor.rig)});
        }
    }
    return actor;
}

} // namespace ts2::scene
