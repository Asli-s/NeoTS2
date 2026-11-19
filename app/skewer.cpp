#include "skewer.hpp"

#include "content/ui_meters.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The puck's skewer, its anchor and its constants table.
constexpr std::uint32_t kSkewer = 0x4BDEFFAB, kAnchor = 0x4BDEFFAC, kConstants = 0x515D96DC;
// The skewer's pieces: the Sims' cap on top, the tile behind their thumbnails and the connectors
// below, then the branch's connector, cap and tile.
constexpr std::uint32_t kCap = 0x0000A001, kTile = 0x0000A002, kSimConnector = 0x0000A003, kSimPetConnector = 0x0000A004;
constexpr std::uint32_t kPetConnector = 0x0000A005, kPetCap = 0x0000A006, kPetTile = 0x0000A007;
// A Sim's thumbnail script and the branch's, its portrait group (background, shadow, portrait) and
// its frame button.
constexpr std::uint32_t kThumbnailScript = 0x49060053, kBranchThumbnailScript = 0xFEED2002;
constexpr std::uint32_t kPortraitGroup = 0x00001000, kPortrait = 0x00000001, kShadow = 0x00000002, kBackground = 0x00000003;
// The mood gradient (best mood at the top) and the background's own picture, whose shape the mood
// colour fills.
constexpr std::uint32_t kImages = 0x499DB772, kMoodGradient = 0xE9100002, kBackgroundPicture = 0xA9300115;
constexpr std::uint32_t kFrame = 0x00002000;
// The frame a Sim's thumbnail shows in place of kFrame while the Sim is away (at work, at school and so on).
constexpr std::uint32_t kAwayFrame = 0x00003000;
// The platinum mood background, shown by the script but hidden in the game for a Sim at home.
constexpr std::uint32_t kPlatinumBackground = 0x00009000;
// The yellow star marking the Sim the camera tracks.
constexpr std::uint32_t kTrackedMark = 0x00004000;
// The away icons, one per bit of the UI icon flags (person data 0x34) from bit 0 up, 0 where a bit has
// none: at work, at school, run away, sneaked out, none, on tour, hiking, jogging, banished by a witch
// and roommate walkabout.
constexpr std::uint32_t kAwayIcons[] = {0x5000, 0x6000, 0x7000, 0x8000, 0, 0x900A, 0xB00A, 0xA00A, 0xD00A, 0xD00B};
// Shown in place of at work for a roommate.
constexpr std::uint32_t kRoommateWork = 0xC00A;
// A pet's away icons, the first of its flags in this order showing alone: run away, at work,
// reinforce behavior, on tour, jogging, hiking, banished by a witch and roommate walkabout.
struct PetAwayIcon {
    std::uint32_t flag, icon;
};
constexpr PetAwayIcon kPetAwayIcons[] = {{0x4, 0x7000},  {0x1, 0x5000},  {0x10, 0xA000},  {0x20, 0x900A},
                                         {0x80, 0xA00A}, {0x40, 0xB00A}, {0x100, 0xD00A}, {0x200, 0xD00B}};
// Every icon a pet's are picked from, all hidden first.
constexpr std::uint32_t kPetIconsHidden[] = {0x5000, 0x7000, 0xA000, 0x900A, 0xA00A, 0xB00A, 0xC00A, 0xD00A, 0xD00B, 0x6000, 0x8000};
// However high the screen, a column has room for at least this many pixels of thumbnails.
constexpr int kMinColumnRoom = 100;

} // namespace

