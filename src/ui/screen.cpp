#include "ui/screen.hpp"

#include "diag/log.hpp"
#include "ui/tip_layout.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <sstream>

namespace ts2::ui {

namespace {

// Which frame of a button's four-frame image strip each state shows (as the game's main menu
// shows them: idle, hovered and pressed). Disabled takes the one left, a bare outline.
constexpr int kButtonFrame[4] = {1, 3, 2, 0};  // normal, hover, pressed, disabled
// A radio button's frame pair for each look (normal, hover, pressed, disabled). The game's art runs
// dark, light, green and grey. Normal is the dark pair, and the others are taken in the art's order.
constexpr int kRadioFrame[4] = {0, 1, 2, 3};
constexpr float kRadioFrames = 8;

// Part of an image (u0..u1 of an iw x ih image) drawn over a window in thirds: corners kept, edges
// and middle stretched. The corners are always drawn whole, overlapping in a window narrower or
// shorter than two of them, and the edges and middle between them only where there is room.
void pushThirds(std::vector<Quad>& out, int texture, float x0, float y0, float x1, float y1, float u0, float u1, float iw, float ih) {
    const float cw = std::floor((u1 - u0) * iw / 3), ch = std::floor(ih / 3);
    const float xs[4] = {x0, x0 + cw, x1 - cw, x1}, ys[4] = {y0, y0 + ch, y1 - ch, y1};
    const float us[4] = {u0, u0 + cw / iw, u1 - cw / iw, u1}, vs[4] = {0, ch / ih, 1 - ch / ih, 1};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            if ((r == 1 && ys[1] >= ys[2]) || (c == 1 && xs[1] >= xs[2])) {
                continue;
            }
            Quad q{xs[c], ys[r], xs[c + 1], ys[r + 1], us[c], vs[r], us[c + 1], vs[r + 1], texture};
            q.joined = static_cast<std::uint8_t>((c > 0 ? 1 : 0) | (r > 0 ? 2 : 0) | (c < 2 ? 4 : 0) | (r < 2 ? 8 : 0));
            out.push_back(q);
        }
    }
}

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
    int index = static_cast<int>(textures_.size());
    if (!freeTextures_.empty()) {
        index = freeTextures_.back();
        freeTextures_.pop_back();
        textures_[static_cast<std::size_t>(index)] = std::move(pixels);
    } else {
        textures_.push_back(std::move(pixels));
    }
    changed_.push_back(index);
    return index;
}

