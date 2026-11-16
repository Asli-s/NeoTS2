#pragma once

#include "content/install.hpp"
#include "formats/scenegraph.hpp"
#include "scene/outfit_catalog.hpp"

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

namespace ts2::scene {

// The catalog bins (TSData/Res/Catalog/Bins): collections (0x6C4F359D) and binary indexes
// (0x0C560F39) that file each part under a collection. The game's Sim builder offers only the parts a
// binary index files under a collection of the kind it asks for, so a part property set no bin lists
// is never offered, in Create-A-Sim or at a mirror.

// A collection's kinds by its "type" text: clothing 0x80 and specialoutfit 0x20000 hold outfit parts.
inline constexpr std::uint32_t kClothingKind = 0x80, kSpecialOutfitKind = 0x20000;

struct CatalogCollection {
    scenegraph::Link key;
    std::uint32_t kind = 0;   // the bit for its "type" text, 0 for a type the game doesn't know
    std::uint32_t flags = 0;  // its "flags" field
    std::int32_t sortIndex = 0;
};

// A binary index: `object` filed under the collection `bin`, ordered in it by `sortIndex`.
struct CatalogBinEntry {
    scenegraph::Link object, bin;
    std::int32_t sortIndex = 0;
};

// The bins as read, each package's binary indexes in that package's own order.
struct CatalogBins {
    std::vector<CatalogCollection> collections;
    std::vector<std::vector<CatalogBinEntry>> packages;
};

std::uint32_t collectionKind(std::string_view type);

// Every product's Catalog/Bins packages, in the game's product order.
std::vector<std::filesystem::path> catalogBinPackages(const std::vector<content::Product>& products);

// Reads the bins from those packages. Entries whose reference list can't name their object or bin
// are left out. It can run on any thread.
CatalogBins readCatalogBins(const std::vector<std::filesystem::path>& packages);

// The collections in the game's order: its eight built-in ones (0x0FFEFEFE:0x0FFE0001..0x0FFE0080,
// one per kind from 1 to 0x80) with the bins' own, gathered by group then instance and sorted by sort
// index with the game's sort. Which copy the game keeps of a key in two packages is not known yet,
// and the first one read is kept.
std::vector<CatalogCollection> rankedCollections(const CatalogBins& bins);

// The catalog's parts that collections of `kind` list, as the game's Sim builder orders them: by
// product, newest first (0 first, 0xFFFF last), then by the place of their collection, then by sort
// index (largest first for product 0). A part listed twice keeps the first collection's entry. Parts
// that tie on all three keep the bins' order, as the game's order for them is not known yet.
std::vector<const CatalogPart*> binnedParts(const CatalogBins& bins, const OutfitCatalog& catalog, std::uint32_t kind);

// What the Sim builder's part search asks of a binned part. Age, gender, species and parts are
// flags and a part needs a bit in common with each (a part's species 0 is human). A part with no
// parts flags passes the parts test. A tone given asks
// for parts of that tone or of none. A part with no category fits any. A part with flag 0x1 is
// left out unless `withHidden`.
struct BuilderQuery {
    std::uint32_t age = 0, gender = 0, species = 1, parts = 0, category = 0;
    std::optional<std::string> hairTone;
    bool withHidden = false;
};

std::vector<const CatalogPart*> builderParts(const std::vector<const CatalogPart*>& binned, const BuilderQuery& query);

} // namespace ts2::scene
