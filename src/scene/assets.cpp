#include "scene/assets.hpp"

#include "content/install.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/hash.hpp"
#include "formats/namemap.hpp"
#include "formats/objd.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/slot.hpp"
#include "formats/str.hpp"
#include "scene/normal_map.hpp"

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
constexpr std::uint32_t kLocalGroup = 0xFFFFFFFF;  // a package's own resources

// `text` read as std::stoul reads it, or nothing where std::stoul would throw: no digits, or
// more than 32 bits. Names and properties come from game files, so a broken one must not throw.
std::optional<std::uint32_t> number(const std::string& text, int base) {
    try {
        const unsigned long long v = std::stoull(text, nullptr, base);
        if (v > 0xFFFFFFFFull) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(v);
    } catch (const std::logic_error&) {
        return std::nullopt;
    }
}
} // namespace

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Assets::Assets(const fs::path& install) : scripts_(content::loadBuildScripts(content::findProducts(install))) {
    const auto products = content::findProducts(install);
    lighting_ = content::readLighting(products);
    std::set<fs::path> catalogFolders;
    for (const auto& objects : content::objectLoadOrder(products)) {
        objects_.addPackage(objects);
        const fs::path res = objects.parent_path().parent_path();
        catalogFolders.insert(res / "Catalog" / "Patterns");
        skinFolders_.push_back(res / "Catalog" / "Skins");
        // The base game keeps its models and animations in Res/Sims3D, the packs in Res/3D.
        std::vector<fs::path> packages;
        for (const char* folder : {"Sims3D", "3D"}) {
            std::error_code ec;
            for (fs::directory_iterator it(res / folder, ec), end; it != end; it.increment(ec)) {
                if (it->path().extension() == ".package") {
                    packages.push_back(it->path());
                }
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
    if (name.size() > 12 && name.starts_with("#0x") && name[11] == '!') {
        const auto group = number(name.substr(3, 8), 16);
        if (!group) {
            return std::nullopt;
        }
        const auto key = hash::scenegraphKey(name.substr(12));
        // A 0x6F group is the number the game gave a package in the session that saved the file,
        // so it means nothing later. Inside a character file it names the file's own resources.
        const bool sessionGroup = (*group >> 24) == 0x6F && localGroup_ && !characterGroup_;
        return byKey({type, sessionGroup ? *localGroup_ : *group, key.instance, key.instanceHigh});
    }
    const auto key = hash::scenegraphKey(name);
    if (auto found = byKey({type, sg::kScenegraphGroup, key.instance, key.instanceHigh}); found || !localGroup_) {
        return found;
    }
    return byKey({type, *localGroup_, key.instance, key.instanceHigh});
}

void Assets::useCharacterFile(const fs::path& file) {
    characterGroup_.reset();
    if (file.empty()) {
        localGroup_.reset();
        return;
    }
    localGroup_ = content::localGroupFor(file);
    if (added_.insert(file).second) {
        models_.addPackage(file, *localGroup_);
    }
}

void Assets::useCharacterGroup(std::uint32_t group) {
    localGroup_ = group;
    characterGroup_ = group;
}

void Assets::readSkins() {
    if (!skinsRead_) {
        skinsRead_ = true;
        for (const auto& folder : skinFolders_) {
            std::error_code ec;
            std::vector<fs::path> packages;
            for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
                if (it->path().extension() == ".package") {
                    packages.push_back(it->path());
                }
            }
            std::sort(packages.begin(), packages.end());
            for (const auto& p : packages) {
                skins_.addPackage(p);
            }
        }
    }
}

void Assets::forEachSkinResource(std::uint32_t type, const std::function<void(const std::vector<std::uint8_t>&)>& visit) {
    readSkins();
    for (const auto& r : skins_.resources()) {
        if (r.entry.key.type != type) {
            continue;
        }
        try {
            visit(skins_.read(r));
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("skins: a resource that can't be read is left out: ") + ex.what());
        }
    }
}

std::optional<std::vector<std::uint8_t>> Assets::skinResource(const sg::Link& link) {
    if (link.group == kLocalGroup) {
        return byKey(link);
    }
    readSkins();
    const auto* r = skins_.find({link.type, link.group, link.instance, link.instanceHigh});
    if (r == nullptr) {
        return std::nullopt;
    }
    return skins_.read(*r);
}

std::optional<std::vector<std::uint8_t>> Assets::byKey(const sg::Link& link) {
    dbpf::ResourceKey key;
    key.type = link.type;
    key.group = link.group == kLocalGroup && localGroup_ ? *localGroup_ : link.group;
    key.instance = link.instance;
    key.instanceHigh = link.instanceHigh;
    const auto* r = models_.find(key);
    if (r == nullptr && key.instanceHigh == 0) {
        // Reference lists may leave out the high instance (a Sim's skeleton): any resource with the
        // rest of the key.
        for (const auto& candidate : models_.resources()) {
            const auto& k = candidate.entry.key;
            if (k.type == key.type && k.group == key.group && k.instance == key.instance) {
                r = &candidate;
            }
        }
    }
    if (r == nullptr) {
        // A character group's looks (an NPC's age data, outfit parts and models) are in the object packages.
        if (const auto* own = characterGroup_ == key.group ? objects_.find(key) : nullptr) {
            return objects_.read(*own);
        }
        return std::nullopt;
    }
    return models_.read(*r);
}

namespace {

// A texture's mip levels. The largest levels may be kept in separate resources. When one of those
// is missing (broken custom content), the texture starts at the largest level that is there.
class TextureLevels {
public:
    static std::optional<TextureLevels> read(Assets& assets, const std::string& name) {
        try {
            const auto data = assets.byName(kTxtr, name + "_txtr");
            if (!data) {
                assets.warnings.push_back("texture " + name + " not found");
                return std::nullopt;
            }
            TextureLevels t(assets, name, sg::parseImage(*data));
            for (std::size_t level = 0; level < t.image_.mips.size(); ++level) {
                if (auto bytes = t.bytes(level); !bytes.empty()) {
                    t.first_ = level;
                    t.firstBytes_ = std::move(bytes);
                    return t;
                }
            }
        } catch (const std::exception& ex) {
            assets.warnings.push_back("texture " + name + ": " + ex.what());
        }
        return std::nullopt;
    }

    // Levels counted from the largest that is there (0).
    std::size_t count() const { return image_.mips.size() - first_; }
    std::uint32_t width(std::size_t level) const { return size(image_.width, first_ + level); }
    std::uint32_t height(std::size_t level) const { return size(image_.height, first_ + level); }

    std::uint32_t format() const { return image_.format; }
    // A bump map's height scale (the game's files all have 10, older versions carry none).
    float amplitude() const { return image_.bumpAmplitude.value_or(10.0f); }

    std::optional<Image> decode(std::size_t level) {
        try {
            const auto data = levelBytes(level);
            if (data.empty()) {
                return std::nullopt;
            }
            const std::uint32_t w = width(level), h = height(level);
            return Image{w, h, sg::decodeRgba(data, w, h, image_.format)};
        } catch (const std::exception& ex) {
            assets_.warnings.push_back("texture " + name_ + ": " + ex.what());
            return std::nullopt;
        }
    }

    // A level's data as stored, the largest level's only once.
    std::vector<std::uint8_t> levelBytes(std::size_t level) {
        return level == 0 ? std::move(firstBytes_) : bytes(first_ + level);
    }

private:
    TextureLevels(Assets& assets, std::string name, sg::Image image)
        : assets_(assets), name_(std::move(name)), image_(std::move(image)) {}

    // `smaller` steps below the largest level in the file.
    static std::uint32_t size(std::uint32_t s, std::size_t smaller) {
        return smaller >= 32 ? 1u : std::max(1u, s >> smaller);
    }

    // A level's data by steps below the largest in the file. Empty when it isn't there.
    std::vector<std::uint8_t> bytes(std::size_t smaller) {
        const sg::MipLevel& mip = image_.mips[image_.mips.size() - 1 - smaller];
        if (mip.largeMipName.empty()) {
            return mip.data;
        }
        if (const auto lifo = assets_.byName(kLifo, mip.largeMipName)) {
            return sg::parseLevelInfo(*lifo).data;
        }
        assets_.warnings.push_back("large mip " + mip.largeMipName + " not found");
        return {};
    }

    Assets& assets_;
    std::string name_;
    sg::Image image_;
    std::size_t first_ = 0;
    std::vector<std::uint8_t> firstBytes_;
};

bool powerOfTwo(std::uint32_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

} // namespace

std::optional<Image> Assets::texture(const std::string& name) {
    auto t = TextureLevels::read(*this, name);
    return t ? t->decode(0) : std::nullopt;
}

std::optional<std::pair<std::uint32_t, std::uint32_t>> Assets::textureSize(const std::string& name) {
    const auto t = TextureLevels::read(*this, name);
    if (!t) {
        return std::nullopt;
    }
    return std::pair{t->width(0), t->height(0)};
}

std::optional<std::size_t> mipLevelOfSize(std::uint32_t width, std::uint32_t height, std::size_t count,
                                          std::uint32_t wantedWidth, std::uint32_t wantedHeight) {
    if ((width != wantedWidth || height != wantedHeight) && (!powerOfTwo(width) || !powerOfTwo(height))) {
        return std::nullopt;
    }
    for (std::size_t level = 0; level < count; ++level) {
        if (width == wantedWidth && height == wantedHeight) {
            return level;
        }
        if (width == 1 && height == 1) {
            break;
        }
        width = (width + 1) / 2;
        height = (height + 1) / 2;
    }
    return std::nullopt;
}

std::optional<Image> Assets::texture(const std::string& name, std::uint32_t width, std::uint32_t height) {
    auto t = TextureLevels::read(*this, name);
    if (!t) {
        return std::nullopt;
    }
    const auto level = mipLevelOfSize(t->width(0), t->height(0), t->count(), width, height);
    return level ? t->decode(*level) : std::nullopt;
}

std::optional<Image> Assets::normalMap(const std::string& name) {
    auto t = TextureLevels::read(*this, name);
    if (!t) {
        return std::nullopt;
    }
    if (t->format() != 6) {
        return t->decode(0);
    }
    // As the game: each level from that level's own heights. Levels stop at the first missing one,
    // and at 1 x 1.
    std::optional<Image> out;
    try {
        for (std::size_t level = 0; level < t->count(); ++level) {
            const std::uint32_t w = t->width(level), h = t->height(level);
            auto rgba = bumpNormals(t->levelBytes(level), w, h, t->amplitude());
            if (rgba.empty()) {
                break;
            }
            if (!out) {
                out = Image{w, h, std::move(rgba)};
            } else {
                out->smallerLevels.push_back(std::move(rgba));
            }
            if (w == 1 && h == 1) {
                break;
            }
        }
    } catch (const std::exception& ex) {
        warnings.push_back("texture " + name + ": " + ex.what());
    }
    if (!out) {
        warnings.push_back("bump map " + name + ": no heights");
    }
    return out;
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

void Assets::readObjectTypes() {
    if (objectTypesRead_) {
        return;
    }
    objectTypesRead_ = true;
    for (const auto& r : objects_.resources()) {
        if (r.entry.key.type != types::kObjectDefinition) {
            continue;
        }
        try {
            auto d = objd::parse(objects_.read(r));
            const std::uint32_t guid = d.guid();
            objectTypes_[guid] = {r.entry.key.group, std::move(d)};
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("objects: an object definition that can't be read is left out: ") + ex.what());
        }
    }
}

const objd::ObjectDefinition* Assets::objectDefinition(std::uint32_t guid) {
    readObjectTypes();
    const auto type = objectTypes_.find(guid);
    return type == objectTypes_.end() ? nullptr : &type->second.definition;
}

std::string Assets::slotBone(std::uint32_t guid, std::size_t slot) {
    const auto& bones = slotBones(guid);
    return slot < bones.size() ? bones[slot] : std::string();
}

const std::vector<std::string>& Assets::slotBones(std::uint32_t guid) {
    if (const auto it = slotBones_.find(guid); it != slotBones_.end()) {
        return it->second;
    }
    auto& bones = slotBones_[guid];
    readObjectTypes();
    const auto type = objectTypes_.find(guid);
    if (type == objectTypes_.end() || type->second.definition.field(objd::field::kSlotsId) == 0) {
        return bones;
    }
    // The type's SLOT entries name bones through its "Model - Bones" string table.
    const std::uint32_t group = type->second.group, slotsId = type->second.definition.field(objd::field::kSlotsId);
    const auto* slots = objects_.find({types::kSlots, group, slotsId, 0});
    const auto* names = objects_.find({types::kStringTable, group, kBoneNames, 0});
    if (slots == nullptr || names == nullptr) {
        return bones;
    }
    try {
        const slot::Slots parsed = slot::parse(objects_.read(*slots));
        const auto table = str::parse(objects_.read(*names));
        const auto entries = table.forLanguage(1);
        for (const slot::Entry& e : parsed.entries) {
            if (e.type == slot::kContainerSlot) {
                const std::int32_t b = e.bone;
                bones.push_back(b >= 0 && static_cast<std::size_t>(b) < entries.size() ? entries[static_cast<std::size_t>(b)]->value
                                                                                       : std::string());
            }
        }
    } catch (const std::exception&) {
        bones.clear();
    }
    return bones;
}

std::optional<std::string> Assets::objectString(std::uint32_t group, std::uint32_t table, std::size_t entry) {
    const auto* strings = objects_.find({types::kStringTable, group, table, 0});
    if (strings == nullptr) {
        return std::nullopt;
    }
    try {
        const auto parsed = str::parse(objects_.read(*strings));
        const auto entries = parsed.forLanguage(1);
        return entry < entries.size() ? std::optional<std::string>(entries[entry]->value) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::string Assets::patternMaterial(const std::string& name, bool wall) const {
    const auto& scripted = wall ? scripts_.wallPatterns : scripts_.floorPatterns;
    if (const auto it = scripted.find(lower(name)); it != scripted.end()) {
        return it->second;
    }
    if (!name.empty() && name.size() <= 8 && name.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        if (const auto it = patternsByGuid_.find(*number(name, 16));
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
            const auto parsed = guid == p.end() ? std::nullopt : number(guid->second, 10);
            if (!parsed) {
                continue;
            }
            const auto id = *parsed;
            if (type == kFenceProperties) {
                fences_[id] = {p["rail"], p["diagrail"], p["post"], p["maywalkover"] == "1"};
            } else if (type == kRoofProperties) {
                roofPatterns_[id] = {p["texturetop"], p["textureunder"], p["textureedges"], p["texturetrim"]};
            } else if (const auto material = p.find("material"); material != p.end()) {
                patternsByGuid_[id] = material->second;
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("catalog: a build or buy catalog entry that can't be read is left out: ") + ex.what());
        }
    }
}

} // namespace ts2::scene
