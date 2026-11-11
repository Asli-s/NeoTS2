#include "phone_book_dialog.hpp"

#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"
#include "dialog_fills.hpp"
#include "names.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
constexpr std::uint32_t kScript = 0x8C159244, kCellScript = 0x8C159245;
constexpr std::uint32_t kTitle = 0xA000, kCaption = 0x6000, kLotName = 0x7000, kPickedPortrait = 0x5002;
constexpr std::uint32_t kGridTab = 0x3000, kListTab = 0x4000;
constexpr std::uint32_t kNameSort = 0x1000, kRelationshipSort = 0x2000, kChemistrySort = 0xB000;
constexpr std::uint32_t kOk = 0xAAAA, kCancel = 0xBBBB;
// The list of names and the grid view with its page arrows, cell area and scroll bar.
constexpr std::uint32_t kList = 0x8001, kGridView = 0x9000;
constexpr std::uint32_t kPrevious = 0x1004, kNext = 0x1005, kCells = 0x1006, kScrollBar = 0x1008;
// A cell's portrait, its button and its marks.
constexpr std::uint32_t kCellPortrait = 1, kCellButton = 2, kFriendMark = 3, kEnemyMark = 4, kLoveMark = 5, kCrushMark = 6;
// The portrait shapes: the cells' and the picked Sim's.
constexpr std::uint32_t kCellShape = 0xA9590255, kPickedShape = 0xA9590995;
// Cells are laid out as the game lays out its thumbnail grids: the cell's own size apart, row by
// row, as many to a row and rows to a page as fit the area.
constexpr int kCellSide = 52;
// The list's rows, from its constants table (row height, font, normal and picked text colours) and
// its mark and scroll bar pictures. Where the game puts the text and marks in a row is not read yet (UI-59).
constexpr int kRowHeight = 20, kRowTextX = 4, kLoveMarkX = 359, kFriendMarkX = 375, kMarkY = 5, kMarkSide = 10;
constexpr std::uint32_t kListFriend = 0xA9590161, kListEnemy = 0xA9590157, kListLove = 0xA9590171, kListCrush = 0xA9590166;
constexpr ListBoxRows::BarImages kListBar{kImages, 0xA9591035, 0xA9591040};
constexpr std::uint32_t kFirstRowId = 0x00C00000;  // window id, leakcheck: ok
constexpr std::array<float, 4> kRowColor{5 / 255.0f, 6 / 255.0f, 113 / 255.0f, 1}, kPickedRowColor{0, 1, 0, 1};

std::string area(int x, int y, int w, int h) {
    return std::format("({},{},{},{})", x, y, x + w, y + h);
}

std::string imageName(std::uint32_t instance) {
    return std::format("{{{:08x},{:08x}}}", kImages, instance);
}

// The game's A-Z order: first name, then surname.
int compareByName(const PhoneBookDialog::Entry& a, const PhoneBookDialog::Entry& b) {
    const int first = compareNames(a.first, b.first);
    return first != 0 ? first : compareNames(a.last, b.last);
}

// The relationship order: Sims with love or a crush first, then the highest daily plus lifetime,
// then A-Z.
int compareByRelationship(const PhoneBookDialog::Entry& a, const PhoneBookDialog::Entry& b) {
    const bool aHeart = a.love || a.crush, bHeart = b.love || b.crush;
    if (aHeart != bHeart) {
        return aHeart ? -1 : 1;
    }
    if (a.relationship != b.relationship) {
        return a.relationship > b.relationship ? -1 : 1;
    }
    return compareByName(a, b);
}

// The chemistry order: the highest chemistry key first, then A-Z.
int compareByChemistry(const PhoneBookDialog::Entry& a, const PhoneBookDialog::Entry& b) {
    if (a.chemistry != b.chemistry) {
        return a.chemistry > b.chemistry ? -1 : 1;
    }
    return compareByName(a, b);
}

ts2::ui::Window markWindow(std::uint32_t image, int x, int y) {
    return {{{"iid", "IGZWinBMP"}, {"winflag_visible", "yes"}, {"area", area(x, y, kMarkSide, kMarkSide)}, {"image", imageName(image)}}, {}};
}

} // namespace