void Screen::release(int texture) {
    if (texture >= 0 && texture != white_) {
        textures_[static_cast<std::size_t>(texture)] = {};
        freeTextures_.push_back(texture);
    }
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
    w.enabled = source.get("winflag_enabled") != "no";
    w.ignoresMouse = source.flag("winflag_ignoremouse");
    w.container = source.flag("winflag_container");
    if (source.flag("winflag_mousetrans")) {
        std::string clsid = source.text("clsid");
        std::transform(clsid.begin(), clsid.end(), clsid.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        w.overChildren = clsid == "0x49495e7f" || clsid == "0x0c683787";
        w.seeThrough = source.flag("winflag_pbuff") && source.flag("winflag_premulalpha");
    }
    w.buffered = source.flag("winflag_pbuff");
    w.keyed = source.flag("winflag_pbufftrans") ||
              (parent >= 0 && static_cast<std::size_t>(parent) < windows_.size() && windows_[static_cast<std::size_t>(parent)].keyed);
    const auto image = source.list("image");
    if (image.size() >= 2) {
        const auto group = static_cast<std::uint32_t>(image[image.size() - 2]), instance = static_cast<std::uint32_t>(image.back());
        const std::uint64_t key = static_cast<std::uint64_t>(group) << 32 | instance;
        auto& cache = w.keyed ? keyedImages_ : images_;
        const auto known = cache.find(key);
        if (known != cache.end()) {
            w.image = known->second;
        } else if (auto px = data_.image(group, instance)) {
            if (w.keyed) {
                // The buffer shows through where a pixel's colour is exactly magenta, whatever its alpha.
                for (std::size_t i = 0; i + 3 < px->rgba.size(); i += 4) {
                    if (px->rgba[i] == 255 && px->rgba[i + 1] == 0 && px->rgba[i + 2] == 255) {
                        px->rgba[i] = px->rgba[i + 1] = px->rgba[i + 2] = px->rgba[i + 3] = 0;
                    }
                }
            }
            w.image = texture(std::move(*px));
            cache[key] = w.image;
        }
    }
    const auto rect = source.list("imagerect");
    if (rect.size() == 4) {
        w.imageRect = {static_cast<int>(rect[0]), static_cast<int>(rect[1]), static_cast<int>(rect[2]), static_cast<int>(rect[3])};
    }
    w.edge = source.flag("edgeimage");
    const auto style = source.text("style");
    w.toggle = style == "toggle";
    w.checkBox = style == "checkbox" || style == "radiobtn" || style == "radiocheck";
    w.radioCheck = style == "radiocheck";
    w.autosize = source.flag("autosize");
    const auto res = source.list("captionres");
    if (res.size() == 2 && res[0] != 0) {
        w.text = data_.text(static_cast<std::uint32_t>(res[0]), static_cast<std::uint32_t>(res[1]));
    } else if (w.kind == Kind::Text || w.kind == Kind::Rect || w.kind == Kind::Generic) {
        // A flat rectangle's or generic window's caption isn't drawn. Some name the window their
        // panel anchors to, and the options panels name a settings group's setting.
        w.text = source.text("caption");
    } else if (w.kind == Kind::TextEdit) {
        w.text = source.text("initvalue");
    }
    w.font = source.text("font");
    // Align names run from "lefttop" to "rightbottom", and "center" centres both ways.
    const std::string align = source.text("align");
    w.hAlign = align.rfind("center", 0) == 0 ? HAlign::Centre : align.rfind("right", 0) == 0 ? HAlign::Right : HAlign::Left;
    w.vAlign = align.size() >= 6 && align.compare(align.size() - 6, 6, "center") == 0 ? VAlign::Middle
               : align.find("bottom") != std::string::npos                          ? VAlign::Bottom
                                                                                       : VAlign::Top;
    w.textColor = color(source.list(w.kind == Kind::Text ? "forecolor" : "colorfontnormal"));
    if (const auto disabled = source.list("colorfontdisabled"); w.kind == Kind::Button && disabled.size() >= 3) {
        w.disabledTextColor = color(disabled);
    }
    if (w.kind == Kind::TextEdit) {
        w.highlightColor = color(source.list("highlightcolor"));
        w.selectedTextColor = color(source.list("colorfonthilited"));
        w.caretColor = color(source.list("caretcolor"));
    }
    w.paint = w.kind != Kind::Generic || source.flag("paint");
    const auto gutters = source.list("gutters");
    // Two gutters are the left and right one, then the top and bottom one.
    if (gutters.size() >= 4) {
        w.gutters = {static_cast<int>(gutters[0]), static_cast<int>(gutters[1]), static_cast<int>(gutters[2]), static_cast<int>(gutters[3])};
    } else if (gutters.size() >= 2) {
        w.gutters = {static_cast<int>(gutters[0]), static_cast<int>(gutters[1]), static_cast<int>(gutters[0]), static_cast<int>(gutters[1])};
    }
    w.wrapped = source.flag("wrapped");
    w.wrapCaption = w.kind == Kind::Button && source.flag("wrapcaption");
    w.showCaption = w.kind != Kind::Button || source.flag("showcaption");
    if (w.kind == Kind::Button && source.flag("tips")) {
        const auto tipres = source.list("tipres");
        w.tip = tipres.size() == 2 && tipres[0] != 0 ? data_.text(static_cast<std::uint32_t>(tipres[0]), static_cast<std::uint32_t>(tipres[1]))
                                                    : source.text("tiptext");
    }
    w.tipTimeout = source.get("tipstimeout") != "no";

    int index = static_cast<int>(windows_.size());
    if (!freeWindows_.empty()) {
        index = freeWindows_.back();
        freeWindows_.pop_back();
        windows_[static_cast<std::size_t>(index)] = std::move(w);
    } else {
        windows_.push_back(std::move(w));
    }
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
    w.lineStarts.clear();
    if (w.text.empty() || !(w.kind == Kind::Text || w.kind == Kind::TextEdit || (w.kind == Kind::Button && w.showCaption))) {
        return;
    }
    const FontStyle& style = fonts_.style(w.font);
    std::vector<std::string> lines;
    if (w.kind == Kind::TextEdit && w.wrapped) {
        // A wrapped text box keeps its text as typed, so that a place in it can be found on its lines.
        w.lineStarts = wrapStarts(w, style);
        w.lineHeight = std::max(1, fonts_.lineHeight(style));
        // Only the whole lines that fit from the one at the top are drawn, so the last one's
        // descenders show below it and nothing of the next line does.
        w.topLine = std::clamp(w.topLine, 0, static_cast<int>(w.lineStarts.size()) - 1);
        const std::size_t first = static_cast<std::size_t>(w.topLine);
        const std::size_t last = std::min(w.lineStarts.size(), first + static_cast<std::size_t>(visibleLines(index)));
        for (std::size_t i = first; i < last; ++i) {
            const std::size_t end = i + 1 < w.lineStarts.size() ? w.lineStarts[i + 1] : w.text.size();
            std::string line = w.text.substr(w.lineStarts[i], end - w.lineStarts[i]);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\n')) {
                line.pop_back();
            }
            lines.push_back(std::move(line));
        }
    } else {
        lines = wrapLines(w, style);
    }
    // Text windows and button captions in a Type 1 font are placed as the game places them. A text
    // window is then drawn line by line (see emitLines), from an image that holds one band per
    // line, each line at the band's left edge.
    const auto gameLine = fonts_.gameLine(style);
    const auto game = w.kind == Kind::Text ? gameLine : std::nullopt;
    const int step = std::max(1, fonts_.lineHeight(style));
    tga::Pixels block;
    block.width = 1;
    std::vector<TextImage> drawn;
    std::uint32_t band = 1;
    for (const auto& l : lines) {
        TextImage d = fonts_.draw(style, l);
        // A broken string of thousands of lines keeps only as many as one text picture holds.
        const std::int64_t row = game ? std::max(band, d.pixels.height) : static_cast<std::uint32_t>(step);
        if (!drawn.empty() && row * static_cast<std::int64_t>(drawn.size() + 1) + 2 > kMaxTextImage) {
            diag::warnOnce("ui: a text's lines past the largest text picture are left out");
            break;
        }
        block.width = std::max(block.width, d.pixels.width);
        band = std::max(band, d.pixels.height);
        drawn.push_back(std::move(d));
    }
    lines.resize(drawn.size());
    const int lineHeight = game ? static_cast<int>(band) : step;
    block.height = static_cast<std::uint32_t>(std::max(1, lineHeight * static_cast<int>(lines.size()) + (game ? 0 : 2)));
    // A wrapped text box's last line keeps its descenders whole.
    if (!w.lineStarts.empty() && !lines.empty()) {
        block.height = std::max(block.height, static_cast<std::uint32_t>(lineHeight * static_cast<int>(lines.size() - 1)) + band);
    }
    block.rgba.assign(static_cast<std::size_t>(block.width) * block.height * 4, 0);
    const bool centered = !game && w.hAlign == HAlign::Centre;
    const bool right = !game && w.hAlign == HAlign::Right;
    w.lineWidths.clear();
    if (game) {
        // The game measures a line without its trailing spaces. Its lines step by the font's line
        // height plus its line spacing, and the first baseline is the spacing plus the ascent below
        // the top of the text.
        for (const auto& l : lines) {
            w.lineWidths.push_back(fonts_.width(style, l.substr(0, l.find_last_not_of(" \t\r\n") + 1)));
        }
        w.band = static_cast<int>(band);
        // A broken font's negative spacing could step lines upward. One pixel is the least.
        w.lineStep = std::max(1, game->height + style.lineSpacing);
        w.firstBaseline = style.lineSpacing + game->ascent;
    }
    for (std::size_t i = 0; i < drawn.size(); ++i) {
        const auto& d = drawn[i].pixels;
        const std::uint32_t dx = centered ? (block.width - d.width) / 2 : right ? block.width - d.width : 0;
        const std::uint32_t dy = static_cast<std::uint32_t>(lineHeight) * static_cast<std::uint32_t>(i);
        for (std::uint32_t y = 0; y < d.height && dy + y < block.height; ++y) {
            const std::size_t from = static_cast<std::size_t>(y) * d.width * 4;
            const std::size_t to = (static_cast<std::size_t>(dy + y) * block.width + dx) * 4;
            std::copy_n(d.rgba.begin() + static_cast<std::ptrdiff_t>(from), static_cast<std::size_t>(d.width) * 4,
                        block.rgba.begin() + static_cast<std::ptrdiff_t>(to));
        }
    }
    w.baseline = drawn.empty() ? 0 : drawn.front().baseline;
    w.lines = std::max(1, static_cast<int>(lines.size()));
    w.captionLine = w.kind == Kind::Button ? gameLine : std::nullopt;
    if (w.captionLine) {
        w.lineStep = std::max(1, w.captionLine->height + style.lineSpacing);
        w.firstBaseline = style.lineSpacing + w.captionLine->ascent;
    }
    if (w.textImage >= 0) {
        textures_[static_cast<std::size_t>(w.textImage)] = std::move(block);
        changed_.push_back(w.textImage);
    } else {
        w.textImage = texture(std::move(block));
    }
}

