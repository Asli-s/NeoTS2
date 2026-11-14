#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::content {

// A story album (a neighborhood's story, or a family's album) as the game keeps it in the user
// folder's Storytelling folder: webentry_<album id>.xml lists its entries, each a snapshot
// (snapshot_<album id>_<snapshot id>.jpg there) and its caption. A neighborhood's album id is the
// neighborhood's own id (1 for N001's premade story); a family's is a number of its own.
struct StoryEntry {
    std::string snapshot;  // the snapshot's file name in the Storytelling folder
    std::string text;      // the caption the player wrote, empty when there is none
    // The caption of a premade story: string (value & 0xFFFF), counted from 1, of string table
    // (value >> 16) in the text package 0x7FA7DC7C (UI text). 0 when the entry has none.
    std::uint32_t textResource = 0;
};

struct StoryAlbum {
    std::string neighborhood;  // the neighborhood's name as the album was saved with it
    std::uint32_t familyId = 0;  // 0 for a neighborhood's story
    std::string style;  // "Album" or "Blog"
    std::vector<StoryEntry> entries;
};

// The text package (group) a premade entry's caption is in.
constexpr std::uint32_t kStoryTextGroup = 0x7FA7DC7C;

// An album from a webentry file's text. Entries without a snapshot are left out.
StoryAlbum parseStoryAlbum(std::string_view xml);

// The webentry file of album `albumId` in the Storytelling folder `storytelling`.
std::filesystem::path storyAlbumFile(const std::filesystem::path& storytelling, std::uint32_t albumId);

} // namespace ts2::content
