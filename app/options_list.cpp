#include "options_list.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// A row is 17 high, the height of its radio dot picture plus 2 (see RowStyle).
// Rows' ids, clear of the ids of the panel's other windows.
constexpr std::uint32_t kFirstRowId = 0x00A00000;  // window id, leakcheck: ok
const std::string kRowImage = "{499db772,ac76e165}";
// The bar's pictures, those the panel's script gives its scroll bars (containerimage, thumbimage).
constexpr ListBoxRows::BarImages kBar{0x499DB772, 0xE9F50525, 0xE9F50520};

} // namespace

void OptionsList::attach(ts2::ui::Screen& screen, int list, const std::string& image) {
    attach(screen, list, RowStyle{});
    image_ = image.empty() ? kRowImage : image;
}

void OptionsList::attach(ts2::ui::Screen& screen, int list, const RowStyle& style) {
    screen_ = &screen;
    list_ = list;
    image_ = kRowImage;
    style_ = style;
    toggled_ = -1;
    // Rows made for an earlier list went with it, and their handles may now name other windows.
    rows_.clear();
    scriptRows_.clear();
    picked_ = -1;
    box_.attach(screen, list, style.layout, kBar);
}

void OptionsList::setRows(const std::vector<std::string>& rows) {
    if (screen_ == nullptr) {
        return;
    }
    for (const int row : rows_) {
        screen_->remove(row);
    }
    rows_.clear();
    scriptRows_.clear();
    picked_ = -1;
    checks_.assign(rows.size(), false);
    std::uint32_t id = 0;
    for (const std::string& text : rows) {
        if (style_.scriptInstance != 0) {
            const int root = screen_->load(style_.scriptGroup, style_.scriptInstance, 0, 0, list_);
            if (root < 0) {
                continue;
            }
            ScriptRow row;
            row.dot = screen_->find(root, 1);
            row.caption = screen_->find(root, 4);
            row.play = screen_->find(root, 2);
            row.stop = screen_->find(root, 3);
            row.hovered = screen_->find(root, 5);
            row.picked = screen_->find(root, 6);
            const int narrow = screen_->find(root, 7);
            row.captionWidth = row.caption >= 0 ? screen_->width(row.caption) : 0;
            row.pickedWidth = narrow >= 0 ? screen_->width(narrow) : row.captionWidth;
            if (row.caption >= 0) {
                screen_->setText(row.caption, text);
            }
            for (const int hidden : {row.play, row.stop, row.hovered, row.picked}) {
                if (hidden >= 0) {
                    screen_->setVisible(hidden, false);
                }
            }
            rows_.push_back(root);
            scriptRows_.push_back(row);
            continue;
        }
        ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstRowId + id++)},
                             {"area", std::format("(0,0,{},{})", style_.width > 0 ? style_.width : box_.width(), style_.height)}, {"image", image_}, {"style", "radiocheck"},
                             {"showcaption", "yes"}, {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"}, {"colorfontdisabled", "(84,85,105)"},
                             {"colorfonthilited", "(204,214,255)"}, {"gutters", style_.gutters}, {"align", style_.align}},
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
        const bool on = multi_ ? checks_[i] : static_cast<int>(i) == picked_;
        if (i < scriptRows_.size()) {
            if (scriptRows_[i].dot >= 0) {
                screen_->setChecked(scriptRows_[i].dot, on);
            }
        } else {
            screen_->setChecked(rows_[i], on);
        }
    }
}

// The picked script row shows its picked bar and play button, its caption cut to the picked width.
// The others show neither, at their caption's full width.
void OptionsList::showPicked() {
    for (std::size_t i = 0; i < scriptRows_.size(); ++i) {
        const ScriptRow& row = scriptRows_[i];
        const bool picked = static_cast<int>(i) == picked_;
        if (row.picked >= 0) {
            screen_->setVisible(row.picked, picked);
        }
        if (row.play >= 0) {
            screen_->setVisible(row.play, picked);
        }
        if (row.stop >= 0) {
            screen_->setVisible(row.stop, false);
        }
        if (row.caption >= 0) {
            screen_->resize(row.caption, picked ? row.pickedWidth : row.captionWidth, screen_->height(row.caption));
        }
    }
}

void OptionsList::mouseMove(int x, int y) {
    box_.mouseMove(x, y);
    if (screen_ == nullptr) {
        return;
    }
    // The hovered bar shows on the row under the pointer.
    for (std::size_t i = 0; i < scriptRows_.size() && i < rows_.size(); ++i) {
        const int root = rows_[i];
        const auto at = screen_->position(root);
        const bool over = screen_->visible(root) && x >= at[0] && x < at[0] + screen_->width(root) && y >= at[1] && y < at[1] + screen_->height(root);
        if (scriptRows_[i].hovered >= 0 && screen_->visible(scriptRows_[i].hovered) != over) {
            screen_->setVisible(scriptRows_[i].hovered, over);
        }
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
    for (std::size_t i = 0; i < scriptRows_.size(); ++i) {
        const ScriptRow& row = scriptRows_[i];
        if (window == row.dot) {
            checks_[i] = !checks_[i];
            toggled_ = static_cast<int>(i);
            showChecks();
            return true;
        }
        if (window == row.caption) {
            toggled_ = -1;
            picked_ = static_cast<int>(i);
            showPicked();
            return true;
        }
        // Playing a song from its row is not done yet. A click elsewhere on the row does nothing:
        // only its dot turns it on or off.
        if (window == row.play || window == row.stop || (i < rows_.size() && window == rows_[i])) {
            toggled_ = -1;
            return true;
        }
    }
    if (!scriptRows_.empty()) {
        return false;
    }
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
