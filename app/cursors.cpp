#include "cursors.hpp"

#include "content/install.hpp"
#include "content/ui_data.hpp"
#include "diag/log.hpp"
#include "formats/cursor.hpp"
#include "ui/drag_arrow.hpp"

#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace app {

namespace {

// The cursors' file names by id, as the game registers them.
constexpr std::pair<std::uint32_t, const char*> kNames[] = {
    {Cursors::kArrow, "arrow"},
    {Cursors::kHourglass, "hourglass"},
    {Cursors::kLiveNothing, "livenothing"},
    {Cursors::kLivePerson, "liveperson"},
    {Cursors::kLiveObjectAvailable, "liveobjectavail"},
    {Cursors::kLiveObjectUnavailable, "liveobjectunavail"},
};

// The game's cursor setting picks the "_8" files (its default, on) or the "_1" ones. Each name is
// looked for as an animated .ani file with that ending, then a .cur file with it, then a .cur without
// one. The game uses the first file that exists, so a broken one is not replaced by the next. What
// the game shows for a broken file is not known yet. Here it is the system arrow.
constexpr const char* kEnding = "_8";

// The drag arrow's picture: UI image 0xFFFF0000, of which the game keeps the green channel.
constexpr std::uint32_t kImageGroup = 0x499DB772, kDragArrow = 0xFFFF0000;
// What show() remembers while the drag arrow is up (no cursor id).
constexpr std::uint32_t kShowingDragArrow = 0xFFFFFFFF;

ts2::platform::CursorPtr make(const ts2::tga::Pixels& pixels, int hotX, int hotY) {
    return ts2::platform::createCursor(pixels.rgba.data(), static_cast<unsigned>(pixels.width), static_cast<unsigned>(pixels.height),
                                       static_cast<unsigned>(hotX), static_cast<unsigned>(hotY));
}

// A cursor's pictures, all made, or nothing.
std::optional<std::vector<ts2::platform::CursorPtr>> makeAll(const std::vector<ts2::cursor::Cursor>& pictures) {
    std::vector<ts2::platform::CursorPtr> out;
    for (const auto& c : pictures) {
        auto made = make(c.pixels, static_cast<int>(c.hotX), static_cast<int>(c.hotY));
        if (!made) {
            return std::nullopt;
        }
        out.push_back(std::move(made));
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> readFile(const std::vector<ts2::content::Product>& products, const std::string& file) {
    const auto path = ts2::content::newestDataFile(products, std::filesystem::path("Res/UI/Cursors") / file);
    if (!path) {
        return std::nullopt;
    }
    std::ifstream in(*path, std::ios::binary);
    return std::vector<std::uint8_t>{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::optional<Cursors::Shape> load(const std::vector<ts2::content::Product>& products, const std::string& name) {
    if (const std::string file = name + kEnding + ".ani"; const auto bytes = readFile(products, file)) {
        auto animated = ts2::cursor::decodeAnimated(*bytes);
        auto frames = animated ? makeAll(animated->frames) : std::nullopt;
        if (!frames) {
            ts2::diag::warnOnce("cursors: " + file + " can't be read");
            return std::nullopt;
        }
        return Cursors::Shape{std::move(*frames), std::move(animated->steps)};
    }
    for (const std::string file : {name + kEnding + ".cur", name + ".cur"}) {
        const auto bytes = readFile(products, file);
        if (!bytes) {
            continue;
        }
        auto cursor = ts2::cursor::decode(*bytes);
        auto frames = cursor ? makeAll({*cursor}) : std::nullopt;
        if (!frames) {
            ts2::diag::warnOnce("cursors: " + file + " can't be read");
            return std::nullopt;
        }
        return Cursors::Shape{std::move(*frames), {}};
    }
    ts2::diag::warnOnce("cursors: " + name + " was not found");
    return std::nullopt;
}

} // namespace

Cursors::Cursors(const std::filesystem::path& install, ts2::content::UiData& ui) {
    const auto products = ts2::content::findProducts(install);
    for (const auto& [id, name] : kNames) {
        if (auto shape = load(products, name)) {
            cursors_[id] = std::move(*shape);
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

Cursors::~Cursors() = default;

void Cursors::show(std::uint32_t id) {
    wanted_ = id;
    if (!busy_ && id != shown_) {
        display(id);
    }
}

void Cursors::display(std::uint32_t id) {
    shown_ = id;
    playback_.restart();
    const auto found = cursors_.find(id);
    const Shape* shape = found != cursors_.end() ? &found->second : nullptr;
    set_ = shape && !shape->frames.empty() ? shape->frames[shape->steps.empty() ? 0 : shape->steps.front().frame].get() : nullptr;
    ts2::platform::setCursor(set_);
}

void Cursors::setBusy(bool busy) {
    if (busy == busy_) {
        return;
    }
    busy_ = busy;
    display(busy ? kHourglass : wanted_);
}

void Cursors::animate(double seconds) {
    const auto found = cursors_.find(shown_);
    if (found == cursors_.end() || found->second.steps.empty()) {
        return;
    }
    const auto& steps = found->second.steps;
    const auto* picture = found->second.frames[steps[playback_.step(steps, seconds)].frame].get();
    if (picture != set_) {
        set_ = picture;
        ts2::platform::setCursor(set_);
    }
}

// The game makes a new cursor each frame of a drag. Here a new one is made only when the angle changes.
void Cursors::showDragArrow(float angle) {
    if (arrow_.empty() || busy_) {
        return;
    }
    if (!turned_ || angle != turnedAngle_ || shown_ != kShowingDragArrow) {
        auto made = make(ts2::ui::turnDragArrow(arrow_, angle), static_cast<int>(ts2::ui::kDragArrowSide / 2),
                         static_cast<int>(ts2::ui::kDragArrowSide / 2));
        if (!made) {
            return;
        }
        ts2::platform::setCursor(made.get());
        // The one before is dropped only now that the system shows the new one.
        turned_ = std::move(made);
        turnedAngle_ = angle;
    }
    shown_ = kShowingDragArrow;
}

} // namespace app
