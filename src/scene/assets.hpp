#pragma once

#include "content/build_scripts.hpp"
#include "content/lighting.hpp"
#include "content/resource_index.hpp"
#include "formats/objd.hpp"
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
    // A name may name its group ("#0x7f93b1db!age3_0_gmdc"). While a character file is in use, its
    // own resources are found too: by name, and by links into the file (group 0xFFFFFFFF).
    std::optional<std::vector<std::uint8_t>> byName(std::uint32_t type, const std::string& name);
    std::optional<std::vector<std::uint8_t>> byKey(const scenegraph::Link& link);

    // Makes a Sim's character file (Neighborhoods/<hood>/Characters/*.package) the one in use; its
    // resources are indexed under the file's own group on first use. An empty path stops using one.
    void useCharacterFile(const std::filesystem::path& file);

    // A resource of the Sims' outfit catalog (Catalog/Skins: property sets and their reference
    // lists), or of the character file in use.
    std::optional<std::vector<std::uint8_t>> skinResource(const scenegraph::Link& link);

    // The largest mip level of a texture, decoded; empty when it is missing or unreadable.
    std::optional<Image> texture(const std::string& name);
    // Every face of a texture (six for a cube map, in the file's order), the largest mip level each.
    std::vector<Image> textureFaces(const std::string& name);

    // The game's lighting settings (Lights/Lighting.txt of every product).
    const content::Lighting& lighting() const { return lighting_; }

    // Names of every material in the Sims3D packages (for finding doors' and windows' wall masks).
    const std::vector<std::string>& materialNames() const { return materialNames_; }

    // The object definition (OBJD) of the object type `guid`; null when no objects package has it.
    const objd::ObjectDefinition* objectDefinition(std::uint32_t guid);

    // The model bone container slot `slot` of the object type `guid` sits at; empty for none.
    std::string slotBone(std::uint32_t guid, std::size_t slot);

    // The material a lot's floor or wall pattern name shows: a built-in pattern from the catalog
    // scripts, or a catalog pattern by its GUID in hex.
    std::string patternMaterial(const std::string& name, bool wall) const;

    // A fence's models: one tile of rail along +x, the same diagonally (1.41 long), and a post; and
    // whether Sims may walk over it (flowerbed edging).
    struct Fence {
        std::string rail, diagonalRail, post;
        bool mayWalkOver = false;
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
    content::ResourceIndex skins_;     // Catalog/Skins packages, read on first use
    std::vector<std::filesystem::path> skinFolders_;
    bool skinsRead_ = false;
    std::optional<std::uint32_t> localGroup_;  // the character file in use
    std::set<std::filesystem::path> added_;
    std::vector<std::string> materialNames_;
    content::BuildScripts scripts_;
    content::Lighting lighting_;
    std::map<std::uint32_t, std::string> patternsByGuid_;
    std::map<std::uint32_t, Fence> fences_;
    std::map<std::uint32_t, RoofPattern> roofPatterns_;
    // Object types by GUID: their group and SLOT instance, read on first use.
    struct ObjectType {
        std::uint32_t group = 0;  // the objects package group it was found in
        objd::ObjectDefinition definition;
    };
    std::map<std::uint32_t, ObjectType> objectTypes_;  // by GUID
    void readObjectTypes();
    bool objectTypesRead_ = false;
};

std::string lower(std::string s);

} // namespace ts2::scene
