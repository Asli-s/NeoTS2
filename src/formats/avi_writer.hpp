#pragma once

#include <cstdint>
#include <ostream>
#include <span>
#include <vector>

namespace ts2::avi {

// Writes a video as an AVI file of Motion JPEG frames (one video stream, "MJPG", every frame a key
// frame, with an idx1 index), written as frames come so a long recording isn't held in memory. The
// stream is written from its current place on; finish() goes back to fill in the counts and sizes.
class MjpegWriter {
public:
    MjpegWriter(std::ostream& out, std::uint32_t width, std::uint32_t height, std::uint32_t fps);

    // Adds a frame: a baseline JPEG of the video's size.
    void addFrame(std::span<const std::uint8_t> jpeg);
    std::uint32_t frames() const { return static_cast<std::uint32_t>(index_.size()); }

    // Writes the index and the header's counts. Nothing can be added after.
    void finish();

private:
    struct IndexEntry {
        std::uint32_t offset, size;
    };
    std::ostream& out_;
    std::uint32_t width_, height_, fps_;
    std::streampos start_, totalFramesAt_, lengthAt_, moviAt_, maxBytesAt_, bufferAt_, streamBufferAt_;
    std::vector<IndexEntry> index_;
    std::uint32_t largest_ = 0;
    bool finished_ = false;
};

} // namespace ts2::avi
