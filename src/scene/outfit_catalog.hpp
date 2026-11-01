#pragma once

#include "content/install.hpp"
#include "formats/scenegraph.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ts2::scene {

// An outfit part (a part property set) with the fields the skin catalog matches on. Tones are GUIDs
// as lowercase hex digits only, all zeros for none.
struct CatalogPart {
    scenegraph::Link key;
    std::uint32_t age = 0, gender = 0, species = 1, parts = 0, flags = 0, category = 0, fitness = 0;
    std::uint32_t subtype = 0, layer = 0;  // a face overlay's kind (1 eyebrows, 3 eyes...) and drawing order
    std::string skinTone, hairTone, family;
};

// The skin catalog's outfit parts, skin and hair tones, face overlays (mesh overlays, then texture
// overlays) and face templates (face archetypes), one per key, in the order the game lists them: by
// type, group, high instance, then instance. A key in several packages keeps the copy of the last
// one read. A tone's, overlay's or template's own GUID is its family.
struct OutfitCatalog {
    std::vector<CatalogPart> parts, skinTones, hairTones, faceOverlays, faceTemplates;
};

// A part property set's outfit categories as the game reads them. Outerwear (0x1000) came later, so
// everyday hairs of version 4 or older, and everyday faces for one gender of version 5 or older,
// are worn with outerwear too.
std::uint32_t partCategories(const std::map<std::string, std::string>& properties);

// Reads a part property set's fields as the catalog keeps them.
CatalogPart catalogPart(const scenegraph::Link& key, const std::map<std::string, std::string>& properties);

// The catalog's part with this key, or null.
const CatalogPart* findPart(const OutfitCatalog& catalog, const scenegraph::Link& key);

// The game's other catalog query, which tests only the fields it is given. Species, age and gender
// need a bit in common. A part or query with no category fits any category. A part needs every
// required flag and none of the excluded ones, and exactly the family given (all zeros for none).
// A tone fits when either side has none.
struct PartFilter {
    std::uint32_t parts = 0;
    std::optional<std::uint32_t> species, age, gender, category, requiredFlags, excludedFlags;
    std::optional<std::string> family, skinTone, hairTone;
};

bool fitsFilter(const CatalogPart& part, const PartFilter& filter);

// The first `most` parts the filter fits, for each bit of its parts lowest first, in key order.
std::vector<const CatalogPart*> filterParts(const OutfitCatalog& catalog, const PartFilter& filter, std::size_t most);

// Whether the catalog has the skin tone `tone` (its family) for a Sim of this species, age and gender.
bool hasSkinTone(const OutfitCatalog& catalog, const std::string& tone, std::uint32_t species, std::uint32_t age, std::uint32_t gender);

// The packages the game reads its skin catalog from: every product's Skins.package, then every
// product's Jewelry.package, each in the game's product order.
std::vector<std::filesystem::path> skinCatalogPackages(const std::vector<content::Product>& products);

// Reads the catalog from those packages. It takes a while, and can run on any thread.
OutfitCatalog readOutfitCatalog(const std::vector<std::filesystem::path>& packages);

// What the game asks the catalog for. Age, gender and species are flags, and a part needs a bit in
// common with each (species 0 asks for any). A part with no category fits every category, and so
// does a query with category 0. A tone given asks for parts of that tone or of none.
struct PartQuery {
    std::uint32_t age = 0, gender = 0, species = 0, part = 0, category = 0;
    std::optional<std::string> skinTone, hairTone;
};

// The parts a query matches, as the game lists them: for each bit of `part`, lowest first, every
// matching part with that bit in key order. A part with two of the bits is listed twice.
std::vector<const CatalogPart*> findParts(const OutfitCatalog& catalog, const PartQuery& query);

// A GUID as the catalog compares it: its hex digits only, lowercase.
std::string catalogGuid(const std::string& text);

} // namespace ts2::scene
