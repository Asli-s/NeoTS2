#include "change_appearance_dialog.hpp"

#include "content/ui_meters.hpp"
#include "dialog_fills.hpp"
#include "scene/outfit_catalog.hpp"

#include <algorithm>
#include <cstdint>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kScript = 0x8C159280, kHairItem = 0x8C159283;
constexpr std::uint32_t kOk = 0xA000, kCancel = 0xA001;
constexpr std::uint32_t kHairGrid = 0x8CB8C2B9, kNextPage = 0xACB8C2CF, kPreviousPage = 0x8CB8C2C3;
// The script's kHairGridNumColumns and kHairGridNumPerPage, and its item size.
constexpr std::size_t kColumns = 2, kPerPage = 8;
constexpr int kItemSize = 75;
// The colour swatches and their hair tones (Black, Brown, Blond, Red), then the custom colour's.
constexpr std::pair<std::uint32_t, const char*> kSwatches[] = {{0x8800, "00000001-0000-0000-0000-000000000000"},
                                                             {0x8801, "00000002-0000-0000-0000-000000000000"},
                                                             {0x8802, "00000003-0000-0000-0000-000000000000"},
                                                             {0x8803, "00000004-0000-0000-0000-000000000000"}};
constexpr std::uint32_t kCustomSwatch = 0x8804;
// Each colour swatch's colour picture (window 1 beside its button) and the mask picture beside it,
// whose shape cuts the colour to fit the swatch's frame.
constexpr std::uint32_t kImages = 0x499DB772, kSwatchMask = 0xCA200870, kSwatchColour = 0x1;
constexpr std::pair<std::uint32_t, std::uint32_t> kSwatchColours[] = {
    {0x8800, 0xCA2A0015}, {0x8801, 0xCA2A0010}, {0x8802, 0xCA2A0000}, {0x8803, 0xCA2A0005}, {0x8805, 0xCA2A0017}};
// The custom colour lists the hairs of the "Other" hair tone.
constexpr const char* kOtherTone = "00000006-0000-0000-0000-000000000000";
// The item's picture window, over window 1, whose own picture (a blue rounded square) shows behind
// the part's picture.
constexpr std::uint32_t kItemPictures[] = {0x6};
constexpr std::uint32_t kItemShape = 0xCF090965;  // the rounded square
constexpr std::uint32_t kItemButton = 0x5;
constexpr std::uint32_t kSimView = 0xACB91188;

bool sameHair(const ts2::scenegraph::Link& a, const ts2::scenegraph::Link& b) {
    return a.type == b.type && a.group == b.group && a.instance == b.instance && a.instanceHigh == b.instanceHigh;
}

} // namespace

bool ChangeAppearanceDialog::open(ts2::ui::Screen& screen, int width, int height) {
    close(screen);
    root_ = screen.load(kScripts, kScript, 0, 0);
    if (root_ < 0) {
        return false;
    }
    modalBefore_ = screen.modal();
    paintFills(screen, root_, kScripts, kScript);
    screen.move(root_, (width - screen.width(root_)) / 2, (height - screen.height(root_)) / 2);
    screen.raise(root_);
    cutSwatches(screen);
    screen.setModal(root_);
    showHairPage(screen);
    return true;
}

void ChangeAppearanceDialog::close(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    screen.remove(root_);
    screen.setModal(modalBefore_);
    root_ = -1;
    modalBefore_ = -1;
    hairs_.clear();
    source_ = {};
    tone_.clear();
    workingTone_.clear();
    slots_ = {};
    turn_ = 0;
    turnFrom_.reset();
    turned_ = false;
    customHairs_.clear();
    items_.clear();
    pictures_.clear();
    first_ = 0;
}

// The custom swatch can be pressed only when there are custom hairs. The Sim's colour picks its
// swatch, else the custom one.
void ChangeAppearanceDialog::setHairs(ts2::ui::Screen& screen, const std::string& tone, const ts2::scene::HairSlots& slots, HairSource source) {
    source_ = std::move(source);
    workingTone_ = tone;
    slots_ = slots;
    pictures_.clear();
    customHairs_ = source_.hairsOf ? source_.hairsOf(kOtherTone, true) : std::vector<ts2::scenegraph::Link>{};
    if (root_ >= 0) {
        if (const int custom = screen.find(root_, kCustomSwatch); custom >= 0) {
            screen.setEnabled(custom, !customHairs_.empty());
        }
    }
    const auto own = ts2::scene::catalogGuid(tone);
    const bool swatched =
        std::any_of(std::begin(kSwatches), std::end(kSwatches), [&](const auto& s) { return ts2::scene::catalogGuid(s.second) == own; });
    pickColour(screen, tone);
    if (!swatched && !customHairs_.empty()) {
        pickCustom(screen);
    }
    showSimView(screen);
}

// The game builds the Sim view's Sim anew after every change of hair or colour.
void ChangeAppearanceDialog::showSimView(ts2::ui::Screen& screen) {
    const int view = root_ >= 0 ? screen.find(root_, kSimView) : -1;
    if (view < 0 || !source_.simView || screen.width(view) <= 0 || screen.height(view) <= 0) {
        return;
    }
    turned_ = false;
    if (auto picture = source_.simView(slots_, workingTone_, static_cast<unsigned>(screen.width(view)), static_cast<unsigned>(screen.height(view)),
                                       turn_)) {
        screen.setImage(view, std::move(*picture));
    }
}

