#include "scene/cas_picture.hpp"

#include "scene/assets.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace ts2::scene {

namespace {

namespace sg = scenegraph;

constexpr std::uint32_t kCres = 0xE519C933;
constexpr std::uint32_t kToddler = 1, kChild = 2, kTeen = 4;
// Lit as Sim portraits are: half the Sim material's 0.8, then doubled.
constexpr float kThumbnailScale = 0.4f;

using Quat = std::array<float, 4>;  // x, y, z, w
using Vec = std::array<float, 3>;

Quat times(const Quat& a, const Quat& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Vec turned(const Quat& q, const Vec& v) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    return {(1 - 2 * (y * y + z * z)) * v[0] + 2 * (x * y - w * z) * v[1] + 2 * (x * z + w * y) * v[2],
            2 * (x * y + w * z) * v[0] + (1 - 2 * (x * x + z * z)) * v[1] + 2 * (y * z - w * x) * v[2],
            2 * (x * z - w * y) * v[0] + 2 * (y * z + w * x) * v[1] + (1 - 2 * (x * x + y * y)) * v[2]};
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Each node's turn in the model: its own after its parents'. Nodes no parent reaches keep their own.
std::vector<Quat> worldTurns(const sg::ResourceNode& tree) {
    std::vector<int> parent(tree.nodes.size(), -1);
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        for (const auto c : tree.nodes[i].children) {
            if (c < parent.size() && c != i && parent[c] < 0) {
                parent[c] = static_cast<int>(i);
            }
        }
    }
    std::vector<Quat> out(tree.nodes.size());
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        Quat q = tree.nodes[i].rotation;
        // A chain can't be longer than the tree, which also stops a loop.
        int p = parent[i];
        for (std::size_t steps = 0; p >= 0 && steps < tree.nodes.size(); ++steps) {
            q = times(tree.nodes[static_cast<std::size_t>(p)].rotation, q);
            p = parent[static_cast<std::size_t>(p)];
        }
        out[i] = q;
    }
    return out;
}

// Adds the lights of light rig `rig` (a model of light nodes) to `into`, with the light files'
// settings over each light's own.
void addRigLights(const sg::ResourceNode& rig, const std::vector<sg::LightResource>& rigLights, const content::Lighting& lighting,
                  ObjectLighting& into) {
    std::map<std::string, const sg::LightResource*> own;
    for (const auto& l : rigLights) {
        own.emplace(lowered(l.name), &l);
    }
    const auto turns = worldTurns(rig);
    for (std::size_t i = 0; i < rig.nodes.size(); ++i) {
        const auto& node = rig.nodes[i];
        const std::string name = lowered(node.name);
        const auto it = own.find(name);
        if (node.kind != sg::SceneNode::Kind::LightRef || it == own.end()) {
            continue;
        }
        // The game draws Create-A-Sim part pictures without their rig's light from below, for a reason not known yet.
        if (name == "casthumb_z") {
            continue;
        }
        const auto* attrs = lighting.light(node.name);
        const float intensity = attrs && attrs->intensity ? *attrs->intensity : it->second->intensity;
        const Vec color = attrs && attrs->color ? *attrs->color : it->second->color;
        if (intensity <= 0) {
            continue;
        }
        const Vec shine{color[0] * intensity, color[1] * intensity, color[2] * intensity};
        if (it->second->kind == "cAmbientLight") {
            for (int k = 0; k < 3; ++k) {
                into.ambient[k] += shine[k];
            }
        } else if (it->second->kind == "cDirectionalLight") {
            // A directional light shines along its node's -z, so toward it is +z.
            ObjectLighting::Light light;
            light.direction = turned(turns[i], {0, 0, 1});
            light.color = shine;
            into.lights.push_back(light);
        }
    }
}

} // namespace

PictureAge pictureAge(std::uint32_t age) {
    return age == kToddler ? PictureAge::Toddler : age == kChild ? PictureAge::Child : age == kTeen ? PictureAge::Teen : PictureAge::Adult;
}

std::string picturePose(PictureAge age) {
    return age == PictureAge::Toddler ? "p-CAS-clothingPose" : age == PictureAge::Child ? "c-pose-neutral-stand-CAS" : "a-pose-neutral-stand-CAS";
}

