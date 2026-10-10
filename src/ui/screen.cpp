#include "ui/screen.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace ts2::ui {

namespace {

// Which frame of a button's four-frame image strip each state shows (as the game's main menu
// shows them: idle, hovered and pressed); disabled takes the one left, a bare outline.
constexpr int kButtonFrame[4] = {1, 3, 2, 0};  // normal, hover, pressed, disabled

std::array<float, 4> color(const std::vector<std::int64_t>& rgb) {
    if (rgb.size() < 3) {
        return {1, 1, 1, 1};
    }
    return {static_cast<float>(rgb[0]) / 255, static_cast<float>(rgb[1]) / 255, static_cast<float>(rgb[2]) / 255, 1};
}

} // namespace

Screen::Screen(content::UiData& data, Fonts& fonts) : data_(data), fonts_(fonts) {
    white_ = texture({1, 1, {255, 255, 255, 255}});
}

int Screen::texture(tga::Pixels pixels) {
    textures_.push_back(std::move(pixels));
    changed_.push_back(static_cast<int>(textures_.size()) - 1);
    return static_cast<int>(textures_.size()) - 1;
}

int Screen::add(const ui::Window& source, int parent) {
    Window w;
    const std::string iid = source.text("iid");
    w.kind = iid == "IGZWinGen" ? Kind::Generic
             : iid == "IGZWinBMP" ? Kind::Bitmap
             : iid == "IGZWinBtn" ? Kind::Button
             : iid == "IGZWinText" ? Kind::Text
             : iid == "IGZWinTextEdit" ? Kind::TextEdit
             : iid == "IGZWinFlatRect" ? Kind::Rect
                                      : Kind::Other;
    w.id = source.number("id").value_or(0);
    w.parent = parent;
    const auto area = source.list("area");
    if (area.size() == 4) {
        w.x = static_cast<int>(area[0]);
        w.y = static_cast<int>(area[1]);
        w.w = static_cast<int>(area[2] - area[0]);
        w.h = static_cast<int>(area[3] - area[1]);
    }
    w.visible = source.flag("winflag_visible");
    const auto image = source.list("image");
    if (image.size() >= 2) {
        const auto group = static_cast<std::uint32_t>(image[image.size() - 2]), instance = static_cast<std::uint32_t>(image.back());
        const std::uint64_t key = static_cast<std::uint64_t>(group) << 32 | instance;
        const auto known = images_.find(key);
        if (known != images_.end()) {
            w.image = known->second;
        } else if (auto px = data_.image(group, instance)) {
            w.image = texture(std::move(*px));
            images_[key] = w.image;
        }
    }
    const auto rect = source.list("imagerect");
    if (rect.size() == 4) {
        w.imageRect = {static_cast<int>(rect[0]), static_cast<int>(rect[1]), static_cast<int>(rect[2]), static_cast<int>(rect[3])};
    }
    w.edge = source.flag("edgeimage");
    const auto res = source.list("captionres");
    if (res.size() == 2 && res[0] != 0) {
        w.text = data_.text(static_cast<std::uint32_t>(res[0]), static_cast<std::uint32_t>(res[1]));
    } else if (w.kind == Kind::Text) {
        w.text = source.text("caption");
    } else if (w.kind == Kind::TextEdit) {
        w.text = source.text("initvalue");
    }
    w.font = source.text("font");
    w.align = source.text("align");
    w.textColor = color(source.list(w.kind == Kind::Text ? "forecolor" : "colorfontnormal"));
    w.paint = w.kind != Kind::Generic || source.flag("paint");
    const auto gutters = source.list("gutters");
    if (gutters.size() >= 2) {
        w.gutters = {static_cast<int>(gutters[0]), static_cast<int>(gutters[1])};
    }
    w.wrapped = source.flag("wrapped");
    w.showCaption = w.kind != Kind::Button || source.flag("showcaption");
    if (w.kind == Kind::Button && source.flag("tips")) {
        const auto tipres = source.list("tipres");
        w.tip = tipres.size() == 2 && tipres[0] != 0 ? data_.text(static_cast<std::uint32_t>(tipres[0]), static_cast<std::uint32_t>(tipres[1]))
                                                    : source.text("tiptext");
    }

    const int index = static_cast<int>(windows_.size());
    windows_.push_back(std::move(w));
    if (parent >= 0) {
        windows_[static_cast<std::size_t>(parent)].children.push_back(index);
    }
    for (const auto& child : source.children) {
        add(child, index);
    }
    layoutText(index);
    return index;
}

