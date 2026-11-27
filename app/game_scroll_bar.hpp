#pragma once

#include "ui/screen.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>

namespace app {

// The game's scroll bar without arrows (IGZWinScrollbar2), upright as its text boxes and list boxes
// use it, or lying flat as the catalogs do. Its value is the first row or line shown, from 0 to the
// rows less those shown, and it shows only while they don't all fit. The track and the thumb are
// pictures drawn in three slices along the bar, their caps a quarter of the picture's length that way.
// The thumb's picture holds four frames across the bar (side by side when upright, one above the other
// when flat): disabled, normal, under the mouse and pressed. Its length is its share of the track, the
// rows shown out of all, no shorter than its two caps. A press on the track above or below the thumb
// moves a page (the rows shown less one), held it repeats every 300 ms while the mouse was last on
// that part, and the thumb follows a drag. A pressed thumb shows its pressed frame wherever the
// mouse is.
class GameScrollBar {
public:
    // Builds the pictures inside `bar`, a window the size the bar is drawn at, from the pictures
    // `track` and `thumb` of image group `images`, `flat` for a bar running left to right. False when
    // either picture is missing.
    bool attach(ts2::ui::Screen& screen, int bar, std::uint32_t images, std::uint32_t track, std::uint32_t thumb, bool flat = false) {
        bar_ = -1;
        flat_ = flat;
        // A press held on an earlier bar, closed while held, never sees its release.
        pressed_ = Part::None;
        hover_ = Part::None;
        const auto trackImage = screen.data().image(images, track);
        const auto thumbImage = screen.data().image(images, thumb);
        if (bar < 0 || !trackImage || !thumbImage || trackImage->width == 0 || thumbImage->width < kFrames) {
            return false;
        }
        bar_ = bar;
        trackLength_ = static_cast<int>(flat ? trackImage->width : trackImage->height);
        trackAcross_ = static_cast<int>(flat ? trackImage->height : trackImage->width);
        trackCap_ = trackLength_ / 4;
        thumbLong_ = static_cast<int>(flat ? thumbImage->width : thumbImage->height);
        thumbCap_ = thumbLong_ / 4;
        thumbFrame_ = static_cast<int>(flat ? thumbImage->height : thumbImage->width) / kFrames;
        const std::string trackName = std::format("{{{:08x},{:08x}}}", images, track), thumbName = std::format("{{{:08x},{:08x}}}", images, thumb);
        // The track's pieces first, so that the thumb is drawn over them.
        for (int& part : track_) {
            part = screen.addWindows({piece(trackName)}, 0, 0, bar_);
        }
        for (int& part : thumb_) {
            part = screen.addWindows({piece(thumbName)}, 0, 0, bar_);
        }
        const int ends[] = {0, trackCap_, trackLength_ - trackCap_, trackLength_};
        for (int k = 0; k < 3; ++k) {
            screen.setImagePart(track_[k], slice(ends[k], ends[k + 1], 0, trackAcross_));
        }
        layTrack(screen);
        showFrame(screen);
        screen.setVisible(bar_, false);
        return true;
    }
    // The widest of the track and a thumb frame, which the bar is drawn as wide as.
    static int width(ts2::ui::Screen& screen, std::uint32_t images, std::uint32_t track, std::uint32_t thumb) {
        const auto trackImage = screen.data().image(images, track);
        const auto thumbImage = screen.data().image(images, thumb);
        return std::max(trackImage ? static_cast<int>(trackImage->width) : 0, thumbImage ? static_cast<int>(thumbImage->width) / kFrames : 0);
    }

    // `rows` rows with `shown` of them shown from row `first`, which is kept within the range.
    void set(ts2::ui::Screen& screen, int rows, int shown, int first) {
        rows_ = rows;
        shown_ = shown;
        value_ = std::clamp(first, 0, last());
        show(screen);
    }
    int value() const { return value_; }
    bool shown(const ts2::ui::Screen& screen) const { return bar_ >= 0 && screen.visible(bar_); }