std::optional<PictureView> pictureView(const sg::ResourceNode& cameraNode, const sg::ResourceNode& rig,
                                       const std::vector<sg::LightResource>& rigLights, const content::Lighting& lighting) {
    const auto camera = std::find_if(cameraNode.nodes.begin(), cameraNode.nodes.end(), [](const sg::SceneNode& n) { return n.lens.has_value(); });
    if (camera == cameraNode.nodes.end()) {
        return std::nullopt;
    }
    // The camera looks along its own y, its z up.
    const Quat turn = worldTurns(cameraNode)[static_cast<std::size_t>(camera - cameraNode.nodes.begin())];
    const Vec ahead = turned(turn, {0, 1, 0});
    PictureView v;
    v.eye = camera->translation;
    v.target = {v.eye[0] + ahead[0], v.eye[1] + ahead[1], v.eye[2] + ahead[2]};
    v.up = turned(turn, {0, 0, 1});
    v.fovY = camera->lens->fovY;
    v.nearPlane = camera->lens->nearPlane;
    v.farPlane = camera->lens->farPlane;
    v.lighting.thumbnailScale = kThumbnailScale;
    addRigLights(rig, rigLights, lighting, v.lighting);
    return v;
}

std::optional<PictureView> hairPictureView(Assets& assets, PictureAge age) {
    static constexpr const char* kAgeWords[] = {"toddler", "child", "teen", "adult"};
    const std::string rigName = "casThumbnailCameras2";
    const std::string camera = rigName + "_camera_thumbnail_" + kAgeWords[static_cast<int>(age)] + "_face_hair_squareShape_cres";
    const auto cameraData = assets.byName(kCres, camera);
    const auto rigData = assets.byName(kCres, rigName + "_cres");
    if (!cameraData || !rigData) {
        return std::nullopt;
    }
    try {
        const auto cameraNode = sg::parseResourceNode(*cameraData);
        const auto rig = sg::parseResourceNode(*rigData);
        std::vector<sg::LightResource> lights;
        for (const auto& link : rig.container.links) {
            if ((link.type & 0xFFFFFF00u) != 0xC9C81B00u) {
                continue;  // not a light resource
            }
            if (const auto data = assets.byKey(link)) {
                try {
                    lights.push_back(sg::parseLight(*data));
                } catch (const std::exception&) {
                    // A light that can't be read is left out.
                }
            }
        }
        return pictureView(cameraNode, rig, lights, assets.lighting());
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

// The lights of a model's light nodes, read from the light resources it links.
std::vector<sg::LightResource> rigLights(Assets& assets, const sg::ResourceNode& rig) {
    std::vector<sg::LightResource> lights;
    for (const auto& link : rig.container.links) {
        if ((link.type & 0xFFFFFF00u) != 0xC9C81B00u) {
            continue;  // not a light resource
        }
        if (const auto data = assets.byKey(link)) {
            try {
                lights.push_back(sg::parseLight(*data));
            } catch (const std::exception&) {
                // A light that can't be read is left out.
            }
        }
    }
    return lights;
}

// A node's place in its model: its own after its parents'. A chain can't be longer than the tree.
Transform placeInModel(const sg::ResourceNode& tree, std::size_t node) {
    std::vector<int> parent(tree.nodes.size(), -1);
    for (std::size_t i = 0; i < tree.nodes.size(); ++i) {
        for (const auto c : tree.nodes[i].children) {
            if (c < parent.size() && c != i && parent[c] < 0) {
                parent[c] = static_cast<int>(i);
            }
        }
    }
    Transform place = Transform::of(tree.nodes[node].translation, tree.nodes[node].rotation);
    int p = parent[node];
    for (std::size_t steps = 0; p >= 0 && steps < tree.nodes.size(); ++steps) {
        const auto& n = tree.nodes[static_cast<std::size_t>(p)];
        place = Transform::of(n.translation, n.rotation) * place;
        p = parent[static_cast<std::size_t>(p)];
    }
    return place;
}

// Adds the lights an object picture gets to `into`: the "Thumbnails" light rig's four brightest, by the
// length of their colour, brightest first. Its mirror glass shows the blue the game's mirror material
// gives pictures. False when the rig can't be read.
bool addObjectPictureLights(Assets& assets, ObjectLighting& into) {
    constexpr std::size_t kPictureLights = 4;
    constexpr std::array<float, 3> kPictureGlass{0.5f, 0.6f, 0.75f};
    into.glassColor = kPictureGlass;
    const auto rigData = assets.byName(kCres, "Thumbnails_cres");
    if (!rigData) {
        return false;
    }
    try {
        const auto rig = sg::parseResourceNode(*rigData);
        addRigLights(rig, rigLights(assets, rig), assets.lighting(), into);
    } catch (const std::exception&) {
        return false;
    }
    const auto strength = [](const ObjectLighting::Light& l) { return l.color[0] * l.color[0] + l.color[1] * l.color[1] + l.color[2] * l.color[2]; };
    std::stable_sort(into.lights.begin(), into.lights.end(), [&](const auto& a, const auto& b) { return strength(a) > strength(b); });
    if (into.lights.size() > kPictureLights) {
        into.lights.resize(kPictureLights);
    }
    return true;
}

} // namespace

std::optional<FamilyPictureStage> familyPictureStage(Assets& assets, const std::string& camera) {
    const std::string stageName = "casNeighborhoodPose";
    const auto stageData = assets.byName(kCres, stageName + "_cres");
    const auto cameraData = assets.byName(kCres, stageName + "_camera_thumbnail_" + stageName + "_" + camera + "Shape_cres");
    if (!stageData || !cameraData) {
        return std::nullopt;
    }
    try {
        const auto stage = sg::parseResourceNode(*stageData);
        const auto cameraNode = sg::parseResourceNode(*cameraData);
        auto view = pictureView(cameraNode, stage, rigLights(assets, stage), assets.lighting());
        if (!view) {
            return std::nullopt;
        }
        FamilyPictureStage out;
        out.view = *view;
        const std::string mark = "simmark_";
        for (std::size_t i = 0; i < stage.nodes.size(); ++i) {
            const std::string name = lowered(stage.nodes[i].name);
            if (name.size() > mark.size() && name.compare(0, mark.size(), mark) == 0) {
                const std::string number = name.substr(mark.size());
                if (number.size() <= 2 && std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                    out.slots.emplace(std::stoi(number), placeInModel(stage, i));
                }
            }
        }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<PictureView> objectPictureView(Assets& assets, std::array<float, 3> low, std::array<float, 3> high) {
    // The game's thumbnail camera: 20 degrees from top to bottom.
    constexpr float kFovY = 0.34906585f;
    constexpr float kDegrees = 3.14159265f / 180;
    constexpr float kYaw = -160 * kDegrees, kTilt = -30 * kDegrees;
    // The camera never comes nearer than this, and its planes are this much in front of and beyond the box.
    constexpr float kNearest = 0.01f, kNearShare = 0.8f, kFarShare = 1.2f;
    if (!(low[0] <= high[0] && low[1] <= high[1] && low[2] <= high[2])) {
        return std::nullopt;
    }
    PictureView v;
    if (!addObjectPictureLights(assets, v.lighting)) {
        return std::nullopt;
    }
    low[2] = std::max(low[2], 0.0f);
    high[2] = std::max(high[2], low[2]);
    const Vec size{high[0] - low[0], high[1] - low[1], high[2] - low[2]};
    const Vec centre{low[0] + size[0] / 2, low[1] + size[1] / 2, low[2] + size[2] / 2};
    // The width fits the picture, or the height when the object is taller than wide. The camera
    // stands back that far plus half the box's depth.
    const float fitted = size[0] / size[2] < 1 ? size[2] : size[0];
    const float distance = std::max(fitted / (2 * std::tan(kFovY / 2)), kNearest);
    const float back = size[1] / 2 + distance;
    const Quat yaw{0, 0, std::sin(kYaw / 2), std::cos(kYaw / 2)};
    const Quat tilt{std::sin(kTilt / 2), 0, 0, std::cos(kTilt / 2)};
    // The camera looks along its own y, its z up.
    const Quat turn = times(yaw, tilt);
    const Vec ahead = turned(turn, {0, 1, 0});
    v.target = centre;
    v.eye = {centre[0] - ahead[0] * back, centre[1] - ahead[1] * back, centre[2] - ahead[2] * back};
    v.up = turned(turn, {0, 0, 1});
    v.fovY = kFovY;
    v.nearPlane = distance * kNearShare;
    v.farPlane = back * kFarShare;
    return v;
}

std::optional<PictureView> objectPictureView(Assets& assets, const std::string& camera) {
    const auto cameraData = assets.byName(kCres, camera);
    if (!cameraData) {
        return std::nullopt;
    }
    try {
        const auto cameraNode = sg::parseResourceNode(*cameraData);
        const auto lens = std::find_if(cameraNode.nodes.begin(), cameraNode.nodes.end(), [](const sg::SceneNode& n) { return n.lens.has_value(); });
        if (lens == cameraNode.nodes.end()) {
            return std::nullopt;
        }
        // The camera looks along its own y, its z up.
        const Quat turn = worldTurns(cameraNode)[static_cast<std::size_t>(lens - cameraNode.nodes.begin())];
        const Vec ahead = turned(turn, {0, 1, 0});
        PictureView v;
        v.eye = lens->translation;
        v.target = {v.eye[0] + ahead[0], v.eye[1] + ahead[1], v.eye[2] + ahead[2]};
        v.up = turned(turn, {0, 0, 1});
        v.fovY = lens->lens->fovY;
        v.nearPlane = lens->lens->nearPlane;
        v.farPlane = lens->lens->farPlane;
        if (!addObjectPictureLights(assets, v.lighting)) {
            return std::nullopt;
        }
        return v;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace ts2::scene
