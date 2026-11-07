#include "content/cameras.hpp"

#include "content/install.hpp"
#include "diag/log.hpp"
#include "formats/dbpf.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>
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
        // The shipped folders name each camera once. Which of two files naming the same camera the game
        // takes is not known yet, so files are read in name order to give the same one on every system.
        std::vector<fs::path> files;
        for (const auto& item : fs::directory_iterator(folder)) {
            if (lower(item.path().extension().string()) == ".txt") {
                files.push_back(item.path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            std::ifstream in(file, std::ios::binary);
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
}

const effects::File* EffectsFiles::all() {
    if (read_) {
        return joined_ ? &*joined_ : nullptr;
    }
    read_ = true;
    const auto start = std::chrono::steady_clock::now();
    struct Read {
        std::uint32_t instance = 0;
        effects::File file;
    };
    std::vector<Read> files;
    for (const auto& path : paths_) {
        try {
            dbpf::Package package(path);
            for (const auto& entry : package.entries()) {
                if (entry.key.type == effects::kEffectsType) {
                    files.push_back({entry.key.instance, effects::parse(package.read(entry))});
                }
            }
        } catch (const std::exception& ex) {
            diag::warnOnce("effects: " + path.string() + " can't be read and is passed over: " + ex.what());
        }
    }
    std::stable_sort(files.begin(), files.end(), [](const Read& a, const Read& b) { return a.instance < b.instance; });
    for (auto& read : files) {
        if (joined_) {
            effects::merge(*joined_, std::move(read.file));
        } else {
            joined_ = std::move(read.file);
        }
    }
    if (joined_) {
        diag::info(std::format("effects: {} files, {} names, read in {:.0f} ms", files.size(), joined_->names.size(),
                               std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
    }
    return joined_ ? &*joined_ : nullptr;
}

const effects::File* EffectsFiles::with(std::string_view name) {
    const auto* joined = all();
    return joined != nullptr && joined->find(name) != nullptr ? joined : nullptr;
}

} // namespace ts2::content
