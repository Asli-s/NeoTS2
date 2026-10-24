#include "pause_frame.hpp"

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kFrameScript = 0x8C364B93;
// The pieces by id: the sign, the top edge, the corners top right, the left edge, the right edge,
// the corner bottom left, the bottom edge and the corner bottom right.
constexpr std::uint32_t kFirstPiece = 0x6C364770;
enum Piece { Sign, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight };

} // namespace

void PauseFrame::attach(ts2::ui::Screen& screen, int width, int height) {
    screen_ = &screen;
    const auto script = screen.data().script(kScripts, kFrameScript);
    if (!script) {
        return;
    }
    // The script's pieces are all top-level windows.
    for (const auto& window : *script) {
        const int added = screen.addWindows({window}, 0, 0);
        const std::uint32_t id = screen.id(added);
        if (id >= kFirstPiece && id < kFirstPiece + pieces_.size()) {
            pieces_[id - kFirstPiece] = added;
        }
        screen.setVisible(added, false);
    }
    for (const int piece : pieces_) {
        if (piece < 0) {
            return;
        }
    }
    auto w = [&](Piece p) { return screen.width(pieces_[p]); };
    auto h = [&](Piece p) { return screen.height(pieces_[p]); };
    // The game lays the frame out only when the corners fit on the screen.
    if (w(Sign) + 1 + w(TopRight) > width || h(Sign) + 1 + h(BottomLeft) > height) {
        pieces_.fill(-1);
        return;
    }
    auto place = [&](Piece p, int x, int y, int pw, int ph) {
        screen.move(pieces_[p], x, y);
        screen.resize(pieces_[p], pw, ph);
    };
    place(Sign, 0, 0, w(Sign), h(Sign));
    place(TopRight, width - w(TopRight), 0, w(TopRight), h(TopRight));
    place(BottomLeft, 0, height - h(BottomLeft), w(BottomLeft), h(BottomLeft));
    place(BottomRight, width - w(BottomRight), height - h(BottomRight), w(BottomRight), h(BottomRight));
    place(Top, w(Sign), 0, width - w(TopRight) - w(Sign), h(Top));
    place(Left, 0, h(Sign), w(Left), height - h(BottomLeft) - h(Sign));
    place(Right, width - w(Right), h(TopRight), w(Right), height - h(BottomRight) - h(TopRight));
    place(Bottom, w(BottomLeft), height - h(Bottom), width - w(BottomRight) - w(BottomLeft), h(Bottom));
}

void PauseFrame::show(bool shown) {
    if (screen_ == nullptr || shown == shown_ || pieces_[Sign] < 0) {
        return;
    }
    shown_ = shown;
    for (const int piece : pieces_) {
        screen_->setVisible(piece, shown);
        if (shown) {
            screen_->raise(piece);
        }
    }
}

} // namespace app
