#include "type_picker_dialog.hpp"

#include "dialog_fills.hpp"
#include "names.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kScript = 0x8C159243;
constexpr std::uint32_t kTitle = 0x1000, kCaption = 0x2000, kIcon = 0x3001, kList = 0x4000;
constexpr std::uint32_t kOk = 0xAAAA, kCancel = 0xBBBB;
// The list's rows, from the script's constants table: row height, font, and the normal, picked and
// highlighted text colours. Where the game puts the text in a row is not read yet (UI-57).
constexpr int kRowHeight = 21, kRowTextX = 4;
constexpr std::uint32_t kFirstRowId = 0x00C10000;  // window id, leakcheck: ok
// The list's scroll bar pictures, from the same table.
constexpr ListBoxRows::BarImages kBar{0x499DB772, 0xA9591035, 0xA9591040};
constexpr std::array<float, 4> kRowColor{5 / 255.0f, 6 / 255.0f, 113 / 255.0f, 1}, kPickedRowColor{0, 1, 0, 1};

} // namespace

bool TypePickerDialog::open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& caption,
                            std::vector<Option> options, const ts2::tga::Pixels* icon) {
    close(screen);
    root_ = screen.load(kScripts, kScript, 0, 0);
    if (root_ < 0) {
        return false;
    }
    modalBefore_ = screen.modal();
    options_ = std::move(options);
    std::stable_sort(options_.begin(), options_.end(), [](const Option& a, const Option& b) { return compareNames(a.name, b.name) < 0; });
    paintFills(screen, root_, kScripts, kScript);
    screen.setText(screen.find(root_, kTitle), title);
    screen.setText(screen.find(root_, kCaption), caption);
    if (icon != nullptr && icon->width > 0) {
        screen.setImage(screen.find(root_, kIcon), *icon);
    }
    const int list = screen.find(root_, kList);
    list_.attach(screen, list, kRowHeight, kBar);
    const int rowWidth = list_.width();
    for (std::size_t i = 0; i < options_.size(); ++i) {
        ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstRowId + i)},
                             {"area", std::format("(0,0,{},{})", rowWidth, kRowHeight)}, {"showcaption", "yes"}, {"font", "PhoneBooksListBox"},
                             {"colorfontnormal", "(5,6,113)"}, {"colorfonthilited", "(204,214,255)"}, {"gutters", std::format("({},0,0,0)", kRowTextX)},
                             {"align", "left"}},
                            {}};
        const int made = screen.addWindows({row}, 0, 0, list);
        screen.setText(made, options_[i].name);
        rows_.push_back(made);
    }
    list_.setRows(rows_);
    picked_ = 0;
    showPicked(screen);
    screen.move(root_, (width - screen.width(root_)) / 2, (height - screen.height(root_)) / 2);
    screen.raise(root_);
    screen.setModal(root_);
    return true;
}

void TypePickerDialog::close(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    screen.remove(root_);
    screen.setModal(modalBefore_);
    root_ = -1;
    modalBefore_ = -1;
    options_.clear();
    rows_.clear();
    picked_ = 0;
}

void TypePickerDialog::showPicked(ts2::ui::Screen& screen) {
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        screen.setTextColor(rows_[i], i == picked_ ? kPickedRowColor : kRowColor);
    }
}

bool TypePickerDialog::clicked(ts2::ui::Screen& screen, int window, std::optional<std::uint32_t>& answer) {
    if (root_ < 0 || window < 0) {
        return false;
    }
    const std::uint32_t id = screen.id(window);
    if (id == kOk) {
        answer = picked_ < options_.size() ? std::optional<std::uint32_t>(options_[picked_].guid) : std::nullopt;
        return true;
    }
    if (id == kCancel) {
        answer.reset();
        return true;
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (window == rows_[i]) {
            picked_ = i;
            showPicked(screen);
            break;
        }
    }
    return false;
}

} // namespace app