bool PhoneBookDialog::open(ts2::ui::Screen& screen, int width, int height, const std::string& title, const std::string& caption,
                           std::vector<Entry> entries) {
    close(screen);
    root_ = screen.load(kScripts, kScript, 0, 0);
    if (root_ < 0) {
        return false;
    }
    modalBefore_ = screen.modal();
    entries_ = std::move(entries);
    caption_ = caption;
    shape_ = screen.data().image(kImages, kCellShape).value_or(ts2::tga::Pixels{});
    screen.setText(screen.find(root_, kTitle), title);
    // What the lot line under the caption holds is not known yet, and it is blank in the game's
    // picture of a call.
    screen.setText(screen.find(root_, kLotName), "");
    screen.setVisible(screen.find(root_, kScrollBar), false);
    // The panel behind the views is a picture window with no picture, filled with its colour.
    paintFills(screen, root_, kScripts, kScript);
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return compareByName(a, b) < 0; });
    makeCells(screen);
    makeRows(screen);
    picked_ = 0;
    sortEntries(screen, Sort::Name);
    showView(screen, true);
    screen.move(root_, (width - screen.width(root_)) / 2, (height - screen.height(root_)) / 2);
    screen.raise(root_);
    screen.setModal(root_);
    return true;
}

void PhoneBookDialog::close(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    screen.remove(root_);
    screen.setModal(modalBefore_);
    root_ = -1;
    modalBefore_ = -1;
    entries_.clear();
    cells_.clear();
    rows_.clear();
    picked_ = 0;
    page_ = 0;
}

std::int16_t PhoneBookDialog::picked() const {
    return picked_ < entries_.size() ? entries_[picked_].neighbor : std::int16_t{0};
}

void PhoneBookDialog::makeCells(ts2::ui::Screen& screen) {
    const int area = screen.find(root_, kCells);
    cells_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const int cell = screen.load(kScripts, kCellScript, 0, 0, area);
        cells_.push_back(cell);
        if (cell < 0) {
            continue;
        }
        const Entry& e = entries_[i];
        screen.setVisible(screen.find(cell, kFriendMark), e.friendMark);
        screen.setVisible(screen.find(cell, kEnemyMark), !e.friendMark && e.enemy);
        screen.setVisible(screen.find(cell, kLoveMark), e.love);
        screen.setVisible(screen.find(cell, kCrushMark), !e.love && e.crush);
        showPicture(screen, i);
    }
}

void PhoneBookDialog::makeRows(ts2::ui::Screen& screen) {
    const int list = screen.find(root_, kList);
    list_.attach(screen, list, kRowHeight, kListBar);
    const int width = list_.width();
    rows_.clear();
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        ts2::ui::Window row{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstRowId + i)},
                             {"area", area(0, 0, width, kRowHeight)}, {"showcaption", "yes"}, {"font", "PhoneBooksListBox"},
                             {"colorfontnormal", "(5,6,113)"}, {"colorfonthilited", "(204,214,255)"}, {"gutters", std::format("({},0,0,0)", kRowTextX)},
                             {"align", "left"}},
                            {}};
        const int made = screen.addWindows({row}, 0, 0, list);
        screen.setText(made, fullName(e.first, e.last));
        // Each mark's picture is a strip of square frames: the first is shown.
        auto mark = [&](std::uint32_t image, int x) {
            const int window = screen.addWindows({markWindow(image, x, kMarkY)}, x, kMarkY, made);
            const auto size = screen.imageSize(window);
            screen.setImagePart(window, {0, 0, size[1], size[1]});
        };
        if (e.love || e.crush) {
            mark(e.love ? kListLove : kListCrush, kLoveMarkX);
        }
        if (e.friendMark || e.enemy) {
            mark(e.friendMark ? kListFriend : kListEnemy, kFriendMarkX);
        }
        rows_.push_back(made);
    }
}

void PhoneBookDialog::showPicture(ts2::ui::Screen& screen, std::size_t entry) {
    const Entry& e = entries_[entry];
    const int portrait = cells_[entry] >= 0 ? screen.find(cells_[entry], kCellPortrait) : -1;
    if (portrait < 0) {
        return;
    }
    screen.setVisible(portrait, e.picture != nullptr);
    if (e.picture != nullptr) {
        screen.setImage(portrait, shape_.width > 0 ? ts2::content::masked(ts2::content::scaled(*e.picture, shape_.width, shape_.height), shape_)
                                                   : *e.picture);
    }
}

void PhoneBookDialog::updatePictures(ts2::ui::Screen& screen, const std::function<const ts2::tga::Pixels*(std::int16_t)>& pictureOf) {
    if (root_ < 0) {
        return;
    }
    bool pickedChanged = false;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const ts2::tga::Pixels* picture = pictureOf(entries_[i].neighbor);
        if (picture != entries_[i].picture) {
            entries_[i].picture = picture;
            showPicture(screen, i);
            pickedChanged = pickedChanged || i == picked_;
        }
    }
    if (pickedChanged) {
        showPicked(screen);
    }
}

