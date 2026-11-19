#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace ts2::scene {
class Assets;
struct BabyCatalog;
struct MadeLook;
struct OutfitCatalog;
}

namespace ts2::sim {

class LotSimulation;

// A character file a save adds for a Sim made during play.
struct NewCharacterFile {
    std::int16_t neighbor = 0;
    std::filesystem::path path;  // in the neighborhood's Characters folder, not there yet
    std::vector<std::uint8_t> bytes;
};

// The character files a save of the running lot writes for the neighbors made since the
// neighborhood in `hoodFolder` loaded, one for each without one yet: a newborn's (see
// newbornCharacterPackage), and a Sim's Make New Character made from a template (see
// madeCharacterPackage), whose look `madeLook` gives by neighbor id. Each is named after the first
// free file number, made from its template as installed, and gets the installed packs record the
// neighborhood's other files hold. Its face material names count on from its GUID, as the game
// counts both with time, past the names the other files have. Every file's scenegraph names carry
// a group number of its own.
//
// All or nothing: when any file can't be made, there are none, and `problem` says why. A made Sim
// whose look or names in every language aren't known can't be made.
struct NewCharacterFiles {
    std::vector<NewCharacterFile> files;
    // Every neighbor made this run that has a character file once the save is written: those in
    // `files` and those an earlier save wrote (LotSimulation::addCharacterFile).
    std::set<std::int16_t> neighbors;
    std::string problem;
};
using MadeLookOf = std::function<const scene::MadeLook*(std::int16_t)>;
NewCharacterFiles newCharacterFiles(LotSimulation& simulation, scene::Assets& assets, const scene::BabyCatalog& catalog,
                                    const std::filesystem::path& hoodFolder, const scene::OutfitCatalog* outfits = nullptr,
                                    const MadeLookOf& madeLook = {});

} // namespace ts2::sim