namespace {

// A window's place on the screen, in UI units.
std::array<int, 2> placeOf(const ts2::ui::Screen& screen, int window) {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen.parent(w)) {
        at[0] += screen.x(w);
        at[1] += screen.y(w);
    }
    return at;
}

} // namespace

bool ChangeAppearanceDialog::overSimView(const ts2::ui::Screen& screen, int x, int y) const {
    const int view = root_ >= 0 ? screen.find(root_, kSimView) : -1;
    if (view < 0 || !screen.visible(view)) {
        return false;
    }
    const auto [vx, vy] = placeOf(screen, view);
    return x >= vx && y >= vy && x < vx + screen.width(view) && y < vy + screen.height(view);
}

void ChangeAppearanceDialog::startTurn(int x) {
    turnFrom_ = x;
}

// The game turns the Sim 0.01 radians for every unit the pointer moves across.
void ChangeAppearanceDialog::turnTo(int x) {
    constexpr float kTurnPerUnit = 0.01f;
    if (turnFrom_ && x != *turnFrom_) {
        turn_ += static_cast<float>(x - *turnFrom_) * kTurnPerUnit;
        turnFrom_ = x;
        turned_ = true;
    }
}

std::array<int, 2> ChangeAppearanceDialog::endTurn(const ts2::ui::Screen& screen) {
    turnFrom_.reset();
    const int view = root_ >= 0 ? screen.find(root_, kSimView) : -1;
    if (view < 0) {
        return {0, 0};
    }
    const auto [vx, vy] = placeOf(screen, view);
    return {vx + screen.width(view) / 2, vy + screen.height(view) / 2};
}

void ChangeAppearanceDialog::update(ts2::ui::Screen& screen) {
    if (turned_) {
        showSimView(screen);
    }
}

// A colour lists its hairs from the first page.
void ChangeAppearanceDialog::pickColour(ts2::ui::Screen& screen, const std::string& tone) {
    tone_ = tone;
    hairs_ = source_.hairsOf ? source_.hairsOf(tone, false) : std::vector<ts2::scenegraph::Link>{};
    first_ = 0;
    std::uint32_t swatch = 0;
    for (const auto& [id, swatchTone] : kSwatches) {
        if (ts2::scene::catalogGuid(swatchTone) == ts2::scene::catalogGuid(tone)) {
            swatch = id;
        }
    }
    checkSwatch(screen, swatch);
    showHairPage(screen);
}

// As the game does, the custom hairs take the first places of the list shown and the rest of it
// stays. The colour picked stays too.
void ChangeAppearanceDialog::pickCustom(ts2::ui::Screen& screen) {
    std::vector<ts2::scenegraph::Link> hairs = customHairs_;
    for (std::size_t i = hairs.size(); i < hairs_.size(); ++i) {
        hairs.push_back(hairs_[i]);
    }
    hairs_ = std::move(hairs);
    first_ = 0;
    checkSwatch(screen, kCustomSwatch);
    showHairPage(screen);
}

void ChangeAppearanceDialog::cutSwatches(ts2::ui::Screen& screen) {
    const auto mask = screen.data().image(kImages, kSwatchMask);
    if (!mask) {
        return;
    }
    for (const auto& [swatch, image] : kSwatchColours) {
        const int button = screen.find(root_, swatch);
        const int colour = button >= 0 ? screen.find(screen.parent(button), kSwatchColour) : -1;
        auto picture = colour >= 0 ? screen.data().image(kImages, image) : std::nullopt;
        if (!picture) {
            continue;
        }
        screen.setImage(colour, ts2::content::masked(*picture, *mask));
    }
}

// The picked swatch shows pressed, the others not.
void ChangeAppearanceDialog::checkSwatch(ts2::ui::Screen& screen, std::uint32_t swatch) {
    if (root_ < 0) {
        return;
    }
    for (const auto& [id, tone] : kSwatches) {
        if (const int w = screen.find(root_, id); w >= 0) {
            screen.setChecked(w, id == swatch);
        }
    }
    if (const int w = screen.find(root_, kCustomSwatch); w >= 0) {
        screen.setChecked(w, swatch == kCustomSwatch);
    }
}