void Skewer::attach(ts2::ui::Screen& screen, int puck) {
    screen_ = &screen;
    puck_ = puck;
    skewer_ = screen.find(puck, kSkewer);
    for (const auto& line : screen.childTexts(screen.find(skewer_, kConstants))) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string name = line.substr(0, eq);
        const int value = std::atoi(line.c_str() + eq + 1);
        if (name == "kThumbnailOffsetX") {
            offsetX_ = value;
        } else if (name == "kThumbnailSpacer") {
            spacer_ = value;
        } else if (name == "kThumbnailTopMargin1") {
            topMargin_[0] = value;
        } else if (name == "kThumbnailTopMargin2") {
            topMargin_[1] = value;
        } else if (name == "kThumbnailBottomMargin1") {
            bottomMargin_[0] = value;
        } else if (name == "kThumbnailBottomMargin2") {
            bottomMargin_[1] = value;
        } else if (name == "kSkewerCeiling1") {
            ceiling_[0] = value;
        } else if (name == "kSkewerCeiling2") {
            ceiling_[1] = value;
        } else if (name == "kOverflowCeiling") {
            overflowCeiling_ = value;
        }
    }
    moodGradient_ = screen.data().image(kImages, kMoodGradient).value_or(ts2::tga::Pixels{});
    backgroundShape_ = screen.data().image(kImages, kBackgroundPicture).value_or(ts2::tga::Pixels{});
    screen.setVisible(skewer_, false);
    // The game takes the skewer out of the puck and shows it beside the puck, just under it.
    screen.detach(skewer_, puck);
}

void Skewer::setHousehold(std::vector<Member> members) {
    if (skewer_ < 0) {
        return;
    }
    auto& s = *screen_;
    for (const Thumbnail& t : thumbnails_) {
        s.remove(t.window);
    }
    thumbnails_.clear();
    for (Member& m : members) {
        // The script goes by the column: a roommate on the branch has the pets' icons.
        const int thumbnail = s.load(kScripts, m.branch ? kBranchThumbnailScript : kThumbnailScript, 0, 0, skewer_);
        if (thumbnail < 0) {
            break;
        }
        s.setVisible(s.find(thumbnail, kPlatinumBackground), false);
        s.setVisible(s.find(thumbnail, kTrackedMark), m.sim == tracked_);
        const int group = s.find(thumbnail, kPortraitGroup);
        // Over the mood colour: the portrait's soft shadow, then the portrait (the script's own
        // shadow picture would cover the mood colour), both cut to the thumbnail's rounded shape.
        s.setVisible(s.find(group, kShadow), m.portrait.has_value());
        if (m.portrait) {
            s.setImage(s.find(group, kShadow), thumbnailPicture(ts2::content::portraitSilhouette(*m.portrait)));
            s.setImage(s.find(group, kPortrait), thumbnailPicture(*m.portrait));
        }
        thumbnails_.push_back({m.sim, m.branch, m.pet, thumbnail, s.find(group, kBackground), -1});
    }
    layout();
    select(selected_);
}

void Skewer::setPortrait(std::int16_t sim, const ts2::tga::Pixels& portrait) {
    if (skewer_ < 0) {
        return;
    }
    auto& s = *screen_;
    for (const Thumbnail& t : thumbnails_) {
        if (t.sim != sim) {
            continue;
        }
        const int group = s.find(t.window, kPortraitGroup);
        s.setVisible(s.find(group, kShadow), true);
        s.setImage(s.find(group, kShadow), thumbnailPicture(ts2::content::portraitSilhouette(portrait)));
        s.setImage(s.find(group, kPortrait), thumbnailPicture(portrait));
    }
}

void Skewer::select(std::int16_t sim) {
    selected_ = sim;
    for (const Thumbnail& t : thumbnails_) {
        screen_->setChecked(screen_->find(t.window, kFrame), t.sim == sim);
        screen_->setChecked(screen_->find(t.window, kAwayFrame), t.sim == sim);
    }
}

void Skewer::setTracked(std::int16_t sim) {
    if (sim == tracked_ || screen_ == nullptr) {
        return;
    }
    tracked_ = sim;
    for (const Thumbnail& t : thumbnails_) {
        screen_->setVisible(screen_->find(t.window, kTrackedMark), t.sim == sim);
    }
}

void Skewer::setName(std::int16_t sim, const std::string& name) {
    for (const Thumbnail& t : thumbnails_) {
        if (t.sim == sim) {
            // The script marks the portrait group to show its caption as a tip,
            // under the thumbnail's button, which has none.
            screen_->setTip(screen_->find(t.window, kPortraitGroup), name);
        }
    }
}

