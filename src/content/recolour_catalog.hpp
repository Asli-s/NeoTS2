#pragma once

#include "content/install.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts2::content {

// One material of a recolour (an MMAT, "material override"): the model and subset it colours, the
// material it gives them, and its family, the GUID every material of one colour choice shares.
// Names are kept as written, the family lowercase.
struct RecolourMaterial {
    std::string model;   // modelName, e.g. "lightingceilingcentralasian_cres"
    std::string subset;  // subsetName
    std::string name;    // the material
    std::string family;
    std::uint32_t instance = 0;    // the resource's instance, which orders a strip of colour choices
    std::uint32_t stateFlags = 0;  // materialStateFlags: 0 for the object's usual state, else lit or dirty ones
    bool isDefault = false;        // defaultMaterial: the colour a new object has
    std::uint32_t objectGuid = 0;  // objectGUID: the catalog item it was made for
};

// One colour choice of a catalog item's strip: the recolour family and the material that stands
// for it (the one of the usual state).
struct RecolourChoice {
    std::string model, subset, material, family;
};
// A strip of colour choices for one subset (a recolourable part of the model), in resource order,
// and the choice a new object starts with.
struct RecolourStrip {
    std::string subset;
    std::vector<RecolourChoice> choices;
    std::size_t chosen = 0;
};

// The recolours of every product's material catalogs (Res/Catalog/Materials), each listed once for
// each catalog item it was made for, the first product read first.
class RecolourCatalog {
public:
    void add(RecolourMaterial material);
    // The recolour material for this model, subset and material (names in any case), or null.
    [[nodiscard]] const RecolourMaterial* find(const std::string& model, const std::string& subset, const std::string& name) const;
    // Every material of `family` (any case) for `model` (any case).
    [[nodiscard]] std::vector<const RecolourMaterial*> family(const std::string& model, const std::string& family) const;
    [[nodiscard]] std::size_t size() const { return materials_.size(); }
    // Every material for `model` (any case, with or without "_cres").
    [[nodiscard]] std::vector<const RecolourMaterial*> materials(const std::string& model) const;
    // Every material made for the catalog item `guid`, whatever its model. GUID 0 gives every
    // material, as in the game.
    [[nodiscard]] std::vector<const RecolourMaterial*> materialsOf(std::uint32_t guid) const;

private:
    std::vector<RecolourMaterial> materials_;
    std::unordered_map<std::string, std::vector<std::size_t>> byModel_;  // lowercase model
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> byGuid_;
};

RecolourCatalog loadRecolourCatalog(const std::vector<Product>& products);

// A subset the Design tool recolours: its name, the model its recolours are filed under (the
// geometry node's tsMaterialsMeshName), the object's own model, which its swatches show, and the
// catalog item's GUID. A node that names no model takes the materials made for that GUID.
struct DesignSubset {
    std::string subset, mesh, model;
    std::uint32_t guid = 0;
};
// The colour strips of an object's design subsets, one each in their order when it has recolours of
// that subset. A strip lists each family once, in the order of its first material's instance, and
// starts at the default family.
std::vector<RecolourStrip> recolourStrips(const RecolourCatalog& catalog, std::span<const DesignSubset> subsets);

} // namespace ts2::content
