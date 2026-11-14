#include "story_editor.hpp"

#include "content/ui_meters.hpp"
#include "diag/log.hpp"
#include "formats/image.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <fstream>
#include <iterator>

namespace app {

namespace {

namespace fs = std::filesystem;

// The editor's views, and their parts.
constexpr std::uint32_t kOrganizeView = 0xACB252C1, kStoryView = 0x8CB2529E, kBlogView = 0x4CB2526C;
constexpr std::uint32_t kEditAlbum = 0x2C8006B9, kEditBlog = 0x2C8006BC, kTitle = 0x2C8006C4, kOrganize = 0x0CB23B3C, kEditStory = 0x6D4898DD,
                        kTeleport = 0x0C8006A7;
// The story view: its picture frames (large, medium, small: the large one is used), the text shown
// with no entries, Previous, Next, Remove Entry From Story and the caption.
constexpr std::uint32_t kLargeFrame = 0x6DED4CD2, kLargePicture = 0x8DED4CDB, kMediumFrame = 0x4DED4CA8, kSmallFrame = 0x6DED4BD4;
constexpr std::uint32_t kNoEntries = 0x0D49F038, kPrevious = 0xACB79625, kNext = 0x6CB795EC, kRemovePage = 0x6CD88190, kCaption = 0x2C8006CA;
// The organize view: the bin's strip (its tile area, Previous, Next, Delete Snapshot), the bin
// buttons and backgrounds, the hint shown with an empty bin, the chosen snapshot's frame, Add to
// Album, Remove Entry From Story, and the story's column (its tile area, Scroll Up, Scroll Down,
// Move Entry Up and Down).
constexpr std::uint32_t kBinStrip = 0x6C855807, kBinStripMedium = 0x6C855808, kTileArea = 0x00001006, kStripPrevious = 0x00001004,
                        kStripNext = 0x00001005, kDeleteSnapshot = 0xEC80057A;
constexpr std::uint32_t kFamilyBin = 0x4C86A43C, kNeighborhoodBin = 0x4C86A436, kSimshotBin = 0x2D6BE6D2, kMoreBin = 0x2D6BE6D1;
constexpr std::uint32_t kVacationBins[] = {0xACDC2000, 0xACDC2001, 0xACDC2002, 0xACDC2003};
constexpr std::uint32_t kHouseBinBackground = 0x6CB2578B, kOtherBinBackground = 0x8CB25796, kSimshotBinBackground = 0x8CB25797;
constexpr std::uint32_t kBinHint = 0xCD869748, kPreviewFrame = 0x4D10E455, kPreviewPicture = 0x8DED2C0D, kAddToAlbum = 0x2C8006BF,
                        kRemoveEntry = 0xECBCE67F;
constexpr std::uint32_t kColumn = 0x4DEBC7E1, kMoveUp = 0x4C8006DB, kMoveDown = 0x4C8006D8;
// The blog view: its scrolling part (holding the entries' area, Scroll Up and Down and a scroll bar),
// Remove Entry From Story and the text shown with no entries.
constexpr std::uint32_t kBlogScroller = 0x0DED8B21, kBlogScrollBar = 0x00001008, kBlogRemove = 0x4DED8B79, kBlogNoEntries = 0x4DED8C66;
// The blog shows two entries at a time, each a 200 x 150 snapshot with its caption beside it.
constexpr std::size_t kBlogRows = 2;
constexpr int kBlogPictureWidth = 200, kBlogPictureHeight = 150, kBlogGap = 12;
constexpr int kBlogCaptionMargin = 4;  // a caption box is its row's height less this, so a long caption fits
// UIText (table 0xa9): the bins' hints for a family and a neighborhood, the delete question.
constexpr std::uint32_t kUiText = 0x7FA7DC7C;
constexpr std::uint32_t kFamilyHint = 0x00A9002D, kNeighborhoodHint = 0x00A9002E;  // string ids, leakcheck: ok
constexpr std::uint32_t kConfirmTitle = 0x00A90025, kConfirmDelete = 0x00A90026, kYesText = 0x008E0003, kNoText = 0x008E0004;  // string ids, leakcheck: ok
constexpr std::uint32_t kYesButton = 1, kNoButton = 2;
// The bin holds 4 x 2 thumbnails, the column 5, each in the game's 4:3.
constexpr int kBinColumns = 4, kBinRows = 2, kBinThumbWidth = 96, kBinThumbHeight = 72;
constexpr int kColumnRows = 5, kColumnThumbWidth = 72, kColumnThumbHeight = 54;
// Unchosen thumbnails are drawn dimmer.
constexpr std::array<float, 4> kChosen{1, 1, 1, 1}, kNotChosen{0.6f, 0.6f, 0.6f, 1};
// A caption or title is kept to this many bytes, so typing can't grow it without end.
constexpr std::size_t kMaxText = 4096;

ts2::ui::Window window(const std::string& kind, int w, int h, std::vector<std::pair<std::string, std::string>> more = {}) {
    ts2::ui::Window out;
    out.attributes = {{"iid", kind}, {"area", "(0,0," + std::to_string(w) + "," + std::to_string(h) + ")"}, {"winflag_visible", "yes"}};
    out.attributes.insert(out.attributes.end(), more.begin(), more.end());
    return out;
}

// UTF-8: where the character before or after `at` starts.
std::size_t previousChar(const std::string& s, std::size_t at) {
    while (at > 0 && (static_cast<unsigned char>(s[--at]) & 0xC0) == 0x80) {
    }
    return at;
}
std::size_t nextChar(const std::string& s, std::size_t at) {
    if (at >= s.size()) {
        return s.size();
    }
    ++at;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) {
        ++at;
    }
    return at;
}

std::vector<std::uint8_t> readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

void StoryEditor::attach(int root) {
    root_ = root;
    if (root_ < 0) {
        return;
    }
    // The other bins and packaging for upload aren't built yet, nor the Blog view's layout.
    for (const std::uint32_t id : {kSimshotBin, kMoreBin}) {
        screen_.setEnabled(screen_.find(root_, id), false);
    }
    for (const std::uint32_t id : kVacationBins) {
        screen_.setVisible(screen_.find(root_, id), false);  // the vacation bins' buttons show in their own bins
    }
    screen_.setVisible(screen_.find(root_, kTeleport), false);
    screen_.setVisible(screen_.find(root_, kBlogView), false);
    screen_.setVisible(screen_.find(root_, kBinStripMedium), false);
    screen_.setVisible(screen_.find(root_, kOtherBinBackground), false);
    screen_.setVisible(screen_.find(root_, kSimshotBinBackground), false);
    screen_.setVisible(screen_.find(root_, kHouseBinBackground), true);
    screen_.setVisible(screen_.find(root_, kMediumFrame), false);
    screen_.setVisible(screen_.find(root_, kSmallFrame), false);
    screen_.setVisible(screen_.find(root_, kLargeFrame), true);
}

void StoryEditor::open(std::optional<std::uint32_t> albumId, const fs::path& storytelling, const AlbumInfo& info) {
    albumId_ = albumId;
    folder_ = storytelling;
    album_ = {};
    changed_ = false;
    focus_ = Focus::None;
    page_ = 0;
    chosenBin_.reset();
    chosenEntry_.reset();
    binFirst_ = columnFirst_ = blogFirst_ = 0;
    blogChosen_.reset();
    captionEntry_ = 0;
    if (albumId_) {
        const auto file = ts2::content::storyAlbumFile(folder_, *albumId_);
        const auto bytes = readFile(file);
        if (!bytes.empty()) {
            album_ = ts2::content::parseStoryAlbum(std::string(bytes.begin(), bytes.end()));
        } else {
            album_.neighborhood = info.neighborhood;
            album_.familyId = info.familyId;
            album_.lotId = info.lotId;
        }
    }
    if (root_ < 0) {
        return;
    }
    // The bin of the album's own kind (a family's house or the neighborhood) shows chosen.
    for (const auto& [id, shown] : {std::pair{kFamilyBin, !info.neighborhoodStory}, std::pair{kNeighborhoodBin, info.neighborhoodStory}}) {
        const int w = screen_.find(root_, id);
        screen_.setVisible(w, shown);
        screen_.setChecked(w, shown);
    }
    screen_.setText(screen_.find(root_, kBinHint), data_.text(kUiText, info.neighborhoodStory ? kNeighborhoodHint : kFamilyHint));
    for (const std::uint32_t id : {kTitle, kEditAlbum, kEditBlog, kOrganize, kEditStory}) {
        screen_.setEnabled(screen_.find(root_, id), albumId_.has_value());
    }
    columnPictures_.clear();
    for (const auto& e : album_.entries) {
        columnPictures_.push_back(picture(e.thumbnail.empty() ? e.snapshot : e.thumbnail));
    }
    readBin();
    showTitle();
    showStyle();
    showStoryView(true);
}

void StoryEditor::readBin() {
    bin_.clear();
    binPictures_.clear();
    if (!albumId_) {
        return;
    }
    // The album's snapshots (snapshot_<album>_<id>.jpg) not in its story, oldest first.
    const std::string prefix = std::format("snapshot_{:08x}_", *albumId_);
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    std::error_code ec;
    for (fs::directory_iterator it(folder_, ec), end; it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!name.starts_with(prefix) || it->path().extension() != ".jpg") {
            continue;
        }
        if (std::any_of(album_.entries.begin(), album_.entries.end(), [&](const auto& e) { return e.snapshot == name; })) {
            continue;
        }
        found.emplace_back(it->last_write_time(ec), name);
    }
    std::sort(found.begin(), found.end());
    for (const auto& [time, name] : found) {
        bin_.push_back(name);
        binPictures_.push_back(picture("thumbnail_" + name.substr(9)));
        if (binPictures_.back().width == 0) {
            binPictures_.back() = picture(name);
        }
    }
}

