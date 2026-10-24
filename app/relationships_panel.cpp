#include "relationships_panel.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kImages = 0x499DB772;
constexpr std::uint32_t kThumbnailScript = 0x49065005;

// The panel: its collapsed (one row) and expanded (two rows) halves, each with its thumbnail area,
// page buttons, expand or collapse button and art. The filter buttons sit in both.
constexpr std::uint32_t kCollapsed = 0x1000, kExpanded = 0x2000;
constexpr std::uint32_t kCollapsedArea = 0x1006, kExpandedArea = 0x2006;
constexpr std::uint32_t kPrevious[] = {0x1004, 0x2004}, kNext[] = {0x1005, 0x2005};
constexpr std::uint32_t kExpand = 0x1009, kCollapse = 0x2009;
constexpr std::uint32_t kMaximizedArt[] = {0x10A1, 0x10A3, 0x20A1, 0x20A3}, kMinimizedArt[] = {0x1001, 0x1003, 0x2001, 0x2003};
constexpr std::size_t kCollapsedPage = 6, kExpandedPage = 12, kColumns = 6;
constexpr int kThumbWidth = 62, kThumbHeight = 108;
// Filter buttons by filter (1 household, 2 family, 3 friends, 4 chemistry, 5 all).
constexpr std::uint32_t kFilters[] = {0x0BA16776, 0x0BA16779, 0x0BA16778, 0x0BA16775, 0x0BA16777};
constexpr int kChemistryFilter = 4, kAllFilter = 5;

// A thumbnail's windows: the daily and lifetime meters (fill 3, hidden gradient 4), their numbers,
// the portrait, the icon slots, the family icon, the enemy and contact overlays, chemistry and pet
// icons.
constexpr std::uint32_t kDailyMeter = 0xA1, kLifetimeMeter = 0xA2, kMeterFill = 3;
constexpr std::uint32_t kDailyNumber = 0xA9, kLifetimeNumber = 0xAA, kPortrait = 0xA3, kFrame = 0xA4;
constexpr std::uint32_t kLoveIcon = 0xA5, kFriendIcon = 0xA6, kCoupleIcon = 0xA7, kBestIcon = 0xA8, kFamilyIcon = 0xAB;
constexpr std::uint32_t kEnemy = 0xF0, kContact = 0xFA;
constexpr std::uint32_t kChemistryIcons[] = {0xC0, 0xC1, 0xC2, 0xC3, 0xC4};
constexpr std::uint32_t kPackIcon = 0xC5, kMasterIcon = 0xC6, kPetFamilyIcon = 0xC7, kPetFriendIcon = 0xC8, kMaleIcon = 0xC9,
                        kFemaleIcon = 0xCA, kMineIcon = 0xCB, kPetEnemyIcon = 0xCC, kPetBestFriendIcon = 0xCD;
constexpr std::uint32_t kGradient = 0xA9300047, kDailyFillPicture = 0xAB201735, kLifetimeFillPicture = 0xAB201745;
constexpr std::uint32_t kIconStrip = 0xA9300037, kEnemyStrip = 0xAB201698, kContactStrip = 0xAB201699;
constexpr int kIconFrames = 12, kOverlayFrames = 24, kOverlayWidth = 50;

// Relationship flags (value 1).
bool flag(const RelationshipsPanel::Person& p, int bit) { return (p.flags & (1 << bit)) != 0; }
constexpr float kShownAbove = 0.001f;

bool listed(const RelationshipsPanel::Person& p, int filter) {
    if (p.skip || !flag(p, 15)) {
        return false;
    }
    switch (filter) {
    case 1: return p.household;
    case 2: return p.family;
    case 3: return flag(p, 4) || flag(p, 5) || p.contact > kShownAbove;
    case 4: return p.chemistry >= 5;
    default: return true;
    }
}

// The overlay frame for a value 0..1 (1 the first frame).
int overlayFrame(float v) { return std::clamp(static_cast<int>((1.0f - v) * kOverlayFrames), 0, kOverlayFrames - 1); }

} // namespace

void RelationshipsPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    gradient_ = screen.data().image(kImages, kGradient).value_or(ts2::tga::Pixels{});
    dailyFill_ = screen.data().image(kImages, kDailyFillPicture).value_or(ts2::tga::Pixels{});
    lifetimeFill_ = screen.data().image(kImages, kLifetimeFillPicture).value_or(ts2::tga::Pixels{});
    icons_ = screen.data().image(kImages, kIconStrip).value_or(ts2::tga::Pixels{});
    enemies_ = screen.data().image(kImages, kEnemyStrip).value_or(ts2::tga::Pixels{});
    contacts_ = screen.data().image(kImages, kContactStrip).value_or(ts2::tga::Pixels{});
    makeThumbnails(screen.find(screen.find(content, kCollapsed), kCollapsedArea), kCollapsedPage, collapsed_);
    makeThumbnails(screen.find(screen.find(content, kExpanded), kExpandedArea), kExpandedPage, expandedThumbs_);
    layout();
}