    // The part of the bar at screen (x, y): none off it or while it is hidden.
    enum class Part : std::uint8_t { None, PageUp, PageDown, Thumb };
    Part hit(const ts2::ui::Screen& screen, int x, int y) const {
        if (bar_ < 0 || !visibleOnScreen(screen)) {
            return Part::None;
        }
        const auto [bx, by] = corner(screen);
        if (x < bx || x >= bx + screen.width(bar_) || y < by || y >= by + screen.height(bar_)) {
            return Part::None;
        }
        const int at = flat_ ? x - bx : y - by;
        return at < thumbOffset_ ? Part::PageUp : at < thumbOffset_ + thumbLength_ ? Part::Thumb : Part::PageDown;
    }

    // A left press at screen (x, y) at time `ms`. True when it was on the bar.
    bool mouseDown(ts2::ui::Screen& screen, int x, int y, std::uint64_t ms) {
        const Part part = hit(screen, x, y);
        if (part == Part::None) {
            return false;
        }
        pressed_ = part;
        showFrame(screen);
        if (part == Part::Thumb) {
            grab_ = along(screen, x, y) - thumbOffset_;
            return true;
        }
        page(screen);
        pagedAt_ = ms;
        return true;
    }
    bool holdsMouse() const { return pressed_ != Part::None; }
    // Every mouse move: drags a held thumb, otherwise updates which part of the bar is under the mouse.
    void mouseMove(ts2::ui::Screen& screen, int x, int y) {
        if (bar_ < 0) {
            return;
        }
        if (pressed_ != Part::Thumb) {
            if (const Part part = hit(screen, x, y); part != hover_) {
                hover_ = part;
                showFrame(screen);
            }
            return;
        }
        const int travel = length(screen) - thumbLength_;
        if (last() <= 0 || travel <= 0) {
            return;
        }
        value_ = std::clamp((along(screen, x, y) - grab_) * last() / travel, 0, last());
        show(screen);
    }
    void mouseUp(ts2::ui::Screen& screen, int x, int y) {
        if (pressed_ == Part::None) {
            return;
        }
        pressed_ = Part::None;
        hover_ = hit(screen, x, y);
        showFrame(screen);
    }
    // Called each frame with the time in milliseconds: repeats a held press on the track. True when
    // the value changed.
    bool tick(ts2::ui::Screen& screen, std::uint64_t ms) {
        if ((pressed_ != Part::PageUp && pressed_ != Part::PageDown) || ms - pagedAt_ <= kRepeatMs) {
            return false;
        }
        pagedAt_ = ms;
        const int before = value_;
        // The repeat keeps its beat, but moves only while the mouse was last seen on the pressed part.
        if (hover_ == pressed_) {
            page(screen);
        }
        return value_ != before;
    }

private:
    static constexpr int kFrames = 4, kNormalFrame = 1, kHoverFrame = 2, kPressedFrame = 3;
    static constexpr std::uint64_t kRepeatMs = 300;

