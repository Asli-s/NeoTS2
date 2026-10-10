#include "content/lighting.hpp"

#include "formats/dbpf.hpp"
#include "formats/image.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

// A product's TSData folder, from one of its object packages.
fs::path dataFolder(const fs::path& objects) {
    fs::path data = objects.parent_path();
    while (data.has_relative_path() && lower(data.filename().string()) != "tsdata") {
        data = data.parent_path();
    }
    return data;
}

// A line's words, with "(...)" kept together and quotes removed.
std::vector<std::string> words(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
            continue;
        }
        std::size_t end = i;
        if (line[i] == '(') {
            end = line.find(')', i);
            end = end == std::string::npos ? line.size() : end + 1;
        } else if (line[i] == '"') {
            end = line.find('"', i + 1);
            out.push_back(line.substr(i + 1, (end == std::string::npos ? line.size() : end) - i - 1));
            i = end == std::string::npos ? line.size() : end + 1;
            continue;
        } else {
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end]))) {
                ++end;
            }
        }
        out.push_back(line.substr(i, end - i));
        i = end;
    }
    return out;
}

std::array<float, 3> triple(const std::string& s) {
    std::array<float, 3> v{0, 0, 0};
    std::string t = s;
    for (char& c : t) {
        if (c == '(' || c == ')' || c == ',') {
            c = ' ';
        }
    }
    std::istringstream in(t);
    for (auto& x : v) {
        in >> x;
    }
    return v;
}

struct Reader {
    Lighting& out;
    std::set<fs::path> reading;

    std::string value(const std::string& word) const {
        if (!word.empty() && word[0] == '$') {
            const auto it = out.values.find(word.substr(1));
            return it == out.values.end() ? std::string() : it->second;
        }
        return word;
    }

    void file(const fs::path& path) {
        if (!fs::is_regular_file(path) || !reading.insert(path).second) {
            return;
        }
        std::ifstream in(path, std::ios::binary);
        Lighting::State* state = nullptr;
        for (std::string line; std::getline(in, line);) {
            if (const auto hash = line.find('#'); hash != std::string::npos) {
                line.resize(hash);
            }
            const auto w = words(line);
            if (w.empty()) {
                continue;
            }
            const std::string key = w[0];
            if (key == "lightingState" && w.size() >= 2) {
                const bool fresh = out.states.find(w[1]) == out.states.end();
                state = &out.states[w[1]];
                if (fresh && w.size() >= 4 && w[2] == ":") {
                    const auto parent = out.states.find(w[3]);
                    if (parent != out.states.end()) {
                        *state = parent->second;
                    }
                }
            } else if (key == "end") {
                state = nullptr;
            } else if ((key == "set" || key == "setf" || key == "seti" || key == "setb") && w.size() >= 3) {
                out.values[w[1]] = value(w[2]);
            } else if ((key == "include" || key == "sinclude") && w.size() >= 2) {
                file(path.parent_path() / w[1]);
            } else if (state && key == "light" && w.size() >= 5) {
                auto& l = state->lights[w[1]];
                l.direction = triple(w[2]);
                l.color = triple(w[3]);
                l.strength = std::strtof(value(w[4]).c_str(), nullptr);
            } else if (state && key == "lightMap" && w.size() >= 2) {
                const auto id = static_cast<std::uint32_t>(std::strtoul(value(w[1]).c_str(), nullptr, 0));
                state->lightMap = id ? std::optional<std::uint32_t>(id) : std::nullopt;
                for (std::size_t i = 2; i + 1 < w.size(); i += 2) {
                    if (w[i] == "-strength") {
                        state->lightMapStrength = std::strtof(value(w[i + 1]).c_str(), nullptr);
                    } else if (w[i] == "-tint") {
                        state->lightMapTint = triple(w[i + 1]);
                    }
                }
            }
        }
        reading.erase(path);
    }
};

} // namespace

float Lighting::number(const std::string& name, float fallback) const {
    const auto it = values.find(name);
    return it == values.end() ? fallback : std::strtof(it->second.c_str(), nullptr);
}

Lighting readLighting(const std::vector<Product>& products) {
    Lighting out;
    Reader reader{out, {}};
    auto ordered = products;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::set<fs::path> seen;
    for (const auto& product : ordered) {
        for (const auto& objects : product.objectPackages) {
            const fs::path file = dataFolder(objects) / "Res" / "Lights" / "Lighting.txt";
            if (seen.insert(file).second) {
                reader.file(file);
            }
        }
    }
    return out;
}

std::optional<LightingImage> lightingImage(const std::vector<Product>& products, std::uint32_t instance) {
    std::optional<LightingImage> found;
    int order = -1;
    for (const auto& product : products) {
        for (const auto& objects : product.objectPackages) {
            const fs::path file = dataFolder(objects) / "Res" / "Lighting" / "lighting.package";
            if (product.releaseOrder <= order || !fs::is_regular_file(file)) {
                continue;
            }
            dbpf::Package package{file};
            for (const auto& e : package.entries()) {
                if (e.key.type != types::kImage || e.key.instance != instance) {
                    continue;
                }
                const auto pixels = image::decode(package.read(e));
                if (pixels) {
                    found = LightingImage{pixels->width, pixels->height, pixels->rgba};
                    order = product.releaseOrder;
                }
            }
        }
    }
    return found;
}

} // namespace ts2::content
