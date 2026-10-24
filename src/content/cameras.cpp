#include "content/cameras.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <fstream>
#include <set>
#include <sstream>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Each product's TSData/Res folder, oldest release first.
std::vector<fs::path> resFolders(const fs::path& installRoot) {
    auto products = findProducts(installRoot);
    std::stable_sort(products.begin(), products.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::vector<fs::path> folders;
    std::set<fs::path> seen;
    for (const auto& product : products) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(res.filename().string()) != "res") {
                res = res.parent_path();
            }
            if (seen.insert(res).second) {
                folders.push_back(res);
            }
        }
    }
    return folders;
}

} // namespace

std::optional<camera::Config> findCamera(const fs::path& installRoot, std::string_view name) {
    std::optional<camera::Config> found;
    for (const auto& res : resFolders(installRoot)) {
        const fs::path folder = res / "UserData" / "Cameras";
        if (!fs::is_directory(folder)) {
            continue;
        }
        for (const auto& item : fs::directory_iterator(folder)) {
            if (lower(item.path().extension().string()) != ".txt") {
                continue;
            }
            std::ifstream in(item.path(), std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            for (auto& c : camera::parse(text.str())) {
                if (c.name == name) {
                    found = std::move(c);
                }
            }
        }
    }
    return found;
}

EffectsFiles::EffectsFiles(const fs::path& installRoot) {
    const auto folders = resFolders(installRoot);
    for (auto res = folders.rbegin(); res != folders.rend(); ++res) {
        if (fs::path file = *res / "Effects" / "effects.package"; fs::is_regular_file(file)) {
            paths_.push_back(std::move(file));
        }
    }
    files_.resize(paths_.size());
    read_.resize(paths_.size());
}

const effects::File* EffectsFiles::file(std::size_t index) {
    if (!read_[index]) {
        read_[index] = true;
        try {
            dbpf::Package package(paths_[index]);
            for (const auto& entry : package.entries()) {
                if (entry.key.type == effects::kEffectsType) {
                    files_[index] = effects::parse(package.read(entry));
                    break;
                }
            }
        } catch (const std::exception& ex) {
            diag::warnOnce("effects: " + paths_[index].string() + " can't be read and is passed over: " + ex.what());
        }
    }
    return files_[index] ? &*files_[index] : nullptr;
}

const effects::File* EffectsFiles::with(std::string_view name) {
    for (std::size_t i = 0; i < paths_.size(); ++i) {
        if (const auto* f = file(i); f != nullptr && f->find(name) != nullptr) {
            return f;
        }
    }
    return nullptr;
}

const effects::File* EffectsFiles::newest() {
    return paths_.empty() ? nullptr : file(0);
}

} // namespace ts2::content
