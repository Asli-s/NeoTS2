#include "content/lighting.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/image.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <optional>
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
            while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) && line[end] != '(') {
                ++end;
            }
        }
        out.push_back(line.substr(i, end - i));
        i = end;
    }
    return out;
}

// A number from the text. Text that isn't a finite number (a broken or modded file) reads as 0.
float finiteNumber(const std::string& s) {
    const float f = std::strtof(s.c_str(), nullptr);
    return std::isfinite(f) ? f : 0.0f;
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
        if (!std::isfinite(x)) {
            x = 0;
        }
    }
    return v;
}

// Limits for the settings files, which mods may change: how deep files include files and macros
// create macros, the lines one macro may hold, and the lines and bytes read in all (macros multiply
// them). The game's own files come to about 23,000 lines and 1.5 MB.
constexpr int kMaxDepth = 16;
constexpr std::size_t kMaxMacroLines = 4096;
constexpr std::size_t kMaxLines = 1u << 20;
constexpr std::size_t kMaxExpandedLine = 64 * 1024;  // macro arguments inside arguments can grow a line exponentially
constexpr std::size_t kMaxBytes = 64u << 20;

std::array<float, 2> pair(const std::string& s) {
    const auto v = triple(s);
    return {v[0], v[1]};
}

bool truth(const std::string& s) {
    const std::string l = lower(s);
    return l == "true" || finiteNumber(l) != 0;
}

// "(text)" without its brackets.
std::string inside(const std::string& s) {
    if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Replaces every `from` in s with `to`. Returns false, leaving s as it was, when the result would
// be longer than `limit`.
bool replaceAll(std::string& s, const std::string& from, const std::string& to, std::size_t limit) {
    if (from.empty()) {
        return s.size() <= limit;
    }
    std::string out;
    std::size_t start = 0;
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, start)) {
        out.append(s, start, at - start);
        out += to;
        start = at + from.size();
        if (out.size() > limit) {
            return false;
        }
    }
    out.append(s, start);
    if (out.size() > limit) {
        return false;
    }
    s = std::move(out);
    return true;
}

void setAttribute(Lighting::LightAttributes& a, const std::string& name, const std::string& value) {
    const float f = finiteNumber(value);
    if (name == "intensity") a.intensity = f;
    else if (name == "falloffRate") a.falloffRate = f;
    else if (name == "relSpecIntensity") a.relSpecIntensity = f;
    else if (name == "range") a.range = f;
    else if (name == "coneAngle") a.coneAngle = f;
    else if (name == "conePenumbra") a.conePenumbra = f;
    else if (name == "color") a.color = triple(value);
    else if (name == "diffusionSourceMin") a.diffusionMin = pair(value);
    else if (name == "diffusionSourceMax") a.diffusionMax = pair(value);
    else if (name == "diffusionSourceIntensity") a.diffusionIntensity = f;
    else if (name == "diffusionSourceColor") a.diffusionColor = triple(value);
    else if (name == "diffusionSourceIsDirectional") a.diffusionDirectional = truth(value);
    else if (name == "diffusionSourceFalloff") a.diffusionFalloff = f;
    else if (name == "diffusionSourceDirectionalFalloff") a.diffusionDirectionalFalloff = f;
    else if (name == "diffusionSourceWallIntensityScale") a.diffusionWallScale = f;
    else if (name == "diffusionSourceFloorIntensityScale") a.diffusionFloorScale = f;
    else if (name == "diffusionSourceInterFloorFalloff") a.diffusionInterFloorFalloff = f;
}

struct Macro {
    std::string parameter;
    std::vector<std::string> lines;
};

struct Reader {
    Lighting& out;
    std::set<fs::path> reading;
    std::map<std::string, Macro> macros;
    Lighting::State* state = nullptr;
    // While a define is open: its name and what it holds so far.
    std::optional<std::pair<std::string, Macro>> defining;
    // Open if blocks: whether this one's lines are read, and whether a branch was taken.
    std::vector<std::pair<bool, bool>> conditions;
    std::size_t lines = 0;
    std::size_t bytes = 0;  // read and expanded so far, including macro lines left out for their length
    bool followIncludes = true;

    explicit Reader(Lighting& lighting) : out(lighting) {}

    bool full() {
        if (lines < kMaxLines && bytes < kMaxBytes) {
            return false;
        }
        diag::warnOnce("lighting: the settings files are too long, the rest is left out");
        return true;
    }

    std::string value(const std::string& word) const {
        if (!word.empty() && word[0] == '$') {
            const auto it = out.values.find(word.substr(1));
            return it == out.values.end() ? std::string() : it->second;
        }
        return word;
    }

    bool active() const {
        for (const auto& c : conditions) {
            if (!c.first) {
                return false;
            }
        }
        return true;
    }

    void file(const fs::path& path, int depth) {
        if (depth > kMaxDepth || !fs::is_regular_file(path) || !reading.insert(path).second) {
            return;
        }
        std::ifstream in(path, std::ios::binary);
        text(in, path, depth);
        reading.erase(path);
    }

    void text(std::istream& in, const fs::path& path, int depth) {
        for (std::string text; !full() && std::getline(in, text);) {
            line(text, path, depth);
        }
    }

    void create(const std::string& name, const std::string& argument, const fs::path& path, int depth) {
        const auto it = macros.find(name);
        if (it == macros.end() || depth > kMaxDepth) {
            return;
        }
        const Macro macro = it->second;  // a line of it may redefine it
        for (std::string text : macro.lines) {
            if (!replaceAll(text, "&{" + macro.parameter + "}", argument, kMaxExpandedLine) ||
                !replaceAll(text, "&" + macro.parameter, argument, kMaxExpandedLine)) {
                diag::warnOnce("lighting: a macro line expanding past 64 KB is left out");
                bytes += kMaxExpandedLine;
                if (full()) {
                    return;
                }
                continue;
            }
            if (full()) {
                return;
            }
            line(text, path, depth + 1);
        }
    }

