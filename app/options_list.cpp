#include "options_list.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// A row is 17 high, the height of its radio dot picture plus 2.
constexpr int kRowHeight = 17;
// The bar is 12 wide and sits 3 past the rows, inside the list.
constexpr int kBarGap = 3, kBarWidth = 12;
constexpr int kThumbHeight = 18;
// Rows' ids, clear of the ids of the panel's other windows.
constexpr std::uint32_t kFirstRowId = 0x00A00000;  // window id, leakcheck: ok
const std::string kBarImage = "{499db772,e9f50525}", kRowImage = "{499db772,ac76e165}";
// The thumb's picture holds four frames side by side, 12 wide each. The second is its usual look.
constexpr std::uint32_t kImages = 0x499DB772, kThumbImage = 0xE9F50520;
constexpr std::uint32_t kThumbFrameWidth = 12;

std::string area(int x, int y, int w, int h) {
    return std::format("({},{},{},{})", x, y, x + w, y + h);
}

ts2::ui::Window barPicture(const std::string& picture, int h) {
    return {{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"edgeimage", "yes"}, {"winflag_visible", "yes"},
             {"area", area(0, 0, kBarWidth, h)}, {"image", picture}}, {}};
}

} // namespace

int addScrollBarParts(ts2::ui::Screen& screen, int bar) {
    screen.addWindows({barPicture(kBarImage, screen.height(bar))}, 0, 0, bar);
    const int thumb = screen.addWindows({barPicture(kBarImage, kThumbHeight)}, 0, 0, bar);
    if (auto frames = screen.data().image(kImages, kThumbImage); frames && frames->width >= 2 * kThumbFrameWidth) {
        ts2::tga::Pixels usual;
        usual.width = kThumbFrameWidth;
        usual.height = frames->height;
        usual.rgba.resize(std::size_t{usual.width} * usual.height * 4);
        for (std::uint32_t y = 0; y < usual.height; ++y) {
            std::copy_n(&frames->rgba[(std::size_t{y} * frames->width + kThumbFrameWidth) * 4], usual.width * 4, &usual.rgba[std::size_t{y} * usual.width * 4]);
        }
        screen.setImage(thumb, std::move(usual));
    }
    return thumb;
}

void OptionsList::attach(ts2::ui::Screen& screen, int list) {
    screen_ = &screen;
    list_ = list;
    width_ = screen.width(list);
    height_ = screen.height(list);
    visible_ = height_ / kRowHeight;
    screen.resize(list, width_ + kBarGap + kBarWidth, height_);
    bar_ = screen.addWindows({ts2::ui::Window{{{"iid", "IGZWinGen"}, {"winflag_visible", "yes"}, {"area", area(0, 0, kBarWidth, height_)}}, {}}}, width_ + kBarGap, 0, list);
    thumb_ = addScrollBarParts(screen, bar_);
    screen.setVisible(bar_, false);
}

void OptionsList::setRows(const std::vector<std::string>& rows) {
    for (const int row : rows_) {
        screen_->remove(row);
    }
    rows_.clear();
    picked_ = -1;
    top_ = 0;
    std::uint32_t id = 0;
    for (const std::string& text : rows) {
        ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstRowId + id++)},
                             {"area", area(0, 0, width_, kRowHeight)}, {"image", kRowImage}, {"style", "radiocheck"}, {"showcaption", "yes"},
                             {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"colorfontdisabled", "(84,85,105)"},
                             {"colorfonthilited", "(204,214,255)"}, {"gutters", "(4,2,4,2)"}, {"align", "center"}},
                            {}};
        const int made = screen_->addWindows({row}, 0, 0, list_);
        screen_->setText(made, text);
        screen_->setEnabled(made, enabled_);
        rows_.push_back(made);
    }
    showRows();
}

int OptionsList::lastTop() const {
    return std::max(0, static_cast<int>(rows_.size()) - visible_);
}

void OptionsList::showRows() {
    if (screen_ == nullptr) {
        return;
    }
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const int row = rows_[static_cast<std::size_t>(i)];
        const bool shown = i >= top_ && i < top_ + visible_;
        screen_->setVisible(row, shown);
        screen_->move(row, 0, (i - top_) * kRowHeight);
        screen_->setChecked(row, i == picked_);
    }
    const bool scrolls = lastTop() > 0;
    screen_->setVisible(bar_, scrolls);
    if (scrolls) {
        screen_->move(thumb_, 0, thumbTravel() * top_ / lastTop());
    }
}

void OptionsList::pick(int row) {
    picked_ = row;
    if (row >= top_ + visible_) {
        top_ = std::min(row - visible_ + 1, lastTop());
    } else if (row >= 0 && row < top_) {
        top_ = row;
    }
    showRows();
}

void OptionsList::setEnabled(bool enabled) {
    enabled_ = enabled;
    for (const int row : rows_) {
        screen_->setEnabled(row, enabled);
    }
}

bool OptionsList::clicked(int window) {
    const auto found = std::find(rows_.begin(), rows_.end(), window);
    if (found == rows_.end()) {
        return false;
    }
    pick(static_cast<int>(found - rows_.begin()));
    return true;
}

int OptionsList::barY() const {
    int y = 0;
    for (int w = bar_; w >= 0; w = screen_->parent(w)) {
        y += screen_->y(w);
    }
    return y;
}

int OptionsList::thumbTravel() const {
    return std::max(1, height_ - kThumbHeight);
}

bool OptionsList::mouseDown(int x, int y) {
    if (screen_ == nullptr || !screen_->visible(bar_) || !screen_->visible(list_)) {
        return false;
    }
    int barX = 0;
    for (int w = bar_; w >= 0; w = screen_->parent(w)) {
        if (!screen_->visible(w)) {
            return false;
        }
        barX += screen_->x(w);
    }
    const int inBar = y - barY();
    if (x < barX || x >= barX + kBarWidth || inBar < 0 || inBar >= height_) {
        return false;
    }
    const int thumbTop = screen_->y(thumb_);
    if (inBar < thumbTop) {
        top_ = std::max(0, top_ - visible_);
    } else if (inBar >= thumbTop + kThumbHeight) {
        top_ = std::min(lastTop(), top_ + visible_);
    } else {
        dragging_ = true;
    }
    showRows();
    return true;
}

void OptionsList::mouseMove(int y) {
    const int place = std::clamp(y - barY() - kThumbHeight / 2, 0, thumbTravel());
    top_ = (place * lastTop() + thumbTravel() / 2) / thumbTravel();
    showRows();
}

bool OptionsList::wheel(int x, int y, int rows) {
    if (screen_ == nullptr) {
        return false;
    }
    int left = 0, up = 0;
    for (int w = list_; w >= 0; w = screen_->parent(w)) {
        if (!screen_->visible(w)) {
            return false;
        }
        left += screen_->x(w);
        up += screen_->y(w);
    }
    if (x < left || x >= left + screen_->width(list_) || y < up || y >= up + height_) {
        return false;
    }
    top_ = std::clamp(top_ - rows, 0, lastTop());
    showRows();
    return true;
}

} // namespace app
