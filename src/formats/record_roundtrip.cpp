#include "formats/record_roundtrip.hpp"

#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/ngbh.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sdsc.hpp"
#include "formats/srel.hpp"
#include "formats/str.hpp"
#include "formats/swaf.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace ts2::roundtrip {

namespace {

constexpr std::size_t kMostProblems = 20;
constexpr std::uint32_t kMaterial = 0x49596978;  // TXMT

std::string key(const dbpf::IndexEntry& e) {
    char text[64];
    std::snprintf(text, sizeof text, "%08X %08X %08X", e.key.type, e.key.group, e.key.instance);
    return text;
}

} // namespace

const std::map<std::uint32_t, std::string>& kinds() {
    static const std::map<std::uint32_t, std::string> names = {
        {types::kFamily, "FAMI"},         {types::kSimDescription, "SDSC"}, {types::kStringTable, "STR#"},
        {types::kSimRelationship, "SREL"}, {types::kFamilyTies, "FAMT"},     {types::kWantsAndFears, "SWAF"},
        {types::kNeighborhoodRecord, "NGBH"}, {types::kSimDna, "SDNA"}, {kMaterial, "TXMT"},
    };
    return names;
}

std::optional<std::vector<std::uint8_t>> rewrite(std::uint32_t type, std::span<const std::uint8_t> data) {
    switch (type) {
    case types::kFamily: return fami::encode(fami::parse(data));
    case types::kSimDescription: return sdsc::encode(sdsc::parse(data));
    case types::kStringTable: return str::encode(str::parse(data));
    case types::kSimRelationship: return srel::encode(srel::parse(data));
    case types::kFamilyTies: return famt::encode(famt::parse(data));
    case types::kWantsAndFears: return swaf::encode(swaf::parse(data));
    case types::kNeighborhoodRecord: return ngbh::encode(ngbh::readRaw(data));
    case types::kSimDna: return propset::encodeText(propset::readText(data));
    case kMaterial: return scenegraph::encodeMaterial(scenegraph::parseMaterial(data));
    default: return std::nullopt;
    }
}

bool Report::allSame() const {
    return std::all_of(kinds.begin(), kinds.end(), [](const auto& k) { return k.second.different == 0 && k.second.unreadable == 0; });
}

Report check(dbpf::Package& package) {
    Report report;
    for (const auto& entry : package.entries()) {
        const auto kind = kinds().find(entry.key.type);
        if (kind == kinds().end()) {
            continue;
        }
        Counts& counts = report.kinds[kind->second];
        try {
            const auto original = package.read(entry);
            const auto again = rewrite(entry.key.type, original);
            if (again && *again == original) {
                ++counts.same;
                continue;
            }
            ++counts.different;
            if (report.problems.size() < kMostProblems && again) {
                // Where they part.
                const auto [a, b] = std::mismatch(original.begin(), original.end(), again->begin(), again->end());
                const auto at = static_cast<std::size_t>(a - original.begin());
                report.problems.push_back(key(entry) + ": " + std::to_string(original.size()) + " bytes written back as " +
                                          std::to_string(again->size()) + ", first different at " + std::to_string(at));
            }
        } catch (const std::exception& ex) {
            ++counts.unreadable;
            if (report.problems.size() < kMostProblems) {
                report.problems.push_back(key(entry) + ": " + ex.what());
            }
        }
    }
    return report;
}

} // namespace ts2::roundtrip
