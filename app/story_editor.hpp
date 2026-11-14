#pragma once

#include "dialog_box.hpp"

#include "content/story_album.hpp"
#include "content/ui_data.hpp"
#include "formats/tga.hpp"
#include "platform/platform.hpp"
#include "ui/screen.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace app {

// The story editor (UI script 0x0C7FE4BF): a neighborhood's story or a family's album, edited as the
// game edits it. Its story view pages through the entries, each a snapshot and a caption the player
// writes; its organize view shows the album's snapshots not in the story (Add to Album puts the one
// chosen at the story's end, Delete deletes its files) beside the story's column (moved up and down,
// or taken out). The album's title and style (Album or Blog) are set at the top. What changed is
// written to the album's webentry file in the Storytelling folder on save().
//
// Not built yet: the Blog style's own layout (a blog shows as an album does here), the other
// snapshot bins (Sim photos, vacation pictures) and packaging the story for upload.
class StoryEditor {
public:
    StoryEditor(ts2::ui::Screen& screen, ts2::content::UiData& data) : screen_(screen), data_(data) {}

    // What a new album file says about itself: its neighborhood, and for a family's album the family
    // and its lot.
    struct AlbumInfo {
        std::string neighborhood;
        std::uint32_t familyId = 0, lotId = 0;
        bool neighborhoodStory = true;  // a neighborhood's story, not a family's album
    };

    // The editor's window (loaded from its script by whoever shows it).
    void attach(int root);
    // Opens album `albumId` from the Storytelling folder, on its story view. Nothing to edit without
    // an album (a lot whose family has none): the editor shows empty.
    void open(std::optional<std::uint32_t> albumId, const std::filesystem::path& storytelling, const AlbumInfo& info);
    // Writes the album when it changed. False when it couldn't be written.
    bool save();
    bool changed() const { return changed_; }

    // A released button of the editor: whether it was one it acts on (Close is left to whoever shows it).
    bool clicked(int window);
    // The mouse went down on `window`: a text box under it takes the keyboard.
    void pointerDown(int window);
    // Whether a text box has the keyboard (typed text and keys go to typed() and key()).
    bool editing() const { return focus_ != Focus::None; }
    // Whether its question box is open (it takes the screen meanwhile).
    bool asking() const { return confirm_.isOpen(); }
    void typed(const std::string& text);
    // A key while a text box has the keyboard. Escape lets the keyboard go.
    void key(ts2::platform::Key key);

private:
    enum class Focus { None, Title, Caption };
    struct Thumb {
        int button = -1, picture = -1;
    };
    void showStoryView(bool story);
    void showPage();
    void showOrganize();
    void showTitle();
    void showStyle();
    void layoutThumbs(int area, std::vector<Thumb>& thumbs, const std::vector<std::size_t>& items, std::size_t first, std::size_t perPage,
                      int columns, int width, int height, const std::vector<ts2::tga::Pixels>& pictures, std::optional<std::size_t> chosen);
    void clearThumbs(std::vector<Thumb>& thumbs);
    ts2::tga::Pixels picture(const std::string& file);
    std::string caption(const ts2::content::StoryEntry& entry) const;
    void readBin();
    void showText(Focus box);
    std::string& focusedText();
    int focusedWindow() const;

    ts2::ui::Screen& screen_;
    ts2::content::UiData& data_;
    int root_ = -1;
    std::optional<std::uint32_t> albumId_;
    std::filesystem::path folder_;
    ts2::content::StoryAlbum album_;
    bool changed_ = false;
    bool storyView_ = true;
    std::size_t page_ = 0;
    // The bin: the album's snapshots not in the story (their snapshot file names), with thumbnails.
    std::vector<std::string> bin_;
    std::vector<ts2::tga::Pixels> binPictures_;
    std::vector<ts2::tga::Pixels> columnPictures_;  // the story's entries' thumbnails
    std::optional<std::size_t> chosenBin_, chosenEntry_;
    std::size_t binFirst_ = 0, columnFirst_ = 0;
    std::vector<Thumb> binThumbs_, columnThumbs_;
    Focus focus_ = Focus::None;
    std::size_t caret_ = 0;
    DialogBox confirm_;  // "Are you sure you want to delete this snapshot?"
};

} // namespace app