ts2::tga::Pixels StoryEditor::picture(const std::string& file) {
    if (file.empty() || file.find_first_of("/\\:") != std::string::npos) {
        return {};
    }
    const auto bytes = readFile(folder_ / file);
    if (auto decoded = bytes.empty() ? std::nullopt : ts2::image::decode(bytes, 2048)) {
        return std::move(*decoded);
    }
    return {};
}

std::string StoryEditor::caption(const ts2::content::StoryEntry& entry) const {
    if (entry.text.empty() && entry.textResource != 0) {
        return data_.text(ts2::content::kStoryTextGroup, entry.textResource);
    }
    return entry.text;
}

void StoryEditor::showStoryView(bool story) {
    storyView_ = story;
    focus_ = Focus::None;
    // A story is shown as its style lays it out: an album a page at a time, a blog as a column.
    const bool blog = isBlog();
    screen_.setVisible(screen_.find(root_, kStoryView), story && !blog);
    screen_.setVisible(screen_.find(root_, kBlogView), story && blog);
    screen_.setVisible(screen_.find(root_, kOrganizeView), !story);
    screen_.setChecked(screen_.find(root_, kOrganize), !story);
    screen_.setChecked(screen_.find(root_, kEditStory), story);
    clearBlog();  // rows are made again whenever the blog shows
    if (story) {
        blog ? showBlog() : showPage();
    } else {
        showOrganize();
    }
    showTitle();
}

