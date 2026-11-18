#pragma once

#include "content/broadcasts.hpp"
#include "formats/movie.hpp"
#include "render/irenderer.hpp"
#include "scene/scene.hpp"
#include "sim/tv_links.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace app {

class ObjectMaterials;

// Plays the TV stations scripts put on TVs (see sim::TvLinks), as the game does: one movie per
// station, shared by every TV on it, which starts with the first TV and closes when the last one
// leaves. A station plays its folder's shows in order with one commercial between them. A games
// station (IsGames) plays its one movie once, and it and a DontRestart station take their TVs off
// when the movie ends. Pictures are decoded finer than the game does (see movie::Quality).
class TvStations {
public:
    explicit TvStations(std::shared_ptr<const ts2::content::Broadcasts> catalog) : catalog_(std::move(catalog)) {}

    // The station of that name, for TvLinks and the materials. Nullopt for a name that isn't one.
    [[nodiscard]] std::optional<ts2::sim::TvLinks::Station> station(std::string_view name) const;

    // Moves every station on by `elapsedMs` and shows each new picture on its screen's image.
    void update(ts2::sim::World& world, double elapsedMs, ObjectMaterials& materials, ts2::render::IRenderer& renderer);

private:
    // Where a station is in its lists. It stays when the station stops, so a TV turned on again
    // goes on with the next show.
    struct Progress {
        std::size_t nextShow = 0;
        bool commercialNext = false;
        std::optional<std::size_t> lastCommercial;
    };
    struct Playing {
        const ts2::content::Station* station = nullptr;
        std::future<std::vector<std::uint8_t>> loading;
        std::vector<std::uint8_t> file;
        std::optional<ts2::movie::Movie> movie;
        ts2::movie::VideoDecoder decoder{ts2::movie::Quality::Fine};
        double clockMs = 0;
        long decoded = -1;  // the last frame decoded
        bool nothingToPlay = false;
    };

    std::optional<std::filesystem::path> nextMovie(const ts2::content::Station& station, Progress& progress);
    void start(Playing& playing, const std::string& name);
    // False when the station ended for good.
    bool play(Playing& playing, const std::string& name, double elapsedMs, ts2::sim::World& world, std::optional<int> image,
              ts2::render::IRenderer& renderer);

    std::shared_ptr<const ts2::content::Broadcasts> catalog_;
    std::map<std::string, Playing> playing_;
    std::map<std::string, Progress> progress_;
    std::mt19937 random_{std::random_device{}()};
    ts2::scene::Image frame_;
};

} // namespace app
