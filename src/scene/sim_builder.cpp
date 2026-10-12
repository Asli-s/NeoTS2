#include "scene/sim_builder.hpp"

namespace ts2::scene {

std::shared_ptr<const Rig> RigLibrary::rig(const std::string& skeleton) {
    if (skeleton.empty()) {
        return nullptr;
    }
    auto& r = rigs_[skeleton];
    if (!r) {
        auto loaded = std::make_shared<Rig>(loadRig(assets_, skeleton));
        if (!loaded->nodes.empty()) {
            r = std::move(loaded);
        }
    }
    return r;
}

std::shared_ptr<const RigAnimation> RigLibrary::animation(const std::string& skeleton, const std::string& name) {
    const auto r = rig(skeleton);
    if (!r || name.empty()) {
        return nullptr;
    }
    auto& a = animations_[skeleton + "|" + name];
    if (!a) {
        if (auto loaded = loadAnimation(assets_, name)) {
            a = std::make_shared<RigAnimation>(bindAnimation(*r, std::make_shared<anim::Animation>(std::move(*loaded))));
        }
    }
    return a;
}

Actor buildSim(SceneBuilder& builder, RigLibrary& rigs, const SimLook& look, const Transform& at, int level,
               const std::string& name, const std::string& animation, std::vector<Transform>& world) {
    Actor actor;
    actor.place = at;
    actor.age = look.age;
    actor.gender = look.gender;
    actor.rig = rigs.rig(look.skeleton);
    actor.animation = rigs.animation(look.skeleton, animation);
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
        if (part.part == kBodyPart) {
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
        const int mesh = actor.rig ? builder.skinnedModel(part.model, overrides) : builder.model(part.model, overrides);
        const std::size_t instance = builder.scene().instances.size();
        if (builder.place(mesh, name + " " + part.model, at, Part::Object, level) && actor.rig &&
            !builder.scene().meshes[static_cast<std::size_t>(mesh)].skin.empty()) {
            actor.skinned.push_back({instance, skinNodes(builder.scene().meshes[static_cast<std::size_t>(mesh)], *actor.rig)});
        }
    }
    return actor;
}

} // namespace ts2::scene
