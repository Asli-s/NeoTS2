#pragma once

#include "content/build_scripts.hpp"
#include "content/resource_index.hpp"
#include "formats/scenegraph.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ts2::scene {

// The game's 3D and build-catalog assets of an installation, all products loaded in order:
// models, materials and textures (Sims3D), object definitions (for slots), the build catalog
// (floor and wall patterns, fences, roof patterns) and its scripts.
class Assets {
public:
    explicit Assets(const std::filesystem::path& install);

    // A scenegraph resource by name (e.g. "fridgeCheap_cres") or by a link from another resource.
    std::optional<std::vector<std::uint8_t>> byName(std::uint32_t type, const std::string& name);
    std::optional<std::vector<std::uint8_t>> byKey(const scenegraph::Link& link);

    // The largest mip level of a texture, decoded; empty when it is missing or unreadable.
    std::optional<Image> texture(const std::string& name);
    // Every face of a texture (six for a cube map, in the file's order), the largest mip level each.
    std::vector<Image> textureFaces(const std::string& name);

    // Names of every material in the Sims3D packages (for finding doors' and windows' wall masks).
    const std::vector<std::string>& materialNames() const { return materialNames_; }

    // The model bone container slot `slot` of the object type `guid` sits at; empty for none.
    std::string slotBone(std::uint32_t guid, std::size_t slot);

    // The material a lot's floor or wall pattern name shows: a built-in pattern from the catalog
    // scripts, or a catalog pattern by its GUID in hex.
    std::string patternMaterial(const std::string& name, bool wall) const;

    // A fence's models: one tile of rail along +x, the same diagonally (1.41 long), and a post.
    struct Fence {
        std::string rail, diagonalRail, post;
    };
    const Fence* fence(std::uint32_t guid) const;

    // A roof pattern's textures.
    struct RoofPattern {
        std::string top, under, edges, trim;
    };
    const RoofPattern* roofPattern(std::uint32_t guid) const;

    const content::BuildScripts& buildScripts() const { return scripts_; }

    // Problems met while loading (missing textures, unreadable files), for tools to report.
    std::vector<std::string> warnings;

private:
    void readMaterialNames(const std::filesystem::path& path);
    void readCatalog();

    content::ResourceIndex models_;    // Sims3D packages
    content::ResourceIndex objects_;   // objects packages
    content::ResourceIndex catalog_;   // Catalog/Patterns packages
    std::set<std::filesystem::path> added_;
    std::vector<std::string> materialNames_;
    content::BuildScripts scripts_;
    std::map<std::uint32_t, std::string> patternsByGuid_;
    std::map<std::uint32_t, Fence> fences_;
    std::map<std::uint32_t, RoofPattern> roofPatterns_;
    // Object types by GUID: their group and SLOT instance, read on first use.
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> objectTypes_;
    bool objectTypesRead_ = false;
};

std::string lower(std::string s);

} // namespace ts2::scene
