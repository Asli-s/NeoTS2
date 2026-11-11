#include "list_box_rows.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// The list grows by the bar and 3 more, and the bar stands 3 past the rows.
constexpr int kBarGap = 3;

} // namespace

void ListBoxRows::attach(ts2::ui::Screen& screen, int list, int rowHeight, BarImages bar) {
    screen_ = &screen;
    list_ = list;
    // Rows taken for an earlier list went with it, and their handles may now name other windows.
    rows_.clear();
    top_ = 0;
    rowHeight_ = std::max(1, rowHeight);
    width_ = screen.width(list);
    height_ = screen.height(list);
    visible_ = height_ / rowHeight_;
    const int barWidth = GameScrollBar::width(screen, bar.images, bar.track, bar.thumb);
    screen.resize(list, width_ + kBarGap + barWidth, height_);
    const ts2::ui::Window window{{{"iid", "IGZWinGen"}, {"winflag_visible", "yes"}, {"area", std::format("(0,0,{},{})", barWidth, visible_ * rowHeight_)}}, {}};
    bar_ = screen.addWindows({window}, width_ + kBarGap, 0, list);
    scroll_.attach(screen, bar_, bar.images, bar.track, bar.thumb);
}

void ListBoxRows::setRows(std::vector<int> rows) {
    rows_ = std::move(rows);
    top_ = 0;
    showRows();
}

int ListBoxRows::lastTop() const {
    return std::max(0, static_cast<int>(rows_.size()) - visible_);
}

void ListBoxRows::showRows() {
    if (screen_ == nullptr) {
        return;
    }
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
        const int row = rows_[static_cast<std::size_t>(i)];
        screen_->setVisible(row, i >= top_ && i < top_ + visible_);
        screen_->move(row, 0, (i - top_) * rowHeight_);
    }
    scroll_.set(*screen_, static_cast<int>(rows_.size()), visible_, top_);
}

void ListBoxRows::showRow(int row) {
    if (row >= top_ + visible_) {
        top_ = std::min(row - visible_ + 1, lastTop());
    } else if (row >= 0 && row < top_) {
        top_ = row;
    }
    showRows();
}

bool ListBoxRows::mouseDown(int x, int y, std::uint64_t ms) {
    if (screen_ == nullptr || !scroll_.mouseDown(*screen_, x, y, ms)) {
        return false;
    }
    followBar();
    return true;
}

void ListBoxRows::mouseMove(int x, int y) {
    if (screen_ != nullptr) {
        scroll_.mouseMove(*screen_, x, y);
        followBar();
    }
}

void ListBoxRows::mouseUp(int x, int y) {
    if (screen_ != nullptr) {
        scroll_.mouseUp(*screen_, x, y);
    }
}

void ListBoxRows::tick(std::uint64_t ms) {
    if (screen_ != nullptr && scroll_.tick(*screen_, ms)) {
        followBar();
    }
}

void ListBoxRows::followBar() {
    if (top_ != scroll_.value()) {
        top_ = scroll_.value();
        showRows();
    }
}

bool ListBoxRows::wheel(int x, int y, int delta) {
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
    // A row a turn, whatever its size, but not over the bar, which takes the wheel and does nothing.
    if (delta != 0 && scroll_.hit(*screen_, x, y) == GameScrollBar::Part::None && !scroll_.holdsMouse()) {
        top_ = std::clamp(top_ + (delta < 0 ? 1 : -1), 0, lastTop());
        showRows();
    }
    return true;
}

} // namespace app