// The page's items fill the grid row by row. The next arrow shows while hairs follow the page and
// the back arrow on every page but the first, as the game shows them.
void ChangeAppearanceDialog::showHairPage(ts2::ui::Screen& screen) {
    if (root_ < 0) {
        return;
    }
    for (const int item : items_) {
        screen.remove(item);
    }
    items_.clear();
    const int grid = screen.find(root_, kHairGrid);
    if (grid >= 0) {
        const std::size_t last = std::min(hairs_.size(), first_ + kPerPage);
        for (std::size_t i = first_; i < last; ++i) {
            const std::size_t at = i - first_;
            const int item = screen.load(kScripts, kHairItem, static_cast<int>(at % kColumns) * kItemSize,
                                         static_cast<int>(at / kColumns) * kItemSize, grid);
            if (item < 0) {
                continue;
            }
            items_.push_back(item);
            const auto& hair = hairs_[i];
            auto [kept, drawn] = pictures_.try_emplace({hair.type, hair.group, hair.instance, hair.instanceHigh});
            if (drawn) {
                if (auto picture = source_.pictureOf ? source_.pictureOf(hair) : std::nullopt) {
                    // Cut to the rounded shape of the item's own picture, inside its frame.
                    const auto shape = screen.data().image(kImages, kItemShape);
                    kept->second = shape ? ts2::content::masked(ts2::content::scaled(*picture, shape->width, shape->height), *shape) : *picture;
                }
            }
            if (kept->second) {
                for (const std::uint32_t id : kItemPictures) {
                    if (const int window = screen.find(item, id); window >= 0) {
                        screen.setImage(window, *kept->second);
                    }
                }
            }
        }
    }
    markChosen(screen);
    if (const int next = screen.find(root_, kNextPage); next >= 0) {
        screen.setVisible(next, hairs_.size() > first_ + kPerPage);
    }
    if (const int previous = screen.find(root_, kPreviousPage); previous >= 0) {
        screen.setVisible(previous, first_ != 0);
    }
}

// Only the everyday hair's item shows marked, on any page and colour that lists it.
void ChangeAppearanceDialog::markChosen(ts2::ui::Screen& screen) {
    const auto& chosen = slots_[0];
    for (std::size_t at = 0; at < items_.size(); ++at) {
        if (const int button = screen.find(items_[at], kItemButton); button >= 0) {
            const std::size_t i = first_ + at;
            screen.setChecked(button, chosen && i < hairs_.size() && sameHair(hairs_[i], *chosen));
        }
    }
}

// Only the game's own colours are kept. As the game does, the everyday hair's version in the new
// colour then goes to the swimwear slot (bit 1), not the everyday one, when the Sim has both.
void ChangeAppearanceDialog::setWorkingTone(const std::string& tone) {
    if (!ts2::scene::isStockHairTone(tone)) {
        return;
    }
    workingTone_ = tone;
    constexpr std::uint32_t kEveryday = 0x1;
    if (slots_[0] && slots_[1] && source_.outfitHair) {
        if (const auto hair = source_.outfitHair(*slots_[0], kEveryday, workingTone_, false)) {
            slots_[1] = *hair;
        }
    }
}

// A hair of another colour of its own changes the Sim's colour first. After every click the
// swatch of the Sim's colour shows pressed, else the custom one. The hair then goes to every outfit
// that has a hair. An outfit with none gets none.
void ChangeAppearanceDialog::pickHair(ts2::ui::Screen& screen, const ts2::scenegraph::Link& hair) {
    const std::string tone = source_.toneOf ? source_.toneOf(hair) : std::string();
    const std::string own = ts2::scene::catalogGuid(tone);
    if (own.find_first_not_of('0') != std::string::npos && own != ts2::scene::catalogGuid(workingTone_)) {
        setWorkingTone(tone);
    }
    std::uint32_t swatch = kCustomSwatch;
    for (const auto& [id, swatchTone] : kSwatches) {
        if (ts2::scene::catalogGuid(swatchTone) == ts2::scene::catalogGuid(workingTone_)) {
            swatch = id;
        }
    }
    checkSwatch(screen, swatch);
    for (auto& slot : slots_) {
        if (slot) {
            slot = hair;
        }
    }
    markChosen(screen);
    showSimView(screen);
}

std::optional<bool> ChangeAppearanceDialog::clicked(ts2::ui::Screen& screen, int window) {
    if (root_ < 0 || window < 0) {
        return std::nullopt;
    }
    const std::uint32_t id = screen.id(window);
    if (id == kOk) {
        return true;
    }
    if (id == kCancel) {
        return false;
    }
    if (id == kNextPage && hairs_.size() > first_ + kPerPage) {
        first_ += kPerPage;
        showHairPage(screen);
    } else if (id == kPreviousPage) {
        first_ = first_ > kPerPage ? first_ - kPerPage : 0;
        showHairPage(screen);
    } else if (id == kItemButton) {
        const auto at = std::find(items_.begin(), items_.end(), screen.parent(window));
        const std::size_t i = first_ + static_cast<std::size_t>(at - items_.begin());
        if (at != items_.end() && i < hairs_.size()) {
            pickHair(screen, hairs_[i]);
        }
    } else if (id == kCustomSwatch && !customHairs_.empty()) {
        pickCustom(screen);
    } else {
        // A colour already picked is left as it is.
        for (const auto& [swatch, tone] : kSwatches) {
            if (id == swatch && (ts2::scene::catalogGuid(tone) != ts2::scene::catalogGuid(tone_) || !screen.checked(window))) {
                const bool recoloured = ts2::scene::catalogGuid(tone) != ts2::scene::catalogGuid(workingTone_);
                if (recoloured) {
                    setWorkingTone(tone);
                }
                pickColour(screen, tone);
                if (recoloured) {
                    showSimView(screen);
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace app
