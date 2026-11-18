#include "options_list.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// A row is 17 high, the height of its radio dot picture plus 2.
constexpr int kRowHeight = 17;
// Rows' ids, clear of the ids of the panel's other windows.
constexpr std::uint32_t kFirstRowId = 0x00A00000;  // window id, leakcheck: ok
const std::string kRowImage = "{499db772,ac76e165}";
// The bar's pictures, those the panel's script gives its scroll bars (containerimage, thumbimage).
constexpr ListBoxRows::BarImages kBar{0x499DB772, 0xE9F50525, 0xE9F50520};

} // namespace

void OptionsList::attach(ts2::ui::Screen& screen, int list, const std::string& image) {
    screen_ = &screen;
    list_ = list;
    image_ = image.empty() ? kRowImage : image;
    toggled_ = -1;
    // Rows made for an earlier list went with it, and their handles may now name other windows.
    rows_.clear();
    picked_ = -1;
    box_.attach(screen, list, kRowHeight, kBar);
}

void OptionsList::setRows(const std::vector<std::string>& rows) {
    if (screen_ == nullptr) {
        return;
    }
    for (const int row : rows_) {
        screen_->remove(row);
    }
    rows_.clear();
    picked_ = -1;
    checks_.assign(rows.size(), false);
    std::uint32_t id = 0;
    for (const std::string& text : rows) {
        ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstRowId + id++)},
                             {"area", std::format("(0,0,{},{})", box_.width(), kRowHeight)}, {"image", image_}, {"style", "radiocheck"},
                             {"showcaption", "yes"}, {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"colorfontdisabled", "(84,85,105)"},
                             {"colorfonthilited", "(204,214,255)"}, {"gutters", "(4,2,4,2)"}, {"align", "center"}},
                            {}};
        const int made = screen_->addWindows({row}, 0, 0, list_);
        screen_->setText(made, text);
        screen_->setEnabled(made, enabled_);
        rows_.push_back(made);
    }
    box_.setRows(rows_);
    showChecks();
}

void OptionsList::showChecks() {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        screen_->setChecked(rows_[i], multi_ ? checks_[i] : static_cast<int>(i) == picked_);
    }
}

void OptionsList::setChecks(const std::vector<bool>& checks) {
    for (std::size_t i = 0; i < checks_.size(); ++i) {
        checks_[i] = i < checks.size() && checks[i];
    }
    if (screen_ != nullptr) {
        showChecks();
    }
}

void OptionsList::pick(int row) {
    picked_ = row;
    box_.showRow(row);
    if (screen_ != nullptr) {
        showChecks();
    }
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
    const int row = static_cast<int>(found - rows_.begin());
    if (multi_) {
        checks_[row] = !checks_[row];
        toggled_ = row;
        showChecks();
    } else {
        pick(row);
    }
    return true;
}

} // namespace app
