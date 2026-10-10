#include "scene/assets.hpp"

#include "content/install.hpp"
#include "formats/dbpf.hpp"
#include "formats/hash.hpp"
#include "formats/namemap.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace fs = std::filesystem;
namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {
constexpr std::uint32_t kTxmt = 0x49596978;
constexpr std::uint32_t kTxtr = 0x1C4A276C;
constexpr std::uint32_t kLifo = 0xED534136;
constexpr std::uint32_t kPatternProperties = 0xCCA8E925;
constexpr std::uint32_t kFenceProperties = 0x2CB230B8;
constexpr std::uint32_t kRoofProperties = 0xACA8EA06;
constexpr std::uint32_t kBoneNames = 0x90;  // an object's "Model - Bones" string table
} // namespace

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Assets::Assets(const fs::path& install) : scripts_(content::loadBuildScripts(content::findProducts(install))) {
    const auto products = content::findProducts(install);
    std::set<fs::path> catalogFolders;
    for (const auto& objects : content::objectLoadOrder(products)) {
        objects_.addPackage(objects);
        const fs::path res = objects.parent_path().parent_path();
        catalogFolders.insert(res / "Catalog" / "Patterns");
        std::error_code ec;
        std::vector<fs::path> packages;
        for (fs::directory_iterator it(res / "Sims3D", ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() == ".package") {
                packages.push_back(it->path());
            }
        }
        std::sort(packages.begin(), packages.end());
        for (const auto& p : packages) {
            if (added_.insert(p).second) {
                models_.addPackage(p);
                readMaterialNames(p);
            }
        }
    }
    for (const auto& folder : catalogFolders) {
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() == ".package") {
                catalog_.addPackage(it->path());
            }
        }
    }
    readCatalog();
}

std::optional<std::vector<std::uint8_t>> Assets::byName(std::uint32_t type, const std::string& name) {
    const auto key = hash::scenegraphKey(name);
    return byKey({type, sg::kScenegraphGroup, key.instance, key.instanceHigh});
}

std::optional<std::vector<std::uint8_t>> Assets::byKey(const sg::Link& link) {
    dbpf::ResourceKey key;
    key.type = link.type;
    key.group = link.group;
    key.instance = link.instance;
    key.instanceHigh = link.instanceHigh;
    const auto* r = models_.find(key);
    if (r == nullptr) {
        return std::nullopt;
    }
    return models_.read(*r);
}

std::optional<Image> Assets::texture(const std::string& name) {
    try {
        const auto data = byName(kTxtr, name + "_txtr");
        if (!data) {
            warnings.push_back("texture " + name + " not found");
            return std::nullopt;
        }
        const sg::Image img = sg::parseImage(*data);
        if (img.mips.empty()) {
            return std::nullopt;
        }
        const sg::MipLevel& top = img.mips.back();
        std::vector<std::uint8_t> bytes = top.data;
        // The largest levels are kept in separate resources.
        if (!top.largeMipName.empty()) {
            const auto lifo = byName(kLifo, top.largeMipName);
            if (!lifo) {
                warnings.push_back("large mip " + top.largeMipName + " not found");
                return std::nullopt;
            }
            bytes = sg::parseLevelInfo(*lifo).data;
        }
        return Image{img.width, img.height, sg::decodeRgba(bytes, img.width, img.height, img.format)};
    } catch (const std::exception& ex) {
        warnings.push_back("texture " + name + ": " + ex.what());
        return std::nullopt;
    }
}

std::vector<Image> Assets::textureFaces(const std::string& name) {
    std::vector<Image> out;
    try {
        const auto data = byName(kTxtr, name + "_txtr");
        if (!data) {
            warnings.push_back("texture " + name + " not found");
            return out;
        }
        const sg::Image img = sg::parseImage(*data);
        for (const auto& face : img.faces) {
            if (face.empty() || !face.back().largeMipName.empty()) {
                warnings.push_back("texture " + name + ": a face without its largest level inline");
                return {};
            }
            out.push_back(Image{img.width, img.height, sg::decodeRgba(face.back().data, img.width, img.height, img.format)});
        }
    } catch (const std::exception& ex) {
        warnings.push_back("texture " + name + ": " + ex.what());
        out.clear();
    }
    return out;
}