void Skewer::setMood(std::int16_t sim, int mood) {
    for (Thumbnail& t : thumbnails_) {
        if (t.sim != sim) {
            continue;
        }
        const int row = ts2::content::moodRow(mood, static_cast<int>(moodGradient_.height));
        if (row != t.moodRow && moodGradient_.height > 0) {
            screen_->setImage(t.background, ts2::content::fillShape(backgroundShape_, ts2::content::pixel(moodGradient_, 0, static_cast<std::uint32_t>(row))));
            t.moodRow = row;
        }
    }
}

void Skewer::setAwayIcons(std::int16_t sim, std::uint16_t flags, bool roommate) {
    for (Thumbnail& t : thumbnails_) {
        if (t.sim != sim || t.awayFlags == flags) {
            continue;
        }
        t.awayFlags = flags;
        if (t.pet) {
            for (const std::uint32_t icon : kPetIconsHidden) {
                screen_->setVisible(screen_->find(t.window, icon), false);
            }
            for (const auto& [flag, icon] : kPetAwayIcons) {
                if ((flags & flag) != 0) {
                    screen_->setVisible(screen_->find(t.window, icon), true);
                    break;
                }
            }
            screen_->setVisible(screen_->find(t.window, kFrame), flags == 0);
            screen_->setVisible(screen_->find(t.window, kAwayFrame), flags != 0);
            continue;
        }
        for (std::size_t bit = 0; bit < std::size(kAwayIcons); ++bit) {
            const bool on = (flags >> bit & 1) != 0;
            if (kAwayIcons[bit] != 0) {
                screen_->setVisible(screen_->find(t.window, kAwayIcons[bit]), on && !(bit == 0 && roommate));
            }
        }
        screen_->setVisible(screen_->find(t.window, kRoommateWork), (flags & 1) != 0 && roommate);
        screen_->setVisible(screen_->find(t.window, kFrame), flags == 0);
        screen_->setVisible(screen_->find(t.window, kAwayFrame), flags != 0);
    }
}

void Skewer::setPressable(std::int16_t sim, bool pressable) {
    for (const Thumbnail& t : thumbnails_) {
        if (t.sim == sim) {
            screen_->setEnabled(screen_->find(t.window, kFrame), pressable);
            screen_->setEnabled(screen_->find(t.window, kAwayFrame), pressable);
        }
    }
}

std::optional<std::int16_t> Skewer::clicked(int window) const {
    for (const Thumbnail& t : thumbnails_) {
        if (screen_->parent(window) == t.window) {
            return t.sim;
        }
    }
    return std::nullopt;
}

ts2::tga::Pixels Skewer::thumbnailPicture(const ts2::tga::Pixels& portrait) const {
    const auto crop = ts2::content::portraitCrop(portrait);
    if (backgroundShape_.width == 0) {
        return crop;
    }
    return ts2::content::masked(ts2::content::scaled(crop, backgroundShape_.width, backgroundShape_.height), backgroundShape_);
}

void Skewer::setLiveMode(bool live) {
    if (live != live_ && screen_ != nullptr) {
        live_ = live;
        layout();
    }
}

void Skewer::setHidden(bool hidden) {
    if (hidden != hidden_ && screen_ != nullptr) {
        hidden_ = hidden;
        layout();
    }
}

