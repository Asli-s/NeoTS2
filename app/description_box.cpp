#include "description_box.hpp"

#include <algorithm>
#include <format>

namespace app {

namespace {

// The bar's pictures, from the preview's script (containerimage, thumbimage). The thumb's picture
// holds four frames side by side: disabled, normal, under the mouse and pressed.
constexpr std::uint32_t kImages = 0x499DB772, kTrackImage = 0x0DDD0150, kThumbImage = 0xCCC30015;
const std::string kTrackName = "{499db772,0ddd0150}", kThumbName = "{499db772,ccc30015}";
constexpr int kThumbFrames = 4, kNormalFrame = 1, kHoverFrame = 2, kPressedFrame = 3;
// A held press on the track moves again after this long.
constexpr std::uint64_t kRepeatMs = 300;

bool continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

std::size_t characters(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) { return !continuation(c); }));
}

std::size_t previous(const std::string& text, std::size_t at) {
    while (at > 0 && continuation(text[--at])) {
    }
    return at;
}

std::size_t next(const std::string& text, std::size_t at) {
    if (at >= text.size()) {
        return text.size();
    }
    while (++at < text.size() && continuation(text[at])) {
    }
    return at;
}

ts2::ui::Window piece(const std::string& image) {
    return {{{"iid", "IGZWinGen"}, {"paint", "yes"}, {"winflag_visible", "yes"}, {"area", "(0,0,1,1)"}, {"image", image}}, {}};
}

} // namespace

void DescriptionBox::attach(ts2::ui::Screen& screen, int box, std::size_t maxCharacters) {
    box_ = box;
    maxCharacters_ = maxCharacters;
    if (box < 0 || screen.children(box).empty()) {
        return;
    }
    bar_ = screen.children(box).front();
    const auto track = screen.data().image(kImages, kTrackImage);
    const auto thumb = screen.data().image(kImages, kThumbImage);
    if (!track || !thumb || track->width == 0 || thumb->width < kThumbFrames) {
        bar_ = -1;
        return;
    }
    // The caps are a quarter of each picture's height, and the bar is as wide as its widest frame.
    trackCap_ = static_cast<int>(track->height) / 4;
    thumbCap_ = static_cast<int>(thumb->height) / 4;
    thumbFrame_ = static_cast<int>(thumb->width) / kThumbFrames;
    thumbHeight_ = static_cast<int>(thumb->height);
    const int width = std::max(static_cast<int>(track->width), thumbFrame_);
    const int trackHeight = static_cast<int>(track->height);
    screen.resize(bar_, width, screen.height(bar_));
    screen.move(bar_, screen.width(box) - width, 0);
    // The track's pieces first, so that the thumb is drawn over them.
    for (int& part : track_) {
        part = screen.addWindows({piece(kTrackName)}, 0, 0, bar_);
    }
    for (int& part : thumb_) {
        part = screen.addWindows({piece(kThumbName)}, 0, 0, bar_);
    }
    const int w = static_cast<int>(track->width);
    screen.setImagePart(track_[0], {0, 0, w, trackCap_});
    screen.setImagePart(track_[1], {0, trackCap_, w, trackHeight - trackCap_});
    screen.setImagePart(track_[2], {0, trackHeight - trackCap_, w, trackHeight});
    showThumbFrame(screen);
    const int h = screen.height(bar_);
    screen.resize(track_[0], w, trackCap_);
    screen.resize(track_[1], w, std::max(0, h - 2 * trackCap_));
    screen.move(track_[1], 0, trackCap_);
    screen.resize(track_[2], w, trackCap_);
    screen.move(track_[2], 0, h - trackCap_);
    screen.setVisible(bar_, false);
}

void DescriptionBox::setText(ts2::ui::Screen& screen, const std::string& text) {
    text_ = text;
    caret_ = text_.size();
    focused_ = false;
    pressed_ = Part::None;
    paging_ = 0;
    screen.setTopLine(box_, 0);
    show(screen);
}

void DescriptionBox::unfocus(ts2::ui::Screen& screen) {
    if (focused_) {
        focused_ = false;
        ts2::platform::stopTextInput();
    }
    show(screen);
}

int DescriptionBox::visibleLines(const ts2::ui::Screen& screen) const {
    return screen.visibleLines(box_);
}