    static ts2::ui::Window piece(const std::string& image) {
        return {{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"winflag_visible", "yes"}, {"area", "(0,0,1,1)"}, {"image", image}}, {}};
    }
    int last() const { return std::max(0, rows_ - shown_); }
    int length(const ts2::ui::Screen& screen) const { return flat_ ? screen.width(bar_) : screen.height(bar_); }
    // How far along the bar screen point (x, y) is.
    int along(const ts2::ui::Screen& screen, int x, int y) const { return flat_ ? x - corner(screen)[0] : y - corner(screen)[1]; }
    // A part of a picture, from `from` to `to` along the bar and from `across0` to `across1` across it.
    std::array<int, 4> slice(int from, int to, int across0, int across1) const {
        return flat_ ? std::array<int, 4>{from, across0, to, across1} : std::array<int, 4>{across0, from, across1, to};
    }
    // Puts piece `w` at `at` along the bar, `size` long and `across` wide.
    void place(ts2::ui::Screen& screen, int w, int at, int size, int across) const {
        screen.resize(w, flat_ ? size : across, flat_ ? across : size);
        screen.move(w, flat_ ? at : 0, flat_ ? 0 : at);
    }
    // The track's caps at the bar's ends and its middle stretched between, for the bar's size now.
    void layTrack(ts2::ui::Screen& screen) const {
        const int n = length(screen);
        place(screen, track_[0], 0, trackCap_, trackAcross_);
        place(screen, track_[1], trackCap_, std::max(0, n - 2 * trackCap_), trackAcross_);
        place(screen, track_[2], n - trackCap_, trackCap_, trackAcross_);
    }
    std::array<int, 2> corner(const ts2::ui::Screen& screen) const {
        std::array<int, 2> at{0, 0};
        for (int w = bar_; w >= 0; w = screen.parent(w)) {
            at[0] += screen.x(w);
            at[1] += screen.y(w);
        }
        return at;
    }
    bool visibleOnScreen(const ts2::ui::Screen& screen) const {
        for (int w = bar_; w >= 0; w = screen.parent(w)) {
            if (!screen.visible(w)) {
                return false;
            }
        }
        return true;
    }
    void page(ts2::ui::Screen& screen) {
        const int step = std::max(1, shown_ - 1);
        value_ = std::clamp(value_ + (pressed_ == Part::PageUp ? -step : step), 0, last());
        show(screen);
    }
    void showFrame(ts2::ui::Screen& screen) {
        const int frame = pressed_ == Part::Thumb ? kPressedFrame : hover_ == Part::Thumb ? kHoverFrame : kNormalFrame;
        const int f0 = frame * thumbFrame_, f1 = f0 + thumbFrame_;
        screen.setImagePart(thumb_[0], slice(0, thumbCap_, f0, f1));
        screen.setImagePart(thumb_[1], slice(thumbCap_, thumbLong_ - thumbCap_, f0, f1));
        screen.setImagePart(thumb_[2], slice(thumbLong_ - thumbCap_, thumbLong_, f0, f1));
    }
    void show(ts2::ui::Screen& screen) {
        if (bar_ < 0) {
            return;
        }
        const bool scrolls = rows_ > shown_ && shown_ > 0;
        screen.setVisible(bar_, scrolls);
        if (!scrolls) {
            return;
        }
        layTrack(screen);
        // The thumb moves over what the track leaves, rounding to the nearest pixel.
        const int track = length(screen), hidden = last();
        thumbLength_ = std::min(track, std::max(shown_ * track / rows_, 2 * thumbCap_));
        thumbOffset_ = (hidden / 2 + value_ * (track - thumbLength_)) / hidden;
        place(screen, thumb_[0], thumbOffset_, thumbCap_, thumbFrame_);
        place(screen, thumb_[1], thumbOffset_ + thumbCap_, std::max(0, thumbLength_ - 2 * thumbCap_), thumbFrame_);
        place(screen, thumb_[2], thumbOffset_ + thumbLength_ - thumbCap_, thumbCap_, thumbFrame_);
    }

    int bar_ = -1;
    std::array<int, 3> track_{-1, -1, -1}, thumb_{-1, -1, -1};  // first cap, stretched middle, last cap
    bool flat_ = false;
    int trackLength_ = 0, trackAcross_ = 0, trackCap_ = 0;  // the track picture's size along and across the bar
    int thumbLong_ = 0, thumbCap_ = 0, thumbFrame_ = 0;     // the thumb picture's length along the bar, cap and frame width
    int thumbLength_ = 0, thumbOffset_ = 0;
    int rows_ = 0, shown_ = 0, value_ = 0;
    Part pressed_ = Part::None, hover_ = Part::None;
    int grab_ = 0;  // where in the thumb it was taken
    std::uint64_t pagedAt_ = 0;
};

} // namespace app
