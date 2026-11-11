#include "notification_stack.hpp"

#include "content/ui_meters.hpp"
#include "content/ui_queue.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kLayouts = 0x8C354A82, kConstantsScript = 0xCCD02692;
// The layouts' top windows: a Sim's bubble, the system's, and the system's with buttons.
constexpr std::uint32_t kSimLayout = 0xA0000000, kSystemLayout = 0xB0000000, kSystemDialogLayout = 0xC0000000;
constexpr std::uint32_t kFrame = 0x1000, kClose = 0x2000, kPicture = 0x3000, kExtraPicture = 0x3100, kBubble = 0x4000,
                        kTail = 0x5000, kText = 0x6000, kSecondText = 0x6001, kInfoIcon = 0x7000, kIgnore = 0x8000,
                        kLearnMore = 0x9000, kConstants = 0xA000;
constexpr std::uint32_t kPictureHolder = 0x3001, kExtraHolder = 0x3101, kThumb = 0x0001, kShadow = 0x0002, kPictureConstants = 0x0004;
// The text boxes' top and bottom gutters in every layout.
constexpr int kTextGutterY = 6;
constexpr int kWidthStep = 5;  // the text box widens this much at a time until its text fits
constexpr int kPictureSide = 64, kSmallPictureSide = 40;

int constantOf(const std::vector<std::string>& lines, std::string_view name, int fallback) {
    for (const auto& line : lines) {
        if (line.size() > name.size() && line.compare(0, name.size(), name) == 0 && line[name.size()] == '=') {
            return std::atoi(line.c_str() + name.size() + 1);
        }
    }
    return fallback;
}

// A move that overshoots by 10 pixels, comes back 15 and settles 5 on, in 300, 100 and 100 ms.
std::vector<std::pair<int, double>> bounce(int d) {
    const int s = d < 0 ? -1 : 1;
    return {{d + 10 * s, 0.3}, {-15 * s, 0.1}, {5 * s, 0.1}};
}

} // namespace

int NotificationStack::load(ts2::ui::Screen& screen, std::uint8_t style) {
    const std::uint32_t layout = style == 0 ? kSimLayout : style == 1 ? kSystemLayout : kSystemDialogLayout;
    const auto script = screen.data().script(kScripts, kLayouts);
    if (!script) {
        return -1;
    }
    for (const auto& w : *script) {
        if (w.number("id") == layout) {
            return screen.addWindows({w}, 0, 0);
        }
    }
    return -1;
}

