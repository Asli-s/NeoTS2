#include "content/wants.hpp"

#include "formats/dbpf.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
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

std::optional<fs::path> wantsPackage(const fs::path& installRoot) {
    std::optional<fs::path> best;
    std::uintmax_t bestSize = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(installRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& path = it->path();
        if (!it->is_regular_file() || lower(path.filename().string()) != "wants.package" ||
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
            defs.byGuid[d.guid] = std::move(d);
        } catch (const std::exception& ex) {
            defs.problems.push_back("want definition " + std::to_string(e.key.instance) + ": " + ex.what());
        }
    }
    return defs;
}

} // namespace ts2::content