void StoryEditor::clearBlog() {
    for (const auto& row : blogRows_) {
        screen_.remove(row.picture.button);
        screen_.remove(row.caption);
    }
    blogRows_.clear();
}

void StoryEditor::showBlog() {
    clearBlog();
    const int view = screen_.find(root_, kBlogView);
    const int scroller = screen_.find(view, kBlogScroller);
    const int area = screen_.find(scroller, kTileArea);
    const bool any = !album_.entries.empty();
    blogFirst_ = any ? std::min(blogFirst_, album_.entries.size() - 1) : 0;
    if (blogChosen_ && *blogChosen_ >= album_.entries.size()) {
        blogChosen_.reset();
    }
    screen_.setVisible(screen_.find(view, kBlogNoEntries), !any);
    screen_.setVisible(screen_.find(scroller, kBlogScrollBar), false);  // the pages turn with Scroll Up and Down
    screen_.setEnabled(screen_.find(scroller, kStripPrevious), blogFirst_ > 0);
    screen_.setEnabled(screen_.find(scroller, kStripNext), blogFirst_ + kBlogRows < album_.entries.size());
    screen_.setEnabled(screen_.find(view, kBlogRemove), blogChosen_.has_value());
    if (area < 0) {
        return;
    }
    // Each entry a row: its snapshot on the left, its caption beside it, as many rows as fit.
    const int rowHeight = screen_.height(area) / kBlogRows;
    const int pictureY = (rowHeight - kBlogPictureHeight) / 2;
    const int captionX = kBlogPictureWidth + kBlogGap;
    for (std::size_t i = blogFirst_; i < album_.entries.size() && i < blogFirst_ + kBlogRows; ++i) {
        const int top = screen_.y(area) + static_cast<int>(i - blogFirst_) * rowHeight + pictureY;
        BlogRow row;
        row.entry = i;
        auto button = window("IGZWinBtn", kBlogPictureWidth, kBlogPictureHeight);
        button.children.push_back(window("IGZWinBMP", kBlogPictureWidth, kBlogPictureHeight, {{"winflag_ignoremouse", "yes"}}));
        row.picture.button = screen_.addWindows({button}, screen_.x(area), top, scroller);
        row.picture.picture = screen_.children(row.picture.button).empty() ? -1 : screen_.children(row.picture.button).front();
        const auto shown = i < columnPictures_.size() && columnPictures_[i].width > 0 ? columnPictures_[i] : picture(album_.entries[i].snapshot);
        if (row.picture.picture >= 0 && shown.width > 0) {
            screen_.setImage(row.picture.picture, ts2::content::scaled(shown, kBlogPictureWidth, kBlogPictureHeight));
            screen_.setImageColor(row.picture.picture, !blogChosen_ || *blogChosen_ == i ? kChosen : kNotChosen);
        }
        // The caption box as the story view's: GenBodyMedium in the album's blue, wrapped.
        row.caption = screen_.addWindows({window("IGZWinTextEdit", screen_.width(area) - captionX, rowHeight - kBlogCaptionMargin,
                                                 {{"font", "GenBodyMedium"}, {"wrapped", "yes"}, {"editable", "yes"}, {"singleline", "no"},
                                                  {"colorfontnormal", "(5,6,113)"}, {"caretcolor", "(5,6,113)"}, {"outline", "no"}})},
                                         screen_.x(area) + captionX, screen_.y(area) + static_cast<int>(i - blogFirst_) * rowHeight + kBlogCaptionMargin / 2, scroller);
        blogRows_.push_back(row);
        showText(Focus::Caption, i);
    }
}