// A window's text as lines. Wrapped text, and a button caption with wrapcaption, breaks between
// words to fit the area inside the gutters,
// each paragraph (a line break in the text) on its own, and loses its blank lines at the end.
std::vector<std::string> Screen::wrapLines(const Window& w, const FontStyle& style) const {
    if (!w.wrapped && !w.wrapCaption) {
        return {w.text};
    }
    // A scroll bar inside the window takes its share of the width.
    int room = w.w - 2 * w.gutters[0];
    for (const int c : w.children) {
        const Window& child = windows_[static_cast<std::size_t>(c)];
        if (child.kind == Kind::Other && child.x > 0) {
            room = std::min(room, child.x - 2 * w.gutters[0]);
        }
    }
    return wrapAt(w.text, style, room);
}

std::vector<std::size_t> Screen::wrapStarts(const Window& w, const FontStyle& style) const {
    // A limit for a broken file's text: past it the rest stays on the last line.
    constexpr std::size_t kMostLines = 4096;
    int room = w.w - 2 * w.gutters[0];
    for (const int c : w.children) {
        const Window& child = windows_[static_cast<std::size_t>(c)];
        if (child.kind == Kind::Other && child.x > 0) {
            room = std::min(room, child.x - 2 * w.gutters[0]);
        }
    }
    const std::string& text = w.text;
    std::vector<std::size_t> starts{0};
    std::size_t line = 0;  // where the current line starts
    std::size_t i = 0;
    while (i < text.size() && starts.size() < kMostLines) {
        if (text[i] == '\n') {
            line = i + 1;
            starts.push_back(line);
            ++i;
            continue;
        }
        if (text[i] == ' ') {
            ++i;
            continue;
        }
        // A word: it moves to a new line when the line it would end can't hold it.
        std::size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '\n') {
            ++end;
        }
        if (i > line && fonts_.width(style, text.substr(line, end - line)) > room) {
            line = i;
            starts.push_back(line);
        }
        i = end;
    }
    return starts;
}

std::vector<std::string> Screen::wrapAt(const std::string& text, const FontStyle& style, int room) const {
    std::vector<std::string> lines;
    std::istringstream paragraphs(text);
    std::string paragraph;
    while (std::getline(paragraphs, paragraph)) {
        std::istringstream words(paragraph);
        std::string line, word;
        while (words >> word) {
            std::string longer = line.empty() ? word : line + " " + word;
            if (!line.empty() && fonts_.width(style, longer) > room) {
                lines.push_back(std::move(line));
                line = word;
            } else {
                line = std::move(longer);
            }
        }
        lines.push_back(std::move(line));
    }
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    return lines;
}

int Screen::load(std::uint32_t group, std::uint32_t instance, int x, int y, int parent) {
    const auto script = data_.script(group, instance);
    if (!script || script->empty()) {
        return -1;
    }
    return addWindows(*script, x, y, parent);
}

