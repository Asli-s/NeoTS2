#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace ts2::sim {

struct SimObject;

// Which TV (or computer, or console's TV) shows which station. Change Material with its TV flag
// tunes a TV to the station its material names, and the app plays each station's movie on the
// TVs tuned to it. A TV's attribute 11 tells its scripts how the movie goes: 1 while it plays, 2
// once it has ended or the TV left the station, -1 when it left a station that doesn't restart.
// Console scripts wait for 2 ("CT - VGS - Movie Playing?").
class TvLinks {
public:
    static constexpr std::size_t kMovieAttribute = 11;

    struct Station {
        bool dontRestart = false;  // DontRestart=1: stops after its movie
    };
    // Tells whether a name is a station. Without it no name is one.
    using Lookup = std::function<std::optional<Station>(std::string_view name)>;
    void setLookup(Lookup lookup) { lookup_ = std::move(lookup); }

    // Takes the TV off its station, then puts it on the named one if that is a station.
    // "tvstation_none" turns it off only.
    void tune(SimObject& tv, std::string_view station);
    // Takes the TV off its station, if it is on one.
    void leave(SimObject& tv);
    // The station's movie has ended for good: every TV on it leaves it.
    void end(std::string_view station, const std::function<SimObject*(std::int16_t)>& find);
    // Forgets a TV that left the lot, without telling its scripts.
    void forget(std::int16_t tv);

    struct Link {
        std::string station;  // lower case
        bool dontRestart = false;
    };
    const std::map<std::int16_t, Link>& links() const { return links_; }
    // Goes up whenever links() changes.
    std::uint64_t version() const { return version_; }

private:
    Lookup lookup_;
    std::map<std::int16_t, Link> links_;
    std::uint64_t version_ = 0;
};

} // namespace ts2::sim
