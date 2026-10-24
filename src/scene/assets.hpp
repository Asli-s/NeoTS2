#pragma once

#include "content/build_scripts.hpp"
#include "content/lighting.hpp"
#include "content/resource_index.hpp"
#include "formats/objd.hpp"
#include "formats/scenegraph.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ts2::scene {

// Which mip level (0 = the largest, `width` x `height`, of `count`) has exactly the size `wanted`:
// the largest halved, rounding up, until it fits, as the game picks a layer for a composite. Only
// sizes in powers of two are halved. Empty when no level has that size.
[[nodiscard]] std::optional<std::size_t> mipLevelOfSize(std::uint32_t width, std::uint32_t height, std::size_t count,
                                          std::uint32_t wantedWidth, std::uint32_t wantedHeight);

// The game's 3D and build-catalog assets of an installation, all products loaded in order:
// models, materials and textures (Sims3D), object definitions (for slots), the build catalog
// (floor and wall patterns, fences, roof patterns) and its scripts.
class Assets {
public:
    explicit Assets(const std::filesystem::path& install);

    // A scenegraph resource by name (e.g. "fridgeCheap_cres") or by a link from another resource.
    // A name may name its group ("#0x7f93b1db!age3_0_gmdc"). While a character file is in use, its
    // own resources are found too: by name, and by links into the file (group 0xFFFFFFFF).
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> byName(std::uint32_t type, const std::string& name);
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> byKey(const scenegraph::Link& link);

    // Makes a Sim's character file (Neighborhoods/<hood>/Characters/*.package) the one in use. Its
    // resources are indexed under the file's own group on first use. An empty path stops using one.
    void useCharacterFile(const std::filesystem::path& file);
    // Makes a group of the object packages the character in use, as a Sim without a character file
    // (an NPC made from its object) keeps its looks there. Links into group 0xFFFFFFFF go to it.
    void useCharacterGroup(std::uint32_t group);

    // A character file or group in use until it goes out of scope, even when what uses it throws.
    // The one in use before comes back then, so holders can nest.
    class CharacterInUse {
    public:
        CharacterInUse(Assets& assets, const std::filesystem::path& file) : CharacterInUse(assets) { assets.useCharacterFile(file); }
        CharacterInUse(Assets& assets, std::uint32_t group) : CharacterInUse(assets) { assets.useCharacterGroup(group); }
        CharacterInUse(const CharacterInUse&) = delete;
        CharacterInUse& operator=(const CharacterInUse&) = delete;
        ~CharacterInUse() {
            assets_.localGroup_ = localGroup_;
            assets_.characterGroup_ = characterGroup_;
        }

    private:
        explicit CharacterInUse(Assets& assets)
            : assets_(assets), localGroup_(assets.localGroup_), characterGroup_(assets.characterGroup_) {}

        Assets& assets_;
        std::optional<std::uint32_t> localGroup_, characterGroup_;
    };

    // A resource of the Sims' outfit catalog (Catalog/Skins: property sets and their reference
    // lists), or of the character file in use.
    [[nodiscard]] std::optional<std::vector<std::uint8_t>> skinResource(const scenegraph::Link& link);
    // Every resource of `type` in the outfit catalog, in load order (unreadable ones are skipped).
    void forEachSkinResource(std::uint32_t type, const std::function<void(const std::vector<std::uint8_t>&)>& visit);

    // The largest mip level of a texture, decoded. Empty when it is missing or unreadable.
    [[nodiscard]] std::optional<Image> texture(const std::string& name);
    // The size of a texture's largest mip level that is there. Empty when it is missing or unreadable.
    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> textureSize(const std::string& name);
    // A texture's own mip level of exactly this size, decoded: the largest level halved (rounding
    // up) until it fits, as the game picks it. Empty when no level has that size.
    [[nodiscard]] std::optional<Image> texture(const std::string& name, std::uint32_t width, std::uint32_t height);
    // The normal map the game makes of a bump map (see bumpNormals), with each mip level made from
    // the file's level of that size. A texture of another format is decoded as it is. Empty when it
    // is missing or unreadable.
    [[nodiscard]] std::optional<Image> normalMap(const std::string& name);
    // Every face of a texture (six for a cube map, in the file's order), the largest mip level each.
    std::vector<Image> textureFaces(const std::string& name);

    // The game's lighting settings (Lights/Lighting.txt of every product).
    const content::Lighting& lighting() const { return lighting_; }

    // Names of every material in the Sims3D packages (for finding doors' and windows' wall masks).
    const std::vector<std::string>& materialNames() const { return materialNames_; }

    // The object definition (OBJD) of the object type `guid`. Null when no objects package has it.
    const objd::ObjectDefinition* objectDefinition(std::uint32_t guid);

    // The model bone container slot `slot` of the object type `guid` sits at. Empty for none.
    std::string slotBone(std::uint32_t guid, std::size_t slot);

    // Entry `entry` (US English) of string table `table` in the objects' code group `group`. Nullopt
    // when there is none.
    [[nodiscard]] std::optional<std::string> objectString(std::uint32_t group, std::uint32_t table, std::size_t entry);

    // The material a lot's floor or wall pattern name shows: a built-in pattern from the catalog
    // scripts, or a catalog pattern by its GUID in hex.
    std::string patternMaterial(const std::string& name, bool wall) const;

    // A fence's models: one tile of rail along +x, the same diagonally (1.41 long), and a post, and
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

    content::ResourceIndex models_;    // Res/Sims3D and Res/3D packages
    content::ResourceIndex objects_;   // objects packages
    content::ResourceIndex catalog_;   // Catalog/Patterns packages
    content::ResourceIndex skins_;     // Catalog/Skins packages, read on first use
    std::vector<std::filesystem::path> skinFolders_;
    bool skinsRead_ = false;
    void readSkins();
    std::optional<std::uint32_t> localGroup_;  // the character file in use
    std::optional<std::uint32_t> characterGroup_;  // the object group in use as a character
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
    // Each object type's container slot bones in slot order (empty names for none), read once.
    std::map<std::uint32_t, std::vector<std::string>> slotBones_;
    const std::vector<std::string>& slotBones(std::uint32_t guid);
    void readObjectTypes();
    bool objectTypesRead_ = false;
};

std::string lower(std::string s);

} // namespace ts2::scene
