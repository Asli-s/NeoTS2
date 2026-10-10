#include "content/material_scripts.hpp"

#include "formats/dbpf.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string_view>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kMaterialScript = 0xCD7FE87A;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

namespace {

// Every "create <macro>(<material> <texture>)" of the material scripts: texture by material.
std::map<std::string, std::string> scriptedTextures(const std::vector<Product>& products, std::string_view macro) {
    std::map<std::string, std::string> out;
    auto ordered = products;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::set<fs::path> seen;
    for (const auto& product : ordered) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(res.filename().string()) != "res") {
                res = res.parent_path();
            }
            const fs::path file = res / "Materials" / "Materials.package";
            if (!fs::is_regular_file(file) || !seen.insert(file).second) {
                continue;
            }
            dbpf::Package package{file};
            for (const auto& e : package.entries()) {
                if (e.key.type != kMaterialScript) {
                    continue;
                }
                const auto bytes = package.read(e);
                const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                const std::string kCreate = "create " + std::string(macro) + "(";
                for (std::size_t at = text.find(kCreate); at != std::string_view::npos; at = text.find(kCreate, at + 1)) {
                    const std::size_t from = at + kCreate.size();
                    const std::size_t end = text.find(')', from);
                    if (end == std::string_view::npos) {
                        break;
                    }
                    std::istringstream in{std::string(text.substr(from, end - from))};
                    std::string material, texture;
                    if (in >> material >> texture) {
                        out[material] = texture;
                    }
                }
            }
        }
    }
    return out;
}

} // namespace

std::map<std::string, std::string> neighborhoodRoadTextures(const std::vector<Product>& products) {
    return scriptedTextures(products, "NHoodRoadMaterial");
}

std::map<std::string, std::string> lotSkirtRoadTextures(const std::vector<Product>& products) {
    return scriptedTextures(products, "LotSkirtRoadMaterialInstance");
}

} // namespace ts2::content
