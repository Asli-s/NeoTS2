// Drawing a lot's imposter pictures again, sharper, on the layout the game gave them (the Lot
// Imposters option's High, not in the original game).
#pragma once

#include "scene/imposters.hpp"

#include <array>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// One picture drawn of a lot for its imposter: which of the lot's parts it shows, from where, and
// which rectangle of which texture it fills. In the lot's own tiles, z up.
struct ImposterShot {
    // The game's scene layers the bake draws: "GroundNoCanvas", "Roof", "Floors", and "WallImposter"
    // with "Portals" and "Walls".
    enum class Layer : std::uint8_t { Ground, Roof, Floors, Walls };
    Layer layer = Layer::Ground;
    std::string texture;          // as in LotImposter::Part::texture
    std::array<float, 4> rect{};  // u0, v0, u1, v1 of the texture it fills, v down the picture
    std::array<float, 3> eye{}, target{}, up{};
    std::array<float, 4> orthographic{};  // left, right, bottom, top around the line from eye to target
    float nearPlane = 0, farPlane = 0;
};

// The shots that draw every texture of an imposter but the terrain's light map, which the game copies
// from the lot instead. `sliceHeights` are the heights the lot's floors were cut at, lowest first.
// Each slice is a slab reaching halfway to the next height below and above, at most half a tile, as
// the game draws it.
std::vector<ImposterShot> imposterShots(const LotImposter& imposter, const std::vector<float>& sliceHeights);

// The heights an imposter's floor slices lie at, lowest first. The game also cuts at heights that
// hold no slice (each wall's bottom and top, and every tile up from the ground), which changes a
// slab only where such a height lies within a tile of a slice.
std::vector<float> imposterSliceHeights(const LotImposter& imposter);

// Whether the game's bake draws an instance of a lot's scene into a layer, as the game tags them:
// the terrain, the ground storey's floors (but those on a stage) and the pool water are the ground,
// every other floor is the floors, and walls with every object but stairs (object types 10 to 12) and
// Sims are the walls. Ceilings and floor edges are in none.
bool inImposterLayer(const Instance& instance, ImposterShot::Layer layer, int groundLevel);

// Makes a lot's scene ready for drawing its imposter: the terrain's canvas and lot texture paints
// are left out, as the game's terrain picture takes only the other paints (the neighborhood's ground
// shows through the rest).
void prepareImposterScene(Scene& scene);

// The side of every imposter texture the game makes, and of High's, in texels.
constexpr unsigned kImposterSide = 128;
constexpr unsigned kHighImposterSide = 512;
// Shots are drawn at this many times their texels each way and averaged down, as the game draws 512
// for its 128.
constexpr unsigned kImposterSupersample = 4;

// A shot's rectangle in its texture, in texels from the top left, x1 and y1 past its end.
struct TexelRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
};
TexelRect texelRect(const ImposterShot& shot, unsigned side);

// A lot's imposter textures being drawn again: the shots' pictures are put into them, then they are
// finished as the game finishes its own.
class ImposterPages {
public:
    explicit ImposterPages(unsigned side) : side_(side) {}
    unsigned side() const { return side_; }
    // Puts a shot's picture into its texture, each texel the mean of the picture's pixels over it. The
    // picture is the shot's texel rectangle times `supersample` each way, rows from the top. Pictures
    // of another size are left out.
    void put(const ImposterShot& shot, const Image& picture, unsigned supersample);
    // Finishes the textures and hands them over by name: wall and slice rectangles get their edges
    // repeated outward, the terrain light map is made from the lot's ground storey light map
    // (`groundLightMap`, 8 texels a tile, rows along +y, none for no light map), and the terrain and
    // its light map fade out at the edge of the lot, `lotWidth` by `lotDepth` tiles.
    std::map<std::string, Image> finish(const Image* groundLightMap, int lotWidth, int lotDepth);

private:
    unsigned side_;
    std::map<std::string, Image> pages_;
    std::vector<std::pair<std::string, TexelRect>> padded_;  // wall and slice rectangles, by texture
};

// Finished imposter textures compressed to DXT5, one level each (imposters are drawn without
// smaller levels), their pixels let go.
std::map<std::string, Image> compressedImposterPictures(std::map<std::string, Image> pages);

} // namespace ts2::scene