std::string Assets::slotBone(std::uint32_t guid, std::size_t slot) {
    if (!objectTypesRead_) {
        objectTypesRead_ = true;
        for (const auto& r : objects_.resources()) {
            if (r.entry.key.type != types::kObjectDefinition) {
                continue;
            }
            try {
                const auto d = objd::parse(objects_.read(r));
                objectTypes_[d.guid()] = {r.entry.key.group, d.field(objd::field::kSlotsId)};
            } catch (const std::exception&) {
            }
        }
    }
    const auto type = objectTypes_.find(guid);
    if (type == objectTypes_.end() || type->second.second == 0) {
        return {};
    }
    // The type's SLOT entries name bones through its "Model - Bones" string table.
    const auto [group, slotsId] = type->second;
    const auto* slots = objects_.find({types::kSlots, group, slotsId, 0});
    const auto* names = objects_.find({types::kStringTable, group, kBoneNames, 0});
    if (slots == nullptr || names == nullptr) {
        return {};
    }
    try {
        const std::int32_t b = slot::parse(objects_.read(*slots)).containerBone(slot);
        const auto table = str::parse(objects_.read(*names));
        const auto entries = table.forLanguage(1);
        return b >= 0 && static_cast<std::size_t>(b) < entries.size() ? entries[static_cast<std::size_t>(b)]->value
                                                                        : std::string();
    } catch (const std::exception&) {
        return {};
    }
}

std::string Assets::patternMaterial(const std::string& name, bool wall) const {
    const auto& scripted = wall ? scripts_.wallPatterns : scripts_.floorPatterns;
    if (const auto it = scripted.find(lower(name)); it != scripted.end()) {
        return it->second;
    }
    if (!name.empty() && name.size() <= 8 && name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        if (const auto it = patternsByGuid_.find(static_cast<std::uint32_t>(std::stoul(name, nullptr, 16)));
            it != patternsByGuid_.end()) {
            return it->second;
        }
    }
    return (wall ? "wall_" : "floor_") + name;
}

const Assets::Fence* Assets::fence(std::uint32_t guid) const {
    const auto it = fences_.find(guid);
    return it != fences_.end() ? &it->second : nullptr;
}

const Assets::RoofPattern* Assets::roofPattern(std::uint32_t guid) const {
    const auto it = roofPatterns_.find(guid);
    return it != roofPatterns_.end() ? &it->second : nullptr;
}

void Assets::readMaterialNames(const fs::path& path) {
    try {
        dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == types::kNameMap && e.key.instance == kTxmt) {
                for (auto& n : namemap::parse(package.read(e))) {
                    materialNames_.push_back(std::move(n.name));
                }
            }
        }
    } catch (const std::exception& ex) {
        warnings.push_back("name map of " + path.string() + ": " + ex.what());
    }
}

void Assets::readCatalog() {
    for (const auto& r : catalog_.resources()) {
        const auto type = r.entry.key.type;
        if (type != kPatternProperties && type != kFenceProperties && type != kRoofProperties) {
            continue;
        }
        try {
            auto p = propset::parse(catalog_.read(r));
            const auto guid = p.find("guid");
            if (guid == p.end()) {
                continue;
            }
            const auto id = static_cast<std::uint32_t>(std::stoul(guid->second));
            if (type == kFenceProperties) {
                fences_[id] = {p["rail"], p["diagrail"], p["post"]};
            } else if (type == kRoofProperties) {
                roofPatterns_[id] = {p["texturetop"], p["textureunder"], p["textureedges"], p["texturetrim"]};
            } else if (const auto material = p.find("material"); material != p.end()) {
                patternsByGuid_[id] = material->second;
            }
        } catch (const std::exception&) {
        }
    }
}

} // namespace ts2::scene
