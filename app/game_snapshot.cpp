// Camera Snapshot (C): a picture of the view, without the panels, saved where the game keeps the
// neighborhood's snapshots for its story.
#include "game.hpp"

#include "content/ui_meters.hpp"
#include "diag/log.hpp"
#include "formats/jpeg.hpp"
#include "platform/platform.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <random>

namespace app {

namespace {

// The game's snapshots and their thumbnails, 4:3.
constexpr std::uint32_t kSnapshotWidth = 400, kSnapshotHeight = 300, kThumbnailWidth = 128, kThumbnailHeight = 96;
constexpr int kSnapshotQuality = 90;

// The middle of a picture cut to 4:3.
ts2::tga::Pixels middleFourByThree(const ts2::scene::Image& image) {
    std::uint32_t w = image.width, h = image.height;
    if (w * 3 > h * 4) {
        w = h * 4 / 3;
    } else {
        h = w * 3 / 4;
    }
    const std::uint32_t x0 = (image.width - w) / 2, y0 = (image.height - h) / 2;
    ts2::tga::Pixels out{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y) {
        const auto from = image.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y0 + y) * image.width + x0) * 4);
        std::copy(from, from + static_cast<std::ptrdiff_t>(w) * 4, out.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * w * 4));
    }
    // The view is opaque: what the renderer leaves in alpha isn't part of the picture.
    for (std::size_t i = 3; i < out.rgba.size(); i += 4) {
        out.rgba[i] = 255;
    }
    return out;
}

bool writeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace

void Game::takeSnapshot() {
    const auto album = hoodUi_.neighborhoodId();
    if (!inNeighborhood_ || !album || options_.user.empty()) {
        return;
    }
    // The view as it is, the panels left out for the one picture.
    std::vector<ts2::render::Sprite> panels;
    renderer_.showSprites(panels, true);
    auto image = renderer_.drawToImage(viewCamera(), static_cast<unsigned>(width_), static_cast<unsigned>(height_));
    renderer_.showSprites(panels, true);
    if (!image || image->width == 0 || image->height == 0 || image->rgba.size() != static_cast<std::size_t>(image->width) * image->height * 4) {
        ts2::diag::warning("camera snapshot: the view couldn't be drawn, so no snapshot was taken");
        return;
    }
    const auto picture = middleFourByThree(*image);
    const auto snapshot = ts2::jpeg::encode(ts2::content::scaled(picture, kSnapshotWidth, kSnapshotHeight), kSnapshotQuality);
    const auto thumbnail = ts2::jpeg::encode(ts2::content::scaled(picture, kThumbnailWidth, kThumbnailHeight), kSnapshotQuality);
    // Filed as the game files them: snapshot_<album>_<snapshot id>.jpg and thumbnail_ with the same
    // ids, an id no snapshot of the album has yet.
    const std::filesystem::path folder = options_.user / "Storytelling";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::mt19937 random(static_cast<unsigned>(ts2::platform::ticksNs()));
    std::filesystem::path snapshotPath, thumbnailPath;
    for (int tries = 0; tries < 100; ++tries) {
        const std::uint32_t id = static_cast<std::uint32_t>(random()) | 1u;  // never 0
        snapshotPath = folder / std::format("snapshot_{:08x}_{:08x}.jpg", static_cast<std::uint32_t>(*album), id);
        thumbnailPath = folder / std::format("thumbnail_{:08x}_{:08x}.jpg", static_cast<std::uint32_t>(*album), id);
        if (!std::filesystem::exists(snapshotPath, ec) && !std::filesystem::exists(thumbnailPath, ec)) {
            break;
        }
        snapshotPath.clear();
    }
    if (snapshotPath.empty() || snapshot.empty() || thumbnail.empty() || !writeFile(snapshotPath, snapshot) ||
        !writeFile(thumbnailPath, thumbnail)) {
        // Neither file is left half saved.
        for (const auto& path : {snapshotPath, thumbnailPath}) {
            if (!path.empty()) {
                std::filesystem::remove(path, ec);
            }
        }
        ts2::diag::warning("camera snapshot: it couldn't be saved in " + ts2::diag::utf8(folder));
        return;
    }
    ts2::diag::info("camera snapshot: saved " + ts2::diag::utf8(snapshotPath.filename()));
}

} // namespace app
