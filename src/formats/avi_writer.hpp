#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <vector>

namespace ts2::avi {

// The sound a video carries: 16-bit PCM of `channels` channels at `rate` frames a second.
struct AudioFormat {
    std::uint16_t channels = 1;
    std::uint32_t rate = 11025;
};

// Writes a video as an AVI file of Motion JPEG frames (stream 0, "MJPG", every frame a key frame)
// and, when given an audio format, 16-bit PCM sound (stream 1), with an idx1 index. It is written as
// frames and sound come, so a long recording isn't held in memory. The stream is written from its
// current place on; finish() goes back to fill in the counts and sizes.
class MjpegWriter {
public:
    MjpegWriter(std::ostream& out, std::uint32_t width, std::uint32_t height, std::uint32_t fps, std::optional<AudioFormat> audio = std::nullopt);

    // Adds a frame: a baseline JPEG of the video's size.
    void addFrame(std::span<const std::uint8_t> jpeg);
    // Adds sound: interleaved samples, a whole number of frames of the audio format. Ignored for a
    // video without sound.
    void addAudio(std::span<const std::int16_t> samples);
    std::uint32_t frames() const { return frames_; }
    std::uint64_t audioFrames() const { return audioFrames_; }

    // Writes the index and the header's counts. Nothing can be added after.
    void finish();

private:
    struct IndexEntry {
        char id[4];
        std::uint32_t offset, size;
    };
    void chunk(const char* id, std::span<const std::uint8_t> data);

    std::ostream& out_;
    std::uint32_t width_, height_, fps_;
    std::optional<AudioFormat> audio_;
    std::streampos start_, totalFramesAt_, lengthAt_, audioLengthAt_, moviAt_, maxBytesAt_, bufferAt_, streamBufferAt_;
    std::vector<IndexEntry> index_;
    std::uint32_t frames_ = 0, largest_ = 0;
    std::uint64_t audioFrames_ = 0;
    bool finished_ = false;
};

} // namespace ts2::avi
