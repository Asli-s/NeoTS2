#pragma once

#include "audio/track.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace ts2::audio {

// A station's songs in shuffled order: every song once before any plays again, and never the same
// song twice in a row.
class Playlist {
public:
    explicit Playlist(std::vector<SampleKey> songs, std::uint32_t seed = 1);

    [[nodiscard]] bool empty() const { return songs_.empty(); }
    // The song that take() gives next.
    [[nodiscard]] SampleKey upcoming();
    SampleKey take();

private:
    void reshuffle();

    std::vector<SampleKey> songs_;
    std::vector<std::size_t> order_;
    std::size_t at_ = 0;
    bool started_ = false;
    std::size_t last_ = 0;
    std::mt19937 random_;
};

} // namespace ts2::audio