    void line(std::string text, const fs::path& path, int depth) {
        ++lines;
        bytes += text.size();
        if (const auto hash = text.find('#'); hash != std::string::npos) {
            text.resize(hash);
        }
        const auto w = words(text);
        if (w.empty()) {
            return;
        }
        const std::string& key = w[0];
        if (defining) {
            if (key == "enddef") {
                macros[defining->first] = std::move(defining->second);
                defining.reset();
            } else if (defining->second.lines.size() < kMaxMacroLines) {
                defining->second.lines.push_back(text);
            }
            return;
        }
        // if / elseif / else / endif, on a variable being non-zero.
        if (key == "if" && w.size() >= 2) {
            const bool on = active() && truth(value(inside(w[1])));
            conditions.push_back({on, on});
            return;
        }
        if ((key == "elseif" || key == "else") && !conditions.empty()) {
            auto& [on, taken] = conditions.back();
            on = !taken && (key == "else" || (w.size() >= 2 && truth(value(inside(w[1])))));
            taken = taken || on;
            return;
        }
        if (key == "endif") {
            if (!conditions.empty()) {
                conditions.pop_back();
            }
            return;
        }
        if (!active()) {
            return;
        }
        if (key == "define" && w.size() >= 3) {
            defining.emplace(w[1], Macro{inside(w[2]), {}});
        } else if (key == "undefine" && w.size() >= 2) {
            macros.erase(w[1]);
        } else if (key == "create" && w.size() >= 3) {
            create(w[1], inside(w[2]), path, depth);
        } else if (key == "lightAttribOverride" && w.size() >= 4) {
            setAttribute(out.lights[lower(w[1])], w[2], value(w[3]));
        } else if (key == "lightingState" && w.size() >= 2) {
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
        } else if ((key == "include" || key == "sinclude") && w.size() >= 2 && followIncludes) {
            file(path.parent_path() / w[1], depth + 1);
        } else if (state && key == "light" && w.size() >= 5) {
            auto& l = state->lights[w[1]];
            l.direction = triple(w[2]);
            l.color = triple(w[3]);
            l.strength = finiteNumber(value(w[4]));
        } else if (state && key == "skylight" && w.size() >= 3) {
            const auto color = triple(w[1]);
            const float strength = finiteNumber(value(w[2]));
            state->skylight = {color[0] * strength, color[1] * strength, color[2] * strength};
        } else if (state && key == "environment" && w.size() >= 3) {
            const auto color = triple(w[1]);
            const float strength = finiteNumber(value(w[2]));
            state->environment = {color[0] * strength, color[1] * strength, color[2] * strength};
        } else if (state && key == "lightMap" && w.size() >= 2) {
            const auto id = static_cast<std::uint32_t>(std::strtoul(value(w[1]).c_str(), nullptr, 0));
            state->lightMap = id ? std::optional<std::uint32_t>(id) : std::nullopt;
            for (std::size_t i = 2; i + 1 < w.size(); i += 2) {
                if (w[i] == "-strength") {
                    state->lightMapStrength = finiteNumber(value(w[i + 1]));
                } else if (w[i] == "-tint") {
                    state->lightMapTint = triple(w[i + 1]);
                }
            }
        } else if (state && key == "modifiers" && w.size() >= 4) {
            auto& m = state->modifiers[w[1]];
            float* fields[] = {&m.practicalStrength, &m.diffusionStrength, &m.practicalFalloff, &m.diffusionFalloff};
            for (std::size_t i = 0; i < 4 && i + 2 < w.size(); ++i) {
                *fields[i] = finiteNumber(value(w[i + 2]));
            }
        } else if (state && key == "unlitRoomColor" && w.size() >= 2) {
            // The game keeps only the last such line of a state.
            Lighting::UnlitRoom u;
            u.color = triple(w[1]);
            for (std::size_t i = 2; i + 1 < w.size(); i += 2) {
                if (w[i] == "-threshold") {
                    u.threshold = finiteNumber(value(w[i + 1]));
                }
            }
            state->unlitRoom = u;
        } else if (state && key == "diffParam" && w.size() >= 3) {
            const std::string v = value(w[2]);
            if (!v.empty() && v.front() == '(') {
                const auto c = triple(v);
                state->diffParams[w[1]] = {c[0], c[1], c[2]};
            } else {
                state->diffParams[w[1]] = {finiteNumber(v)};
            }
        }
    }
};

} // namespace

const Lighting::LightAttributes* Lighting::light(const std::string& name) const {
    const auto it = lights.find(lower(name));
    return it == lights.end() ? nullptr : &it->second;
}

float Lighting::number(const std::string& name, float fallback) const {
    const auto it = values.find(name);
    return it == values.end() ? fallback : finiteNumber(it->second);
}

Lighting readLighting(const std::vector<Product>& products) {
    Lighting out;
    Reader reader{out};
    auto ordered = products;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Product& a, const Product& b) { return a.releaseOrder < b.releaseOrder; });
    std::set<fs::path> seen;
    for (const auto& product : ordered) {
        for (const auto& objects : product.objectPackages) {
            const fs::path file = dataFolder(objects) / "Res" / "Lights" / "Lighting.txt";
            if (seen.insert(file).second) {
                reader.file(file, 0);
            }
        }
    }
    return out;
}

Lighting readLightingText(const std::string& text) {
    Lighting out;
    Reader reader{out};
    reader.followIncludes = false;
    std::istringstream in(text);
    reader.text(in, fs::path(), 0);
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
