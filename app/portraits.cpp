#include "portraits.hpp"

#include "content/ui_meters.hpp"
#include "scene/family_picture_shadow.hpp"
#include "scene/portrait.hpp"
#include "scene/rig.hpp"

namespace app {

std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, std::uint32_t sim, std::mt19937& random) {
    using namespace ts2::scene;
    const Actor* actor = nullptr;
    for (const auto& a : lot.actors) {
        if (sim != 0 && a.object == sim) {
            actor = &a;
        }
    }
    if (actor == nullptr) {
        return std::nullopt;
    }
    return drawPortrait(lot, renderer, animations, *actor, random);
}

std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor, std::mt19937& random) {
    using namespace ts2::scene;
    if (!actor.rig) {
        return std::nullopt;
    }
    std::uniform_int_distribution<int> pick(1, portraitPoseCount(actor.age));
    const auto pose = animations.bound(actor.rig, portraitPose(actor.age, actor.gender, pick(random)));
    PortraitShot shot;
    if (!portraitShot(lot, actor, pose.get(), shot)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> visible(lot.instances.size(), 0);
    for (const auto& [instance, joints] : shot.joints) {
        visible[instance] = 1;
        renderer.setJoints(instance, joints);
    }
    for (const auto& [instance, weights] : shot.morphs) {
        renderer.setMorphWeights(instance, weights);
    }
    for (const auto& [instance, eyes] : shot.eyes) {
        renderer.setEyes(instance, eyes);
    }
    renderer.show(std::move(visible));
    ts2::render::Camera camera;
    camera.eye = shot.camera.eye;
    camera.target = shot.camera.target;
    camera.up = shot.camera.up;
    camera.fovY = shot.camera.fovY;
    camera.nearPlane = shot.camera.nearPlane;
    camera.farPlane = shot.camera.farPlane;
    auto image = renderer.drawInstancesToImage(camera, PortraitCamera::kSize, PortraitCamera::kSize, shot.lighting);
    if (!image) {
        return std::nullopt;
    }
    // Soft edges (blended hair) come out premultiplied, but the UI draws straight alpha.
    return ts2::content::unpremultiplied(ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)});
}

std::optional<ts2::tga::Pixels> drawPartPicture(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                                ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor,
                                                const std::string& pose, const ts2::scene::PictureView& view) {
    using namespace ts2::scene;
    if (!actor.rig) {
        return std::nullopt;
    }
    const auto clip = animations.bound(actor.rig, pose);
    PortraitShot shot;
    if (!posedForPicture(lot, actor, clip.get(), shot)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> visible(lot.instances.size(), 0);
    for (const auto& [instance, joints] : shot.joints) {
        visible[instance] = 1;
        renderer.setJoints(instance, joints);
    }
    for (const auto& [instance, weights] : shot.morphs) {
        renderer.setMorphWeights(instance, weights);
    }
    for (const auto& [instance, eyes] : shot.eyes) {
        renderer.setEyes(instance, eyes);
    }
    renderer.show(std::move(visible));
    ts2::render::Camera camera;
    camera.eye = view.eye;
    camera.target = view.target;
    camera.up = view.up;
    camera.fovY = view.fovY;
    camera.nearPlane = view.nearPlane;
    camera.farPlane = view.farPlane;
    auto image = renderer.drawInstancesToImage(camera, PictureView::kSize, PictureView::kSize, view.lighting);
    if (!image) {
        return std::nullopt;
    }
    return ts2::content::unpremultiplied(ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)});
}

