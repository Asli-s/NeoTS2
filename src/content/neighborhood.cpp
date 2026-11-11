#include "content/neighborhood.hpp"

#include "diag/log.hpp"
#include "formats/dbpf.hpp"
#include "formats/famt.hpp"
#include "formats/hash.hpp"
#include "formats/lot_description.hpp"
#include "formats/neighborhood_id.hpp"
#include "formats/propset.hpp"
#include "formats/resource_types.hpp"
#include "formats/sdsc.hpp"
#include "formats/srel.hpp"
#include "formats/str.hpp"
#include "formats/swaf.hpp"

#include <exception>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string>
#include <system_error>

namespace ts2::content {

namespace fs = std::filesystem;

fs::path neighborhoodOfLot(const fs::path& lotPackage) {
    return lotPackage.parent_path().parent_path();
}

std::optional<std::int32_t> lotNumber(const fs::path& lotPackage) {
    const std::string stem = diag::utf8(lotPackage.stem());
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
    std::error_code ec;
    for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        const auto& entry = *it;
        if (!entry.is_regular_file(ec) || entry.path().extension() != ".package") {
            continue;
        }
        // Finder writes AppleDouble metadata beside files copied to some volumes. It uses `._`
        // names and the AppleDouble magic, but a TS2 package with only one of those is kept. The
        // name is checked as stored, since converting it to text throws on Windows when it holds
        // characters the system's code page lacks.
        const fs::path file = entry.path().filename();
        const auto& name = file.native();
        if (name.size() >= 2 && name[0] == '.' && name[1] == '_') {
            std::array<char, 4> magic{};
            std::ifstream input(entry.path(), std::ios::binary);
            input.read(magic.data(), magic.size());
            if (magic == std::array<char, 4>{0x00, 0x05, 0x16, 0x07}) {
                diag::warnOnce("characters: AppleDouble metadata is left out: " + diag::utf8(entry.path().filename()));
                continue;
            }
        }
        files.push_back(entry.path());
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
    // Joined as a path, since converting the folder name to text throws on Windows when it holds
    // characters the system's code page lacks.
    fs::path file = neighborhood.filename();
    file += "_Neighborhood.package";
    return neighborhood / file;
}

// Reads a lot description into the neighborhood's lot tables. False when it can't be read.
bool addLot(Neighborhood& hood, dbpf::Package& package, const dbpf::IndexEntry& e) {
    constexpr std::uint32_t kBeachLot = 0x80;
    try {
        const auto description = nhood::parseLotDescription(package.read(e));
        const auto id = static_cast<std::int32_t>(e.key.instance);
        hood.lotTypes[id] = description.type;
        hood.lotOwners[id] = description.owner;
        hood.lotShapes[id] = {description.width, description.depth,
                              (description.flags & kBeachLot) != 0 ? std::optional<std::uint8_t>(description.ownFacing) : std::nullopt};
        return true;
    } catch (const std::exception& ex) {
        hood.problems.push_back("lot " + std::to_string(e.key.instance) + ": " + ex.what());
        return false;
    }
}

// Adds the lots each sub-neighborhood package of the folder places (<folder>_Suburb001.package and
// the like), each tied to the neighborhood that package names.
void addSubNeighborhoodLots(Neighborhood& hood, const fs::path& mainPackage) {
    const fs::path folder = mainPackage.parent_path();
    const std::string prefix = diag::utf8(folder.filename()) + "_";
    std::error_code ec;
    std::vector<fs::path> packages;
    for (fs::directory_iterator it(folder, ec), end; it != end; it.increment(ec)) {
        const std::string stem = diag::utf8(it->path().stem());
        if (it->path().extension() == ".package" && stem.rfind(prefix, 0) == 0 && it->path() != mainPackage) {
            packages.push_back(it->path());
        }
    }
    std::sort(packages.begin(), packages.end());
    for (const auto& path : packages) {
        try {
            dbpf::Package package{path};
            std::vector<std::int32_t> lots;
            std::optional<std::int32_t> id;
            for (const auto& e : package.entries()) {
                if (e.key.type == nhood::kLotDescriptionType && addLot(hood, package, e)) {
                    lots.push_back(static_cast<std::int32_t>(e.key.instance));
                } else if (e.key.type == nhoodid::kType) {
                    id = nhoodid::parse(package.read(e)).id;
                }
            }
            for (const std::int32_t lot : lots) {
                if (id) {
                    hood.lotNeighborhoods[lot] = *id;
                }
            }
        } catch (const std::exception& ex) {
            hood.problems.push_back(diag::utf8(path.filename()) + ": " + ex.what());
        }
    }
}

Neighborhood loadNeighborhood(const fs::path& neighborhoodPackage) {
    dbpf::Package package{neighborhoodPackage};
    Neighborhood hood;
    const fs::path lots = neighborhoodPackage.parent_path() / "Lots";
    const std::string prefix = diag::utf8(neighborhoodPackage.parent_path().filename()) + "_Lot";
    std::error_code ec;
    for (fs::directory_iterator it(lots, ec), end; it != end; it.increment(ec)) {
        const std::string stem = diag::utf8(it->path().stem());
        if (it->path().extension() == ".package" && stem.rfind(prefix, 0) == 0) {
            if (const auto n = lotNumber(it->path())) {
                hood.lotIds.insert(*n);
            }
        }
    }
    std::vector<std::int32_t> mainLots;
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
        } else if (e.key.type == types::kFamilyTies) {
            try {
                for (auto& s : famt::parse(package.read(e)).sims) {
                    hood.familyTies[s.sim] = std::move(s.ties);
                }
            } catch (const std::exception& ex) {
                hood.problems.push_back(std::string("family ties: ") + ex.what());
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
        } else if (e.key.type == nhood::kLotDescriptionType) {
            if (addLot(hood, package, e)) {
                mainLots.push_back(static_cast<std::int32_t>(e.key.instance));
            }
        } else if (e.key.type == nhoodid::kType) {
            try {
                hood.identity = nhoodid::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back(std::string("neighborhood id: ") + ex.what());
            }
        } else if (e.key.type == weather::kType) {
            try {
                hood.weather = weather::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back(std::string("weather: ") + ex.what());
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
        } else if (e.key.type == types::kSimDna) {
            const auto id = static_cast<std::int16_t>(e.key.instance);
            try {
                hood.dna[id] = propset::parse(package.read(e));
            } catch (const std::exception& ex) {
                hood.problems.push_back("DNA of " + std::to_string(id) + ": " + ex.what());
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
    if (hood.identity) {
        for (const std::int32_t lot : mainLots) {
            hood.lotNeighborhoods[lot] = hood.identity->id;
        }
    }
    addSubNeighborhoodLots(hood, neighborhoodPackage);
    return hood;
}

std::vector<NeighborhoodEntry> listNeighborhoods(const fs::path& neighborhoods) {
    std::vector<NeighborhoodEntry> out;
    if (!fs::is_directory(neighborhoods)) {
        return out;
    }
    std::error_code ec;
    for (fs::directory_iterator it(neighborhoods, ec), end; it != end; it.increment(ec)) {
        const auto& item = *it;
        const fs::path package = neighborhoodPackage(item.path());
        if (!item.is_directory(ec) || item.path().filename() == "Tutorial" || !fs::exists(package, ec)) {
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
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("neighborhoods: a neighborhood is listed without its name: ") + ex.what());
        }
        out.push_back(std::move(n));
    }
    std::sort(out.begin(), out.end(), [](const NeighborhoodEntry& a, const NeighborhoodEntry& b) { return a.folder.filename() < b.folder.filename(); });
    return out;
}

std::map<std::int32_t, nhoodid::NeighborhoodId> neighborhoodIds(const fs::path& neighborhoods) {
    // A limit on packages opened, so a cluttered folder can't stall a lot's loading.
    constexpr std::size_t kMaxPackages = 1024;
    std::map<std::int32_t, nhoodid::NeighborhoodId> out;
    std::error_code ec;
    std::size_t opened = 0;
    try {
        for (const auto& folder : fs::directory_iterator(neighborhoods, ec)) {
            if (!folder.is_directory(ec)) {
                continue;
            }
            for (const auto& item : fs::directory_iterator(folder.path(), ec)) {
                if (item.path().extension() != ".package" || !item.is_regular_file(ec)) {
                    continue;
                }
                if (++opened > kMaxPackages) {
                    return out;
                }
                try {
                    dbpf::Package p{item.path()};
                    for (const auto& e : p.entries()) {
                        if (e.key.type == nhoodid::kType) {
                            auto record = nhoodid::parse(p.read(e));
                            out.try_emplace(record.id, std::move(record));
                            break;
                        }
                    }
                } catch (const std::exception& ex) {
                    diag::warnOnce(std::string("neighborhoods: a broken package is left out of the list: ") + ex.what());
                }
            }
        }
    } catch (const std::exception&) {  // a folder that can't be listed: what was found so far
    }
    return out;
}

} // namespace ts2::content
