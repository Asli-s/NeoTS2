#include "content/wants.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <exception>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

template <typename Enum, std::size_t N>
std::optional<Enum> lookup(const std::pair<const char*, Enum> (&names)[N], const std::string& name) {
    for (const auto& [text, value] : names) {
        if (name == text) {
            return value;
        }
    }
    return std::nullopt;
}

constexpr std::pair<const char*, WantArgument> kArguments[] = {
    {"None", WantArgument::None},   {"Sim", WantArgument::Sim},       {"Guid", WantArgument::Object},
    {"Category", WantArgument::Category}, {"Skill", WantArgument::Skill}, {"Career", WantArgument::Career},
    {"Badge", WantArgument::Badge},
};

constexpr std::pair<const char*, WantLevel> kLevels[] = {
    {"Transitory", WantLevel::Transitory}, {"Memorable", WantLevel::Memorable}, {"Power", WantLevel::Power},
};

// The definitions use "Then" for "Than".
constexpr std::pair<const char*, WantOperator> kOperators[] = {
    {"None", WantOperator::None},
    {"Equals", WantOperator::Equals},
    {"Less_Then", WantOperator::LessThan},
    {"Greater_Then", WantOperator::GreaterThan},
    {"Less_Then_Or_Equals", WantOperator::LessThanOrEquals},
    {"Greater_Then_Or_Equals", WantOperator::GreaterThanOrEquals},
};

} // namespace

namespace {

// The WantTuning text: its resource in WantTuning.package.
constexpr std::uint32_t kWantTuningGroup = 0xCDA53B6F, kWantTuningInstance = 0x2D7EE26B;

// The largest file of this name in a "Wants" folder of the install: each product ships a
// cumulative copy, so the largest is the newest.
std::optional<fs::path> newestWantsFile(const fs::path& installRoot, const std::string& name) {
    std::optional<fs::path> best;
    std::uintmax_t bestSize = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(installRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& path = it->path();
        if (!it->is_regular_file() || lower(path.filename().string()) != name ||
            lower(path.parent_path().filename().string()) != "wants") {
            continue;
        }
        const auto size = it->file_size(ec);
        if (!ec && size > bestSize) {
            bestSize = size;
            best = path;
        }
    }
    return best;
}

} // namespace

std::optional<fs::path> wantsPackage(const fs::path& installRoot) { return newestWantsFile(installRoot, "wants.package"); }

std::optional<fs::path> wantTuningPackage(const fs::path& installRoot) { return newestWantsFile(installRoot, "wanttuning.package"); }

std::map<std::uint32_t, WantCategory> parseWantCategories(std::string_view tuning) {
    // An attribute's value: the text between the quotes after `name="`.
    auto attribute = [](std::string_view entry, std::string_view name) -> std::optional<std::uint32_t> {
        const std::string key = std::string(name) + "=\"";
        const auto at = entry.find(key);
        if (at == std::string_view::npos) {
            return std::nullopt;
        }
        const auto start = at + key.size();
        const auto end = entry.find('"', start);
        if (end == std::string_view::npos || end - start > 16) {
            return std::nullopt;
        }
        try {
            return static_cast<std::uint32_t>(std::stoul(std::string(entry.substr(start, end - start)), nullptr, 0));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };
    std::map<std::uint32_t, WantCategory> out;
    for (auto at = tuning.find("<category "); at != std::string_view::npos; at = tuning.find("<category ", at + 1)) {
        const auto end = tuning.find('>', at);
        if (end == std::string_view::npos) {
            break;
        }
        const auto entry = tuning.substr(at, end - at);
        const auto id = attribute(entry, "id");
        if (!id) {
            continue;
        }
        out[*id] = {attribute(entry, "primaryIcon").value_or(0), attribute(entry, "secondaryIcon").value_or(0)};
    }
    return out;
}

std::string loadWantTuning(const fs::path& tuningPackage) {
    try {
        dbpf::Package package{tuningPackage};
        for (const auto& e : package.entries()) {
            if (e.key.type == 0 && e.key.group == kWantTuningGroup && e.key.instance == kWantTuningInstance) {
                const auto data = package.read(e);
                return std::string(data.begin(), data.end());
            }
        }
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("wants: the tuning file can't be read and is left out: ") + ex.what());
    }
    return {};
}

float TuningCurve::at(float x) const {
    if (points.empty()) {
        return 0;
    }
    // Finds the last point x is past. Before the first point the curve is the first point's y, and
    // past the last it is the last's.
    std::size_t i = points.size();
    while (i > 0 && !(x - points[i - 1].first > 0)) {
        --i;
    }
    if (i == 0) {
        return points.front().second;
    }
    if (i == points.size()) {
        return points.back().second;
    }
    const auto& [x0, y0] = points[i - 1];
    const auto& [x1, y1] = points[i];
    // The game keeps 1 / (x1 - x0) per span as a float and works in x87 precision.
    const double span = static_cast<float>(1.0f / (x1 - x0));
    return static_cast<float>((static_cast<double>(x) - x0) * span * (static_cast<double>(y1) - y0) + y0);
}

