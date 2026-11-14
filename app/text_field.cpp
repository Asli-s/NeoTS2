#include "text_field.hpp"

#include <algorithm>

namespace app {

namespace {

std::size_t previousChar(const std::string& s, std::size_t at) {
    while (at > 0 && (static_cast<unsigned char>(s[--at]) & 0xC0) == 0x80) {
    }
    return at;
}

std::size_t nextChar(const std::string& s, std::size_t at) {
    if (at >= s.size()) {
        return s.size();
    }
    ++at;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xC0) == 0x80) {
        ++at;
    }
    return at;
}

} // namespace

void TextField::setText(ts2::ui::Screen& screen, std::string text) {
    text_ = std::move(text);
    caret_ = std::min(caret_, text_.size());
    show(screen);
}

void TextField::focus(ts2::ui::Screen& screen, bool focused) {
    focused_ = focused;
    if (focused) {
        caret_ = text_.size();
    }
    show(screen);
}

bool TextField::typed(ts2::ui::Screen& screen, const std::string& text) {
    if (!focused_ || text_.size() + text.size() > maxBytes_) {
        return false;
    }
    std::string accepted;
    for (const char c : text) {
        if (c != '\n' || multiline_) {
            accepted.push_back(c);
        }
    }
    text_.insert(caret_, accepted);
    caret_ += accepted.size();
    show(screen);
    return !accepted.empty();
}

bool TextField::key(ts2::ui::Screen& screen, ts2::platform::Key key) {
    using ts2::platform::Key;
    if (!focused_) {
        return false;
    }
    bool edited = false;
    switch (key) {
    case Key::Backspace:
        if (caret_ > 0) {
            const std::size_t from = previousChar(text_, caret_);
            text_.erase(from, caret_ - from);
            caret_ = from;
            edited = true;
        }
        break;
    case Key::Delete:
        if (caret_ < text_.size()) {
            text_.erase(caret_, nextChar(text_, caret_) - caret_);
            edited = true;
        }
        break;
    case Key::Left: caret_ = previousChar(text_, caret_); break;
    case Key::Right: caret_ = nextChar(text_, caret_); break;
    case Key::Home: caret_ = 0; break;
    case Key::End: caret_ = text_.size(); break;
    case Key::Return:
        if (multiline_ && text_.size() < maxBytes_) {
            text_.insert(caret_, "\n");
            ++caret_;
            edited = true;
        } else {
            focused_ = false;
        }
        break;
    case Key::Escape: focused_ = false; break;
    default: return false;
    }
    show(screen);
    return edited;
}

bool TextField::holds(const ts2::ui::Screen& screen, int window) const {
    for (int w = window; w >= 0; w = screen.parent(w)) {
        if (w == window_) {
            return true;
        }
    }
    return false;
}

void TextField::show(ts2::ui::Screen& screen) const {
    if (window_ < 0) {
        return;
    }
    screen.setText(window_, text_);
    if (focused_) {
        screen.setSelection(window_, caret_, caret_);
        screen.setCaret(window_, caret_);
    } else {
        screen.setSelection(window_, 0, 0);
        screen.setCaret(window_, std::nullopt);
    }
}

} // namespace app