void NotificationStack::fill(ts2::ui::Screen& screen, Bubble& b, const ts2::sim::Notification& n, const Look& look) {
    // A fresh copy of the layout each time, so the text box sizes from the script's own size.
    if (b.root >= 0) {
        screen.remove(b.root);
    }
    b.root = load(screen, n.style);
    b.waiting = false;
    b.version = n.version;
    b.priority = n.priority;
    b.timeout = n.timeout;
    b.neighbor = n.neighbor;
    if (b.root < 0) {
        ts2::diag::warnOnce("notifications: the layouts' UI script is missing, nothing is shown");
        return;
    }
    const int root = b.root;
    const bool simLayout = n.style == 0;
    const int text = screen.find(root, kText);
    screen.setVisible(screen.find(root, kSecondText), false);

    // The picture: on the left of a Sim's bubble, on the right of the system's.
    // A Sim's bubble shows the whole portrait stretched over its picture, with the portrait's
    // shadow moved down and right by the picture's constants, in the portrait's own pixels. The
    // system's show the portrait's middle, or the whole portrait when the extra object is another.
    const int picture = screen.find(root, kPicture);
    const int side = simLayout ? kPictureSide : kSmallPictureSide;
    const bool extraIsOther = extraDiffers(n, look);
    const ts2::tga::Pixels* pixels = n.neighbor != 0 ? look.portrait(n.neighbor, simLayout || extraIsOther ? 0 : side)
                                     : n.objectType != 0 ? look.objectPicture(n.objectType, side)
                                                         : nullptr;
    const int holder = screen.find(picture, kPictureHolder);
    const int shadow = screen.find(holder, kShadow);
    const bool hasPicture = pixels != nullptr && pixels->width > 0;
    const bool withShadow = hasPicture && simLayout && n.neighbor != 0;
    if (hasPicture) {
        const int thumb = screen.find(holder, kThumb);
        screen.setImage(thumb, (simLayout || extraIsOther) && n.neighbor != 0 ? ts2::content::scaled(*pixels, static_cast<std::uint32_t>(screen.width(thumb)),
                                                                                     static_cast<std::uint32_t>(screen.height(thumb)))
                                                            : *pixels);
        screen.setTip(picture, n.neighbor != 0 ? look.neighborName(n.neighbor) : look.objectName(n.objectType));
    }
    if (withShadow) {
        const auto lines = screen.childTexts(screen.find(holder, kPictureConstants));
        const int dx = ts2::content::uiConstant(lines, "kShadowOffsetX").value_or(5), dy = ts2::content::uiConstant(lines, "kShadowOffsetY").value_or(5);
        const auto moved = ts2::content::placed(ts2::content::portraitSilhouette(*pixels), pixels->width, pixels->height, dx, dy);
        screen.setImage(shadow, ts2::content::scaled(moved, static_cast<std::uint32_t>(screen.width(shadow)), static_cast<std::uint32_t>(screen.height(shadow))));
    }
    screen.setVisible(shadow, withShadow);
    screen.setVisible(picture, hasPicture);
    b.waiting = (n.neighbor != 0 || n.objectType != 0) && pixels == nullptr;
    // A Sim's bubble shows the extra object, a Sim or a thing, on its right when it isn't the
    // picture.
    int rightPicture = simLayout ? -1 : (pixels != nullptr && pixels->width > 0 ? picture : -1);
    if (simLayout) {
        const int extraButton = screen.find(root, kExtraPicture);
        const std::int16_t extraNeighbor = extraIsOther ? look.neighborOf(n.extra) : 0;
        const std::uint32_t extraType = extraIsOther && extraNeighbor == 0 ? look.typeOf(n.extra) : 0;
        const ts2::tga::Pixels* extra = extraNeighbor != 0 ? look.portrait(extraNeighbor, kSmallPictureSide)
                                        : extraType != 0  ? look.objectPicture(extraType, kSmallPictureSide)
                                                          : nullptr;
        const bool shown = extra != nullptr && extra->width > 0;
        if (shown) {
            screen.setImage(screen.find(screen.find(extraButton, kExtraHolder), kThumb), *extra);
            screen.setTip(extraButton, extraNeighbor != 0 ? look.neighborName(extraNeighbor) : look.objectName(extraType));
        }
        screen.setVisible(extraButton, shown);
        b.waiting = b.waiting || ((extraNeighbor != 0 || extraType != 0) && extra == nullptr);
        rightPicture = shown ? extraButton : -1;
    }

    // The text box widens 5 pixels at a time, up to kMaxWidth for the whole bubble, until the text
    // fits its height, then grows taller. A picture on the right takes its width from both, and the
    // bubble widens over it.
    const int maxWidth = constantOf(screen.childTexts(screen.find(root, kConstants)), "kMaxWidth", screen.width(root));
    const int textX = screen.x(text);
    const int rightMargin = screen.width(root) - textX - screen.width(text);
    const int iconWidth = rightPicture >= 0 ? screen.width(rightPicture) : 0;
    const int startWidth = screen.width(text) - iconWidth;
    const int widest = std::max(startWidth, maxWidth - textX - rightMargin - iconWidth);
    const int scriptHeight = screen.height(text);
    screen.setWrapped(text, true);
    screen.setText(text, n.text);
    int width = startWidth;
    auto needed = [&] {
        screen.resize(text, width, scriptHeight);
        return screen.textHeight(text) + 2 * kTextGutterY;
    };
    while (needed() > scriptHeight && width < widest) {
        width = std::min(width + kWidthStep, widest);
    }
    // With a picture on the right, a text needing at least two lines more than fit beside it keeps
    // those lines and puts the rest in the second text box, under the first at the full width. The
    // two boxes overlap by their gutters.
    const int step = screen.textLineStep(text);
    const int lines = step > 0 ? screen.textHeight(text) / step : 0;
    const int pictureLines = rightPicture >= 0 && step > 0 ? (screen.height(rightPicture) + step - 1) / step : 0;
    const int second = screen.find(root, kSecondText);
    const auto starts = screen.wrappedLineStarts(text);
    int bottom = 0;
    if (second >= 0 && pictureLines > 0 && lines >= pictureLines + 2 && starts.size() > static_cast<std::size_t>(pictureLines)) {
        const std::size_t split = starts[static_cast<std::size_t>(pictureLines)];
        screen.setText(text, n.text.substr(0, split));
        screen.resize(text, width, pictureLines * step + 2 * kTextGutterY);
        screen.setWrapped(second, true);
        screen.resize(second, width + iconWidth, screen.height(second));
        screen.setText(second, n.text.substr(split));
        screen.resize(second, width + iconWidth, screen.textHeight(second) + 2 * kTextGutterY);
        screen.move(second, screen.x(text), screen.y(text) + screen.height(text) - 2 * kTextGutterY);
        screen.setVisible(second, true);
        bottom = screen.y(second) + screen.height(second);
    } else {
        screen.resize(text, width, std::max(scriptHeight, needed()));
        bottom = screen.y(text) + screen.height(text);
    }
    const int dw = width - startWidth, dh = bottom - (screen.y(text) + scriptHeight);
    const auto moveBy = [&](int w, int x, int y) {
        if (w >= 0) {
            screen.move(w, screen.x(w) + x, screen.y(w) + y);
        }
    };
    if (const int bubble = screen.find(root, kBubble); bubble >= 0) {
        screen.resize(bubble, screen.width(bubble) + dw, screen.height(bubble) + dh);
    }
    // The frame widens but keeps its height, and it moves down with the bubble's bottom like the
    // tail, the info icon, a Sim's picture and the X. A picture on the right stays at the top.
    if (const int frame = screen.find(root, kFrame); frame >= 0) {
        screen.resize(frame, screen.width(frame) + dw, screen.height(frame));
        moveBy(frame, 0, dh);
    }
    moveBy(screen.find(root, kTail), 0, dh);
    moveBy(screen.find(root, kInfoIcon), 0, dh);
    if (simLayout) {
        moveBy(picture, 0, dh);
    }
    moveBy(screen.find(root, kClose), dw, dh);
    moveBy(rightPicture, dw, 0);
    screen.resize(root, screen.width(root) + dw, screen.height(root) + dh);
    // Learn More and Ignore share the width in three equal gaps.
    const int learnMore = screen.find(root, kLearnMore), ignore = screen.find(root, kIgnore);
    if (learnMore >= 0 && ignore >= 0) {
        const int gap = std::max(0, screen.width(root) - screen.width(learnMore) - screen.width(ignore)) / 3;
        screen.move(learnMore, gap, screen.y(learnMore) + dh);
        screen.move(ignore, gap + screen.width(learnMore) + gap, screen.y(ignore) + dh);
    }
}