void RelationshipsPanel::makeThumbnails(int area, std::size_t count, std::vector<Thumbnail>& out) {
    auto& s = *screen_;
    for (std::size_t i = 0; i < count && area >= 0; ++i) {
        const int x = static_cast<int>(i % kColumns) * kThumbWidth, y = static_cast<int>(i / kColumns) * kThumbHeight;
        Thumbnail t;
        t.window = s.load(kScripts, kThumbnailScript, x, y, area);
        if (t.window < 0) {
            break;
        }
        t.daily = s.find(s.find(t.window, kDailyMeter), kMeterFill);
        t.lifetime = s.find(s.find(t.window, kLifetimeMeter), kMeterFill);
        s.setVisible(t.window, false);
        out.push_back(t);
    }
}

bool RelationshipsPanel::clicked(int window) {
    if (screen_ == nullptr || content_ < 0) {
        return false;
    }
    auto& s = *screen_;
    const std::uint32_t id = s.id(window);
    bool inside = false;
    for (int w = window; w >= 0 && !inside; w = s.parent(w)) {
        inside = w == content_;
    }
    if (!inside) {
        return false;
    }
    bool handled = true;
    if (id == kPrevious[0] || id == kPrevious[1]) {
        page_ = std::max(0, page_ - 1);
    } else if (id == kNext[0] || id == kNext[1]) {
        ++page_;
    } else if (id == kExpand || id == kCollapse) {
        expanded_ = id == kExpand;
    } else if (const auto* f = std::find(std::begin(kFilters), std::end(kFilters), id); f != std::end(kFilters)) {
        filter_ = static_cast<int>(f - std::begin(kFilters)) + 1;
        page_ = 0;
    } else {
        handled = false;
    }
    if (handled) {
        layout();
    }
    return handled;
}

void RelationshipsPanel::setMaximized(bool maximized) {
    if (screen_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kMaximizedArt); ++i) {
        screen_->setVisible(screen_->find(content_, kMaximizedArt[i]), maximized);
        screen_->setVisible(screen_->find(content_, kMinimizedArt[i]), !maximized);
    }
}

void RelationshipsPanel::show(std::vector<Person> people, std::int16_t species) {
    if (screen_ == nullptr) {
        return;
    }
    std::erase_if(people, [&](const Person& p) { return !listed(p, filter_); });
    if (filter_ == kChemistryFilter) {
        std::stable_sort(people.begin(), people.end(), [](const Person& a, const Person& b) { return a.chemistryKey > b.chemistryKey; });
    } else {
        std::stable_sort(people.begin(), people.end(), [](const Person& a, const Person& b) {
            if (a.sortGroup != b.sortGroup) return a.sortGroup < b.sortGroup;
            if (a.lifetime != b.lifetime) return a.lifetime > b.lifetime;
            if (const int c = a.sortName.compare(b.sortName); c != 0) return c < 0;
            return a.sortName2 < b.sortName2;
        });
    }
    // The page stays while the list keeps its size.
    if (people.size() != listed_.size()) {
        page_ = 0;
    }
    listed_ = std::move(people);
    species_ = species;
    layout();
}

void RelationshipsPanel::layout() {
    auto& s = *screen_;
    s.setVisible(s.find(content_, kCollapsed), !expanded_);
    s.setVisible(s.find(content_, kExpanded), expanded_);
    for (std::size_t i = 0; i < std::size(kFilters); ++i) {
        for (const std::uint32_t half : {kCollapsed, kExpanded}) {
            s.setChecked(s.find(s.find(content_, half), kFilters[i]), static_cast<int>(i) + 1 == filter_);
        }
    }
    auto& thumbs = expanded_ ? expandedThumbs_ : collapsed_;
    const std::size_t pageSize = thumbs.empty() ? 1 : thumbs.size();
    // A page past the end goes back to the last one with Sims on it.
    std::size_t first = static_cast<std::size_t>(page_) * pageSize;
    while (first != 0 && first >= listed_.size()) {
        --page_;
        first -= pageSize;
    }
    for (std::size_t i = 0; i < thumbs.size(); ++i) {
        const bool shown = first + i < listed_.size();
        s.setVisible(thumbs[i].window, shown);
        if (shown) {
            showThumbnail(thumbs[i], listed_[first + i], species_);
        }
    }
    // The page buttons show only when they lead somewhere.
    const int half = s.find(content_, expanded_ ? kExpanded : kCollapsed);
    const std::size_t at = expanded_ ? 1 : 0;
    s.setVisible(s.find(half, kPrevious[at]), page_ != 0);
    s.setVisible(s.find(half, kNext[at]), first + pageSize < listed_.size());
}

