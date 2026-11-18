#include "sim/recolour_rows.hpp"

#include <array>
#include <cctype>

namespace ts2::sim {

namespace {

constexpr std::uint16_t kMaterialNames = 0x88;
// A broken table can't make the object's names grow without end.
constexpr std::size_t kMaxRows = 1024;

bool endsWithIgnoringCase(const std::string& s, const std::string& end) {
    if (s.size() < end.size()) {
        return false;
    }
    for (std::size_t i = 0; i < end.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(s[s.size() - end.size() + i])) != std::tolower(static_cast<unsigned char>(end[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<std::size_t> recolourRow(std::span<const std::string> rows, const std::string& model, const std::string& subset,
                                       const std::string& material) {
    static constexpr std::array<const char*, 4> kStates{"_dirty", "_clean", "_lit", "_unlit"};
    std::string state;
    for (const char* s : kStates) {
        if (material.find(s) != std::string::npos) {
            state = s;
            break;
        }
    }
    const std::string bare = endsWithIgnoringCase(model, "_cres") ? model.substr(0, model.size() - 5) : model;
    const std::string part = "_" + subset + "_";
    std::optional<std::size_t> row;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::string& r = rows[i];
        if (r == model) {
            return i;
        }
        if (r.find(bare) != std::string::npos && r.find(part) != std::string::npos && (state.empty() || r.find(state) != std::string::npos)) {
            row = i;
        }
    }
    return row;
}

void applyChosenRecolours(const content::Lot& lot, CodeSource& code, World& world, const content::RecolourCatalog& catalog) {
    for (const auto& saved : lot.objects) {
        const auto& tail = saved.state.tail;
        if (!tail || tail->materials.empty() || saved.id == 0 || saved.id > 0x7FFF) {
            continue;
        }
        SimObject* object = world.find(static_cast<std::int16_t>(saved.id));
        if (object == nullptr) {
            continue;
        }
        std::vector<std::string> rows;
        for (std::size_t i = 0; i < kMaxRows; ++i) {
            auto row = code.string(object->group, kMaterialNames, i);
            if (!row) {
                break;
            }
            rows.push_back(std::move(*row));
        }
        for (const auto& chosen : tail->materials) {
            const content::RecolourMaterial* picked = catalog.find(chosen.resource, chosen.subset, chosen.material);
            if (picked == nullptr) {
                continue;
            }
            for (const content::RecolourMaterial* m : catalog.family(picked->model, picked->family)) {
                const auto row = recolourRow(rows, m->model, m->subset, m->name);
                if (!row) {
                    continue;
                }
                if (!object->savedMaterialNames) {
                    object->savedMaterialNames = object->materialNames;
                }
                // Change Material reads row r of the names at r + 1.
                if (object->materialNames.size() < *row + 2) {
                    object->materialNames.resize(*row + 2);
                }
                object->materialNames[*row + 1] = m->name;
            }
        }
    }
}

} // namespace ts2::sim