// Whether the extra object is found and is something other than the picture: another Sim, or a
// thing of another type.
bool NotificationStack::extraDiffers(const ts2::sim::Notification& n, const Look& look) {
    if (n.extra == 0) {
        return false;
    }
    if (const std::int16_t neighbor = look.neighborOf(n.extra); neighbor != 0) {
        return neighbor != n.neighbor;
    }
    const std::uint32_t type = look.typeOf(n.extra);
    return type != 0 && (n.neighbor != 0 || type != n.objectType);
}

// Whether the pictures a notification shows have loaded (or it shows none).
bool NotificationStack::picturesReady(const ts2::sim::Notification& n, const Look& look) {
    const int side = n.style == 0 ? kPictureSide : kSmallPictureSide;
    const bool other = extraDiffers(n, look);
    if ((n.neighbor != 0 && look.portrait(n.neighbor, n.style == 0 || other ? 0 : side) == nullptr) ||
        (n.neighbor == 0 && n.objectType != 0 && look.objectPicture(n.objectType, side) == nullptr)) {
        return false;
    }
    if (n.style != 0 || !other) {
        return true;
    }
    const std::int16_t neighbor = look.neighborOf(n.extra);
    const std::uint32_t type = neighbor == 0 ? look.typeOf(n.extra) : 0;
    return neighbor != 0 ? look.portrait(neighbor, kSmallPictureSide) != nullptr : type == 0 || look.objectPicture(type, kSmallPictureSide) != nullptr;
}

