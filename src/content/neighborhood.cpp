#include "content/neighborhood.hpp"

#include "formats/dbpf.hpp"
#include "formats/hash.hpp"
#include "formats/resource_types.hpp"
#include "formats/sdsc.hpp"
#include "formats/srel.hpp"
#include "formats/str.hpp"
#include "formats/swaf.hpp"

#include <exception>

#include <algorithm>
#include <cctype>
#include <string>
#include <system_error>

namespace ts2::content {

namespace fs = std::filesystem;

fs::path neighborhoodOfLot(const fs::path& lotPackage) {
    return lotPackage.parent_path().parent_path();
}

std::optional<std::int32_t> lotNumber(const fs::path& lotPackage) {
    const std::string stem = lotPackage.stem().string();
    const auto at = stem.rfind("_Lot");
    if (at == std::string::npos || at + 4 >= stem.size() || stem.size() - at - 4 > 9) {
        return std::nullopt;
    }
    std::int32_t n = 0;
    for (std::size_t i = at + 4; i < stem.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(stem[i]))) {
            return std::nullopt;
        }
        n = n * 10 + (stem[i] - '0');
    }
    return n;
}

std::vector<fs::path> characterFiles(const fs::path& neighborhood) {
    std::vector<fs::path> files;
    const fs::path folder = neighborhood / "Characters";
    if (!fs::is_directory(folder)) {
        return files;
    }
    for (const auto& entry : fs::directory_iterator(folder)) {
        if (entry.is_regular_file() && entry.path().extension() == ".package") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::optional<std::uint32_t> portraitImage(std::int16_t lifeStage) {
    // Each portrait's instance is its life stage's flag.
    switch (lifeStage) {
    case 1: return 0x20;  // baby
    case 2: return 0x01;  // toddler
    case 3: return 0x02;  // child
    case 16: return 0x04; // teen
    case 19: return 0x08; // adult
    case 51: return 0x10; // elder
    default: return std::nullopt;
    }
}

std::uint32_t localGroupFor(const fs::path& package) {
    std::string name = package.stem().string();
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return hash::groupId(name);
}

std::size_t addCharacters(ResourceIndex& index, const fs::path& neighborhood) {
    const auto files = characterFiles(neighborhood);
    for (const auto& file : files) {
        index.addPackage(file, localGroupFor(file));
    }
    return files.size();
}

fs::path neighborhoodPackage(const fs::path& neighborhood) {
    return neighborhood / (neighborhood.filename().string() + "_Neighborhood.package");
}

Neighborhood loadNeighborhood(const fs::path& neighborhoodPackage) {
    dbpf::Package package{neighborhoodPackage};
    Neighborhood hood;
    const fs::path lots = neighborhoodPackage.parent_path() / "Lots";
    const std::string prefix = neighborhoodPackage.parent_path().filename().string() + "_Lot";
    std::error_code ec;
    for (fs::directory_iterator it(lots, ec), end; it != end; it.increment(ec)) {
        const std::string stem = it->path().stem().string();
        if (it->path().extension() == ".package" && stem.rfind(prefix, 0) == 0 && stem.size() > prefix.size()) {
            try {
                hood.lotIds.insert(std::stoi(stem.substr(prefix.size())));
            } catch (const std::exception&) {
            }
        }
    }
    for (const auto& e : package.entries()) {
        if (e.key.type == types::kSimDescription) {
            const auto id = static_cast<std::int16_t>(e.key.instance);
            hood.neighborIds.insert(id);
            try {
                auto s = sdsc::parse(package.read(e));
                hood.neighbors[id] = Neighbor{s.guid, std::move(s.personData)};
            } catch (const std::exception& ex) {
                hood.problems.push_back("neighbor " + std::to_string(id) + ": " + ex.what());
            }
        } else if (e.key.type == types::kSimRelationship) {
            const auto from = static_cast<std::int16_t>(e.key.instance >> 16);
            const auto to = static_cast<std::int16_t>(e.key.instance & 0xFFFF);
            try {
                auto rel = srel::parse(package.read(e));
                hood.relationshipsWithExtraBytes += rel.extraBytes != 0 ? 1 : 0;
                hood.relationships[{from, to}] = std::move(rel.values);
            } catch (const std::exception& ex) {
                hood.problems.push_back("relationship " + std::to_string(from) + " -> " + std::to_string(to) +
                                        ": " + ex.what());
            }
        } else if (e.key.type == types::kDataTables) {
            const auto id = static_cast<std::int16_t>(e.key.instance);
            try {
                hood.dataTables[id] = datatables::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back("data tables of " + std::to_string(id) + ": " + ex.what());
            }
        } else if (e.key.type == types::kFamily) {
            try {
                hood.families[e.key.instance] = fami::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back("family " + std::to_string(e.key.instance) + ": " + ex.what());
            }
        } else if (e.key.type == types::kNeighborhoodRecord) {
            try {
                hood.record = ngbh::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back(std::string("neighborhood record: ") + ex.what());
            }
        } else if (e.key.type == types::kWantsAndFears) {
            const auto id = static_cast<std::int16_t>(e.key.instance);
            try {
                auto w = swaf::parse(package.read(e));
                hood.wantsWithExtraBytes += w.extraBytes != 0 ? 1 : 0;
                hood.wants[id] = std::move(w);
            } catch (const std::exception& ex) {
                hood.problems.push_back("wants of " + std::to_string(id) + ": " + ex.what());
            }
        }
    }
    return hood;
}

std::vector<NeighborhoodEntry> listNeighborhoods(const fs::path& neighborhoods) {
    std::vector<NeighborhoodEntry> out;
    if (!fs::is_directory(neighborhoods)) {
        return out;
    }
    for (const auto& item : fs::directory_iterator(neighborhoods)) {
        const fs::path package = neighborhoodPackage(item.path());
        if (!item.is_directory() || item.path().filename() == "Tutorial" || !fs::exists(package)) {
            continue;
        }
        NeighborhoodEntry n;
        n.folder = item.path();
        n.thumbnail = item.path() / (item.path().filename().u8string() + u8"_Neighborhood.png");
        try {
            dbpf::Package p{package};
            for (const auto& e : p.entries()) {
                if (e.key.type == 0x43545353) {  // catalog string set: the name first
                    const auto table = str::parse(p.read(e));
                    const auto names = table.forLanguage(1);
                    if (!names.empty()) {
                        n.name = names.front()->value;
                    }
                    if (names.size() > 1) {
                        n.description = names[1]->value;
                    }
                    break;
                }
            }
        } catch (const std::exception&) {
        }
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(), [](const NeighborhoodEntry& a, const NeighborhoodEntry& b) { return a.folder.filename() < b.folder.filename(); });
    return out;
}

} // namespace ts2::content
