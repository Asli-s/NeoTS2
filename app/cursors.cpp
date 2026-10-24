#include "cursors.hpp"

#include "content/install.hpp"
#include "content/ui_data.hpp"
#include "diag/log.hpp"
#include "formats/cursor.hpp"
#include "ui/drag_arrow.hpp"

#include <SDL3/SDL.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace app {

namespace {

// The cursors' file names by id, as the game registers them.
constexpr std::pair<std::uint32_t, const char*> kNames[] = {
    {Cursors::kArrow, "arrow"},
    {Cursors::kLiveNothing, "livenothing"},
    {Cursors::kLivePerson, "liveperson"},
    {Cursors::kLiveObjectAvailable, "liveobjectavail"},
    {Cursors::kLiveObjectUnavailable, "liveobjectunavail"},
};

// The game's cursor setting picks the "_8" files (its default, on) or the "_1" ones. Each name is
// tried with that ending, then without one. Animated .ani files come first in the game: none of
// these cursors has one, and they are not read yet.
constexpr const char* kEnding = "_8";

// The drag arrow's picture: UI image 0xFFFF0000, of which the game keeps the green channel.
constexpr std::uint32_t kImageGroup = 0x499DB772, kDragArrow = 0xFFFF0000;
// What show() remembers while the drag arrow is up (no cursor id).
constexpr std::uint32_t kShowingDragArrow = 0xFFFFFFFF;

SDL_Cursor* make(const ts2::tga::Pixels& pixels, int hotX, int hotY) {
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(pixels.width), static_cast<int>(pixels.height), SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<std::uint8_t*>(pixels.rgba.data()), static_cast<int>(pixels.width * 4));
    if (!surface) {
        return nullptr;
    }
    SDL_Cursor* made = SDL_CreateColorCursor(surface, hotX, hotY);
    SDL_DestroySurface(surface);
    return made;
}

SDL_Cursor* load(const std::vector<ts2::content::Product>& products, const std::string& name) {
    for (const std::string file : {name + kEnding + ".cur", name + ".cur"}) {
        const auto path = ts2::content::newestDataFile(products, std::filesystem::path("Res/UI/Cursors") / file);
        if (!path) {
            continue;
        }
        std::ifstream in(*path, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        auto cursor = ts2::cursor::decode(bytes);
        if (!cursor) {
            ts2::diag::warnOnce("cursors: " + file + " can't be read");
            return nullptr;
        }
        return make(cursor->pixels, static_cast<int>(cursor->hotX), static_cast<int>(cursor->hotY));
    }
    ts2::diag::warnOnce("cursors: " + name + " was not found");
    return nullptr;
}

} // namespace

Cursors::Cursors(const std::filesystem::path& install, ts2::content::UiData& ui) {
    const auto products = ts2::content::findProducts(install);
    for (const auto& [id, name] : kNames) {
        if (SDL_Cursor* cursor = load(products, name)) {
            cursors_[id] = cursor;
        }
    }
    if (const auto image = ui.image(kImageGroup, kDragArrow); image && image->width == ts2::ui::kDragArrowSide && image->height == ts2::ui::kDragArrowSide) {
        arrow_.resize(std::size_t{ts2::ui::kDragArrowSide} * ts2::ui::kDragArrowSide);
        for (std::size_t i = 0; i < arrow_.size(); ++i) {
            arrow_[i] = image->rgba[i * 4 + 1];
        }
    } else {
        ts2::diag::warnOnce("cursors: the drag arrow's picture was not found");
    }
}

Cursors::~Cursors() {
    for (const auto& [id, cursor] : cursors_) {
        SDL_DestroyCursor(cursor);
    }
    if (turned_) {
        SDL_DestroyCursor(turned_);
    }
}

void Cursors::show(std::uint32_t id) {
    if (id == shown_) {
        return;
    }
    shown_ = id;
    const auto found = cursors_.find(id);
    SDL_SetCursor(found != cursors_.end() ? found->second : SDL_GetDefaultCursor());
}

// The game makes a new cursor each frame of a drag. Here a new one is made only when the angle changes.
void Cursors::showDragArrow(float angle) {
    if (arrow_.empty()) {
        return;
    }
    if (!turned_ || angle != turnedAngle_ || shown_ != kShowingDragArrow) {
        SDL_Cursor* made = make(ts2::ui::turnDragArrow(arrow_, angle), static_cast<int>(ts2::ui::kDragArrowSide / 2),
                                static_cast<int>(ts2::ui::kDragArrowSide / 2));
        if (!made) {
            return;
        }
        SDL_SetCursor(made);
        if (turned_) {
            SDL_DestroyCursor(turned_);
        }
        turned_ = made;
        turnedAngle_ = angle;
    }
    shown_ = kShowingDragArrow;
}

} // namespace app
