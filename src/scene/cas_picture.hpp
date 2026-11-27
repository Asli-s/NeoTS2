#pragma once

#include "content/lighting.hpp"
#include "formats/scenegraph.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::scene {

class Assets;

// The age a Create-A-Sim part picture is made for, which picks its camera and pose. Young adults
// and elders take the adults'.
enum class PictureAge { Toddler, Child, Teen, Adult };
PictureAge pictureAge(std::uint32_t age);  // from an age flag (1 toddler, 2 child, 4 teen...)

// The single-frame pose (an animation name without "_anim") the Sim of a part picture stands in,
// except for bottoms pictures.
std::string picturePose(PictureAge age);

// The camera and lights of a part picture, in the space of the picture's Sim standing at the origin.
struct PictureView {
    std::array<float, 3> eye{}, target{}, up{0, 0, 1};
    float fovY = 0, nearPlane = 0, farPlane = 0;  // radians from top to bottom, then world units
    ObjectLighting lighting;
    static constexpr unsigned kSize = 256;  // the picture is this many pixels square
};

// The view from picture camera `cameraNode` (a model whose camera node holds a lens), lit by the
// lights of the picture light rig `rig` (a model of light nodes) with the light files' settings
// over each light's own. Nullopt when the camera can't be read.
std::optional<PictureView> pictureView(const scenegraph::ResourceNode& cameraNode, const scenegraph::ResourceNode& rig,
                                       const std::vector<scenegraph::LightResource>& rigLights, const content::Lighting& lighting);

// The view a hair's picture is drawn with, from the game's files. Nullopt when they can't be read.
std::optional<PictureView> hairPictureView(Assets& assets, PictureAge age);

// The view for the catalog picture of an object with no picture of its own: lit by the "Thumbnails"
// light rig's four brightest lights, from a camera turned 160 degrees about z and tilted 30 degrees
// down that fits the model's box `low` to `high`. Nullopt when the rig can't be read or the box is empty.
std::optional<PictureView> objectPictureView(Assets& assets, std::array<float, 3> low, std::array<float, 3> high);
// The same for a model that names its own picture camera (its "thumbnailExtension" data list): the
// camera model `camera` (a name ending "_cres"), which looks at the model as it stands at the origin.
std::optional<PictureView> objectPictureView(Assets& assets, const std::string& camera);
// The object picture is drawn this many pixels square.
inline constexpr unsigned kObjectPictureSize = 128;

// Where the household's family picture is taken (casNeighborhoodPose_cres): each slot's place by
// its number (the simMark_<n> nodes), and the view from camera `camera` (a letter, see
// sim::FamilyPicture) lit by the stage's lights. Nullopt when the game's files can't give them.
struct FamilyPictureStage {
    std::map<int, Transform> slots;
    PictureView view;
    static constexpr unsigned kSize = 512;  // the picture is this many pixels square
};
std::optional<FamilyPictureStage> familyPictureStage(Assets& assets, const std::string& camera);

} // namespace ts2::scene