void Screen::layoutText(int index) {
    Window& w = windows_[static_cast<std::size_t>(index)];
    if (w.text.empty() || !(w.kind == Kind::Text || w.kind == Kind::TextEdit || (w.kind == Kind::Button && w.showCaption))) {
        return;
    }
    const FontStyle& style = fonts_.style(w.font);
    // Wrapped text breaks between words to fit the area inside the gutters.
    std::vector<std::string> lines;
    if (w.wrapped) {
        std::istringstream words(w.text);
        std::string line, word;
        // A scroll bar inside the window takes its share of the width.
        int room = w.w - 2 * w.gutters[0];
        for (const int c : w.children) {
            if (windows_[static_cast<std::size_t>(c)].kind == Kind::Other && windows_[static_cast<std::size_t>(c)].x > 0) {
                room = std::min(room, windows_[static_cast<std::size_t>(c)].x - 2 * w.gutters[0]);
            }
        }
        while (words >> word) {
            const std::string longer = line.empty() ? word : line + " " + word;
            if (!line.empty() && fonts_.width(style, longer) > room) {
                lines.push_back(line);
                line = word;
            } else {
                line = longer;
            }
        }
        if (!line.empty()) {
            lines.push_back(line);
        }
    } else {
        lines.push_back(w.text);
    }
    const int lineHeight = fonts_.lineHeight(style);
    tga::Pixels block;
    block.width = 1;
    std::vector<TextImage> drawn;
    for (const auto& l : lines) {
        drawn.push_back(fonts_.draw(style, l));
        block.width = std::max(block.width, drawn.back().pixels.width);
    }
    block.height = static_cast<std::uint32_t>(lineHeight * static_cast<int>(lines.size()) + 2);
    block.rgba.assign(static_cast<std::size_t>(block.width) * block.height * 4, 0);
    const bool centered = w.align.find("center") == 0;
    const bool right = w.align.find("right") == 0;
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const auto& d = drawn[i].pixels;
        const std::uint32_t dx = centered ? (block.width - d.width) / 2 : right ? block.width - d.width : 0;
        const std::uint32_t dy = static_cast<std::uint32_t>(lineHeight) * static_cast<std::uint32_t>(i);
        for (std::uint32_t y = 0; y < d.height && dy + y < block.height; ++y) {
            for (std::uint32_t x = 0; x < d.width; ++x) {
                const std::size_t from = (static_cast<std::size_t>(y) * d.width + x) * 4;
                const std::size_t to = (static_cast<std::size_t>(dy + y) * block.width + dx + x) * 4;
                std::copy_n(&d.rgba[from], 4, &block.rgba[to]);
            }
        }
    }
    w.baseline = drawn.empty() ? 0 : drawn.front().baseline;
    if (w.textImage >= 0) {
        textures_[static_cast<std::size_t>(w.textImage)] = std::move(block);
        changed_.push_back(w.textImage);
    } else {
        w.textImage = texture(std::move(block));
    }
}

int Screen::load(std::uint32_t group, std::uint32_t instance, int x, int y, int parent) {
    const auto script = data_.script(group, instance);
    if (!script || script->empty()) {
        return -1;
    }
    int top = -1;
    for (const auto& w : *script) {
        const int i = add(w, parent);
        if (top < 0) {
            top = i;
        }
        if (parent < 0) {
            roots_.push_back(i);
        }
    }
    move(top, x, y);
    return top;
}

int Screen::find(int root, std::uint32_t id) const {
    if (root < 0) {
        return -1;
    }
    if (windows_[static_cast<std::size_t>(root)].id == id) {
        return root;
    }
    for (const int c : windows_[static_cast<std::size_t>(root)].children) {
        if (const int f = find(c, id); f >= 0) {
            return f;
        }
    }
    return -1;
}

