#include "content/install.hpp"

#include "formats/dbpf.hpp"
#include "paths/game_folders.hpp"
#include "paths/names.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

struct KnownProduct {
    std::string_view code;
    std::string_view name;
    std::string_view keyword;
    std::string_view folder = {};  // another folder name some releases use for it
};

constexpr std::array<KnownProduct, 18> kReleaseOrder = {{
    {"base", "The Sims 2",           "", "the sims 2"},  // the disc installs' folder
    {"ep1", "University",            "university"},
    {"ep2", "Nightlife",             "nightlife"},
    {"ep3", "Open for Business",     "business"},
    {"sp1", "Family Fun Stuff",      "family fun"},
    {"sp2", "Glamour Life Stuff",    "glamour"},
    {"ep4", "Pets",                  "pets"},
    {"sp3", "Happy Holiday Stuff",   "holiday"},
    {"ep5", "Seasons",               "seasons"},
    {"sp4", "Celebration! Stuff",    "celebration"},
    {"sp5", "H&M Fashion Stuff",     "h&m"},
    {"ep6", "Bon Voyage",            "bon voyage"},
    {"sp6", "Teen Style Stuff",      "teen style"},
    {"ep7", "FreeTime",              "free time", "the sims 2 freetime"},  // the disc install's folder
    {"sp7", "Kitchen & Bath Interior Design Stuff", "kitchen"},
    {"sp8", "IKEA Home Stuff",       "ikea"},
    {"ep8", "Apartment Life",        "apartment life"},
    {"sp9", "Mansion & Garden Stuff", "mansion", "ep9"},  // the Legacy Collection's EP9 folder
}};

