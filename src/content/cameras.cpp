#include "content/cameras.hpp"

#include "content/install.hpp"

#include <algorithm>
#include <cctype>
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

} // namespace

std::optional<camera::Config> findCamera(const fs::path& installRoot, std::string_view name) {
    auto products = findProducts(installRoot);
    std::stable_sort(products.begin(), products.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::optional<camera::Config> found;
    std::set<fs::path> seen;
    for (const auto& product : products) {
        for (const auto& objects : product.objectPackages) {
            fs::path res = objects.parent_path();
            while (res.has_relative_path() && lower(res.filename().string()) != "res") {
                res = res.parent_path();
            }
            const fs::path folder = res / "UserData" / "Cameras";
            if (!fs::is_directory(folder) || !seen.insert(folder).second) {
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
    }
    return found;
}

} // namespace ts2::content