int DescriptionBox::lastTop(const ts2::ui::Screen& screen) const {
    return std::max(0, screen.lineCount(box_) - visibleLines(screen));
}

void DescriptionBox::show(ts2::ui::Screen& screen) {
    if (box_ < 0) {
        return;
    }
    screen.setText(box_, text_);
    screen.setCaret(box_, focused_ ? std::optional<std::size_t>(caret_) : std::nullopt);
    showBar(screen);
}

void DescriptionBox::showBar(ts2::ui::Screen& screen) {
    if (bar_ < 0) {
        return;
    }
    const int lines = screen.lineCount(box_), visible = visibleLines(screen);
    const bool shown = lines > visible;
    screen.setVisible(bar_, shown);
    if (!shown) {
        screen.setTopLine(box_, 0);
        return;
    }
    // The thumb's length is its share of the track, the lines shown out of all, no shorter than its
    // two caps, and it moves over what the track leaves.
    const int track = screen.height(bar_), hidden = lines - visible;
    const int top = std::clamp(screen.topLine(box_), 0, hidden);
    screen.setTopLine(box_, top);
    thumbLength_ = std::min(track, std::max(visible * track / lines, 2 * thumbCap_));
    thumbOffset_ = (hidden / 2 + top * (track - thumbLength_)) / hidden;
    screen.resize(thumb_[0], thumbFrame_, thumbCap_);
    screen.move(thumb_[0], 0, thumbOffset_);
    screen.resize(thumb_[1], thumbFrame_, std::max(0, thumbLength_ - 2 * thumbCap_));
    screen.move(thumb_[1], 0, thumbOffset_ + thumbCap_);
    screen.resize(thumb_[2], thumbFrame_, thumbCap_);
    screen.move(thumb_[2], 0, thumbOffset_ + thumbLength_ - thumbCap_);
}

std::array<int, 2> DescriptionBox::corner(const ts2::ui::Screen& screen, int window) const {
    std::array<int, 2> at{0, 0};
    for (int w = window; w >= 0; w = screen.parent(w)) {
        at[0] += screen.x(w);
        at[1] += screen.y(w);
    }
    return at;
}

void DescriptionBox::page(ts2::ui::Screen& screen, int direction) {
    const int step = std::max(1, visibleLines(screen) - 1);
    screen.setTopLine(box_, std::clamp(screen.topLine(box_) + direction * step, 0, lastTop(screen)));
    showBar(screen);
}

DescriptionBox::Part DescriptionBox::hit(const ts2::ui::Screen& screen, int x, int y) const {
    if (bar_ < 0 || !screen.visible(box_) || !screen.visible(bar_)) {
        return Part::None;
    }
    const auto [bx, by] = corner(screen, bar_);
    if (x < bx || x >= bx + screen.width(bar_) || y < by || y >= by + screen.height(bar_)) {
        return Part::None;
    }
    const int at = y - by;
    return at < thumbOffset_ ? Part::PageUp : at < thumbOffset_ + thumbLength_ ? Part::Thumb : Part::PageDown;
}

bool DescriptionBox::inText(const ts2::ui::Screen& screen, int x, int y) const {
    const auto [ex, ey] = corner(screen, box_);
    return screen.visible(box_) && x >= ex && x < ex + screen.width(box_) && y >= ey && y < ey + screen.height(box_) &&
           hit(screen, x, y) == Part::None;
}

void DescriptionBox::showThumbFrame(ts2::ui::Screen& screen) {
    // A pressed thumb shows its pressed frame wherever the mouse is, else it lights up under the mouse.
    const int frame = pressed_ == Part::Thumb ? kPressedFrame : hover_ == Part::Thumb ? kHoverFrame : kNormalFrame;
    const int f0 = frame * thumbFrame_, f1 = f0 + thumbFrame_;
    screen.setImagePart(thumb_[0], {f0, 0, f1, thumbCap_});
    screen.setImagePart(thumb_[1], {f0, thumbCap_, f1, thumbHeight_ - thumbCap_});
    screen.setImagePart(thumb_[2], {f0, thumbHeight_ - thumbCap_, f1, thumbHeight_});
}

