#include "content/nhood_catalog.hpp"

#include "formats/dbpf.hpp"
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
                        out[static_cast<std::uint32_t>(std::stoul(guid->second))] = {p["modelname"], p["sort"]};
                    } catch (const std::exception&) {
                    }
                }
            }
        }
    }
    return out;
}

} // namespace ts2::content
