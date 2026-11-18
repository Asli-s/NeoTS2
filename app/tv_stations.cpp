#include "tv_stations.hpp"

#include "object_materials.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <set>

namespace app {

namespace {

std::vector<std::uint8_t> readMovie(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = static_cast<std::size_t>(std::max<std::streamoff>(in.tellg(), 0));
    if (size > ts2::movie::kMaxFileBytes) {
        return {};
    }
    std::vector<std::uint8_t> data(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    data.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return data;
}

} // namespace

std::optional<ts2::sim::TvLinks::Station> TvStations::station(std::string_view name) const {
    const ts2::content::Station* s = catalog_ ? catalog_->find(name) : nullptr;
    if (s == nullptr) {
        return std::nullopt;
    }
    return ts2::sim::TvLinks::Station{s->entry.dontRestart};
}

// A games station's one movie. Otherwise the next show in list order, then a commercial: the only
// one, or one picked at random that isn't the one before. Without commercials shows follow shows.
std::optional<std::filesystem::path> TvStations::nextMovie(const ts2::content::Station& station, Progress& progress) {
    if (station.entry.isGames) {
        return station.shows.empty() ? std::nullopt : std::optional{station.shows.front()};
    }
    for (int turn = 0; turn < 2; ++turn) {
        if (progress.commercialNext) {
            progress.commercialNext = false;
            const auto& ads = station.commercials;
            if (ads.empty()) {
                continue;
            }
            std::size_t pick = 0;
            if (ads.size() > 1) {
                std::uniform_int_distribution<std::size_t> any(0, ads.size() - 1);
                do {
                    pick = any(random_);
                } while (progress.lastCommercial && pick == *progress.lastCommercial);
            }
            progress.lastCommercial = pick;
            return ads[pick];
        }
        progress.commercialNext = true;
        if (!station.shows.empty()) {
            if (progress.nextShow >= station.shows.size()) {
                progress.nextShow = 0;
            }
            return station.shows[progress.nextShow++];
        }
    }
    return std::nullopt;
}

void TvStations::start(Playing& playing, const std::string& name) {
    playing.movie.reset();
    playing.file.clear();
    playing.clockMs = 0;
    playing.decoded = -1;
    const auto path = nextMovie(*playing.station, progress_[name]);
    playing.nothingToPlay = !path;
    if (path) {
        const auto file = path->filename().u8string();
        ts2::diag::info("TV: " + name + " plays " + std::string(file.begin(), file.end()));
        // Read off the drawing thread, so a show starting never holds a frame up.
        playing.loading = std::async(std::launch::async, [p = *path] { return readMovie(p); });
    }
}

bool TvStations::play(Playing& playing, const std::string& name, double elapsedMs, ts2::sim::World& world, std::optional<int> image,
                      ts2::render::IRenderer& renderer) {
    if (playing.nothingToPlay) {
        return true;
    }
    if (!playing.movie) {
        if (!playing.loading.valid() || playing.loading.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            return true;
        }
        playing.file = playing.loading.get();
        try {
            playing.movie = ts2::movie::read(playing.file);
        } catch (const std::exception& e) {
            ts2::diag::warnOnce(std::string("TV: a movie of ") + name + " can't be played: " + e.what());
            playing.nothingToPlay = true;
            return true;
        }
    }
    const ts2::movie::Movie& movie = *playing.movie;
    playing.clockMs += elapsedMs;
    const auto target = static_cast<long>(playing.clockMs / std::max<int>(movie.frameMs, 1));
    if (target >= static_cast<long>(movie.frames.size())) {
        const auto& entry = playing.station->entry;
        if (entry.isGames || entry.dontRestart) {
            world.tvLinks().end(name, [&world](std::int16_t id) { return world.find(id); });
            return false;
        }
        start(playing, name);
        return true;
    }
    if (target <= playing.decoded) {
        return true;
    }
    // Far behind, decoding starts again at the last key frame at or before the frame due. Frames
    // passed on the way are decoded but not shown.
    long from = playing.decoded + 1;
    for (long k = target; k > from; --k) {
        if (movie.frames[static_cast<std::size_t>(k)].kind == ts2::movie::FrameKind::Key) {
            from = k;
            break;
        }
    }
    for (long n = from; n <= target; ++n) {
        (void)playing.decoder.decode(playing.file, movie, movie.frames[static_cast<std::size_t>(n)]);
    }
    playing.decoded = target;
    if (image) {
        playing.decoder.rgba(frame_.rgba);
        frame_.width = playing.decoder.width();
        frame_.height = playing.decoder.height();
        renderer.setImagePixels(*image, frame_);
    }
    return true;
}

void TvStations::update(ts2::sim::World& world, double elapsedMs, ObjectMaterials& materials, ts2::render::IRenderer& renderer) {
    std::set<std::string> tuned;
    for (const auto& [tv, link] : world.tvLinks().links()) {
        tuned.insert(link.station);
    }
    // The stream closes when the last TV leaves its station.
    std::erase_if(playing_, [&](const auto& p) { return !tuned.contains(p.first); });
    for (const std::string& name : tuned) {
        if (playing_.contains(name)) {
            continue;
        }
        const ts2::content::Station* s = catalog_ ? catalog_->find(name) : nullptr;
        if (s == nullptr) {
            continue;
        }
        Playing& p = playing_[name];
        p.station = s;
        start(p, name);
    }
    for (auto it = playing_.begin(); it != playing_.end();) {
        if (play(it->second, it->first, elapsedMs, world, materials.stationImage(it->first), renderer)) {
            ++it;
        } else {
            it = playing_.erase(it);
        }
    }
}

} // namespace app
