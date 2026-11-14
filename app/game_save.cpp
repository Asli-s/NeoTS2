// Saving a lot. Yes on the quit box's save question (quitting, or leaving the lot) writes what Build
// mode changed that can be saved so far into the lot's package: its floors, its terrain paint, its
// walls built and taken down and their coverings, and the money those took, in the lot's simulator
// state and the household's family record in the neighborhood package. Doors, windows and objects
// changed, and what happened in Live mode, aren't saved yet: the log says so. Each package is
// written again whole and checked before it replaces the old one, the first time keeping the file as
// the game last wrote it beside it (".neots2bak").
#include "game.hpp"

#include "content/lot_save.hpp"
#include "content/neighborhood.hpp"
#include "diag/log.hpp"
#include "sim/wall_save.hpp"

#include <format>

namespace app {

bool Game::saveLot() {
    if (!inLot_ || !lotLife_ || !buildLot_ || lotFile_.empty()) {
        return false;
    }
    if (edits_.objects) {
        ts2::diag::warning(std::format("saving lot {}: objects, doors and windows can't be saved yet and are left as they were", enteredLot_));
    }
    if (lotRan_) {
        ts2::diag::warning(std::format("saving lot {}: what happened in Live mode can't be saved yet and is left as it was", enteredLot_));
    }
    const bool walls = edits_.walls || edits_.coverings;
    if (!edits_.floors && !edits_.paint && !walls) {
        ts2::diag::step(std::format("saving lot {}: nothing that can be saved yet was changed", enteredLot_));
        return true;
    }
    try {
        ts2::content::LotSaveParts parts;
        parts.floors = edits_.floors;
        parts.terrainPaint = edits_.paint;
        ts2::content::LotChanges changes;
        bool wallsSaved = false;
        {
            ts2::dbpf::Package original(lotFile_);
            if (walls) {
                auto wallChanges = ts2::sim::wallChanges(original, *buildLot_);
                for (const auto& problem : wallChanges.problems) {
                    ts2::diag::warning(std::format("saving lot {}: {}; the walls and their coverings are left as they were", enteredLot_, problem));
                }
                wallsSaved = wallChanges.problems.empty();
                if (wallsSaved) {
                    ts2::diag::info(std::format("saving lot {}: {} walls built, {} taken down", enteredLot_, wallChanges.built, wallChanges.removed));
                }
                changes.resources = std::move(wallChanges.resources);
            }
            parts.fundsChange = (parts.floors ? edits_.floorFunds : 0) + (wallsSaved ? edits_.coveringFunds + edits_.wallFunds : 0);
            auto rest = ts2::content::lotChanges(original, *buildLot_, parts);
            changes.resources.insert(changes.resources.end(), std::make_move_iterator(rest.resources.begin()), std::make_move_iterator(rest.resources.end()));
            changes.problems = std::move(rest.problems);
            changes.funds = rest.funds;
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
        // Saved: what was saved now counts from here. Objects stay unsaved.
        edits_.floors = edits_.paint = false;
        edits_.floorFunds = 0;
        if (wallsSaved) {
            edits_.walls = edits_.coverings = false;
            edits_.coveringFunds = edits_.wallFunds = 0;
        }
        return changes.problems.empty() && (!walls || wallsSaved);
    } catch (const std::exception& ex) {
        ts2::diag::error(std::format("saving lot {} failed: {}", enteredLot_, ex.what()));
        return false;
    }
}

} // namespace app
