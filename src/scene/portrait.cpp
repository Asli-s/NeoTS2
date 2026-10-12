#include "scene/portrait.hpp"

#include <cmath>

namespace ts2::scene {

namespace {

constexpr std::uint32_t kToddler = 1, kChild = 2, kBaby = 0x20;

// Where the camera sits from the head joint, in the Sim's model (it faces +y).
constexpr float kAhead = 1.4506f, kAbove = 0.05f;
// 20 degrees from top to bottom.
constexpr float kFovY = 0.34906585f, kNear = 0.6805574f, kFar = 1.7408469f;
// Half the Sim material lighting scale (0.8): the thumbnail material lights at half and doubles.
constexpr float kThumbnailScale = 0.4f;

// The portrait lights in the camera's space (x right, y into the picture, z up): toward each
// light, then its colour.
constexpr float kLights[5][6] = {
    {-0.43493f, -0.79454f, 0.42373f, 1.6452f, 1.6452f, 1.4076f},
    {0.00001f, 0.0f, -1.0f, 0.864f, 0.864f, 0.96f},
    {0.98162f, 0.0f, 0.19084f, 0.84f, 0.798f, 0.714f},
    {-0.29339f, 0.82903f, 0.47606f, 0.6912f, 0.6668f, 0.56333f},
    {0.38028f, 0.91677f, -0.12216f, 0.2976f, 0.39633f, 0.6f},
};

std::array<float, 3> normalized(std::array<float, 3> v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return l > 0 ? std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l} : std::array<float, 3>{0, 0, 1};
}

} // namespace

ObjectLighting portraitLighting(const PortraitCamera& camera) {
    const auto forward = normalized({camera.target[0] - camera.eye[0], camera.target[1] - camera.eye[1], camera.target[2] - camera.eye[2]});
    // The camera never tilts, so its right is level and its up is the world's.
    const auto right = normalized({forward[1], -forward[0], 0});
    const std::array<float, 3> up{0, 0, 1};
    ObjectLighting l;
    l.thumbnailScale = kThumbnailScale;
    for (const auto& k : kLights) {
        ObjectLighting::Light light;
        for (int i = 0; i < 3; ++i) {
            light.direction[i] = k[0] * right[i] + k[1] * forward[i] + k[2] * up[i];
        }
        light.direction = normalized(light.direction);
        light.color = {k[3], k[4], k[5]};
        l.lights.push_back(light);
    }
    return l;
}

int portraitPoseCount(std::uint32_t age) { return age == kBaby ? 4 : 8; }

std::string portraitPose(std::uint32_t age, std::uint32_t gender, int pick) {
    const int count = portraitPoseCount(age);
    pick = pick < 1 ? 1 : pick > count ? count : pick;
    const std::string number = "0" + std::to_string(pick);
    if (age == kBaby) {
        return "b-UIpose-" + number;
    }
    const bool female = gender == 1;
    if (age == kToddler) {
        return std::string("p-UIpose-") + (female ? "F" : "m") + number;
    }
    return std::string(age == kChild ? "c" : "a") + "-UIpose-" + (female ? "F" : "M") + number;
}

PortraitCamera portraitCamera(const Transform& place, const std::array<float, 3>& head) {
    PortraitCamera c;
    float eye[3] = {head[0], head[1] + kAhead, head[2] + kAbove};
    float back[3] = {0, -1, 0};
    place.apply(eye, true);
    place.apply(back, false);
    c.eye = {eye[0], eye[1], eye[2]};
    c.target = {eye[0] + back[0], eye[1] + back[1], eye[2] + back[2]};
    c.fovY = kFovY;
    c.nearPlane = kNear;
    c.farPlane = kFar;
    return c;
}

bool portraitShot(const Scene& scene, const Actor& actor, const RigAnimation* pose, PortraitShot& out) {
    out.joints.clear();
    if (!actor.rig) {
        return false;
    }
    const int head = actor.rig->find("head");
    std::vector<Transform> world;
    poseRig(*actor.rig, pose, 0, world);
    if (head < 0 || static_cast<std::size_t>(head) >= world.size()) {
        return false;
    }
    const auto& m = world[static_cast<std::size_t>(head)].m;
    out.camera = portraitCamera(actor.place, {m[3], m[7], m[11]});
    out.lighting = portraitLighting(out.camera);
    for (const auto& s : actor.skinned) {
        if (s.instance >= scene.instances.size()) {
            continue;
        }
        const int mesh = scene.instances[s.instance].mesh;
        if (mesh < 0 || static_cast<std::size_t>(mesh) >= scene.meshes.size()) {
            continue;
        }
        auto& joints = out.joints.emplace_back(s.instance, std::vector<Transform>{}).second;
        skinMatrices(scene.meshes[static_cast<std::size_t>(mesh)], s.nodes, *actor.rig, pose, world, joints);
    }
    return !out.joints.empty();
}

} // namespace ts2::scene
