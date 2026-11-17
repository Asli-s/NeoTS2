#include "content/recolour_catalog.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <tuple>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t kMaterialOverride = 0x4C697E5A;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string value(const std::map<std::string, std::string>& set, const char* key) {
    const auto it = set.find(key);
    return it != set.end() ? it->second : std::string{};
}

} // namespace

void RecolourCatalog::add(RecolourMaterial material) {
    byModel_[lower(material.model)].push_back(materials_.size());
    materials_.push_back(std::move(material));
}

const RecolourMaterial* RecolourCatalog::find(const std::string& model, const std::string& subset, const std::string& name) const {
    const auto it = byModel_.find(lower(model));
    if (it == byModel_.end()) {
        return nullptr;
    }
    const std::string s = lower(subset), n = lower(name);
    for (const std::size_t i : it->second) {
        if (lower(materials_[i].subset) == s && lower(materials_[i].name) == n) {
            return &materials_[i];
        }
    }
    return nullptr;
}

std::vector<const RecolourMaterial*> RecolourCatalog::family(const std::string& model, const std::string& family) const {
    std::vector<const RecolourMaterial*> out;
    const auto it = byModel_.find(lower(model));
    if (it == byModel_.end()) {
        return out;
    }
    const std::string f = lower(family);
    for (const std::size_t i : it->second) {
        if (materials_[i].family == f) {
            out.push_back(&materials_[i]);
        }
    }
    return out;
}

RecolourCatalog loadRecolourCatalog(const std::vector<Product>& products) {
    RecolourCatalog catalog;
    std::set<std::tuple<std::string, std::string, std::string, std::string>> seen;
    std::set<fs::path> folders;
    for (const auto& objects : objectLoadOrder(products)) {
        const fs::path folder = objects.parent_path().parent_path() / "Catalog" / "Materials";
        if (!folders.insert(folder).second) {
            continue;
        }
        std::vector<fs::path> packages;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
            if (it->path().extension() == ".package") {
                packages.push_back(it->path());
            }
        }
        std::sort(packages.begin(), packages.end());
        for (const auto& path : packages) {
            try {
                dbpf::Package package{path};
                for (const auto& e : package.entries()) {
                    if (e.key.type != kMaterialOverride) {
                        continue;
                    }
                    const auto set = propset::parse(package.read(e));
                    RecolourMaterial m{value(set, "modelName"), value(set, "subsetName"), value(set, "name"), lower(value(set, "family"))};
                    if (m.model.empty() || m.name.empty() || m.family.empty()) {
                        continue;
                    }
                    if (seen.emplace(lower(m.model), lower(m.subset), lower(m.name), m.family).second) {
                        catalog.add(std::move(m));
                    }
                }
            } catch (const std::exception& ex) {
                diag::warnOnce(std::string("recolours: a material catalog that can't be read is left out: ") + ex.what());
            }
        }
    }
    return catalog;
}

} // namespace ts2::content
