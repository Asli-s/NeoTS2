// Text windows and button captions laid out by Screen, with our own tiny Type 1 font
// (tests/data/neots2_test.pfa, made by make_test_font.py) in place of the game's fonts.
#include "test_support.hpp"

#include "content/ui_data.hpp"
#include "ui/font_metrics.hpp"
#include "ui/fonts.hpp"
#include "ui/screen.hpp"

#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>

namespace {

namespace fs = std::filesystem;
using ts2::ui::Fonts;
using ts2::ui::Screen;

std::vector<std::uint8_t> testFont() {
    std::ifstream in(fs::path(NEOTS2_SOURCE_DIR) / "tests" / "data" / "neots2_test.pfa", std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The test font's lines as the game measures them: its 'f' is 714 units tall and its 'g' 230 deep.
ts2::ui::LineMetrics measures(int size) {
    return ts2::ui::typeOneLine(714, -230, 1000, size);
}

// A screen with no game data, drawing with the test font in the given FontStyle.ini styles.
struct Rig {
    explicit Rig(const std::string& styles)
        : data(emptyFolder()), fonts("[Font Styles]\n" + styles, {testFont()}), screen(data, fonts) {}
    static fs::path emptyFolder() {
        const fs::path folder = fs::temp_directory_path() / "neots2_text_layout_tests";
        fs::create_directories(folder);
        return folder;
    }
    ts2::content::UiData data;
    Fonts fonts;
    Screen screen;
};

ts2::ui::Window window(const std::string& iid, const std::string& area, const std::string& font, const std::string& align,
                       const std::string& caption, bool wrapped = false) {
    return {{{"iid", iid},
             {"winflag_visible", "yes"},
             {"area", area},
             {"font", font},
             {"align", align},
             {"gutters", "(2,2)"},
             {"caption", caption},
             {"showcaption", "yes"},
             {"wrapped", wrapped ? "yes" : "no"}},
            {}};
}

// The rows and columns of the screen where the text's glyphs are inked (half covered or more).
struct Ink {
    std::vector<int> rows;
    int left = INT_MAX, right = INT_MIN;
    // The last row of each run of inked rows: the row above each line's baseline for our glyphs.
    std::vector<int> lineBottoms() const {
        std::vector<int> out;
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i + 1 == rows.size() || rows[i + 1] != rows[i] + 1) {
                out.push_back(rows[i]);
            }
        }
        return out;
    }
};

Ink ink(const Screen& screen) {
    std::set<int> rows;
    Ink out;
    for (const auto& q : screen.quads()) {
        const auto& t = screen.textures()[static_cast<std::size_t>(q.texture)];
        const float u = q.u0 * static_cast<float>(t.width), v = q.v0 * static_cast<float>(t.height);
        for (int y = static_cast<int>(q.y0); y < static_cast<int>(q.y1); ++y) {
            for (int x = static_cast<int>(q.x0); x < static_cast<int>(q.x1); ++x) {
                const int ix = static_cast<int>(u + static_cast<float>(x) - q.x0), iy = static_cast<int>(v + static_cast<float>(y) - q.y0);
                if (ix < 0 || iy < 0 || ix >= static_cast<int>(t.width) || iy >= static_cast<int>(t.height) ||
                    t.rgba[(static_cast<std::size_t>(iy) * t.width + static_cast<std::size_t>(ix)) * 4 + 3] < 128) {
                    continue;
                }
                rows.insert(y);
                out.left = std::min(out.left, x);
                out.right = std::max(out.right, x);
            }
        }
    }
    out.rows.assign(rows.begin(), rows.end());
    return out;
}

// A text window's first baseline is its top gutter plus the font's line spacing plus the game's
// ascent below the window's top, and its text starts at the left gutter.
void textLinesSitOnTheGameBaseline() {
    Rig rig("Plain = \"NeoTS2 Test\", \"20\", \"bold\"\n");
    rig.screen.addWindows({window("IGZWinText", "(0,0,200,40)", "Plain", "lefttop", "H")}, 10, 20);
    const Ink plain = ink(rig.screen);
    CHECK(!plain.rows.empty() && plain.rows.back() == 20 + 2 + measures(20).ascent - 1);
    CHECK(plain.rows.size() == 14);  // 'H' is 700 units tall
    CHECK(plain.left == 12);

    Rig spaced("Spaced = \"NeoTS2 Test\", \"22\", \"bold|linespacing=3\"\n");
    spaced.screen.addWindows({window("IGZWinText", "(0,0,200,40)", "Spaced", "lefttop", "H")}, 0, 0);
    const Ink s = ink(spaced.screen);
    CHECK(!s.rows.empty() && s.rows.back() == 2 + 3 + measures(22).ascent - 1);
}

// Lines step by the font's line height plus its spacing, and each is centred on its own: half the
// area's width less half the line's (trailing spaces not counted), each half rounded toward zero.
void textLinesStepAndCentreOneByOne() {
    Rig rig("Spaced = \"NeoTS2 Test\", \"20\", \"bold|linespacing=2\"\n");
    rig.screen.addWindows({window("IGZWinText", "(0,0,201,120)", "Spaced", "centertop", "H\nHH   \nH", true)}, 0, 0);
    const auto bottoms = ink(rig.screen).lineBottoms();
    const int step = measures(20).height + 2;
    CHECK(bottoms.size() == 3 && bottoms[1] - bottoms[0] == step && bottoms[2] - bottoms[1] == step);

    Rig one("Plain = \"NeoTS2 Test\", \"20\", \"bold\"\n");
    one.screen.addWindows({window("IGZWinText", "(0,0,201,40)", "Plain", "centertop", "HH   ")}, 0, 0);
    const int width = one.fonts.width(one.fonts.style("Plain"), "HH");
    CHECK(ink(one.screen).left == 2 + (199 - 2) / 2 - width / 2);
}

// Centred text sits at half the height its lines leave in the window (gutters not counted), and
// bottom text that far up from the bottom gutter.
void textCentresAndSinksAsTheGame() {
    const auto m = measures(20);
    Rig middle("Plain = \"NeoTS2 Test\", \"20\", \"bold\"\n");
    middle.screen.addWindows({window("IGZWinText", "(0,0,200,41)", "Plain", "leftcenter", "H")}, 0, 0);
    const Ink a = ink(middle.screen);
    CHECK(!a.rows.empty() && a.rows.back() == (41 - m.height) / 2 + m.ascent - 1);

    Rig bottom("Plain = \"NeoTS2 Test\", \"20\", \"bold\"\n");
    bottom.screen.addWindows({window("IGZWinText", "(0,0,200,41)", "Plain", "leftbottom", "H")}, 0, 0);
    const Ink b = ink(bottom.screen);
    CHECK(!b.rows.empty() && b.rows.back() == 41 - 2 - m.height + m.ascent - 1);
}

// A button's caption is centred between its gutters by the font's line height, and its baseline
// is the line spacing plus the ascent below that.
void buttonCaptionsTakeTheLineSpacing() {
    Rig rig("Button = \"NeoTS2 Test\", \"20\", \"bold|linespacing=3\"\n");
    // A button's caption comes from a string resource or from code.
    const int button = rig.screen.addWindows({window("IGZWinBtn", "(0,0,100,34)", "Button", "center", "")}, 0, 0);
    rig.screen.setText(button, "H");
    const auto m = measures(20);
    const Ink caption = ink(rig.screen);
    CHECK(!caption.rows.empty() && caption.rows.back() == 2 + (34 - 4 - m.height) / 2 + 3 + m.ascent - 1);
}

// Styles with the same face, size and marks share their font as in the game: the last style that
// sets a line spacing sets it for all, a style without one changes nothing, and a private style
// or another size keeps its own.
void fontStylesShareTheirFont() {
    Rig rig("A = \"NeoTS2 Test\", \"16\", \"bold\"\n"
            "B = \"NeoTS2 Test\", \"16\", \"bold|linespacing=3\"\n"
            "C = \"NeoTS2 Test\", \"16\", \"bold|private\"\n"
            "D = \"NeoTS2 Test\", \"18\", \"bold\"\n"
            "E = \"NeoTS2 Test\", \"16\", \"bold|underline\"\n"
            "F = \"NeoTS2 Test\", \"20\", \"bold|linespacing=2\"\n"
            "G = \"NeoTS2 Test\", \"20\", \"bold\"\n"
            "H = \"NeoTS2 Test\", \"22\", \"bold|xscale=0.8\"\n"
            "I = \"NeoTS2 Test\", \"22\", \"bold\"\n");
    const auto& f = rig.fonts;
    CHECK(f.style("A").lineSpacing == 3 && f.style("B").lineSpacing == 3);
    CHECK(f.style("C").lineSpacing == 0 && f.style("D").lineSpacing == 0 && f.style("E").lineSpacing == 0);
    CHECK(f.style("F").lineSpacing == 2 && f.style("G").lineSpacing == 2);
    CHECK(f.style("I").xScale == 0.8f);
}

// Guards for broken text and fonts: a cleared text has no height, lines never step upward, and a
// text picture never grows past the largest the GPU takes.
void textGuardsHold() {
    Rig rig("Plain = \"NeoTS2 Test\", \"20\", \"bold\"\nBroken = \"NeoTS2 Test\", \"21\", \"bold|linespacing=-2048\"\n");
    const int plain = rig.screen.addWindows({window("IGZWinText", "(0,0,200,40)", "Plain", "lefttop", "H")}, 0, 0);
    CHECK(rig.screen.textHeight(plain) == measures(20).height);
    rig.screen.setText(plain, "");
    CHECK(rig.screen.textHeight(plain) == 0);

    const int broken = rig.screen.addWindows({window("IGZWinText", "(0,0,200,40)", "Broken", "lefttop", "H\nH\nH", true)}, 0, 0);
    CHECK(rig.screen.textHeight(broken) == 3);

    std::string many;
    for (int i = 0; i < 5000; ++i) {
        many += "H\n";
    }
    rig.screen.addWindows({window("IGZWinText", "(0,0,200,40)", "Plain", "lefttop", many, true)}, 0, 0);
    for (const auto& t : rig.screen.textures()) {
        CHECK(t.height <= static_cast<std::uint32_t>(ts2::ui::kMaxTextImage) + 2 && t.width <= static_cast<std::uint32_t>(ts2::ui::kMaxTextImage));
    }
    const auto wide = rig.fonts.draw(rig.fonts.style("Plain"), std::string(5000, 'H'));
    CHECK(wide.pixels.width == static_cast<std::uint32_t>(ts2::ui::kMaxTextImage));
}

} // namespace

std::vector<Test> textLayoutTests() {
    return {
        {"text lines sit on the game baseline", textLinesSitOnTheGameBaseline},
        {"text lines step and centre one by one", textLinesStepAndCentreOneByOne},
        {"text centres and sinks as the game", textCentresAndSinksAsTheGame},
        {"button captions take the line spacing", buttonCaptionsTakeTheLineSpacing},
        {"font styles share their font", fontStylesShareTheirFont},
        {"text guards hold", textGuardsHold},
    };
}
