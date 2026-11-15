#include "audio/music.hpp"

#include <numeric>
#include <utility>

namespace ts2::audio {

Playlist::Playlist(std::vector<SampleKey> songs, std::uint32_t seed) : songs_(std::move(songs)), random_(seed) {
    order_.resize(songs_.size());
    at_ = order_.size();
}

void Playlist::reshuffle() {
    std::iota(order_.begin(), order_.end(), std::size_t{0});
    // Fisher-Yates by hand: std::shuffle's result differs between standard libraries.
    for (std::size_t i = order_.size(); i > 1; --i) {
        std::swap(order_[i - 1], order_[random_() % i]);
    }
    if (started_ && order_.size() > 1 && order_.front() == last_) {
        std::swap(order_[0], order_[1]);
    }
    at_ = 0;
}

SampleKey Playlist::upcoming() {
    if (at_ >= order_.size()) {
        reshuffle();
    }
    return songs_[order_[at_]];
}

SampleKey Playlist::take() {
    const SampleKey song = upcoming();
    last_ = order_[at_++];
    started_ = true;
    return song;
}

} // namespace ts2::audio
