// Prints a UI script's windows as a tree: kind, id, area, caption (from its string resource when
// it names one), image size (or missing) and font.
#include "content/ui_data.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

void print(ts2::content::UiData& ui, const ts2::ui::Window& w, int depth) {
    std::string line(static_cast<std::size_t>(depth) * 2, ' ');
    line += w.text("iid");
    if (const auto id = w.number("id")) {
        char buf[16];
        std::snprintf(buf, sizeof buf, " 0x%08x", *id);
        line += buf;
    }
    line += " " + w.text("area");
    if (!w.flag("winflag_visible")) {
        line += " hidden";
    }
    const auto res = w.list("captionres");
    if (res.size() == 2 && res[0] != 0) {
        line += " \"" + ui.text(static_cast<std::uint32_t>(res[0]), static_cast<std::uint32_t>(res[1])) + "\"";
    } else if (w.get("caption")) {
        line += " (" + w.text("caption") + ")";
    }
    const auto image = w.list("image");
    if (image.size() >= 2) {
        const auto g = static_cast<std::uint32_t>(image[image.size() - 2]), i = static_cast<std::uint32_t>(image.back());
        char buf[48];
        if (const auto px = ui.image(g, i)) {
            std::snprintf(buf, sizeof buf, " image %08x %ux%u", i, px->width, px->height);
        } else {
            std::snprintf(buf, sizeof buf, " image %08x missing", i);
        }
        line += buf;
    }
    if (w.get("font")) {
        line += " font " + w.text("font");
    }
    std::printf("%s\n", line.c_str());
    for (const auto& c : w.children) {
        print(ui, c, depth + 1);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: uidump <sims2-install-folder> <group> <instance>\n");
        return 2;
    }
    try {
        ts2::content::UiData ui(reinterpret_cast<const char8_t*>(argv[1]));
        const auto group = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 16));
        const auto instance = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 16));
        const auto windows = ui.script(group, instance);
        if (!windows) {
            std::fprintf(stderr, "no UI script %08x %08x\n", group, instance);
            return 1;
        }
        for (const auto& w : *windows) {
            print(ui, w, 0);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
