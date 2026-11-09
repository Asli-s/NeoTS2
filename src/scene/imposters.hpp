#pragma once

#include "formats/lot_description.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::dbpf {
class Package;
}

namespace ts2::scene {

// A lot's imposter as its lot package holds it, in the lot's own tiles (x and y on the ground, z up).
struct LotImposter {
    struct Part {
        PaintedTerrain::ImposterPart::Kind kind = PaintedTerrain::ImposterPart::Kind::Wall;
        std::string texture;   // its texture's name in the package, without the lot's prefix ("walls_3")
        std::string lightMap;  // the terrain's light map's name the same way, empty for none
        std::array<float, 3> lightMapTint{1, 1, 1};
        // x, y, z, u, v, then u, v of the second set. Roofs have no texture coordinates of their own
        // and get the roof shader's (see readImposter).
        std::vector<float> vertices;
        std::vector<std::uint32_t> indices;
    };
    std::string prefix;  // what its texture names start with in the package ("N002_Lot10!")
    std::vector<Part> parts;
};

// Reads a lot package's imposter (its "imposter" model and materials). Nullopt when it has none.
// Problems go to `warnings`.
std::optional<LotImposter> readImposter(dbpf::Package& package, std::vector<std::string>& warnings);

// A lot object that the imposter carries as a model of its own: trees, shrubs, stairs,
// chimneys and the like. The game saves them in the lot's "imposter_cres" when it draws the
// imposter, each as the model it shows far away (the object's "LOD90" model when it has one).
struct ImposterObject {
    std::string model;  // the model's name without "_cres"
    std::vector<scenegraph::MaterialOverride> overrides;  // its colour choices ("DesignModes")
    Transform place;    // in the lot's own tiles, as the imposter's vertices are
};

// Reads the objects of a lot package's imposter. Empty when it has none. Problems go to `warnings`.
std::vector<ImposterObject> readImposterObjects(dbpf::Package& package, std::vector<std::string>& warnings);

// Adds a lot's imposter (the "imposter" model and textures in its lot package) to the terrain's
// imposters, placed where the neighborhood has the lot and lifted by `zOffset` (the game's
// ImposterZOffset). Returns false when the lot has no imposter. Problems go to `warnings`. `top` gets
// the height of the imposter's highest point. With `pictures` (a bake, named as in Part::texture) the
// package's own textures are not used at all. `objects` gets the imposter's objects, placed with it.
bool addImposter(PaintedTerrain& terrain, const std::filesystem::path& lotPackage, const nhood::LotDescription& lot,
                 float zOffset, std::vector<std::string>& warnings, float* top = nullptr,
                 const std::map<std::string, Image>* pictures = nullptr, std::vector<ImposterObject>* objects = nullptr);

// Shows a lot's imposter as `with` holds it (made by addImposter into a terrain of its own) in place
// of its parts in `terrain`. Textures no part uses any more are kept, so that those uploaded stay
// where they are, until they come to more than kMostUnusedImposterBytes: then they are let go and
// the rest numbered anew, and it returns true (the imposters must be uploaded again whole).
constexpr std::size_t kMostUnusedImposterBytes = std::size_t{64} << 20;
bool replaceImposter(PaintedTerrain& terrain, std::uint32_t lot, PaintedTerrain&& with);

} // namespace ts2::scene