void StoryEditor::showPage() {
    const bool any = !album_.entries.empty();
    page_ = any ? std::min(page_, album_.entries.size() - 1) : 0;
    const int view = screen_.find(root_, kStoryView);
    screen_.setVisible(screen_.find(view, kNoEntries), !any);
    screen_.setVisible(screen_.find(view, kLargeFrame), any);
    const int captionBox = screen_.find(view, kCaption);
    screen_.setVisible(captionBox, any);
    screen_.setEnabled(screen_.find(view, kPrevious), any && page_ > 0);
    screen_.setEnabled(screen_.find(view, kNext), any && page_ + 1 < album_.entries.size());
    screen_.setEnabled(screen_.find(view, kRemovePage), any);
    if (!any) {
        return;
    }
    const auto& entry = album_.entries[page_];
    const int frame = screen_.find(view, kLargePicture);
    const auto shown = picture(entry.snapshot);
    if (shown.width > 0) {
        screen_.setImage(frame, ts2::content::scaled(shown, static_cast<std::uint32_t>(screen_.width(frame)), static_cast<std::uint32_t>(screen_.height(frame))));
    }
    showText(Focus::Caption, page_);
}

void StoryEditor::clearThumbs(std::vector<Thumb>& thumbs) {
    for (const auto& t : thumbs) {
        screen_.remove(t.button);
    }
    thumbs.clear();
}

void StoryEditor::layoutThumbs(int area, std::vector<Thumb>& thumbs, const std::vector<std::size_t>& items, std::size_t first,
                               std::size_t perPage, int columns, int width, int height, const std::vector<ts2::tga::Pixels>& pictures,
                               std::optional<std::size_t> chosen) {
    clearThumbs(thumbs);
    if (area < 0) {
        return;
    }
    const int parent = screen_.parent(area);
    const int rows = static_cast<int>((perPage + static_cast<std::size_t>(columns) - 1) / static_cast<std::size_t>(columns));
    // Spread evenly over the tile area.
    const int gapX = columns > 1 ? (screen_.width(area) - columns * width) / (columns - 1) : 0;
    const int gapY = rows > 1 ? (screen_.height(area) - rows * height) / (rows - 1) : 0;
    const int left = columns > 1 ? 0 : (screen_.width(area) - width) / 2;
    for (std::size_t i = first; i < items.size() && i < first + perPage; ++i) {
        const int at = static_cast<int>(i - first);
        const int x = screen_.x(area) + left + at % columns * (width + gapX), y = screen_.y(area) + at / columns * (height + std::max(gapY, 4));
        auto button = window("IGZWinBtn", width, height);
        button.children.push_back(window("IGZWinBMP", width, height, {{"winflag_ignoremouse", "yes"}}));
        Thumb t;
        t.button = screen_.addWindows({button}, x, y, parent);
        t.picture = screen_.children(t.button).empty() ? -1 : screen_.children(t.button).front();
        const std::size_t item = items[i];
        if (t.picture >= 0 && item < pictures.size() && pictures[item].width > 0) {
            screen_.setImage(t.picture, ts2::content::scaled(pictures[item], static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)));
            screen_.setImageColor(t.picture, chosen && *chosen == item ? kChosen : kNotChosen);
        }
        thumbs.push_back(t);
    }
}