int Screen::findShown(int root, std::uint32_t id) const {
    if (root < 0 || !windows_[static_cast<std::size_t>(root)].visible) {
        return -1;
    }
    if (windows_[static_cast<std::size_t>(root)].id == id) {
        return root;
    }
    for (const int c : windows_[static_cast<std::size_t>(root)].children) {
        if (const int f = findShown(c, id); f >= 0) {
            return f;
        }
    }
    return -1;
}

void Screen::setChecked(int window, bool checked) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].checked = checked;
    }
}

void Screen::setVisible(int window, bool visible) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].visible = visible;
    }
}

bool Screen::visible(int window) const {
    return window >= 0 && windows_[static_cast<std::size_t>(window)].visible;
}

void Screen::setEnabled(int window, bool enabled) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].state = enabled ? ButtonState::Normal : ButtonState::Disabled;
    }
}

bool Screen::inside(int window, int root) const {
    for (int i = window; i >= 0; i = windows_[static_cast<std::size_t>(i)].parent) {
        if (i == root) {
            return true;
        }
    }
    return false;
}

void Screen::setText(int window, const std::string& text) {
    if (window >= 0 && windows_[static_cast<std::size_t>(window)].text != text) {
        windows_[static_cast<std::size_t>(window)].text = text;
        layoutText(window);
    }
}

void Screen::setImage(int window, tga::Pixels pixels) {
    if (window < 0) {
        return;
    }
    Window& w = windows_[static_cast<std::size_t>(window)];
    if (w.ownImage) {
        textures_[static_cast<std::size_t>(w.image)] = std::move(pixels);
        changed_.push_back(w.image);
    } else {
        w.image = texture(std::move(pixels));
        w.ownImage = true;
    }
    w.imageRect = {};
    w.edge = false;
}

std::vector<int> Screen::takeChangedTextures() {
    std::vector<int> out;
    out.swap(changed_);
    return out;
}

void Screen::move(int window, int x, int y) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].x = x;
        windows_[static_cast<std::size_t>(window)].y = y;
    }
}

void Screen::resize(int window, int w, int h) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].w = w;
        windows_[static_cast<std::size_t>(window)].h = h;
    }
}

void Screen::raise(int window) {
    const auto at = std::find(roots_.begin(), roots_.end(), window);
    if (at != roots_.end()) {
        roots_.erase(at);
        roots_.push_back(window);
    }
}

int Screen::textWidth(int window) {
    const Window& w = windows_[static_cast<std::size_t>(window)];
    return fonts_.width(fonts_.style(w.font), w.text);
}

std::vector<std::string> Screen::childTexts(int window) const {
    std::vector<std::string> out;
    if (window >= 0) {
        for (const int c : windows_[static_cast<std::size_t>(window)].children) {
            out.push_back(windows_[static_cast<std::size_t>(c)].text);
        }
    }
    return out;
}

int Screen::width(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].w : 0;
}

int Screen::height(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].h : 0;
}

std::array<int, 2> Screen::origin(int window) const {
    std::array<int, 2> at{0, 0};
    for (int i = window; i >= 0; i = windows_[static_cast<std::size_t>(i)].parent) {
        at[0] += windows_[static_cast<std::size_t>(i)].x;
        at[1] += windows_[static_cast<std::size_t>(i)].y;
    }
    return at;
}

bool Screen::shown(int window) const {
    for (int i = window; i >= 0; i = windows_[static_cast<std::size_t>(i)].parent) {
        if (!windows_[static_cast<std::size_t>(i)].visible) {
            return false;
        }
    }
    return true;
}

std::uint32_t Screen::id(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].id : 0;
}

int Screen::parent(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].parent : -1;
}

void Screen::setTip(int window, const std::string& text) {
    if (window >= 0 && windows_[static_cast<std::size_t>(window)].tip != text) {
        windows_[static_cast<std::size_t>(window)].tip = text;
        windows_[static_cast<std::size_t>(window)].tipImage = -1;
    }
}

bool Screen::over(int x, int y) const {
    for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
        const Window& w = windows_[static_cast<std::size_t>(i)];
        if ((w.kind != Kind::Button && w.image < 0) || !shown(i)) {
            continue;
        }
        const auto at = origin(i);
        if (x >= at[0] && y >= at[1] && x < at[0] + w.w && y < at[1] + w.h) {
            return true;
        }
    }
    return false;
}

