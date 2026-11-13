#pragma once

#include "paths/save_folders.hpp"

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace ts2::content {

// A save that could not be written. The save folder is as it was before the save began.
class SaveError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What a save does to its target file: replace the package there, or make a new one.
enum class SaveTarget { Replace, Create };

// Writes the packages of one save into a save folder, so that either every one of them is replaced
// or none is. It never writes outside the folder's Neighborhoods, only to .package files, and only
// while NeoTS2 holds the folder's lock.
//
// Each new package is checked before it is written, written beside its target, flushed to the disk
// and read back. Only when all of them are there does a journal beside the save folder list the set,
// and the targets are swapped in: the old package moved aside, the new one moved into its place. The
// journal goes once every target holds its new package, and only then are the old ones deleted. A
// save that stops part way, in a crash or a power cut, is undone by recoverInterruptedSave.
class SaveWriter {
public:
    explicit SaveWriter(const paths::SaveFolderLock& lock);

    // Adds a package to the save. `bytes` must be a whole package as dbpf::writeImage makes it.
    // Throws SaveError when the target can't take part: outside the Neighborhoods folder, not a
    // .package file, missing for Replace or present for Create, already in this save, or when the
    // package fails its check.
    void add(const std::filesystem::path& target, std::vector<std::uint8_t> bytes, SaveTarget kind);

    // Writes the save. Throws SaveError when it can't, with every target as it was before.
    void commit();

    // Makes commit fail at its `count`-th file move, as a crash there would, to test the undo.
    void failAtMoveForTests(int count) { failAtMove_ = count; }

private:
    struct Item {
        std::filesystem::path target;
        std::vector<std::uint8_t> bytes;
        SaveTarget kind = SaveTarget::Replace;
    };

    void move(const std::filesystem::path& from, const std::filesystem::path& to);
    void undo(std::size_t count);

    const paths::SaveFolderLock& lock_;
    std::filesystem::path neighborhoods_;
    std::vector<Item> items_;
    int moves_ = 0;
    int failAtMove_ = -1;
};

struct SaveRecovery {
    int restored = 0;  // packages put back as they were before a save that stopped
    int cleaned = 0;   // leftover work files removed
    // Why the folder could not be put right, for the player. While it is set, no save is written.
    std::string problem;
};

// Puts a save folder right after a save that stopped part way. With a journal left, the save never
// finished: every listed target gets its old package back. Without one, it finished and only work
// files are left, which go. Called by SaveWriter::commit before it writes, and should be called before a
// save folder is read.
SaveRecovery recoverInterruptedSave(const paths::SaveFolderLock& lock);

// The work files of a save, beside its target and beside the save folder.
std::filesystem::path newPackagePath(const std::filesystem::path& target);
std::filesystem::path oldPackagePath(const std::filesystem::path& target);
std::filesystem::path saveJournalPath(const std::filesystem::path& saveFolder);

} // namespace ts2::content
