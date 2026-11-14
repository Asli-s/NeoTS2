#include "content/nhood_catalog.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/image.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kCatalogEntry = 0x6D619378;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::map<std::uint32_t, NeighborhoodObjectType> neighborhoodObjectTypes(const std::vector<Product>& products) {
    std::map<std::uint32_t, NeighborhoodObjectType> out;
    auto ordered = products;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::set<fs::path> seen;
    for (const auto& product : ordered) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(res.filename().string()) != "res") {
                res = res.parent_path();
            }
            const fs::path folder = res / "Catalog" / "CANHObjects";
            std::error_code ec;
            if (!fs::is_directory(folder, ec) || !seen.insert(folder).second) {
                continue;
            }
            for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
                if (it->path().extension() != ".package") {
                    continue;
                }
                dbpf::Package package{it->path()};
                for (const auto& e : package.entries()) {
                    if (e.key.type != kCatalogEntry) {
                        continue;
                    }
                    try {
                        auto p = propset::parse(package.read(e));
                        const auto guid = p.find("guid");
                        if (guid == p.end() || guid->second.empty()) {
                            continue;
                        }
                        auto number = [&](const char* key) -> std::uint32_t {
                            const auto v = p.find(key);
                            return v == p.end() || v->second.empty() ? 0 : static_cast<std::uint32_t>(std::stoul(v->second));
                        };
                        out[static_cast<std::uint32_t>(std::stoul(guid->second))] = {p["modelname"], p["sort"], p["name"], p.find("showincatalog") == p.end() || number("showincatalog") != 0,
                                                                                   number("stringsetgroupid"), number("stringsetid")};
                    } catch (const std::exception& ex) {
                        diag::warnOnce(std::string("neighborhood objects: a broken catalog entry is left out: ") + ex.what());
                    }
                }
            }
        }
    }
    return out;
}


std::map<std::uint32_t, tga::Pixels> neighborhoodObjectThumbnails(const std::vector<Product>& products) {
    constexpr std::uint32_t kThumbnailType = 0x4D533EDD;
    std::map<std::uint32_t, tga::Pixels> out;
    auto ordered = products;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::set<fs::path> seen;
    for (const auto& product : ordered) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(res.filename().string()) != "res") {
                res = res.parent_path();
            }
            const fs::path file = res / "UserData" / "Thumbnails" / "CANHObjectsThumbnails.package";
            std::error_code ec;
            if (!fs::is_regular_file(file, ec) || !seen.insert(file).second) {
                continue;
            }
            try {
                dbpf::Package package{file};
                for (const auto& e : package.entries()) {
                    if (e.key.type == kThumbnailType) {
                        if (auto picture = image::decode(package.read(e))) {
                            out[e.key.instance] = std::move(*picture);
                        }
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("neighborhood objects: catalog pictures that can't be read are left out: ") + ex.what());
            }
        }
    }
    return out;
}
} // namespace ts2::content