void RelationshipsPanel::showThumbnail(Thumbnail& t, const Person& p, std::int16_t species) {
    if (t.shown == p && t.shownFilter == filter_ && t.shownSpecies == species) {
        return;
    }
    t.shown = p;
    t.shownFilter = filter_;
    t.shownSpecies = species;
    auto& s = *screen_;
    auto window = [&](std::uint32_t id) { return s.find(t.window, id); };
    showMeter(t.daily, p.daily, t.dailyRow, dailyFill_);
    showMeter(t.lifetime, p.lifetime, t.lifetimeRow, lifetimeFill_);
    for (const auto& [id, value] : {std::pair{kDailyNumber, p.daily}, std::pair{kLifetimeNumber, p.lifetime}}) {
        const std::string text = std::to_string(value);
        if (s.text(window(id)) != text) {
            s.setText(window(id), text);
        }
    }
    s.setVisible(window(kPortrait), p.picture != nullptr);
    if (p.picture) {
        s.setImage(window(kPortrait), *p.picture);
    }
    s.setChecked(window(kFrame), false);

    // A Sim's thumbnail shows the people icons. A pet's (or a pet's own list) shows the pet icons.
    const bool petMode = p.pet || species != 0;
    auto icon = [&](std::uint32_t id, int frame) {
        s.setVisible(window(id), frame >= 0 && !petMode);
        if (frame >= 0 && !petMode) {
            showFrame(window(id), icons_, frame, icons_.width / kIconFrames);
        }
    };
    icon(kLoveIcon, flag(p, 0) ? 1 : -1);
    icon(kFriendIcon, flag(p, 4) || p.contact > kShownAbove ? 7 : -1);
    icon(kCoupleIcon, flag(p, 3) ? 5 : flag(p, 2) ? 6 : flag(p, 6) ? 10 : flag(p, 1) ? 0 : -1);
    icon(kBestIcon, p.coins ? 11 : flag(p, 5) ? 9 : flag(p, 7) ? 8 : -1);
    s.setVisible(window(kFamilyIcon), p.family && !p.pet && !petMode);

    const bool enemy = p.enemy > kShownAbove, contact = !enemy && p.contact > kShownAbove;
    s.setVisible(window(kEnemy), enemy);
    s.setVisible(window(kContact), contact);
    if (enemy) {
        showFrame(window(kEnemy), enemies_, overlayFrame(p.enemy), kOverlayWidth);
    } else if (contact) {
        showFrame(window(kContact), contacts_, overlayFrame(p.contact), kOverlayWidth);
    }

    // Chemistry shows in the chemistry and all filters: levels 1-3 bad, 4 none, then mild, medium, strong.
    const bool chemistry = filter_ == kChemistryFilter || filter_ == kAllFilter;
    const int level = p.chemistry <= 0 || p.chemistry > 7 ? -1 : p.chemistry <= 3 ? 0 : p.chemistry - 3;
    for (int i = 0; i < 5; ++i) {
        s.setVisible(window(kChemistryIcons[i]), chemistry && i == level);
    }

    s.setVisible(window(kPackIcon), petMode && flag(p, 9) && (species == 1 || species == 2));
    s.setVisible(window(kPetFamilyIcon), petMode && p.family && p.pet);
    s.setVisible(window(kPetFriendIcon), petMode && flag(p, 12) && species == 0 && p.pet);
    s.setVisible(window(kPetBestFriendIcon), petMode && flag(p, 13) && species == 0 && p.pet);
    s.setVisible(window(kMasterIcon), petMode && flag(p, 10));
    s.setVisible(window(kMineIcon), petMode && flag(p, 11) && species == 3);
    s.setVisible(window(kPetEnemyIcon), petMode && flag(p, 7));
    s.setVisible(window(kMaleIcon), petMode && p.pet && !p.female);
    s.setVisible(window(kFemaleIcon), petMode && p.pet && p.female);
}

void RelationshipsPanel::showMeter(int fill, int value, int& shownRow, const ts2::tga::Pixels& fillPicture) {
    // Every row of the fill takes one row of the gradient, picked by the value.
    const int row = ts2::content::meterRow(value, -100, 100, static_cast<int>(gradient_.height));
    if (fill < 0 || row == shownRow || gradient_.height == 0) {
        return;
    }
    shownRow = row;
    screen_->setImage(fill, ts2::content::paintBar(fillPicture, gradient_, row));
}

void RelationshipsPanel::showFrame(int window, const ts2::tga::Pixels& strip, int frame, int frameWidth) {
    if (window < 0 || frameWidth <= 0) {
        return;
    }
    const auto w = static_cast<std::uint32_t>(screen_->width(window)), h = static_cast<std::uint32_t>(screen_->height(window));
    screen_->setImage(window, ts2::content::placed(strip, w, h, -frame * frameWidth, 0));
}

} // namespace app
