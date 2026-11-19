#pragma once

#include "formats/scenegraph.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ts2::scene {

class Assets;

// The most vertices a mesh overlay, its mask or a face may have for fitting. The game's have under a
// thousand. The fit compares every overlay vertex with every face vertex, so a broken file with
// more is refused instead of holding the save up.
constexpr std::size_t kMaxFitVertices = 16384;

// A mesh overlay (glasses) fitted to a Sim's face, as the game fits one when it writes the Sim's
// look. `rest` holds the overlay's own positions (three floats per vertex, all its vertex lists in
// order), `maskPositions` and `maskColours` its mask mesh's positions and colours (four bytes per
// vertex, alpha last), `baseFace` the catalog base face's positions and `face` the Sim's own face's,
// vertex for vertex. Gives the fitted positions.
//
// An overlay vertex within 1e-4 on every axis of a mask vertex whose alpha is over half is tied to
// the face: it takes the base face vertex at its place (squared distance under 1e-6) to that vertex's
// place on the Sim's face, or else moves by the average move of its 3 nearest base face vertices.
// Every other overlay vertex moves by the average move of its 5 nearest tied vertices. The nearest
// lists keep the closest first and drop the farthest when full. When the mask gives no vertex over
// half (no mask, or a mask without colours) every vertex is tied. Empty when the sizes don't fit
// together or pass kMaxFitVertices.
std::vector<float> fitMeshOverlay(std::span<const float> rest, std::span<const float> maskPositions, std::span<const std::uint8_t> maskColours,
                                  std::span<const float> baseFace, std::span<const float> face);

// What a catalog mesh overlay part is drawn from, by its part's fields and reference list: its shape
// ("shapekeyidx") with the shape's geometry node and mesh, its mask mesh ("maskshapekeyidx", empty
// when it has none) and each mesh group (subset) with its material: the overrides in the part's
// order, then the shape's other groups with the shape's own material.
struct MeshOverlayCatalog {
    std::vector<std::uint8_t> shape, node, mesh, maskMesh;
    struct Subset {
        std::string subset;
        std::vector<std::uint8_t> material;
    };
    std::vector<Subset> subsets;
};

// None when the part or a piece it names isn't in the catalog. `problem` then says which.
std::optional<MeshOverlayCatalog> meshOverlayCatalog(Assets& assets, const scenegraph::Link& part, std::string& problem);

// A mesh's positions (three floats per vertex) and colours (four bytes per vertex) of every vertex
// list it draws from, the lists in the mesh's order.
std::vector<float> meshPositions(std::span<const std::uint8_t> mesh);
std::vector<std::uint8_t> meshColours(std::span<const std::uint8_t> mesh);

} // namespace ts2::scene