void PhoneBookDialog::sortEntries(ts2::ui::Screen& screen, Sort sort) {
    sort_ = sort;
    const std::int16_t picked = this->picked();
    std::vector<std::size_t> order(entries_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    auto compare = sort == Sort::Name ? compareByName : sort == Sort::Relationship ? compareByRelationship : compareByChemistry;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return compare(entries_[a], entries_[b]) < 0; });
    std::vector<Entry> entries;
    std::vector<int> cells, rows;
    for (const std::size_t i : order) {
        entries.push_back(std::move(entries_[i]));
        cells.push_back(cells_[i]);
        rows.push_back(rows_[i]);
    }
    entries_ = std::move(entries);
    cells_ = std::move(cells);
    rows_ = std::move(rows);
    const auto it = std::ranges::find_if(entries_, [&](const Entry& e) { return e.neighbor == picked; });
    picked_ = it != entries_.end() ? static_cast<std::size_t>(it - entries_.begin()) : 0;
    page_ = 0;
    screen.setChecked(screen.find(root_, kNameSort), sort == Sort::Name);
    screen.setChecked(screen.find(root_, kRelationshipSort), sort == Sort::Relationship);
    screen.setChecked(screen.find(root_, kChemistrySort), sort == Sort::Chemistry);
    list_.setRows(rows_);
    showPage(screen);
    showPicked(screen);
}

void PhoneBookDialog::showView(ts2::ui::Screen& screen, bool grid) {
    grid_ = grid;
    screen.setVisible(screen.find(root_, kGridView), grid);
    screen.setVisible(screen.find(root_, kCells), grid);
    // The list's frame stays: it paints the panel behind both views.
    screen.setVisible(screen.find(root_, kList), !grid);
    screen.setChecked(screen.find(root_, kGridTab), grid);
    screen.setChecked(screen.find(root_, kListTab), !grid);
    showPage(screen);
}

void PhoneBookDialog::showPage(ts2::ui::Screen& screen) {
    const int area = screen.find(root_, kCells);
    const int columns = std::max(1, screen.width(area) / kCellSide), pageRows = std::max(1, screen.height(area) / kCellSide);
    const std::size_t perPage = static_cast<std::size_t>(columns) * static_cast<std::size_t>(pageRows);
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i] < 0) {
            continue;
        }
        const bool shown = i / perPage == page_;
        screen.setVisible(cells_[i], shown);
        const auto slot = static_cast<int>(i % perPage);
        screen.move(cells_[i], (slot % columns) * kCellSide, (slot / columns) * kCellSide);
    }
    // As on the game's thumbnail grids, the page arrows show only while they can turn a page.
    screen.setVisible(screen.find(root_, kPrevious), grid_ && page_ > 0);
    screen.setVisible(screen.find(root_, kNext), grid_ && (page_ + 1) * perPage < cells_.size());
}

void PhoneBookDialog::showPicked(ts2::ui::Screen& screen) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (cells_[i] >= 0) {
            screen.setChecked(screen.find(cells_[i], kCellButton), i == picked_);
        }
        screen.setTextColor(rows_[i], i == picked_ ? kPickedRowColor : kRowColor);
    }
    const int portrait = screen.find(root_, kPickedPortrait);
    if (picked_ >= entries_.size()) {
        screen.setText(screen.find(root_, kCaption), "");
        screen.setVisible(portrait, false);
        return;
    }
    const Entry& e = entries_[picked_];
    const std::string name = fullName(e.first, e.last);
    screen.setText(screen.find(root_, kCaption), ts2::content::formatted(caption_, {name}));
    screen.setVisible(portrait, e.picture != nullptr);
    if (e.picture != nullptr) {
        const ts2::tga::Pixels shape = screen.data().image(kImages, kPickedShape).value_or(ts2::tga::Pixels{});
        screen.setImage(portrait, shape.width > 0 ? ts2::content::masked(ts2::content::scaled(*e.picture, shape.width, shape.height), shape) : *e.picture);
    }
}

bool PhoneBookDialog::clicked(ts2::ui::Screen& screen, int window, std::optional<std::int16_t>& answer) {
    if (root_ < 0 || window < 0) {
        return false;
    }
    const std::uint32_t id = screen.id(window);
    if (id == kOk) {
        answer = picked() != 0 ? std::optional<std::int16_t>(picked()) : std::nullopt;
        return true;
    }
    if (id == kCancel) {
        answer.reset();
        return true;
    }
    if (id == kGridTab || id == kListTab) {
        showView(screen, id == kGridTab);
    } else if (id == kNameSort || id == kRelationshipSort || id == kChemistrySort) {
        sortEntries(screen, id == kNameSort ? Sort::Name : id == kRelationshipSort ? Sort::Relationship : Sort::Chemistry);
    } else if (id == kPrevious && page_ > 0) {
        --page_;
        showPage(screen);
    } else if (id == kNext) {
        ++page_;
        showPage(screen);
    } else {
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            const bool cell = cells_[i] >= 0 && window == screen.find(cells_[i], kCellButton);
            if (cell || window == rows_[i]) {
                picked_ = i;
                showPicked(screen);
                break;
            }
        }
    }
    return false;
}

} // namespace app
