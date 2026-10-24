#include "portraits.hpp"

#include "content/ui_meters.hpp"
#include "scene/portrait.hpp"

namespace app {

std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::Renderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, std::uint32_t sim, std::mt19937& random) {
    using namespace ts2::scene;
    const Actor* actor = nullptr;
    for (const auto& a : lot.actors) {
        if (sim != 0 && a.object == sim) {
            actor = &a;
        }
    }
    if (actor == nullptr || !actor->rig) {
        return std::nullopt;
    }
    std::uniform_int_distribution<int> pick(1, portraitPoseCount(actor->age));
    const auto pose = animations.bound(actor->rig, portraitPose(actor->age, actor->gender, pick(random)));
    PortraitShot shot;
    if (!portraitShot(lot, *actor, pose.get(), shot)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> visible(lot.instances.size(), 0);
    for (const auto& [instance, joints] : shot.joints) {
        visible[instance] = 1;
        renderer.setJoints(instance, joints);
    }
    renderer.show(std::move(visible));
    ts2::render::Camera camera;
    camera.eye = shot.camera.eye;
    camera.target = shot.camera.target;
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

} // namespace app