void StoryEditor::showOrganize() {
    const int view = screen_.find(root_, kOrganizeView);
    const int strip = screen_.find(view, kBinStrip);
    const int column = screen_.find(view, kColumn);
    std::vector<std::size_t> binItems(bin_.size()), columnItems(album_.entries.size());
    for (std::size_t i = 0; i < binItems.size(); ++i) {
        binItems[i] = i;
    }
    for (std::size_t i = 0; i < columnItems.size(); ++i) {
        columnItems[i] = i;
    }
    const std::size_t binPage = static_cast<std::size_t>(kBinColumns * kBinRows);
    layoutThumbs(screen_.find(strip, kTileArea), binThumbs_, binItems, binFirst_, binPage, kBinColumns, kBinThumbWidth, kBinThumbHeight,
                 binPictures_, chosenBin_);
    layoutThumbs(screen_.find(column, kTileArea), columnThumbs_, columnItems, columnFirst_, kColumnRows, 1, kColumnThumbWidth, kColumnThumbHeight,
                 columnPictures_, chosenEntry_);
    screen_.setEnabled(screen_.find(strip, kStripPrevious), binFirst_ > 0);
    screen_.setEnabled(screen_.find(strip, kStripNext), binFirst_ + binPage < bin_.size());
    screen_.setEnabled(screen_.find(column, kStripPrevious), columnFirst_ > 0);
    screen_.setEnabled(screen_.find(column, kStripNext), columnFirst_ + kColumnRows < album_.entries.size());
    screen_.setVisible(screen_.find(view, kBinHint), bin_.empty());
    screen_.setEnabled(screen_.find(strip, kDeleteSnapshot), chosenBin_.has_value());
    screen_.setEnabled(screen_.find(view, kAddToAlbum), chosenBin_.has_value());
    screen_.setEnabled(screen_.find(view, kRemoveEntry), chosenEntry_.has_value());
    screen_.setEnabled(screen_.find(column, kMoveUp), chosenEntry_ && *chosenEntry_ > 0);
    screen_.setEnabled(screen_.find(column, kMoveDown), chosenEntry_ && *chosenEntry_ + 1 < album_.entries.size());
    // The chosen snapshot (from the bin or the story) in the frame.
    const int frame = screen_.find(view, kPreviewFrame);
    const ts2::tga::Pixels* shown = chosenBin_ && *chosenBin_ < binPictures_.size()         ? &binPictures_[*chosenBin_]
                                    : chosenEntry_ && *chosenEntry_ < columnPictures_.size() ? &columnPictures_[*chosenEntry_]
                                                                                             : nullptr;
    screen_.setVisible(frame, shown != nullptr && shown->width > 0);
    if (shown != nullptr && shown->width > 0) {
        const int pictureWindow = screen_.find(frame, kPreviewPicture);
        screen_.setImage(pictureWindow, ts2::content::scaled(*shown, static_cast<std::uint32_t>(screen_.width(pictureWindow)),
                                                             static_cast<std::uint32_t>(screen_.height(pictureWindow))));
    }
}

void StoryEditor::showTitle() { showText(Focus::Title); }

void StoryEditor::showStyle() {
    screen_.setChecked(screen_.find(root_, kEditAlbum), album_.style != "Blog");
    screen_.setChecked(screen_.find(root_, kEditBlog), album_.style == "Blog");
}

int StoryEditor::captionWindow(std::size_t entry) const {
    if (!isBlog()) {
        return entry == page_ ? screen_.find(root_, kCaption) : -1;
    }
    for (const auto& row : blogRows_) {
        if (row.entry == entry) {
            return row.caption;
        }
    }
    return -1;
}

