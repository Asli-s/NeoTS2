// Saving a lot, first stage. Yes on the quit box's save question (quitting, or leaving the lot)
// writes what Build mode changed that can be saved so far into the lot's package: its floors, its
// wall coverings (when its walls weren't changed), its terrain paint, and the money those took, in the
// lot's simulator state and the household's family record in the neighborhood package. Walls, doors,
// windows and objects changed, and what happened in Live mode, aren't saved yet: the log says so.
// Each package is written again whole and checked before it replaces the old one, the first time
// keeping the file as the game last wrote it beside it (".neots2bak").
#include "game.hpp"

#include "content/lot_save.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"

#include <format>

namespace app {

bool Game::saveLot() {
    if (!inLot_ || !lotLife_ || !buildLot_ || lotFile_.empty()) {
        return false;
    }
    std::vector<std::string> unsaved;
    if (edits_.walls) {
        unsaved.emplace_back("walls");
        if (edits_.coverings) {
            unsaved.emplace_back("wall coverings (as the walls changed)");
        }
    }
    if (edits_.objects) {
        unsaved.emplace_back("objects, doors and windows");
    }
    if (lotRan_) {
        unsaved.emplace_back("what happened in Live mode");
    }
    for (const auto& what : unsaved) {
        ts2::diag::warning(std::format("saving lot {}: {} can't be saved yet and are left as they were", enteredLot_, what));
    }

    ts2::content::LotSaveParts parts;
    parts.floors = edits_.floors;
    parts.wallCoverings = edits_.coverings && !edits_.walls;
    parts.terrainPaint = edits_.paint;
    parts.fundsChange = (parts.floors ? edits_.floorFunds : 0) + (parts.wallCoverings ? edits_.coveringFunds : 0);
    if (!parts.floors && !parts.wallCoverings && !parts.terrainPaint) {
        ts2::diag::step(std::format("saving lot {}: nothing that can be saved yet was changed", enteredLot_));
        return true;
    }
    try {
        ts2::content::LotChanges changes;
        {
            ts2::dbpf::Package original(lotFile_);
            changes = ts2::content::lotChanges(original, *buildLot_, parts);
        }
        for (const auto& problem : changes.problems) {
            ts2::diag::warning(std::format("saving lot {}: {}", enteredLot_, problem));
        }
        if (!changes.resources.empty()) {
            const auto report = ts2::content::rewritePackage(lotFile_, changes.resources);
            ts2::diag::step(std::format("saved lot {}: {} resources written, {} added, {} kept{}", enteredLot_, report.replaced, report.added,
                                        report.copied, report.madeBackup ? ", the old package kept as " + report.backup.filename().string() : ""));
        } else {
            ts2::diag::step(std::format("saving lot {}: it was already saved so", enteredLot_));
        }
        if (changes.funds) {
            const auto hoodFile = ts2::content::neighborhoodPackage(ts2::content::neighborhoodOfLot(lotFile_));
            std::vector<ts2::content::ReplacedResource> family;
            {
                ts2::dbpf::Package hood(hoodFile);
                family = ts2::content::familyFundsChanges(hood, static_cast<std::int32_t>(enteredLot_), *changes.funds);
            }
            if (!family.empty()) {
                ts2::content::rewritePackage(hoodFile, family);
            }
            ts2::diag::step(std::format("saved lot {}: the household has {} (family records written: {})", enteredLot_, *changes.funds, family.size()));
        }
        // Saved: what was saved now counts from here. Walls and objects stay unsaved.
        edits_.floors = edits_.paint = false;
        edits_.floorFunds = 0;
        if (parts.wallCoverings) {
            edits_.coverings = false;
            edits_.coveringFunds = 0;
        }
        return changes.problems.empty();
    } catch (const std::exception& ex) {
        ts2::diag::error(std::format("saving lot {} failed: {}", enteredLot_, ex.what()));
        return false;
    }
}

} // namespace app