void DescriptionBox::setHover(ts2::ui::Screen& screen, Part part) {
    if (hover_ != part) {
        hover_ = part;
        showThumbFrame(screen);
    }
}

bool DescriptionBox::mouseDown(ts2::ui::Screen& screen, int x, int y, std::uint64_t ms) {
    if (box_ < 0 || !screen.visible(box_)) {
        return false;
    }
    if (const Part part = hit(screen, x, y); part != Part::None) {
        pressed_ = part;
        showThumbFrame(screen);
        if (part == Part::Thumb) {
            grab_ = y - corner(screen, bar_)[1] - thumbOffset_;
            return true;
        }
        paging_ = part == Part::PageUp ? -1 : 1;
        page(screen, paging_);
        pagedAt_ = ms;
        return true;
    }
    if (inText(screen, x, y)) {
        caret_ = screen.textIndexAt(box_, x, y);
        if (!focused_) {
            focused_ = true;
            ts2::platform::startTextInput();
        }
        show(screen);
        return true;
    }
    return false;
}

void DescriptionBox::mouseMove(ts2::ui::Screen& screen, int x, int y) {
    if (bar_ < 0) {
        return;
    }
    // While the thumb is dragged it keeps its frame, otherwise the part under the mouse lights up.
    if (pressed_ != Part::Thumb) {
        setHover(screen, hit(screen, x, y));
        return;
    }
    const int hidden = lastTop(screen), travel = screen.height(bar_) - thumbLength_;
    if (hidden <= 0 || travel <= 0) {
        return;
    }
    const int at = y - corner(screen, bar_)[1] - grab_;
    screen.setTopLine(box_, std::clamp(at * hidden / travel, 0, hidden));
    showBar(screen);
}

void DescriptionBox::mouseUp(ts2::ui::Screen& screen, int x, int y) {
    if (pressed_ == Part::None) {
        return;
    }
    pressed_ = Part::None;
    paging_ = 0;
    hover_ = hit(screen, x, y);
    showThumbFrame(screen);
}

bool DescriptionBox::wheel(ts2::ui::Screen& screen, int x, int y, int delta) {
    // The wheel works over the text only, not over the bar or while the bar holds the mouse. Each
    // full 100 of the wheel's delta (120 a notch) moves a line, and the rest is dropped.
    if (box_ < 0 || pressed_ != Part::None || !inText(screen, x, y)) {
        return false;
    }
    screen.setTopLine(box_, std::clamp(screen.topLine(box_) - delta / 100, 0, lastTop(screen)));
    showBar(screen);
    return true;
}

void DescriptionBox::tick(ts2::ui::Screen& screen, std::uint64_t ms) {
    // The repeat keeps its beat, but moves only while the mouse was last seen on the pressed part. A
    // thumb that slides under a still mouse doesn't stop it.
    if (paging_ != 0 && ms - pagedAt_ > kRepeatMs) {
        if (hover_ == pressed_) {
            page(screen, paging_);
        }
        pagedAt_ = ms;
    }
}

void DescriptionBox::type(ts2::ui::Screen& screen, const std::string& typed) {
    if (!focused_) {
        return;
    }
    // Characters past the box's limit are left out.
    for (std::size_t at = 0; at < typed.size() && characters(text_) < maxCharacters_;) {
        const std::size_t end = next(typed, at);
        text_.insert(caret_, typed, at, end - at);
        caret_ += end - at;
        at = end;
    }
    show(screen);
}

void DescriptionBox::key(ts2::ui::Screen& screen, ts2::platform::Key key) {
    using ts2::platform::Key;
    if (!focused_) {
        return;
    }
    switch (key) {
    case Key::Backspace:
        if (caret_ > 0) {
            const std::size_t from = previous(text_, caret_);
            text_.erase(from, caret_ - from);
            caret_ = from;
        }
        break;
    case Key::Delete:
        if (caret_ < text_.size()) {
            text_.erase(caret_, next(text_, caret_) - caret_);
        }
        break;
    case Key::Left: caret_ = previous(text_, caret_); break;
    case Key::Right: caret_ = next(text_, caret_); break;
    case Key::Home: caret_ = 0; break;
    case Key::End: caret_ = text_.size(); break;
    default: return;
    }
    show(screen);
}

} // namespace app