void StoryEditor::showText(Focus box, std::optional<std::size_t> entry) {
    const std::size_t which = entry.value_or(box == Focus::Caption && focus_ == Focus::Caption ? captionEntry_ : page_);
    const int w = box == Focus::Title ? screen_.find(root_, kTitle) : captionWindow(which);
    if (w < 0) {
        return;
    }
    if (box == Focus::Caption) {
        if (which >= album_.entries.size()) {
            return;
        }
        screen_.setText(w, caption(album_.entries[which]));
    } else {
        screen_.setText(w, album_.title);
    }
    if (focus_ == box && (box == Focus::Title || which == captionEntry_)) {
        screen_.setSelection(w, caret_, caret_);
        screen_.setCaret(w, caret_);
    } else {
        screen_.setSelection(w, 0, 0);
        screen_.setCaret(w, std::nullopt);
    }
}

void StoryEditor::showCaptions() {
    if (!storyView_) {
        return;
    }
    if (isBlog()) {
        for (const auto& row : blogRows_) {
            showText(Focus::Caption, row.entry);
        }
    } else {
        showText(Focus::Caption, page_);
    }
}

std::string& StoryEditor::focusedText() {
    return focus_ == Focus::Title ? album_.title : album_.entries[captionEntry_].text;
}

void StoryEditor::pointerDown(int window) {
    if (root_ < 0 || !albumId_) {
        return;
    }
    const Focus before = focus_;
    focus_ = Focus::None;
    for (int w = window; w >= 0 && focus_ == Focus::None; w = screen_.parent(w)) {
        if (w == screen_.find(root_, kTitle)) {
            focus_ = Focus::Title;
            continue;
        }
        if (!storyView_) {
            continue;
        }
        // A caption: the page's, or one of the blog's rows (which is then the one chosen).
        for (std::size_t i = 0; i < album_.entries.size(); ++i) {
            if (w >= 0 && w == captionWindow(i)) {
                focus_ = Focus::Caption;
                captionEntry_ = i;
                if (isBlog()) {
                    blogChosen_ = i;
                }
                // A premade caption becomes the player's own text to edit.
                auto& entry = album_.entries[i];
                if (entry.text.empty() && entry.textResource != 0) {
                    entry.text = caption(entry);
                }
                break;
            }
        }
    }
    if (focus_ != Focus::None) {
        caret_ = focusedText().size();
    }
    if (before != focus_ || focus_ != Focus::None) {
        showText(Focus::Title);
        showCaptions();
        if (isBlog() && storyView_ && focus_ == Focus::Caption) {
            showBlogChoice();
        }
    }
}

void StoryEditor::typed(const std::string& text) {
    if (focus_ == Focus::None) {
        return;
    }
    std::string& s = focusedText();
    if (s.size() + text.size() > kMaxText) {
        return;
    }
    s.insert(caret_, text);
    caret_ += text.size();
    if (focus_ == Focus::Caption) {
        album_.entries[captionEntry_].textResource = 0;  // the player's own caption from now on
    }
    changed_ = true;
    showText(focus_);
}

void StoryEditor::key(ts2::platform::Key key) {
    using ts2::platform::Key;
    if (focus_ == Focus::None) {
        return;
    }
    std::string& s = focusedText();
    bool edited = false;
    switch (key) {
    case Key::Backspace:
        if (caret_ > 0) {
            const std::size_t from = previousChar(s, caret_);
            s.erase(from, caret_ - from);
            caret_ = from;
            edited = true;
        }
        break;
    case Key::Delete:
        if (caret_ < s.size()) {
            s.erase(caret_, nextChar(s, caret_) - caret_);
            edited = true;
        }
        break;
    case Key::Left: caret_ = previousChar(s, caret_); break;
    case Key::Right: caret_ = nextChar(s, caret_); break;
    case Key::Home: caret_ = 0; break;
    case Key::End: caret_ = s.size(); break;
    case Key::Return:
        // A caption has lines of its own; Return ends the title.
        if (focus_ == Focus::Caption && s.size() < kMaxText) {
            s.insert(caret_, "\n");
            ++caret_;
            edited = true;
        } else {
            focus_ = Focus::None;
        }
        break;
    case Key::Escape: focus_ = Focus::None; break;
    default: return;
    }
    if (edited) {
        changed_ = true;
        if (focus_ == Focus::Caption && captionEntry_ < album_.entries.size()) {
            album_.entries[captionEntry_].textResource = 0;
        }
    }
    showText(Focus::Title);
    showCaptions();
}