std::optional<int> Screen::mouse(int x, int y, bool down, double seconds) {
    // The topmost shown button under the mouse: the last one drawn.
    int over = -1;
    for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
        const Window& w = windows_[static_cast<std::size_t>(i)];
        if (w.kind != Kind::Button || !shown(i) || w.state == ButtonState::Disabled || (modal_ >= 0 && !inside(i, modal_))) {
            continue;
        }
        const auto at = origin(i);
        if (x >= at[0] && y >= at[1] && x < at[0] + w.w && y < at[1] + w.h) {
            over = i;
        }
    }
    if (over != hover_) {
        hover_ = over;
        hoverSince_ = seconds;
        // Tooltips are written in Helvetica Neue Medium at 12 (as the game's tooltips are; which of
        // the styles set so the game uses is not known yet).
        if (over >= 0) {
            Window& b = windows_[static_cast<std::size_t>(over)];
            if (!b.tip.empty() && b.tipImage < 0) {
                auto drawn = fonts_.draw(fonts_.style("HoverTipTitle"), b.tip);
                b.tipBaseline = drawn.baseline;
                b.tipImage = texture(std::move(drawn.pixels));
            }
        }
    }
    std::optional<int> clicked;
    if (!down && pressed_ >= 0 && pressed_ == over) {
        clicked = over;
    }
    if (down && pressed_ < 0) {
        pressed_ = over;
    } else if (!down) {
        pressed_ = -1;
    }
    for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
        Window& w = windows_[static_cast<std::size_t>(i)];
        if (w.kind == Kind::Button && w.state != ButtonState::Disabled) {
            w.state = i == pressed_ && i == over ? ButtonState::Pressed : i == over && pressed_ < 0 ? ButtonState::Hover : ButtonState::Normal;
        }
    }
    return clicked;
}

void Screen::emit(int index, std::vector<Quad>& out) const {
    const Window& w = windows_[static_cast<std::size_t>(index)];
    if (!w.visible) {
        return;
    }
    const auto at = origin(index);
    const float x0 = static_cast<float>(at[0]), y0 = static_cast<float>(at[1]);
    const float x1 = x0 + static_cast<float>(w.w), y1 = y0 + static_cast<float>(w.h);
    if (w.image >= 0 && w.paint) {
        const auto& img = textures_[static_cast<std::size_t>(w.image)];
        const float iw = static_cast<float>(img.width), ih = static_cast<float>(img.height);
        if (w.kind == Kind::Button) {
            const int frame = kButtonFrame[static_cast<int>(w.checked && w.state != ButtonState::Disabled ? ButtonState::Pressed : w.state)];
            out.push_back({x0, y0, x1, y1, static_cast<float>(frame) / 4, 0, static_cast<float>(frame + 1) / 4, 1, w.image});
        } else if (w.edge) {
            // Corners kept, edges and middle stretched: the image in thirds.
            const float cw = std::floor(iw / 3), ch = std::floor(ih / 3);
            const float xs[4] = {x0, x0 + cw, x1 - cw, x1}, ys[4] = {y0, y0 + ch, y1 - ch, y1};
            const float us[4] = {0, cw / iw, 1 - cw / iw, 1}, vs[4] = {0, ch / ih, 1 - ch / ih, 1};
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    out.push_back({xs[c], ys[r], xs[c + 1], ys[r + 1], us[c], vs[r], us[c + 1], vs[r + 1], w.image});
                }
            }
        } else if (w.imageRect[2] > w.imageRect[0]) {
            // Part of the image (one frame of the plumbob's animation strip), drawn at the window's size
            // from the image's top left: not known yet.
            out.push_back({x0, y0, x1, y1, 0, 0, static_cast<float>(w.w) / iw, static_cast<float>(w.h) / ih, w.image});
        } else {
            out.push_back({x0, y0, x1, y1, 0, 0, 1, 1, w.image});
        }
    }
    if (w.textImage >= 0 && !w.text.empty()) {
        const auto& t = textures_[static_cast<std::size_t>(w.textImage)];
        const float tw = static_cast<float>(t.width), th = static_cast<float>(t.height);
        const float gx = static_cast<float>(w.gutters[0]), gy = static_cast<float>(w.gutters[1]);
        float tx = x0 + gx;
        if (w.align.find("center") == 0) {
            tx = std::floor((x0 + x1 - tw) / 2);
        } else if (w.align.find("right") == 0) {
            tx = x1 - gx - tw;
        }
        float ty = y0 + gy;
        if (w.align.size() >= 6 && w.align.compare(w.align.size() - 6, 6, "center") == 0) {
            ty = std::floor((y0 + y1 - th) / 2);
        } else if (w.align.find("bottom") != std::string::npos) {
            ty = y1 - gy - th;
        }
        // Text is cut off inside the window's gutters.
        const float cx0 = std::max(tx, x0 + gx), cy0 = std::max(ty, y0 + gy), cx1 = std::min(tx + tw, x1 - gx), cy1 = std::min(ty + th, y1 - gy);
        if (cx1 > cx0 && cy1 > cy0) {
            out.push_back({cx0, cy0, cx1, cy1, (cx0 - tx) / tw, (cy0 - ty) / th, (cx1 - tx) / tw, (cy1 - ty) / th, w.textImage, w.textColor});
        }
    }
    for (const int c : w.children) {
        emit(c, out);
    }
}

