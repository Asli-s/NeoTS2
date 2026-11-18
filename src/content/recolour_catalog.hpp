#pragma once

#include "content/install.hpp"

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
};

// The recolours of every product's material catalogs (Res/Catalog/Materials), each listed once,
// the first product read first.
class RecolourCatalog {
public:
    void add(RecolourMaterial material);
    // The recolour material for this model, subset and material (names in any case), or null.
    [[nodiscard]] const RecolourMaterial* find(const std::string& model, const std::string& subset, const std::string& name) const;
    // Every material of `family` (any case) for `model` (any case).
    [[nodiscard]] std::vector<const RecolourMaterial*> family(const std::string& model, const std::string& family) const;
    [[nodiscard]] std::size_t size() const { return materials_.size(); }

private:
    std::vector<RecolourMaterial> materials_;
    std::unordered_map<std::string, std::vector<std::size_t>> byModel_;  // lowercase model
};

RecolourCatalog loadRecolourCatalog(const std::vector<Product>& products);

} // namespace ts2::content