void Skewer::layout() {
    auto& s = *screen_;
    const int n = static_cast<int>(thumbnails_.size());
    s.setVisible(skewer_, n > 0 && live_ && !hidden_);
    if (n == 0) {
        return;
    }
    auto piece = [&](std::uint32_t id) { return s.find(skewer_, id); };
    auto bottom = [&](int w) { return s.y(w) + s.height(w); };
    auto moveY = [&](int w, int y) { s.move(w, s.x(w), y); };
    const int cap = piece(kCap), tile = piece(kTile), joint = piece(kSimPetConnector);
    const int petConnector = piece(kPetConnector), petCap = piece(kPetCap), petTile = piece(kPetTile);

    // Each column's thumbnails, bottom first: the last member stands at the bottom.
    std::array<std::vector<int>, 2> columns;
    for (auto t = thumbnails_.rbegin(); t != thumbnails_.rend(); ++t) {
        columns[t->branch ? 1 : 0].push_back(t->window);
    }
    const int thumbnailHeight = s.height(thumbnails_[0].window);
    const int step = thumbnailHeight + spacer_;

    // A column's tile is as tall as its thumbnails, up to the room the screen leaves over the anchor.
    const int anchor = s.find(puck_, kAnchor);
    const int anchorBottom = s.y(puck_) + s.y(anchor) + s.height(anchor);
    int parentTop = 0;
    for (int w = s.parent(skewer_); w >= 0; w = s.parent(w)) {
        parentTop += s.y(w);
    }
    auto columnHeight = [&](std::size_t kind) {
        const int count = static_cast<int>(columns[kind].size());
        if (count == 0 || thumbnailHeight <= 0) {
            return 0;
        }
        const int below = kind == 0 ? s.height(piece(kSimConnector)) : s.height(joint) + s.height(petConnector);
        const int room = std::max(kMinColumnRoom, anchorBottom - parentTop - below + spacer_ - ceiling_[kind] - bottomMargin_[kind] - topMargin_[kind]);
        return std::max(0, std::min(step * count - spacer_ + bottomMargin_[kind] + topMargin_[kind], room));
    };

    // The pieces stack up from the skewer's bottom: the joint to the puck, then the Sims' tile and
    // cap over it, and the branch's connector, tile and cap over it beside them.
    moveY(joint, s.height(skewer_) - s.height(joint));
    s.resize(tile, s.width(tile), columnHeight(0));
    moveY(tile, s.y(joint) - s.height(tile));
    moveY(cap, s.y(tile) - s.height(cap));
    moveY(petConnector, s.y(joint) - s.height(petConnector));
    s.resize(petTile, s.width(petTile), columnHeight(1));
    moveY(petTile, s.y(petConnector) - s.height(petTile));
    moveY(petCap, s.y(petTile) - s.height(petCap));
    const bool branch = !columns[1].empty();
    s.setVisible(petConnector, branch);
    s.setVisible(petTile, branch);
    s.setVisible(petCap, branch);
    s.setVisible(tile, !columns[0].empty());

    // The thumbnails stand up their tile from its bottom margin. Those past its top go on in columns
    // to the right of the branch, shared by both, each from the Sims' tile's bottom up to under the
    // overflow ceiling. A column's end takes the next place there too.
    const int overflowStep = s.x(petTile) - s.x(tile);
    const int overflowBottom = bottom(tile) - bottomMargin_[0] - thumbnailHeight;
    int overflowX = offsetX_ + 2 * overflowStep, overflowY = overflowBottom;
    auto nextOverflow = [&] {
        overflowY -= step;
        if (overflowY < s.y(cap) + overflowCeiling_ - ceiling_[0] + topMargin_[0]) {
            overflowX += overflowStep;
            overflowY = overflowBottom;
        }
    };
    for (const std::size_t kind : {std::size_t{0}, std::size_t{1}}) {
        const int column = kind == 0 ? tile : petTile;
        int x = s.x(column) + offsetX_, y = bottom(column) - bottomMargin_[kind] - thumbnailHeight;
        const int top = s.y(column) + topMargin_[kind];
        bool over = false;
        for (const int w : columns[kind]) {
            s.move(w, x, y);
            if (!over) {
                y -= step;
                over = y < top;
            }
            if (over) {
                x = overflowX;
                y = overflowY;
                nextOverflow();
            }
        }
    }

    // The skewer grows to hold every piece and thumbnail, shown or not, and never shrinks. Its
    // children keep their places on the screen.
    int left = 0, top = 0, right = s.width(skewer_), bottomEdge = s.height(skewer_);
    for (const int w : s.children(skewer_)) {
        if (s.width(w) > 0 && s.height(w) > 0) {
            left = std::min(left, s.x(w));
            top = std::min(top, s.y(w));
            right = std::max(right, s.x(w) + s.width(w));
            bottomEdge = std::max(bottomEdge, bottom(w));
        }
    }
    if (left < 0 || top < 0) {
        for (const int w : s.children(skewer_)) {
            s.move(w, s.x(w) - left, s.y(w) - top);
        }
    }
    s.resize(skewer_, right - left, bottomEdge - top);
    // Its bottom left corner sits on the puck's anchor.
    s.move(skewer_, s.x(puck_) + s.x(anchor), anchorBottom - s.height(skewer_));
}

} // namespace app