std::vector<Quad> Screen::quads(double seconds) const {
    std::vector<Quad> out;
    for (const int r : roots_) {
        emit(r, out);
    }
    // The tooltip of the button the mouse rests on, as the game draws it: a box the tip's width
    // plus 22 and 21 high, centered on the button and its top 32 above the button's middle; a dark
    // blue border, a see-through beige inside and a soft shadow below and to the right. The time it
    // takes to show is not known yet.
    constexpr double kTipDelay = 0.5;
    if (hover_ < 0 || pressed_ >= 0 || seconds - hoverSince_ < kTipDelay) {
        return out;
    }
    const Window& b = windows_[static_cast<std::size_t>(hover_)];
    if (b.tip.empty() || b.tipImage < 0) {
        return out;
    }
    const auto& text = textures_[static_cast<std::size_t>(b.tipImage)];
    const auto at = origin(hover_);
    const int tw = static_cast<int>(text.width) - 2;  // the drawn line is 2 wider than the text
    const int w = tw + 22, h = 21;
    const int cx = at[0] + b.w / 2, cy = at[1] + b.h / 2;
    const float l = static_cast<float>(cx - (w / 2 + 1)), t = static_cast<float>(cy - 32);
    const float r = l + static_cast<float>(w), bt = t + static_cast<float>(h);
    auto rect = [&](float x0, float y0, float x1, float y1, std::array<float, 4> c) { out.push_back({x0, y0, x1, y1, 0, 0, 1, 1, white_, c}); };
    const float shadowBelow[3] = {0.4f, 0.2f, 0.07f}, shadowRight[2] = {0.2f, 0.07f};
    for (int k = 0; k < 3; ++k) {
        rect(l + 2, bt + static_cast<float>(k), r + 2, bt + static_cast<float>(k + 1), {0, 0, 0, shadowBelow[k]});
    }
    for (int k = 0; k < 2; ++k) {
        rect(r + static_cast<float>(k), t + 2, r + static_cast<float>(k + 1), bt, {0, 0, 0, shadowRight[k]});
    }
    rect(l + 1, t + 1, r - 1, bt - 1, {236 / 255.f, 210 / 255.f, 140 / 255.f, 212 / 255.f});
    const std::array<float, 4> edge{29 / 255.f, 39 / 255.f, 126 / 255.f, 1};
    rect(l, t, r, t + 1, edge);
    rect(l, bt - 1, r, bt, edge);
    rect(l, t + 1, l + 1, bt - 1, edge);
    rect(r - 1, t + 1, r, bt - 1, edge);
    const float px = l + 12, py = t + 15 - static_cast<float>(b.tipBaseline);
    out.push_back({px, py, px + static_cast<float>(text.width), py + static_cast<float>(text.height), 0, 0, 1, 1, b.tipImage, {0, 0, 0, 1}});
    return out;
}

} // namespace ts2::ui
