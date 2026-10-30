#include "sim/genetics.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>

namespace {

// One parent's DNA properties from the input: lines of "key=value" up to a zero byte.
std::map<std::string, std::string> properties(std::string_view& rest) {
    std::map<std::string, std::string> out;
    const std::size_t end = std::min(rest.find('\0'), rest.size());
    std::string_view part = rest.substr(0, end);
    rest = rest.substr(std::min(end + 1, rest.size()));
    while (!part.empty()) {
        const std::size_t line = std::min(part.find('\n'), part.size());
        const std::string_view entry = part.substr(0, line);
        part = part.substr(std::min(line + 1, part.size()));
        const std::size_t equals = entry.find('=');
        if (equals != std::string_view::npos) {
            out[std::string(entry.substr(0, equals))] = std::string(entry.substr(equals + 1));
        }
    }
    return out;
}

} // namespace

// Reads two parents' DNA properties and makes a baby's genes from them. The catalog's colours have
// short names so inputs find them, and finite values only, as the catalog reader keeps. The skins
// include an unblended one (0). The skin a baby shows is always one of the catalog's.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    std::string_view rest(reinterpret_cast<const char*>(data), size);
    const auto a = ts2::sim::genomeFromProperties(properties(rest));
    const auto b = ts2::sim::genomeFromProperties(properties(rest));
    ts2::content::GeneticsCatalog catalog;
    catalog.skins = {{"s1", 0.1f}, {"s2", 0.3f}, {"s3", 0.6f}, {"s4", 0.9f}, {"s0", 0}};
    catalog.hairs = {{"h1", 1}, {"h2", 2}, {"h0", 0}};
    catalog.eyes = {{"e1", 1}, {"e2", 2}};
    ts2::sim::SimRandom random(static_cast<std::uint32_t>(size));
    ts2::sim::FacePicks picks;
    const auto child = ts2::sim::childGenome(a, b, catalog, random, &picks);
    const auto skin = child.find(ts2::sim::kSkinGene);
    if (skin != child.end() && ts2::content::findColour(catalog.skins, skin->second.dominant) == nullptr) {
        std::abort();
    }
    return 0;
}