TuningCurve TuningCurve::inverse() const {
    TuningCurve out;
    for (const auto& [x, y] : points) {
        out.points.emplace_back(y, x);
    }
    return out;
}

std::optional<TuningCurve> parseInfluenceCurve(std::string_view tuning) {
    const auto entry = tuning.find("<influenceTuning");
    if (entry == std::string_view::npos) {
        return std::nullopt;
    }
    const auto open = tuning.find('"', entry);
    const auto close = open == std::string_view::npos ? open : tuning.find('"', open + 1);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view text = tuning.substr(open + 1, close - open - 1);
    constexpr std::size_t kMaxPoints = 64;
    // A number running up to `end`, all of it read.
    auto number = [](std::string_view s) -> std::optional<float> {
        // libc++ before macOS 26 declares floating-point from_chars unavailable.  Parsing with
        // the classic locale keeps the game's dot decimal separator independent of the Mac's locale.
        std::istringstream input{std::string(s)};
        input.imbue(std::locale::classic());
        float value = 0;
        input >> std::noskipws >> value;
        return input && input.eof() ? std::optional<float>(value) : std::nullopt;
    };
    TuningCurve curve;
    for (std::size_t at = text.find('('); at != std::string_view::npos; at = text.find('(', at + 1)) {
        const auto split = text.find(';', at), end = text.find(')', at);
        if (curve.points.size() >= kMaxPoints || split == std::string_view::npos || end == std::string_view::npos || split > end) {
            return std::nullopt;
        }
        const auto x = number(text.substr(at + 1, split - at - 1)), y = number(text.substr(split + 1, end - split - 1));
        if (!x || !y) {
            return std::nullopt;
        }
        curve.points.emplace_back(*x, *y);
    }
    if (curve.points.empty()) {
        return std::nullopt;
    }
    return curve;
}

int influenceMeterValue(const TuningCurve* influenceLevels, std::int16_t points) {
    const float x = static_cast<float>(points);
    double level = influenceLevels ? static_cast<double>(influenceLevels->at(x)) * 0.2f : static_cast<double>(x) * (1.0f / 75.0f);
    level = std::clamp(level, 0.0, 1.0);
    return static_cast<int>(level * 100.0f);
}

WantDefinitions loadWantDefinitions(const fs::path& wantsPackage) {
    dbpf::Package package{wantsPackage};
    WantDefinitions defs;
    for (const auto& e : package.entries()) {
        if (e.key.type != types::kWantDefinition) {
            continue;
        }
        try {
            const auto props = propset::parse(package.read(e));
            auto text = [&props](const char* key) {
                const auto it = props.find(key);
                return it != props.end() ? it->second : std::string();
            };
            WantDefinition d;
            d.guid = static_cast<std::uint32_t>(std::stoul(text("id"), nullptr, 0));
            d.name = text("nodeText");
            const auto argument = lookup(kArguments, text("objectType"));
            const auto op = lookup(kOperators, text("integerOperator"));
            if (!argument || !op) {
                defs.problems.push_back(d.name + ": unknown object type or operator");
            }
            d.argument = argument.value_or(WantArgument::None);
            d.op = op.value_or(WantOperator::None);
            if (const auto score = text("score"); !score.empty()) {
                d.score = std::stoi(score, nullptr, 0);
            }
            d.simArrayTree = text("simArrayTree");
            if (const auto influence = text("influence"); !influence.empty()) {
                d.influence = std::stoi(influence, nullptr, 0);
            }
            d.level = lookup(kLevels, text("level")).value_or(WantLevel::Transitory);
            if (const auto icon = text("primaryIcon"); !icon.empty()) {
                d.primaryIcon = static_cast<std::uint32_t>(std::stoul(icon, nullptr, 0));
            }
            if (const auto icon = text("secondaryIcon"); !icon.empty()) {
                d.secondaryIcon = static_cast<std::uint32_t>(std::stoul(icon, nullptr, 0));
            }
            if (const auto set = text("stringSet"); !set.empty()) {
                d.stringSet = static_cast<std::uint32_t>(std::stoul(set, nullptr, 0));
            }
            defs.byGuid[d.guid] = std::move(d);
        } catch (const std::exception& ex) {
            defs.problems.push_back("want definition " + std::to_string(e.key.instance) + ": " + ex.what());
        }
    }
    return defs;
}

} // namespace ts2::content