void StoryEditor::showBlogChoice() {
    for (const auto& row : blogRows_) {
        if (row.picture.picture >= 0) {
            screen_.setImageColor(row.picture.picture, !blogChosen_ || *blogChosen_ == row.entry ? kChosen : kNotChosen);
        }
    }
    screen_.setEnabled(screen_.find(screen_.find(root_, kBlogView), kBlogRemove), blogChosen_.has_value());
}

bool StoryEditor::clicked(int window) {
    if (root_ < 0 || window < 0) {
        return false;
    }
    if (const auto answer = confirm_.clicked(screen_, window)) {
        confirm_.close(screen_);
        if (*answer == kYesButton && chosenBin_ && *chosenBin_ < bin_.size()) {
            // The snapshot's files go, as the game's Delete Snapshot does (only one not in the story).
            std::error_code ec;
            const std::string name = bin_[*chosenBin_];
            fs::remove(folder_ / name, ec);
            fs::remove(folder_ / ("thumbnail_" + name.substr(9)), ec);
            ts2::diag::step("story editor: deleted snapshot " + name);
            chosenBin_.reset();
            readBin();
            binFirst_ = std::min(binFirst_, bin_.empty() ? 0 : (bin_.size() - 1) / (kBinColumns * kBinRows) * (kBinColumns * kBinRows));
            showOrganize();
        }
        return true;
    }
    const std::uint32_t id = screen_.id(window);
    const int storyView = screen_.find(root_, kStoryView), organize = screen_.find(root_, kOrganizeView);
    const int strip = screen_.find(organize, kBinStrip), column = screen_.find(organize, kColumn);
    const int blogView = screen_.find(root_, kBlogView), blogScroller = screen_.find(blogView, kBlogScroller);
    auto is = [&](int parentView, std::uint32_t wanted) { return window == screen_.find(parentView, wanted); };
    // A thumbnail chosen in the bin or the story's column.
    for (std::size_t i = 0; i < binThumbs_.size(); ++i) {
        if (window == binThumbs_[i].button) {
            chosenBin_ = binFirst_ + i;
            chosenEntry_.reset();
            showOrganize();
            return true;
        }
    }
    for (std::size_t i = 0; i < columnThumbs_.size(); ++i) {
        if (window == columnThumbs_[i].button) {
            chosenEntry_ = columnFirst_ + i;
            chosenBin_.reset();
            showOrganize();
            return true;
        }
    }
    if (window == screen_.find(root_, kOrganize)) {
        showStoryView(false);
    } else if (window == screen_.find(root_, kEditStory)) {
        showStoryView(true);
    } else if (window == screen_.find(root_, kEditAlbum) || window == screen_.find(root_, kEditBlog)) {
        const std::string style = id == kEditBlog ? "Blog" : "Album";
        changed_ = changed_ || style != album_.style;
        album_.style = style;
        showStyle();
        if (storyView_) {
            showStoryView(true);  // laid out in the new style
        }
    } else if (const auto row = std::find_if(blogRows_.begin(), blogRows_.end(), [&](const BlogRow& r) { return r.picture.button == window; });
               row != blogRows_.end()) {
        blogChosen_ = row->entry;
        showBlogChoice();
    } else if (is(blogScroller, kStripPrevious) && blogFirst_ > 0) {
        --blogFirst_;
        focus_ = Focus::None;
        showBlog();
    } else if (is(blogScroller, kStripNext) && blogFirst_ + kBlogRows < album_.entries.size()) {
        ++blogFirst_;
        focus_ = Focus::None;
        showBlog();
    } else if (is(blogView, kBlogRemove) && blogChosen_ && *blogChosen_ < album_.entries.size()) {
        // Out of the story, back to the bin (its files stay).
        album_.entries.erase(album_.entries.begin() + static_cast<std::ptrdiff_t>(*blogChosen_));
        if (*blogChosen_ < columnPictures_.size()) {
            columnPictures_.erase(columnPictures_.begin() + static_cast<std::ptrdiff_t>(*blogChosen_));
        }
        changed_ = true;
        blogChosen_.reset();
        focus_ = Focus::None;
        readBin();
        showBlog();
    } else if (is(storyView, kPrevious) && page_ > 0) {
        --page_;
        focus_ = Focus::None;
        showPage();
    } else if (is(storyView, kNext) && page_ + 1 < album_.entries.size()) {
        ++page_;
        focus_ = Focus::None;
        showPage();
    } else if ((is(storyView, kRemovePage) && !album_.entries.empty()) || (is(organize, kRemoveEntry) && chosenEntry_)) {
        // Out of the story, back to the bin (its files stay).
        const std::size_t at = storyView_ ? page_ : *chosenEntry_;
        if (at < album_.entries.size()) {
            album_.entries.erase(album_.entries.begin() + static_cast<std::ptrdiff_t>(at));
            columnPictures_.erase(columnPictures_.begin() + static_cast<std::ptrdiff_t>(at));
            changed_ = true;
        }
        chosenEntry_.reset();
        focus_ = Focus::None;
        readBin();
        storyView_ ? showPage() : showOrganize();
    } else if (is(organize, kAddToAlbum) && chosenBin_ && *chosenBin_ < bin_.size()) {
        // The chosen snapshot at the story's end, with an empty caption.
        ts2::content::StoryEntry entry;
        entry.snapshot = bin_[*chosenBin_];
        entry.thumbnail = "thumbnail_" + entry.snapshot.substr(9);
        entry.timeStamp = ts2::content::storyTimeStamp(std::time(nullptr));
        album_.entries.push_back(entry);
        columnPictures_.push_back(binPictures_[*chosenBin_]);
        changed_ = true;
        chosenBin_.reset();
        readBin();
        columnFirst_ = album_.entries.size() > kColumnRows ? album_.entries.size() - kColumnRows : 0;
        showOrganize();
    } else if (is(strip, kDeleteSnapshot) && chosenBin_ && *chosenBin_ < bin_.size()) {
        // The game asks first.
        const auto [w, h] = screen_.size();
        confirm_.open(screen_, w, h, data_.text(kUiText, kConfirmTitle), data_.text(kUiText, kConfirmDelete),
                      {{kYesButton, data_.text(kUiText, kYesText)}, {kNoButton, data_.text(kUiText, kNoText)}});
    } else if ((is(column, kMoveUp) || is(column, kMoveDown)) && chosenEntry_) {
        const std::size_t from = *chosenEntry_;
        const std::size_t to = is(column, kMoveUp) ? from - 1 : from + 1;
        if (from < album_.entries.size() && to < album_.entries.size()) {
            std::swap(album_.entries[from], album_.entries[to]);
            std::swap(columnPictures_[from], columnPictures_[to]);
            chosenEntry_ = to;
            changed_ = true;
            if (to < columnFirst_) {
                columnFirst_ = to;
            } else if (to >= columnFirst_ + kColumnRows) {
                columnFirst_ = to + 1 - kColumnRows;
            }
        }
        showOrganize();
    } else if (is(strip, kStripPrevious) && binFirst_ > 0) {
        binFirst_ -= std::min<std::size_t>(binFirst_, kBinColumns * kBinRows);
        showOrganize();
    } else if (is(strip, kStripNext) && binFirst_ + kBinColumns * kBinRows < bin_.size()) {
        binFirst_ += kBinColumns * kBinRows;
        showOrganize();
    } else if (is(column, kStripPrevious) && columnFirst_ > 0) {
        --columnFirst_;
        showOrganize();
    } else if (is(column, kStripNext) && columnFirst_ + kColumnRows < album_.entries.size()) {
        ++columnFirst_;
        showOrganize();
    } else {
        return false;
    }
    return true;
}

bool StoryEditor::save() {
    focus_ = Focus::None;
    if (!changed_ || !albumId_) {
        return true;
    }
    const auto file = ts2::content::storyAlbumFile(folder_, *albumId_);
    std::error_code ec;
    fs::create_directories(folder_, ec);
    // Written beside it first, then put in its place, so a failed write leaves the album as it was.
    const auto temporary = fs::path(file).concat(".neots2new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        const std::string text = ts2::content::writeStoryAlbum(album_, *albumId_);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            ts2::diag::error("story editor: the album couldn't be written: " + ts2::diag::utf8(file.filename()));
            return false;
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        ts2::diag::error("story editor: the album couldn't be saved: " + ec.message());
        return false;
    }
    ts2::diag::step(std::format("story editor: saved {} ({} entries)", ts2::diag::utf8(file.filename()), album_.entries.size()));
    changed_ = false;
    return true;
}

} // namespace app