int Screen::addWindows(const std::vector<ui::Window>& windows, int x, int y, int parent) {
    if (windows.empty()) {
        return -1;
    }
    int top = -1;
    for (const auto& w : windows) {
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

int Screen::findTop(std::uint32_t id) const {
    for (auto r = roots_.rbegin(); r != roots_.rend(); ++r) {
        if (windows_[static_cast<std::size_t>(*r)].id == id) {
            return *r;
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

bool Screen::checked(int window) const {
    return window >= 0 && windows_[static_cast<std::size_t>(window)].checked;
}

void Screen::remove(int window) {
    // A handle removed before stays free, or two later windows would share its slot.
    if (window < 0 || std::find(freeWindows_.begin(), freeWindows_.end(), window) != freeWindows_.end()) {
        return;
    }
    // The windows inside first (each takes itself out of this one's children).
    for (const int child : std::vector<int>(windows_[static_cast<std::size_t>(window)].children)) {
        remove(child);
    }
    Window& w = windows_[static_cast<std::size_t>(window)];
    release(w.textImage);
    release(w.tipImage);
    if (w.ownImage) {
        release(w.image);
    }
    std::vector<int>& siblings = w.parent >= 0 ? windows_[static_cast<std::size_t>(w.parent)].children : roots_;
    siblings.erase(std::remove(siblings.begin(), siblings.end(), window), siblings.end());
    for (int* held : {&pressed_, &modal_, &hover_}) {
        if (*held == window) {
            *held = -1;
        }
    }
    w = Window{};
    w.visible = false;
    freeWindows_.push_back(window);
}

void Screen::setPassive(int window) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].passive = true;
        for (const int c : windows_[static_cast<std::size_t>(window)].children) {
            setPassive(c);
        }
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
    if (window < 0) {
        return;
    }
    // Only a change counts, so setting it every frame leaves a button's hover and pressed looks alone.
    ButtonState& state = windows_[static_cast<std::size_t>(window)].state;
    if (enabled == (state == ButtonState::Disabled)) {
        state = enabled ? ButtonState::Normal : ButtonState::Disabled;
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

void Screen::setSelection(int window, std::size_t begin, std::size_t end) {
    if (window >= 0) {
        Window& w = windows_[static_cast<std::size_t>(window)];
        w.selectionBegin = std::min(begin, end);
        w.selectionEnd = std::max(begin, end);
    }
}

void Screen::setCaret(int window, std::optional<std::size_t> at) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].caret = at;
    }
}

std::size_t Screen::textIndexAt(int window, int x) {
    if (window < 0) {
        return 0;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    const FontStyle& style = fonts_.style(w.font);
    const int local = x - origin(window)[0] - w.gutters[0];
    // The boundary whose left part is nearest in width to the click.
    std::size_t best = 0;
    int bestDistance = std::abs(local);
    for (std::size_t i = 1; i <= w.text.size(); ++i) {
        if (i < w.text.size() && (static_cast<unsigned char>(w.text[i]) & 0xC0) == 0x80) {
            continue;
        }
        const int distance = std::abs(fonts_.width(style, w.text.substr(0, i)) - local);
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
}

int Screen::lineCount(int window) const {
    return window < 0 ? 0 : static_cast<int>(windows_[static_cast<std::size_t>(window)].lineStarts.size());
}

int Screen::lineHeight(int window) const {
    return window < 0 ? 0 : windows_[static_cast<std::size_t>(window)].lineHeight;
}

int Screen::visibleLines(int window) const {
    if (window < 0) {
        return 1;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    return std::max(1, (w.h - w.gutters[1] - w.gutters[3]) / std::max(1, w.lineHeight));
}

std::vector<std::size_t> Screen::lineStarts(int window) const {
    return window < 0 ? std::vector<std::size_t>{} : windows_[static_cast<std::size_t>(window)].lineStarts;
}

void Screen::setTopLine(int window, int line) {
    if (window >= 0) {
        Window& w = windows_[static_cast<std::size_t>(window)];
        const int top = std::clamp(line, 0, std::max(0, static_cast<int>(w.lineStarts.size()) - 1));
        if (top != w.topLine) {
            w.topLine = top;
            layoutText(window);
        }
    }
}

int Screen::topLine(int window) const {
    return window < 0 ? 0 : windows_[static_cast<std::size_t>(window)].topLine;
}

std::size_t Screen::textIndexAt(int window, int x, int y) {
    if (window < 0) {
        return 0;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    if (w.lineStarts.empty() || w.lineHeight <= 0) {
        return textIndexAt(window, x);
    }
    const int local = y - origin(window)[1] - w.gutters[1];
    const int line = std::clamp(w.topLine + (local < 0 ? -1 : local / w.lineHeight), 0, static_cast<int>(w.lineStarts.size()) - 1);
    const std::size_t begin = w.lineStarts[static_cast<std::size_t>(line)];
    const std::size_t end = static_cast<std::size_t>(line) + 1 < w.lineStarts.size() ? w.lineStarts[static_cast<std::size_t>(line) + 1] : w.text.size();
    const FontStyle& style = fonts_.style(w.font);
    const int across = x - origin(window)[0] - w.gutters[0];
    // The boundary on that line whose left part is nearest in width to the click. A line that
    // wrapped ends before the space it broke at.
    std::size_t last = end;
    while (last > begin && (w.text[last - 1] == ' ' || w.text[last - 1] == '\n') && end != w.text.size()) {
        --last;
    }
    std::size_t best = begin;
    int bestDistance = std::abs(across);
    for (std::size_t i = begin + 1; i <= last; ++i) {
        if (i < w.text.size() && (static_cast<unsigned char>(w.text[i]) & 0xC0) == 0x80) {
            continue;
        }
        const int distance = std::abs(fonts_.width(style, w.text.substr(begin, i - begin)) - across);
        if (distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return best;
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

void Screen::setImagePart(int window, const std::array<int, 4>& part) {
    if (window < 0 || static_cast<std::size_t>(window) >= windows_.size()) {
        return;
    }
    windows_[static_cast<std::size_t>(window)].part = part;
}

void Screen::setImageColor(int window, const std::array<float, 4>& color) {
    if (window >= 0 && static_cast<std::size_t>(window) < windows_.size()) {
        windows_[static_cast<std::size_t>(window)].imageColor = color;
    }
}

std::vector<int> Screen::takeChangedTextures() {
    std::vector<int> out;
    out.swap(changed_);
    // Pictures freed since they changed have nothing left to draw.
    out.erase(std::remove_if(out.begin(), out.end(), [&](int t) { return textures_[static_cast<std::size_t>(t)].width == 0; }), out.end());
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
        Window& target = windows_[static_cast<std::size_t>(window)];
        const bool widthChanged = target.w != w;
        target.w = w;
        target.h = h;
        if (widthChanged && target.wrapped) {
            layoutText(window);
        }
    }
}

void Screen::raise(int window) {
    const auto at = std::find(roots_.begin(), roots_.end(), window);
    if (at != roots_.end()) {
        roots_.erase(at);
        roots_.push_back(window);
    }
}

void Screen::placeUnder(int window, int sibling) {
    if (window < 0 || sibling < 0 || window == sibling || parent(window) < 0 || parent(window) != parent(sibling)) {
        return;
    }
    auto& list = windows_[static_cast<std::size_t>(parent(window))].children;
    std::erase(list, window);
    list.insert(std::find(list.begin(), list.end(), sibling), window);
}

void Screen::detach(int window, int under) {
    if (window < 0 || static_cast<std::size_t>(window) >= windows_.size()) {
        return;
    }
    Window& w = windows_[static_cast<std::size_t>(window)];
    if (w.parent >= 0) {
        const auto at = origin(window);
        std::erase(windows_[static_cast<std::size_t>(w.parent)].children, window);
        w.parent = -1;
        w.x = at[0];
        w.y = at[1];
    } else {
        std::erase(roots_, window);
    }
    roots_.insert(std::find(roots_.begin(), roots_.end(), under), window);
}

void Screen::setWrapped(int window, bool wrapped) {
    if (window >= 0 && windows_[static_cast<std::size_t>(window)].wrapped != wrapped) {
        windows_[static_cast<std::size_t>(window)].wrapped = wrapped;
        layoutText(window);
    }
}

int Screen::textHeight(int window) const {
    if (window < 0) {
        return 0;
    }
    const int image = windows_[static_cast<std::size_t>(window)].textImage;
    const Window& w = windows_[static_cast<std::size_t>(window)];
    if (w.text.empty()) {
        return 0;
    }
    if (!w.lineWidths.empty()) {
        return static_cast<int>(w.lineWidths.size()) * w.lineStep;
    }
    return image >= 0 && !w.text.empty() ? static_cast<int>(textures_[static_cast<std::size_t>(image)].height) : 0;
}

void Screen::fitHeightToText(int window) {
    if (window < 0) {
        return;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    resize(window, w.w, textHeight(window) + 2 * w.gutters[1]);
}

void Screen::fitToText(int window) {
    if (window < 0) {
        return;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    resize(window, textWidth(window) + 2 * w.gutters[0], textHeight(window) + 2 * w.gutters[1]);
}

int Screen::textWidth(int window) {
    if (window < 0) {
        return 0;
    }
    const Window& w = windows_[static_cast<std::size_t>(window)];
    return fonts_.width(fonts_.style(w.font), w.text);
}

std::array<int, 2> Screen::textSize(const std::string& font, const std::string& text) {
    const FontStyle& style = fonts_.style(font);
    return {fonts_.width(style, text), fonts_.lineHeight(style)};
}

std::vector<std::string> Screen::wrapText(const std::string& font, const std::string& text, int width) {
    return wrapAt(text, fonts_.style(font), width);
}

void Screen::setTextColor(int window, const std::array<float, 4>& color) {
    if (window >= 0) {
        windows_[static_cast<std::size_t>(window)].textColor = color;
    }
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

int Screen::x(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].x : 0;
}

int Screen::y(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].y : 0;
}

// A missing window (-1, as a missing or modded script gives) has no text and no children.
const std::string& Screen::text(int window) const {
    static const std::string none;
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].text : none;
}

const std::vector<int>& Screen::children(int window) const {
    static const std::vector<int> none;
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].children : none;
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

std::array<int, 2> Screen::imageSize(int window) const {
    if (window < 0 || windows_[static_cast<std::size_t>(window)].image < 0) {
        return {0, 0};
    }
    const auto& image = textures_[static_cast<std::size_t>(windows_[static_cast<std::size_t>(window)].image)];
    return {static_cast<int>(image.width), static_cast<int>(image.height)};
}

std::uint32_t Screen::id(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].id : 0;
}

int Screen::parent(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].parent : -1;
}

std::string Screen::tip(int window) const {
    return window >= 0 ? windows_[static_cast<std::size_t>(window)].tip : std::string();
}

void Screen::setTip(int window, const std::string& text) {
    if (window >= 0 && windows_[static_cast<std::size_t>(window)].tip != text) {
        windows_[static_cast<std::size_t>(window)].tip = text;
        release(windows_[static_cast<std::size_t>(window)].tipImage);
        windows_[static_cast<std::size_t>(window)].tipImage = -1;
        // A tip already showing is drawn again with the new text at once.
        if (window == hover_) {
            drawTip(windows_[static_cast<std::size_t>(window)]);
        }
    }
}

void Screen::setSpotTip(const std::string& text) {
    if (text == spotTip_.text) {
        return;
    }
    release(spotTip_.image);
    spotTip_.image = -1;
    spotTip_.text = text;
    if (text.empty()) {
        return;
    }
    TextImage line = fonts_.draw(fonts_.style("Default"), text);
    // As for window tips, a drawn line is 2 pixels wider than the text the game measures.
    spotTip_.width = std::max(0, static_cast<int>(line.pixels.width) - 2);
    spotTip_.baseline = line.baseline;
    spotTip_.image = texture(std::move(line.pixels));
}

void Screen::emitTip(int x, int y, int w, int h, int image, int baseline, int baselineY, std::vector<Quad>& out) const {
    const auto& text = textures_[static_cast<std::size_t>(image)];
    const float l = static_cast<float>(x), t = static_cast<float>(y);
    const float r = static_cast<float>(x + w - 2), bt = static_cast<float>(y + h - 3);
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
    const float px = l + 12, py = static_cast<float>(baselineY - baseline);
    out.push_back({px, py, px + static_cast<float>(text.width), py + static_cast<float>(text.height), 0, 0, 1, 1, image, {0, 0, 0, 1}});
}

void Screen::drawTip(Window& b) {
    if (b.tip.empty() || b.tipImage >= 0) {
        return;
    }
    // Each line break (a new line or a carriage return) starts a line, a line's height lower. The
    // game measures at most 500 pixels of lines, 41 of them.
    constexpr std::size_t kMaxLines = 41;
    const FontStyle& style = fonts_.style("HoverTipTitle");
    const int advance = fonts_.lineHeight(style);
    std::vector<TextImage> lines;
    std::size_t start = 0;
    while (lines.size() < kMaxLines) {
        const std::size_t end = b.tip.find_first_of("\r\n", start);
        lines.push_back(fonts_.draw(style, b.tip.substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    // One image holding every line, each baseline an advance below the one before.
    int baseline = 0, below = 0;
    std::uint32_t width = 0;
    for (const TextImage& line : lines) {
        baseline = std::max(baseline, line.baseline);
        below = std::max(below, static_cast<int>(line.pixels.height) - line.baseline);
        width = std::max(width, line.pixels.width);
    }
    const int lineCount = static_cast<int>(lines.size());
    tga::Pixels image;
    image.width = width;
    image.height = static_cast<std::uint32_t>(std::max(0, baseline + (lineCount - 1) * advance + below));
    image.rgba.assign(static_cast<std::size_t>(image.width) * image.height * 4, 0);
    for (int k = 0; k < lineCount; ++k) {
        const tga::Pixels& p = lines[static_cast<std::size_t>(k)].pixels;
        const int top = baseline - lines[static_cast<std::size_t>(k)].baseline + k * advance;
        for (std::uint32_t y = 0; y < p.height; ++y) {
            const auto row = static_cast<std::size_t>(top) + y;
            if (row >= image.height) {
                break;
            }
            // A line's image reaches into the line above it, so it must not clear that line's descenders.
            for (std::uint32_t x = 0; x < p.width; ++x) {
                const auto from = p.rgba.begin() + static_cast<std::ptrdiff_t>((y * p.width + x) * 4);
                const auto to = image.rgba.begin() + static_cast<std::ptrdiff_t>((row * image.width + x) * 4);
                if (from[3] > to[3]) {
                    std::copy_n(from, 4, to);
                }
            }
        }
    }
    // A drawn line is 2 pixels wider than the text the game measures.
    int widest = 0;
    for (const TextImage& line : lines) {
        widest = std::max(widest, static_cast<int>(line.pixels.width) - 2);
    }
    b.tipBaseline = baseline;
    b.tipWidth = widest;
    b.tipLines = lineCount;
    b.tipImage = texture(std::move(image));
}

bool Screen::over(int x, int y) const {
    for (int i = 0; i < static_cast<int>(windows_.size()); ++i) {
        const Window& w = windows_[static_cast<std::size_t>(i)];
        if ((w.kind != Kind::Button && w.image < 0) || w.passive || !shown(i)) {
            continue;
        }
        const auto at = origin(i);
        if (x >= at[0] && y >= at[1] && x < at[0] + w.w && y < at[1] + w.h) {
            return true;
        }
    }
    return false;
}

std::vector<int> Screen::drawOrder() const {
    std::vector<int> order;
    std::vector<int> pending(roots_.rbegin(), roots_.rend());
    while (!pending.empty()) {
        const int i = pending.back();
        pending.pop_back();
        order.push_back(i);
        const auto& children = windows_[static_cast<std::size_t>(i)].children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return order;
}

bool Screen::takesMouse(int window) const {
    const Window& w = windows_[static_cast<std::size_t>(window)];
    const bool enabled = w.kind == Kind::Button ? w.state != ButtonState::Disabled : w.enabled;
    return w.visible && enabled && !w.ignoresMouse && !w.passive;
}

bool Screen::holds(int window, int x, int y) const {
    const Window& w = windows_[static_cast<std::size_t>(window)];
    const auto at = origin(window);
    if (x < at[0] || y < at[1] || x >= at[0] + w.w || y >= at[1] + w.h) {
        return false;
    }
    auto overChild = [&](bool ignoring) {
        return std::any_of(w.children.begin(), w.children.end(), [&](int c) { return shownHolds(c, x, y, ignoring); });
    };
    if (w.container && !overChild(true)) {
        return false;
    }
    // Of the two child-only kinds, the generic windows don't count children that ignore the mouse.
    if (w.overChildren && !overChild(w.kind != Kind::Generic)) {
        return false;
    }
    return !w.seeThrough || drawnAt(window, x, y);
}

bool Screen::shownHolds(int window, int x, int y, bool ignoring) const {
    const Window& w = windows_[static_cast<std::size_t>(window)];
    if (!w.visible || (!ignoring && w.ignoresMouse)) {
        return false;
    }
    return std::any_of(w.children.begin(), w.children.end(), [&](int c) { return shownHolds(c, x, y, ignoring); }) || holds(window, x, y);
}

bool Screen::drawnAt(int window, int x, int y) const {
    std::vector<Quad> quads;
    emit(window, quads);
    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
    return std::any_of(quads.begin(), quads.end(), [&](const Quad& q) {
        if (px < q.x0 || py < q.y0 || px >= q.x1 || py >= q.y1 || q.color[3] <= 0 || q.texture < 0 ||
            static_cast<std::size_t>(q.texture) >= textures_.size()) {
            return false;
        }
        const tga::Pixels& t = textures_[static_cast<std::size_t>(q.texture)];
        if (t.width == 0 || t.height == 0) {
            return false;
        }
        const float u = q.u0 + (px - q.x0) / (q.x1 - q.x0) * (q.u1 - q.u0), v = q.v0 + (py - q.y0) / (q.y1 - q.y0) * (q.v1 - q.v0);
        const auto tx = static_cast<std::size_t>(std::clamp(u * static_cast<float>(t.width), 0.0f, static_cast<float>(t.width - 1)));
        const auto ty = static_cast<std::size_t>(std::clamp(v * static_cast<float>(t.height), 0.0f, static_cast<float>(t.height - 1)));
        return t.rgba[(ty * t.width + tx) * 4 + 3] != 0;
    });
}

int Screen::mouseWindowIn(const std::vector<int>& windows, int x, int y) const {
    // The one drawn on top is tried first.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        if (takesMouse(*it) && holds(*it, x, y)) {
            const int inner = mouseWindowIn(windows_[static_cast<std::size_t>(*it)].children, x, y);
            return inner >= 0 ? inner : *it;
        }
    }
    return -1;
}

int Screen::mouseWindow(int x, int y) const {
    if (modal_ >= 0) {
        return shown(modal_) ? mouseWindowIn({modal_}, x, y) : -1;
    }
    return mouseWindowIn(roots_, x, y);
}

std::optional<int> Screen::buttonAt(int x, int y) const {
    const int window = mouseWindow(x, y);
    return window >= 0 && windows_[static_cast<std::size_t>(window)].kind == Kind::Button ? std::optional<int>(window) : std::nullopt;
}

int Screen::tipWindowAt(int x, int y, int within) const {
    int over = -1;
    for (const int i : drawOrder()) {
        const Window& w = windows_[static_cast<std::size_t>(i)];
        if (w.tip.empty() || !shown(i) || (modal_ >= 0 && !inside(i, modal_)) || (within >= 0 && !inside(i, within))) {
            continue;
        }
        const auto at = origin(i);
        if (x >= at[0] && y >= at[1] && x < at[0] + w.w && y < at[1] + w.h) {
            over = i;
        }
    }
    return over;
}

bool Screen::held(int window) const {
    return window >= 0 && static_cast<std::size_t>(window) < windows_.size() &&
           windows_[static_cast<std::size_t>(window)].state == ButtonState::Pressed;
}

std::optional<int> Screen::mouse(int x, int y, bool down, double seconds) {
    const int over = buttonAt(x, y).value_or(-1);
    // Any window can carry a tip (the queue's icons do, and a household thumbnail's portrait under
    // its button). A button with a tip under the mouse comes first. A button without one passes the
    // mouse on only to windows beside it in its parent, so a panel never shows the tip of what it covers.
    const bool buttonTip = over >= 0 && !windows_[static_cast<std::size_t>(over)].tip.empty();
    const int within = over < 0 ? -1 : parent(over) >= 0 ? parent(over) : over;
    const int hovered = buttonTip ? over : tipWindowAt(x, y, within);
    if (hovered != hover_) {
        hover_ = hovered;
        hoverSince_ = seconds;
        // Tooltips are written in Helvetica Neue Medium at 12, as the game's tooltips are. Which of
        // the styles set so the game uses is not known yet.
        if (hovered >= 0) {
            drawTip(windows_[static_cast<std::size_t>(hovered)]);
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
    const std::size_t start = out.size();
    const auto at = origin(index);
    const float x0 = static_cast<float>(at[0]), y0 = static_cast<float>(at[1]);
    const float x1 = x0 + static_cast<float>(w.w), y1 = y0 + static_cast<float>(w.h);
    if (w.image >= 0 && w.paint) {
        const std::size_t first = out.size();
        const auto& img = textures_[static_cast<std::size_t>(w.image)];
        const float iw = static_cast<float>(img.width), ih = static_cast<float>(img.height);
        if (w.kind == Kind::Button && w.radioCheck) {
            // A radio button's picture holds 8 frames, a chosen and an unchosen one for each look,
            // drawn at their own size at the button's top left with the caption after them.
            const int pair = kRadioFrame[static_cast<int>(w.state)];
            const int frame = pair * 2 + (w.checked ? 0 : 1);
            const float fw = iw / kRadioFrames;
            out.push_back({x0, y0, x0 + fw, y0 + ih, static_cast<float>(frame) / kRadioFrames, 0,
                           static_cast<float>(frame + 1) / kRadioFrames, 1, w.image});
        } else if (w.kind == Kind::Button) {
            // Holding a toggle button down shows it hovered. It looks pressed only while checked.
            ButtonState look = w.toggle && w.state == ButtonState::Pressed ? ButtonState::Hover : w.state;
            if (w.checked && look != ButtonState::Disabled) {
                look = ButtonState::Pressed;
            }
            const int frame = kButtonFrame[static_cast<int>(look)];
            const float u0 = static_cast<float>(frame) / 4, u1 = static_cast<float>(frame + 1) / 4;
            // A button wider or narrower than its picture's frame keeps the frame's corners: the
            // frame in thirds, edges and middle stretched. Not an autosize button without a caption,
            // nor a check box or radio button.
            const float fw = iw / 4;
            if ((!w.autosize || w.showCaption) && !w.checkBox && static_cast<float>(w.w) != fw) {
                pushThirds(out, w.image, x0, y0, x1, y1, u0, u1, iw, ih);
            } else {
                // Otherwise the frame is drawn at its own size from the button's top left, cut to the
                // button, so a button taller than its picture doesn't stretch it.
                const float dw = std::min(static_cast<float>(w.w), fw), dh = std::min(static_cast<float>(w.h), ih);
                out.push_back({x0, y0, x0 + dw, y0 + dh, u0, 0, u0 + dw / iw, dh / ih, w.image});
            }
        } else if (w.edge) {
            pushThirds(out, w.image, x0, y0, x1, y1, 0, 1, iw, ih);
        } else if (w.part[2] > w.part[0] && w.part[3] > w.part[1]) {
            out.push_back({x0, y0, x1, y1, static_cast<float>(w.part[0]) / iw, static_cast<float>(w.part[1]) / ih,
                           static_cast<float>(w.part[2]) / iw, static_cast<float>(w.part[3]) / ih, w.image});
        } else if (w.imageRect[2] > w.imageRect[0]) {
            // Part of the image, such as one icon of the options panels' title strip, drawn at the
            // window's size from the part's top left.
            const float u0 = static_cast<float>(w.imageRect[0]), v0 = static_cast<float>(w.imageRect[1]);
            out.push_back({x0, y0, x1, y1, u0 / iw, v0 / ih, (u0 + static_cast<float>(w.w)) / iw, (v0 + static_cast<float>(w.h)) / ih, w.image});
        } else if (w.kind == Kind::Bitmap && !w.ownImage) {
            // A script's bitmap is drawn at its own size, whatever the window's size.
            out.push_back({x0, y0, x0 + iw, y0 + ih, 0, 0, 1, 1, w.image});
        } else {
            out.push_back({x0, y0, x1, y1, 0, 0, 1, 1, w.image});
        }
        for (std::size_t i = first; i < out.size(); ++i) {
            out[i].color = w.imageColor;
        }
    }
    if (w.textImage >= 0 && !w.text.empty() && !w.lineWidths.empty()) {
        emitLines(w, x0, y0, out);
    } else if (w.textImage >= 0 && !w.text.empty()) {
        const auto& t = textures_[static_cast<std::size_t>(w.textImage)];
        const float tw = static_cast<float>(t.width), th = static_cast<float>(t.height);
        const float gx = static_cast<float>(w.gutters[0]), gy = static_cast<float>(w.gutters[1]);
        float tx = x0 + gx;
        if (w.kind == Kind::Button && w.radioCheck && w.image >= 0) {
            tx = x0 + gx + static_cast<float>(textures_[static_cast<std::size_t>(w.image)].width) / kRadioFrames;
        } else if (w.hAlign == HAlign::Centre) {
            tx = std::floor((x0 + x1 - tw) / 2);
        } else if (w.hAlign == HAlign::Right) {
            tx = x1 - gx - tw;
        }
        float ty = y0 + gy;
        if (w.vAlign == VAlign::Middle) {
            ty = std::floor((y0 + y1 - th) / 2);
        } else if (w.vAlign == VAlign::Bottom) {
            ty = y1 - gy - th;
        }
        // The game centres a button's caption between its top and bottom gutters by the font's
        // line height (rounding toward zero, so it may start above the top gutter), or by its lines
        // with their spacing when wrapped. The baseline is the font's line spacing plus its ascent
        // below that. Only the window cuts it off.
        float top = y0 + gy, bottom = y1 - gy;
        if (w.captionLine) {
            const int height = w.wrapCaption ? w.lineStep * w.lines : w.captionLine->height;
            const int inner = w.h - w.gutters[1] - w.gutters[3];
            ty = y0 + static_cast<float>(w.gutters[1] + (inner - height) / 2 + w.firstBaseline - w.baseline);
            top = y0;
            bottom = y1;
        }
        // Other text is cut off inside the window's gutters.
        const float cx0 = std::max(tx, x0 + gx), cy0 = std::max(ty, top), cx1 = std::min(tx + tw, x1 - gx), cy1 = std::min(ty + th, bottom);
        if (cx1 > cx0 && cy1 > cy0) {
            const std::array<float, 4> color = w.state == ButtonState::Disabled && w.disabledTextColor ? *w.disabledTextColor : w.textColor;
            // A text box's selection is a highlighted stretch with its text in the selected colour.
            float sx0 = cx1, sx1 = cx1;
            if (w.kind == Kind::TextEdit && w.lineStarts.empty() && w.selectionEnd > w.selectionBegin) {
                const FontStyle& style = fonts_.style(w.font);
                sx0 = std::clamp(tx + static_cast<float>(fonts_.width(style, w.text.substr(0, w.selectionBegin))), cx0, cx1);
                sx1 = std::clamp(tx + static_cast<float>(fonts_.width(style, w.text.substr(0, w.selectionEnd))), cx0, cx1);
            }
            auto part = [&](float a, float b, const std::array<float, 4>& c) {
                if (b > a) {
                    out.push_back({a, cy0, b, cy1, (a - tx) / tw, (cy0 - ty) / th, (b - tx) / tw, (cy1 - ty) / th, w.textImage, c});
                }
            };
            if (sx1 > sx0) {
                out.push_back({sx0, cy0, sx1, cy1, 0, 0, 1, 1, white_, w.highlightColor});
            }
            part(cx0, sx0, color);
            part(sx0, sx1, w.selectedTextColor);
            part(sx1, cx1, color);
        }
        if (w.caret && !w.lineStarts.empty()) {
            // On the line it is on, cut off with the text.
            const std::size_t caretAt = std::min(*w.caret, w.text.size());
            const auto after = std::upper_bound(w.lineStarts.begin(), w.lineStarts.end(), caretAt);
            const std::size_t line = static_cast<std::size_t>(after - w.lineStarts.begin()) - 1;
            const std::size_t begin = w.lineStarts[line];
            const float before = static_cast<float>(fonts_.width(fonts_.style(w.font), w.text.substr(begin, caretAt - begin)));
            const float cx = std::min(tx + before, x1 - gx - 1);
            const float lineTop = ty + static_cast<float>((static_cast<int>(line) - w.topLine) * w.lineHeight);
            const float c0 = std::max(lineTop, top), c1 = std::min(lineTop + static_cast<float>(w.lineHeight), bottom);
            const int shownLine = static_cast<int>(line) - w.topLine;
            if (c1 > c0 && shownLine >= 0 && shownLine < visibleLines(index)) {
                out.push_back({cx, c0, cx + 1, c1, 0, 0, 1, 1, white_, w.caretColor});
            }
        } else if (w.caret) {
            const float before = static_cast<float>(fonts_.width(fonts_.style(w.font), w.text.substr(0, std::min(*w.caret, w.text.size()))));
            const float cx = std::min(tx + before, x1 - gx - 1);
            out.push_back({cx, cy0, cx + 1, cy1, 0, 0, 1, 1, white_, w.caretColor});
        }
    } else if (w.caret) {
        const float gx = static_cast<float>(w.gutters[0]), gy = static_cast<float>(w.gutters[1]);
        out.push_back({x0 + gx, y0 + gy, x0 + gx + 1, y1 - gy, 0, 0, 1, 1, white_, w.caretColor});
    }
    for (const int c : w.children) {
        emit(c, out);
    }
    if (w.buffered) {
        clip(out, start, x0, y0, x1, y1);
    }
}

// Cuts the quads from `start` on to a rectangle, their pictures with them, and drops those outside it.
void Screen::clip(std::vector<Quad>& quads, std::size_t start, float x0, float y0, float x1, float y1) {
    std::size_t kept = start;
    for (std::size_t i = start; i < quads.size(); ++i) {
        Quad q = quads[i];
        if (q.x1 <= x0 || q.x0 >= x1 || q.y1 <= y0 || q.y0 >= y1 || q.x1 <= q.x0 || q.y1 <= q.y0) {
            continue;
        }
        const float du = (q.u1 - q.u0) / (q.x1 - q.x0), dv = (q.v1 - q.v0) / (q.y1 - q.y0);
        if (q.x0 < x0) {
            q.u0 += (x0 - q.x0) * du;
            q.x0 = x0;
        }
        if (q.x1 > x1) {
            q.u1 -= (q.x1 - x1) * du;
            q.x1 = x1;
        }
        if (q.y0 < y0) {
            q.v0 += (y0 - q.y0) * dv;
            q.y0 = y0;
        }
        if (q.y1 > y1) {
            q.v1 -= (q.y1 - y1) * dv;
            q.y1 = y1;
        }
        quads[kept++] = q;
    }
    quads.resize(kept);
}

// A text window's lines as the game draws them. The text's area is the window inside its gutters,
// moved for vertical centring to half the height the lines leave (gutters not counted) or up from
// the bottom gutter. Each line starts at the area's left, at half the area's width less half its
// own (each half rounded toward zero), or at the area's right less its width. Lines are drawn while
// their top is above the area's bottom, and the window cuts them off.
void Screen::emitLines(const Window& w, float x0, float y0, std::vector<Quad>& out) const {
    const auto& t = textures_[static_cast<std::size_t>(w.textImage)];
    const int lines = static_cast<int>(w.lineWidths.size());
    const int textHeight = lines * w.lineStep;
    const int left = w.gutters[0], right = w.w - w.gutters[2];
    int top = w.gutters[1], bottom = w.h - w.gutters[3];
    if (w.vAlign != VAlign::Top) {
        top = w.vAlign == VAlign::Middle ? (w.h - textHeight) / 2 : w.h - w.gutters[3] - textHeight;
        bottom = top + textHeight;
    }
    const float tw = static_cast<float>(t.width), th = static_cast<float>(t.height);
    const float x1 = x0 + static_cast<float>(w.w), y1 = y0 + static_cast<float>(w.h);
    for (int i = 0; i < lines; ++i) {
        const int pen = top + i * w.lineStep;
        if (pen >= bottom) {
            break;
        }
        const int width = w.lineWidths[static_cast<std::size_t>(i)];
        int x = left;
        if (w.hAlign == HAlign::Centre) {
            x = left + (right - left) / 2 - width / 2;
        } else if (w.hAlign == HAlign::Right) {
            x = right - width;
        }
        const float tx = x0 + static_cast<float>(x), ty = y0 + static_cast<float>(pen + w.firstBaseline - w.baseline);
        const float band = static_cast<float>(i * w.band), bh = static_cast<float>(w.band);
        const float cx0 = std::max(tx, x0), cy0 = std::max(ty, y0), cx1 = std::min(tx + tw, x1), cy1 = std::min(ty + bh, y1);
        if (cx1 > cx0 && cy1 > cy0) {
            out.push_back({cx0, cy0, cx1, cy1, (cx0 - tx) / tw, (band + cy0 - ty) / th, (cx1 - tx) / tw, (band + cy1 - ty) / th, w.textImage,
                           w.textColor});
        }
    }
}

std::vector<Quad> Screen::quads(double seconds) const {
    std::vector<Quad> out;
    quads(seconds, out);
    return out;
}

void Screen::quads(double seconds, std::vector<Quad>& out) const {
    out.clear();
    for (const int r : roots_) {
        emit(r, out);
    }
    // The tip over a Sim in the world: its text in the Default style, the box the text's width plus
    // 20 by a line plus 12, kept 28 pixels from its spot, 8 from the screen's top and left edges and
    // 16 from its others.
    if (spotTip_.image >= 0 && size_[0] > 0 && size_[1] > 0) {
        constexpr int kSpotGap = 28, kSpotInset = 8, kSpotFar = 16;
        const FontStyle& style = fonts_.style("Default");
        const TipBox box = placeTip(spotTip_.at[0], spotTip_.at[1], spotTip_.width + 20, fonts_.lineHeight(style) + 12,
                                    {kSpotInset, kSpotInset, size_[0] - kSpotFar, size_[1] - kSpotFar}, kSpotGap);
        // The line's top is 6 pixels into the box, its baseline the font's ascent below as the game's
        // font engine measures it (8 for the Default font, where the face itself says 11).
        const auto line = fonts_.gameLine(style);
        const int ascent = line ? line->ascent : fonts_.ascent(style);
        emitTip(box.x, box.y, box.w, box.h, spotTip_.image, spotTip_.baseline, box.y + 6 + ascent, out);
    }
    // The tooltip of the button the mouse rests on, as the game draws it: it shows 0.4 s after the
    // mouse came to the button and, unless the button keeps it (tipstimeout=no), hides 3 s after
    // the mouse came. The box is the text's width plus 24 and its lines plus 12, placed by placeTip.
    // The frame draws a dark blue border and a see-through beige inside on all but its last 2 columns and
    // 3 rows, which hold a soft shadow.
    constexpr double kTipDelay = 0.4, kTipTimeout = 3;
    if (hover_ < 0 || pressed_ >= 0 || seconds - hoverSince_ < kTipDelay) {
        return;
    }
    const Window& b = windows_[static_cast<std::size_t>(hover_)];
    if (b.tip.empty() || b.tipImage < 0 || (b.tipTimeout && seconds - hoverSince_ >= kTipTimeout)) {
        return;
    }
    const auto at = origin(hover_);
    const int advance = fonts_.lineHeight(fonts_.style("HoverTipTitle"));
    const TipBox box = placeTip(at[0] + b.w / 2, at[1] + b.h / 2, b.tipWidth + 24, b.tipLines * advance + 12, size_);
    emitTip(box.x, box.y, box.w, box.h, b.tipImage, b.tipBaseline, box.y + 15, out);
    return;
}

} // namespace ts2::ui
