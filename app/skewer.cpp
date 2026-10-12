#include "skewer.hpp"

#include "content/ui_meters.hpp"

#include <cstdlib>
#include <string>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The puck's skewer, its anchor and its constants table.
constexpr std::uint32_t kSkewer = 0x4BDEFFAB, kAnchor = 0x4BDEFFAC, kConstants = 0x515D96DC;
// The skewer's pieces: the cap on top, the tile behind the thumbnails and the connectors below.
constexpr std::uint32_t kCap = 0x0000A001, kTile = 0x0000A002, kSimConnector = 0x0000A003, kSimPetConnector = 0x0000A004;
// A Sim's thumbnail script, its portrait group (background, shadow, portrait) and its frame button.
constexpr std::uint32_t kThumbnailScript = 0x49060053;
constexpr std::uint32_t kPortraitGroup = 0x00001000, kPortrait = 0x00000001, kShadow = 0x00000002, kBackground = 0x00000003;
// The mood gradient (best mood at the top) and the background's own picture, whose shape the mood
// colour fills.
constexpr std::uint32_t kImages = 0x499DB772, kMoodGradient = 0xE9100002, kBackgroundPicture = 0xA9300115;
constexpr std::uint32_t kFrame = 0x00002000;
// Shown by the script but hidden in the game for a Sim at home: the platinum mood background, the
// second frame and the small marker.
constexpr std::uint32_t kHiddenAtHome[] = {0x00009000, 0x00003000, 0x00004000};
// The connector's top sits this far above the tile's bottom.
constexpr int kSimConnectorOverlap = 9;

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
            topMargin_ = value;
        } else if (name == "kThumbnailBottomMargin1") {
            bottomMargin_ = value;
        }
    }
    moodGradient_ = screen.data().image(kImages, kMoodGradient).value_or(ts2::tga::Pixels{});
    backgroundShape_ = screen.data().image(kImages, kBackgroundPicture).value_or(ts2::tga::Pixels{});
    screen.setVisible(skewer_, false);
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
        const int thumbnail = s.load(kScripts, kThumbnailScript, 0, 0, skewer_);
        if (thumbnail < 0) {
            break;
        }
        for (const std::uint32_t id : kHiddenAtHome) {
            s.setVisible(s.find(thumbnail, id), false);
        }
        const int group = s.find(thumbnail, kPortraitGroup);
        // Over the mood colour: the portrait's soft shadow, then the portrait (the script's own
        // shadow picture would cover the mood colour), both cut to the thumbnail's rounded shape.
        s.setVisible(s.find(group, kShadow), m.portrait.has_value());
        if (m.portrait) {
            s.setImage(s.find(group, kShadow), thumbnailPicture(ts2::content::portraitSilhouette(*m.portrait)));
            s.setImage(s.find(group, kPortrait), thumbnailPicture(*m.portrait));
        }
        thumbnails_.push_back({m.sim, thumbnail, s.find(group, kBackground), -1});
    }
    layout();
    select(selected_);
}

void Skewer::select(std::int16_t sim) {
    selected_ = sim;
    for (const Thumbnail& t : thumbnails_) {
        screen_->setChecked(screen_->find(t.window, kFrame), t.sim == sim);
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

void Skewer::layout() {
    auto& s = *screen_;
    const int n = static_cast<int>(thumbnails_.size());
    s.setVisible(skewer_, n > 0);
    if (n == 0) {
        return;
    }
    // The thumbnails stand in a column under the cap, the first one at the bottom.
    const int capHeight = s.height(s.find(skewer_, kCap));
    const int step = s.height(thumbnails_[0].window) + spacer_;
    const int top = capHeight + topMargin_;
    for (int i = 0; i < n; ++i) {
        s.move(thumbnails_[static_cast<std::size_t>(i)].window, offsetX_, top + (n - 1 - i) * step);
    }
    // The tile runs behind them, the connector joins it to the puck.
    const int tile = s.find(skewer_, kTile);
    s.move(tile, 0, capHeight);
    s.resize(tile, s.width(tile), topMargin_ + n * step - spacer_ + bottomMargin_);
    const int tileBottom = capHeight + s.height(tile);
    const int connector = s.find(skewer_, kSimPetConnector);
    s.move(s.find(skewer_, kSimConnector), 0, tileBottom - kSimConnectorOverlap);
    s.move(connector, 0, tileBottom);
    // The skewer's bottom left corner sits on the puck's anchor.
    const int height = tileBottom + s.height(connector);
    s.resize(skewer_, s.width(skewer_), height);
    const int anchor = s.find(puck_, kAnchor);
    s.move(skewer_, s.x(anchor), s.y(anchor) + s.height(anchor) - height);
}

} // namespace app