std::optional<ts2::tga::Pixels> drawSimView(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                            ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor,
                                            const std::string& pose, unsigned width, unsigned height, float turn) {
    using namespace ts2::scene;
    if (!actor.rig || width == 0 || height == 0) {
        return std::nullopt;
    }
    const auto clip = animations.bound(actor.rig, pose);
    PortraitShot shot;
    if (!simViewShot(lot, actor, clip.get(), width, height, turn, shot)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> visible(lot.instances.size(), 0);
    for (const auto& [instance, joints] : shot.joints) {
        visible[instance] = 1;
        renderer.setJoints(instance, joints);
    }
    for (const auto& [instance, weights] : shot.morphs) {
        renderer.setMorphWeights(instance, weights);
    }
    for (const auto& [instance, eyes] : shot.eyes) {
        renderer.setEyes(instance, eyes);
    }
    renderer.show(std::move(visible));
    ts2::render::Camera camera;
    camera.eye = shot.camera.eye;
    camera.target = shot.camera.target;
    camera.up = shot.camera.up;
    camera.fovY = shot.camera.fovY;
    camera.nearPlane = shot.camera.nearPlane;
    camera.farPlane = shot.camera.farPlane;
    auto image = renderer.drawInstancesToImage(camera, width, height, shot.lighting);
    if (!image) {
        return std::nullopt;
    }
    return ts2::content::unpremultiplied(ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)});
}

std::optional<ts2::tga::Pixels> drawFamilyPicture(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                                  ts2::scene::AnimationLibrary& animations, const std::vector<FamilyPictureActor>& actors,
                                                  const ts2::scene::FamilyPictureStage& stage) {
    using namespace ts2::scene;
    std::vector<std::uint8_t> visible(lot.instances.size(), 0);
    bool any = false;
    familyshadow::Box box;
    std::vector<float> triangles, posed;
    for (const auto& a : actors) {
        if (!a.actor.rig) {
            continue;
        }
        const auto clip = animations.bound(a.actor.rig, a.pose);
        PortraitShot shot;
        if (!posedForPicture(lot, a.actor, clip.get(), shot)) {
            continue;
        }
        for (const auto& [instance, joints] : shot.joints) {
            visible[instance] = 1;
            renderer.setJoints(instance, joints);
            const Instance& placed = lot.instances[instance];
            if (placed.mesh < 0 || static_cast<std::size_t>(placed.mesh) >= lot.meshes.size()) {
                continue;
            }
            const Mesh& mesh = lot.meshes[static_cast<std::size_t>(placed.mesh)];
            familyshadow::growByJoints(box, mesh, joints, placed.transform);
            for (const auto& p : mesh.primitives) {
                if (!isSkinned(p) || p.material == kHiddenMaterial) {
                    continue;
                }
                posedPositions(p, joints, posed);
                const std::size_t vertices = posed.size() / 3;
                for (const std::uint32_t i : p.indices) {
                    if (i >= vertices) {
                        triangles.resize(triangles.size() - triangles.size() % 9);
                        break;
                    }
                    float v[3] = {posed[3 * i], posed[3 * i + 1], posed[3 * i + 2]};
                    placed.transform.apply(v, true);
                    triangles.insert(triangles.end(), v, v + 3);
                }
                triangles.resize(triangles.size() - triangles.size() % 9);
            }
        }
        for (const auto& [instance, weights] : shot.morphs) {
            renderer.setMorphWeights(instance, weights);
        }
        for (const auto& [instance, eyes] : shot.eyes) {
            renderer.setEyes(instance, eyes);
        }
        any = true;
    }
    if (!any) {
        return std::nullopt;
    }
    renderer.show(std::move(visible));
    const PictureView& view = stage.view;
    ts2::render::Camera camera;
    camera.eye = view.eye;
    camera.target = view.target;
    camera.up = view.up;
    camera.fovY = view.fovY;
    camera.nearPlane = view.nearPlane;
    camera.farPlane = view.farPlane;
    auto image = renderer.drawInstancesToImage(camera, FamilyPictureStage::kSize, FamilyPictureStage::kSize, view.lighting);
    if (!image) {
        return std::nullopt;
    }
    if (const auto light = familyshadow::camera(box)) {
        familyshadow::lay(*image, view.eye, view.target, view.up, view.fovY, *light,
                          familyshadow::blurred(familyshadow::coverage(triangles, *light)));
    }
    // The game keeps the picture's colour as drawn over black, its soft edges darkened, with the
    // alpha beside it.
    return ts2::tga::Pixels{image->width, image->height, std::move(image->rgba)};
}

} // namespace app