// The packs in the order of the game's "EPsInstalled" list, which is also its numbering of them.
constexpr std::string_view kGameOrder[] = {
    "ep1", "ep2", "ep3", "sp1", "sp2", "ep4", "ep5", "sp4", "sp5",
    "ep6", "sp6", "sp3", "ep7", "sp7", "sp8", "ep8", "sp9",
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

int matchComponent(const std::string& component) {
    for (std::size_t i = 0; i < kReleaseOrder.size(); ++i) {
        if (component == kReleaseOrder[i].code || component == kReleaseOrder[i].folder) {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < kReleaseOrder.size(); ++i) {
        const auto keyword = kReleaseOrder[i].keyword;
        if (!keyword.empty() && component.find(keyword) != std::string::npos) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string utf8(const fs::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

bool isObjectPackage(const fs::path& path) {
    const std::string file = lower(utf8(path.filename()));
    if (file != "objects.package" && file != "spobjects.package") {
        return false;
    }
    const fs::path folder = path.parent_path();
    if (lower(utf8(folder.filename())) != "objects") {
        return false;
    }
    const std::string above = lower(utf8(folder.parent_path().filename()));
    if (above == "res") {
        return true;
    }
    return above == "stuffpack" &&
           lower(utf8(folder.parent_path().parent_path().filename())) == "res";
}

// A catalog bundle holding objects (TSData/Res/Catalog/Bins/*.bundle.package): the Holiday packs'
// objects, among them the holiday NPCs, are only in these. globalcatbin holds no objects.
bool isObjectBundle(const fs::path& path) {
    const std::string file = lower(utf8(path.filename()));
    constexpr std::string_view kSuffix = ".bundle.package";
    if (file.size() <= kSuffix.size() || !file.ends_with(kSuffix) || file == "globalcatbin.bundle.package") {
        return false;
    }
    const fs::path folder = path.parent_path();
    return lower(utf8(folder.filename())) == "bins" && lower(utf8(folder.parent_path().filename())) == "catalog";
}

fs::path childNamed(const fs::path& folder, std::string_view name) {
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        if (paths::foldedName(it->path().filename()) == name) {
            return it->path();
        }
    }
    return {};
}

// The pack named by the folder's sound files (TSData/Sys/TSAudioPackedEP4.ini), or -1.
// The base game has none.
int soundFilesOrder(const fs::path& packFolder) {
    // A Sys folder holds about 15 files.
    constexpr int kMaxEntries = 1000;
    const fs::path tsData = childNamed(packFolder, "tsdata");
    const fs::path sys = tsData.empty() ? fs::path() : childNamed(tsData, "sys");
    if (sys.empty()) {
        return -1;
    }
    std::error_code error;
    int entries = 0;
    for (fs::directory_iterator it(sys, error), end; !error && it != end && ++entries <= kMaxEntries; it.increment(error)) {
        const std::string name = paths::foldedName(it->path().filename());
        for (const std::string_view kind : {"tsaudiolocale", "tsaudiopacked", "tsaudiounpacked"}) {
            if (name.size() == kind.size() + 7 && name.starts_with(kind) && name.ends_with(".ini")) {
                const std::string code = name.substr(kind.size(), 3);
                if ((code[0] == 'e' || code[0] == 's') && code[1] == 'p' && code[2] >= '1' && code[2] <= '9') {
                    return matchComponent(code);
                }
            }
        }
    }
    return -1;
}

int recordedOrder(const fs::path& packFolder, const std::vector<paths::RegisteredPack>& registered) {
    if (const int order = soundFilesOrder(packFolder); order >= 0) {
        return order;
    }
    for (const auto& pack : registered) {
        if (!paths::sameFolder(pack.folder, packFolder)) {
            continue;
        }
        if (pack.number == 0) {
            return matchComponent("base");
        }
        if (pack.number <= static_cast<int>(std::size(kGameOrder))) {
            return matchComponent(std::string(kGameOrder[static_cast<std::size_t>(pack.number - 1)]));
        }
    }
    return -1;
}

// Read once, because every part of the game asks for the packs.
std::vector<paths::RegisteredPack> registeredPacksOnce(const fs::path& installRoot) {
    static std::mutex mutex;
    static fs::path root;
    static std::vector<paths::RegisteredPack> packs;
    const std::lock_guard lock(mutex);
    if (root.empty() || root != installRoot) {
        packs = paths::registeredPacks(installRoot);
        root = installRoot;
    }
    return packs;
}

} // namespace

std::vector<Product> findProducts(const fs::path& installRoot) {
    std::map<int, Product> byOrder;
    int nextUnknown = static_cast<int>(kReleaseOrder.size());
    // A pack is named by its sound files, then the install records, then folder names.
    const std::vector<paths::RegisteredPack> registered = registeredPacksOnce(installRoot);
    std::map<fs::path, int> folderOrders;

    // A complete install has about 11,000 files and folders. A folder far larger (a whole drive
    // chosen by mistake) isn't an install, and walking all of it would look like a hang.
    constexpr int kMaxEntries = 500000;
    int entries = 0;
    for (const auto& item : fs::recursive_directory_iterator(
             installRoot, fs::directory_options::skip_permission_denied)) {
        if (++entries > kMaxEntries) {
            throw std::runtime_error(utf8(installRoot) + " holds far more files than a game install");
        }
        if (!item.is_regular_file()) {
            continue;
        }
        const bool bundle = isObjectBundle(item.path());
        if (!bundle && !isObjectPackage(item.path())) {
            continue;
        }

        std::error_code relativeError;
        const fs::path relative = fs::relative(item.path(), installRoot, relativeError);
        if (relativeError) {
            continue;
        }
        std::vector<std::string> components;
        fs::path packFolder = installRoot;
        bool belowTsData = false;
        for (const auto& part : relative.parent_path()) {
            components.push_back(lower(utf8(part)));
            belowTsData = belowTsData || components.back() == "tsdata";
            if (!belowTsData) {
                packFolder /= part;
            }
        }

        int order = -1;
        if (belowTsData) {
            auto [known, added] = folderOrders.try_emplace(packFolder, -1);
            if (added) {
                known->second = recordedOrder(packFolder, registered);
            }
            order = known->second;
        }
        for (auto it = components.rbegin(); it != components.rend() && order < 0; ++it) {
            order = matchComponent(*it);
        }

        Product* product = nullptr;
        if (order >= 0) {
            product = &byOrder[order];
            const KnownProduct& known = kReleaseOrder[static_cast<std::size_t>(order)];
            product->code = std::string(known.code);
            product->name = std::string(known.name);
        } else {
            product = &byOrder[nextUnknown];
            product->code = "?";
            product->name = utf8(*relative.begin());
            order = nextUnknown++;
        }
        product->releaseOrder = order;
        (bundle ? product->objectBundles : product->objectPackages).push_back(item.path());
    }

    std::vector<Product> products;
    // By name, so the order is the same on every system and not the folder listing's.
    const auto byName = [](const fs::path& a, const fs::path& b) { return lower(utf8(a.filename())) < lower(utf8(b.filename())); };
    for (auto& [order, product] : byOrder) {
        std::sort(product.objectPackages.begin(), product.objectPackages.end(), byName);
        std::sort(product.objectBundles.begin(), product.objectBundles.end(), byName);
        products.push_back(std::move(product));
    }
    return products;
}

std::uint32_t installedPacksMask(const std::vector<Product>& products) {
    constexpr std::uint32_t kAlwaysSet = 0x1000;
    std::uint32_t mask = kAlwaysSet;
    for (const auto& product : products) {
        if (product.code == "base") {
            mask |= 1u;
        }
        for (std::size_t i = 0; i < std::size(kGameOrder); ++i) {
            if (product.code == kGameOrder[i]) {
                mask |= 1u << (i + 1);
            }
        }
    }
    return mask;
}

int gameNumber(const Product& product) {
    if (product.code == "base") {
        return 0;
    }
    for (std::size_t i = 0; i < std::size(kGameOrder); ++i) {
        if (product.code == kGameOrder[i]) {
            return static_cast<int>(i) + 1;
        }
    }
    return -1;
}

std::vector<Product> inGameOrder(const std::vector<Product>& products) {
    std::vector<Product> out;
    for (const auto& product : products) {
        if (gameNumber(product) >= 0) {
            out.push_back(product);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Product& a, const Product& b) { return gameNumber(a) < gameNumber(b); });
    return out;
}

std::vector<fs::path> objectLoadOrder(const std::vector<Product>& products) {
    struct Snapshot {
        std::uint32_t entries;
        int releaseOrder;
        fs::path path;
    };
    std::vector<Snapshot> snapshots;
    std::vector<fs::path> stuffPackContent;

    for (const auto& product : products) {
        for (const auto& path : product.objectPackages) {
            if (lower(utf8(path.filename())) == "spobjects.package") {
                stuffPackContent.push_back(path);
            } else {
                snapshots.push_back({dbpf::Package::readHeaderOnly(path).indexEntryCount, product.releaseOrder, path});
            }
        }
    }

    std::stable_sort(snapshots.begin(), snapshots.end(), [](const Snapshot& a, const Snapshot& b) {
        return a.entries != b.entries ? a.entries < b.entries : a.releaseOrder < b.releaseOrder;
    });

    std::vector<fs::path> order;
    for (const auto& s : snapshots) {
        order.push_back(s.path);
    }
    order.insert(order.end(), stuffPackContent.begin(), stuffPackContent.end());
    for (const auto& product : products) {
        order.insert(order.end(), product.objectBundles.begin(), product.objectBundles.end());
    }
    return order;
}

std::optional<fs::path> newestDataFile(const std::vector<Product>& products, const fs::path& relative) {
    std::optional<fs::path> found;
    int order = -1;
    for (const auto& product : products) {
        for (const auto& objects : product.objectPackages) {
            fs::path data = objects.parent_path();
            while (data.has_relative_path() && lower(utf8(data.filename())) != "tsdata") {
                data = data.parent_path();
            }
            const fs::path file = data / relative;
            if (product.releaseOrder > order && fs::is_regular_file(file)) {
                found = file;
                order = product.releaseOrder;
            }
        }
    }
    return found;
}

} // namespace ts2::content
