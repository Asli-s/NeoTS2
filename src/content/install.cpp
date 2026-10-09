#include "content/install.hpp"

#include "formats/dbpf.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <string_view>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

struct KnownProduct {
    std::string_view code;
    std::string_view name;
    std::string_view keyword;
};

constexpr std::array<KnownProduct, 18> kReleaseOrder = {{
    {"base", "The Sims 2",           ""},
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
    {"ep7", "FreeTime",              "free time"},
    {"sp7", "Kitchen & Bath Interior Design Stuff", "kitchen"},
    {"sp8", "IKEA Home Stuff",       "ikea"},
    {"ep8", "Apartment Life",        "apartment life"},
    {"sp9", "Mansion & Garden Stuff", "mansion"},
}};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

int matchComponent(const std::string& component) {
    for (std::size_t i = 0; i < kReleaseOrder.size(); ++i) {
        if (component == kReleaseOrder[i].code) {
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

} // namespace

std::vector<Product> findProducts(const fs::path& installRoot) {
    std::map<int, Product> byOrder;
    int nextUnknown = static_cast<int>(kReleaseOrder.size());

    for (const auto& item : fs::recursive_directory_iterator(
             installRoot, fs::directory_options::skip_permission_denied)) {
        if (!item.is_regular_file() || !isObjectPackage(item.path())) {
            continue;
        }

        const fs::path relative = fs::relative(item.path(), installRoot);
        std::vector<std::string> components;
        for (const auto& part : relative.parent_path()) {
            components.push_back(lower(utf8(part)));
        }

        int order = -1;
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
        product->objectPackages.push_back(item.path());
    }

    std::vector<Product> products;
    for (auto& [order, product] : byOrder) {
        std::sort(product.objectPackages.begin(), product.objectPackages.end(),
                  [](const fs::path& a, const fs::path& b) {
                      return lower(utf8(a.filename())) < lower(utf8(b.filename()));
                  });
        products.push_back(std::move(product));
    }
    return products;
}

std::uint32_t installedPacksMask(const std::vector<Product>& products) {
    static constexpr std::string_view kPackBits[] = {
        "ep1", "ep2", "ep3", "sp1", "sp2", "ep4", "ep5", "sp4", "sp5",
        "ep6", "sp6", "sp3", "ep7", "sp7", "sp8", "ep8", "sp9",
    };
    constexpr std::uint32_t kAlwaysSet = 0x1000;
    std::uint32_t mask = kAlwaysSet;
    for (const auto& product : products) {
        for (std::size_t bit = 0; bit < std::size(kPackBits); ++bit) {
            if (product.code == kPackBits[bit]) {
                mask |= 1u << bit;
            }
        }
    }
    return mask;
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
                const dbpf::Package package{path};
                snapshots.push_back({package.header().indexEntryCount, product.releaseOrder, path});
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
    return order;
}

} // namespace ts2::content
