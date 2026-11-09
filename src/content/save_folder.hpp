#pragma once

#include "paths/save_folders.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ts2::content {

struct SaveFolderFill {
    bool filled = false;
    int files = 0;
    std::uintmax_t bytes = 0;
    // Why the folder wasn't filled when it needed to be, for the player. Empty when it was filled
    // or already had content.
    std::string problem;
};

// Fills a new save folder as the game does on its first start: each pack's UserData folders copied
// (the newest pack's copy of a shared file kept), Thumbnails packages merged and shipped lots imported
// into the lot catalog. Only a missing or empty folder is filled, never an original game's
// (paths::isOriginalGamesFolder) or one paths::saveFolderRefusal refuses, and links are never followed.
// The copy is built beside the folder and moved in at once, so a fill that stops changes nothing.
SaveFolderFill fillNewSaveFolder(const std::filesystem::path& installRoot, const paths::SaveFolderLock& lock);

// Adds the install's shipped neighborhoods to a save folder that holds files but no neighborhoods
// (see paths::lacksNeighborhoods), when the player asked for it. They are copied as the first start
// copies them, the newest pack's copy of a shared file kept. The copy is made beside the save folder
// and moved into it as Neighborhoods in one step, so a copy that stops leaves the folder as it was.
// Nothing else in the folder changes, and a folder that has neighborhoods by then is left alone.
SaveFolderFill addShippedNeighborhoods(const std::filesystem::path& installRoot, const paths::SaveFolderLock& lock);

} // namespace ts2::content
