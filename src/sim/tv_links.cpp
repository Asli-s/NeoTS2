#include "sim/tv_links.hpp"

#include "sim/world.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace ts2::sim {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

void TvLinks::leave(SimObject& tv) {
    const auto it = links_.find(tv.id);
    if (it == links_.end()) {
        return;
    }
    tv.attributes[kMovieAttribute] = it->second.dontRestart ? -1 : 2;
    links_.erase(it);
    ++version_;
}

void TvLinks::tune(SimObject& tv, std::string_view station) {
    leave(tv);
    const std::string name = lower(station);
    if (name == "tvstation_none" || !lookup_) {
        return;
    }
    const auto found = lookup_(name);
    if (!found) {
        return;
    }
    // The game writes the station's play state here, which is 1 while a games station plays its
    // movie. For a TV station only its being neither 0 nor 2 is read.
    tv.attributes[kMovieAttribute] = 1;
    links_[tv.id] = {name, found->dontRestart};
    ++version_;
}

void TvLinks::end(std::string_view station, const std::function<SimObject*(std::int16_t)>& find) {
    const std::string name = lower(station);
    std::vector<std::int16_t> tvs;
    for (const auto& [id, link] : links_) {
        if (link.station == name) {
            tvs.push_back(id);
        }
    }
    for (const std::int16_t id : tvs) {
        if (SimObject* tv = find ? find(id) : nullptr) {
            leave(*tv);
        } else {
            forget(id);
        }
    }
}

void TvLinks::forget(std::int16_t tv) {
    if (links_.erase(tv) != 0) {
        ++version_;
    }
}

} // namespace ts2::sim