void NotificationStack::startMoves(Bubble& b, std::vector<Move> moves) {
    b.moves = std::move(moves);
    b.moveStarted = now_;
}

bool NotificationStack::moving() const {
    const auto busy = [](const Bubble& b) { return !b.moves.empty(); };
    return std::ranges::any_of(shown_, busy) || (sliding_ && busy(*sliding_));
}

// Moves each bubble along its moves, and settles those whose moves have ended.
void NotificationStack::animate(ts2::ui::Screen& screen) {
    auto step = [&](Bubble& b) {
        if (b.root < 0 || b.moves.empty()) {
            return;
        }
        double t = now_ - b.moveStarted, x = b.fromX, y = b.fromY;
        bool done = true;
        for (const Move& m : b.moves) {
            const double part = m.seconds <= 0 ? 1 : std::clamp(t / m.seconds, 0.0, 1.0);
            x += m.dx * part;
            y += m.dy * part;
            t -= m.seconds;
            if (part < 1) {
                done = false;
                break;
            }
        }
        screen.move(b.root, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
        if (done) {
            b.moves.clear();
            b.fromX = screen.x(b.root);
            b.fromY = screen.y(b.root);
        }
    };
    for (Bubble& b : shown_) {
        step(b);
    }
    if (sliding_) {
        step(*sliding_);
    }
}

// Each shown bubble's place, top first: the right edge kRightOffset from the screen's, each
// kNotificationMargin below the one above. A bubble not there bounces to it.
void NotificationStack::layOut(ts2::ui::Screen& screen) {
    int y = constants_->topOffset;
    for (Bubble& b : shown_) {
        if (b.root < 0) {
            continue;
        }
        const int x = screenWidth_ - constants_->rightOffset - screen.width(b.root);
        const int dx = x - screen.x(b.root), dy = y - screen.y(b.root);
        if ((dx != 0 || dy != 0) && b.moves.empty()) {
            b.fromX = screen.x(b.root);
            b.fromY = screen.y(b.root);
            std::vector<Move> moves;
            const auto bx = bounce(dx), by = bounce(dy);
            for (std::size_t i = 0; i < bx.size(); ++i) {
                moves.push_back({dx != 0 ? bx[i].first : 0, dy != 0 ? by[i].first : 0, bx[i].second});
            }
            startMoves(b, std::move(moves));
        }
        y += screen.height(b.root) + constants_->margin;
    }
}

std::vector<std::int16_t> NotificationStack::update(ts2::ui::Screen& screen, const std::vector<ts2::sim::Notification>& shown,
                                                    double realSeconds, bool paused, const Look& look) {
    if (!constants_) {
        Constants c;
        if (const auto script = screen.data().script(kScripts, kConstantsScript); script && !script->empty()) {
            std::vector<std::string> lines;
            for (const auto& child : script->front().children) {
                lines.push_back(child.text("caption"));
            }
            c.topOffset = constantOf(lines, "kTopOffset", c.topOffset);
            c.rightOffset = constantOf(lines, "kRightOffset", c.rightOffset);
            c.margin = constantOf(lines, "kNotificationMargin", c.margin);
        }
        constants_ = c;
    }
    now_ += realSeconds;
    clock_ += paused ? 0 : realSeconds;
    bool changed = screen.size()[0] != screenWidth_;
    screenWidth_ = screen.size()[0];

    // Bubbles whose notification is gone leave at once. Changed ones are filled anew and their
    // countdown starts again.
    auto sync = [&](std::vector<Bubble>& list, bool onStack) {
        std::erase_if(list, [&](Bubble& b) {
            const auto it = std::ranges::find(shown, b.id, &ts2::sim::Notification::id);
            if (it == shown.end()) {
                if (b.root >= 0) {
                    screen.remove(b.root);
                }
                changed = true;
                return true;
            }
            // A picture still loading goes in once it has loaded.
            if (it->version != b.version || (b.waiting && picturesReady(*it, look))) {
                const bool fresh = it->version != b.version;
                const int x = b.root >= 0 ? screen.x(b.root) : 0, y = b.root >= 0 ? screen.y(b.root) : 0;
                fill(screen, b, *it, look);
                screen.move(b.root, x, y);
                if (fresh) {
                    b.shownAt = clock_;
                    changed = changed || onStack;
                }
            }
            return false;
        });
    };
    sync(shown_, true);
    sync(pending_, false);
    if (sliding_ && std::ranges::find(shown, sliding_->id, &ts2::sim::Notification::id) == shown.end()) {
        screen.remove(sliding_->root);
        sliding_.reset();
        state_ = 0;
    }
    for (const auto& n : shown) {
        const auto has = [&](const Bubble& b) { return b.id == n.id; };
        if (std::ranges::any_of(shown_, has) || std::ranges::any_of(pending_, has) || (sliding_ && has(*sliding_))) {
            continue;
        }
        Bubble b;
        b.id = n.id;
        fill(screen, b, n, look);
        b.queuedAt = now_;
        screen.move(b.root, screenWidth_, constants_->topOffset);  // off the screen until it slides in
        pending_.push_back(std::move(b));
    }
    const auto byPriority = [](const Bubble& a, const Bubble& b) { return a.priority > b.priority; };
    std::ranges::stable_sort(pending_, byPriority);

    animate(screen);
    // The game shows a bubble with its pictures at once. Ours waits for a portrait read from a
    // character file, but not for ever.
    constexpr double kMostPictureWait = 2;
    if (state_ == 0 && !moving() && !pending_.empty() && (!pending_.front().waiting || now_ - pending_.front().queuedAt >= kMostPictureWait)) {
        // Push the stack down by the new bubble's height, then slide it in.
        sliding_ = std::move(pending_.front());
        pending_.erase(pending_.begin());
        const int push = screen.height(sliding_->root) + constants_->margin;
        for (Bubble& b : shown_) {
            b.fromX = screen.x(b.root);
            b.fromY = screen.y(b.root);
            std::vector<Move> moves;
            for (const auto& [d, s] : bounce(push)) {
                moves.push_back({0, d, s});
            }
            startMoves(b, std::move(moves));
        }
        state_ = 1;
    } else if (state_ == 1 && !moving()) {
        Bubble& b = *sliding_;
        screen.move(b.root, screenWidth_, constants_->topOffset);
        b.fromX = screenWidth_;
        b.fromY = constants_->topOffset;
        std::vector<Move> moves;
        for (const auto& [d, s] : bounce(-constants_->rightOffset - screen.width(b.root))) {
            moves.push_back({d, 0, s});
        }
        startMoves(b, std::move(moves));
        state_ = 2;
    } else if (state_ == 2 && sliding_->moves.empty()) {
        sliding_->shownAt = clock_;
        shown_.insert(shown_.begin(), std::move(*sliding_));
        sliding_.reset();
        state_ = 0;
    }
    if (state_ == 0 && changed) {
        std::ranges::stable_sort(shown_, byPriority);
    }
    if (state_ == 0 && !moving()) {
        layOut(screen);
    }

    std::vector<std::int16_t> expired;
    for (const Bubble& b : shown_) {
        if (b.timeout != 0 && std::floor(clock_ - b.shownAt) >= b.timeout) {
            expired.push_back(b.id);
        }
    }
    return expired;
}

std::optional<NotificationStack::Click> NotificationStack::clicked(ts2::ui::Screen& screen, int window) const {
    const std::uint32_t id = screen.id(window);
    int top = window;
    while (screen.parent(top) >= 0) {
        top = screen.parent(top);
    }
    const auto find = [&](const std::vector<Bubble>& list) -> const Bubble* {
        const auto it = std::ranges::find(list, top, &Bubble::root);
        return it != list.end() ? &*it : nullptr;
    };
    const Bubble* b = find(shown_);
    b = b != nullptr ? b : (sliding_ && sliding_->root == top ? &*sliding_ : nullptr);
    if (b == nullptr) {
        return std::nullopt;
    }
    Click click;
    if (id == kClose || id == kIgnore) {
        click.close = b->id;
    } else if (id == kPicture && b->neighbor != 0) {
        click.neighbor = b->neighbor;
    }
    return click;
}

void NotificationStack::clear(ts2::ui::Screen& screen) {
    for (auto* list : {&shown_, &pending_}) {
        for (const Bubble& b : *list) {
            screen.remove(b.root);
        }
        list->clear();
    }
    if (sliding_) {
        screen.remove(sliding_->root);
        sliding_.reset();
    }
    state_ = 0;
}

} // namespace app
