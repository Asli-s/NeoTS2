#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The game's .movie files: EA chunked files holding MAD video (MADk, MADm and MADe chunks) and
// EA ADPCM sound (an SCHl header, SCDl blocks). TVs and console games show them.
namespace ts2::movie {

// Limits far above anything the game ships, so a broken file can't ask for huge amounts of memory
// or time. The largest game movie is about 5 MB with 2,000 frames.
constexpr std::size_t kMaxFileBytes = 64u << 20;
constexpr std::size_t kMaxChunks = 1u << 18;
constexpr std::uint16_t kMaxSide = 1024;

enum class FrameKind : std::uint8_t {
    Key,        // MADk: every block drawn from its own data
    Predicted,  // MADm: blocks may copy from the frame before, and later frames build on it
    Passing,    // MADe: like MADm, but the next frame builds on the one before it, not on this one
};

// Where a chunk's data (after its 8-byte tag and size) lies in the file.
struct Chunk {
    std::uint32_t offset = 0, size = 0;
};

struct VideoFrame {
    FrameKind kind = FrameKind::Key;
    Chunk chunk;
};

// The sound's header. Only revision 1 sound (ADPCM in SCDl blocks) is read, as the game's movies use.
struct Sound {
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t samples = 0;  // per channel, from the header
    std::vector<Chunk> blocks;
};

struct Movie {
    std::uint16_t width = 0, height = 0;
    std::uint16_t frameMs = 0;  // how long each frame shows, from the first frame's header
    std::vector<VideoFrame> frames;
    Sound sound;  // no channels when there is none the game would play
};

// Reads a movie's chunk list. Video frames of another size than the first are left out. Throws
// std::runtime_error when the file is not a movie or has no video.
[[nodiscard]] Movie read(std::span<const std::uint8_t> file);

// How a decoder turns the data into pictures.
enum class Quality : std::uint8_t {
    // As the game does: samples in 7 bits (half the 8-bit value) packed two pixels to a 32-bit word
    // (Cr, Cb, the odd pixel's Y, the even pixel's Y), each chroma sample over 2 x 2 pixels, and
    // full-range BT.601 colour tables.
    Game,
    // The same data in full 8 bits, chroma blended between its samples, and the 8 x 8 block edges
    // smoothed by how coarsely the frame is quantised. The smoothing is left out of the frames
    // later ones build on, as the encoder made them.
    Fine,
};

// Decodes a movie's frames in order. A frame needs the frames before it, so after a seek start
// again from a key frame.
class VideoDecoder {
public:
    explicit VideoDecoder(Quality quality = Quality::Game) : quality_(quality) {}
    // Decodes one frame of `file`. False when its data is broken, which leaves the picture as far as
    // it got. A Predicted or Passing frame with no frame before it builds on a black one.
    bool decode(std::span<const std::uint8_t> file, const Movie& movie, const VideoFrame& frame);
    // The last decoded picture as RGBA, `width` x `height`, alpha 255.
    void rgba(std::vector<std::uint8_t>& out) const;
    [[nodiscard]] std::uint16_t width() const { return width_; }
    [[nodiscard]] std::uint16_t height() const { return height_; }
    // The last picture's samples, for tests: Game packs 2 bytes a pixel, Fine holds the Y plane,
    // then the Cb and Cr planes at half the width and height.
    [[nodiscard]] const std::vector<std::uint8_t>& packed() const { return shownIsReference_ ? reference_ : out_; }

private:
    bool macroblock(std::size_t mbX, std::size_t mbY);
    void store(std::size_t mbX, std::size_t mbY);
    void predictFine(std::size_t mbX, std::size_t mbY, int block, int delta);
    void rgbaFine(std::vector<std::uint8_t>& out) const;
    bool codedBlock(std::int32_t* work, std::size_t stride);
    void predictLuma(std::size_t at, int bx, int by, std::uint8_t add);
    void predictChroma(std::size_t at, bool cb, std::uint8_t add);
    int motion();
    std::uint32_t peek(int count) const;
    void skip(int count);
    std::uint8_t refByte(std::ptrdiff_t at) const;

    Quality quality_;
    std::uint16_t width_ = 0, height_ = 0;
    int quantiser_ = 0;  // the frame's quantiser scale
    std::vector<std::uint8_t> out_, reference_;
    bool shownIsReference_ = false;  // the last frame shown became the reference
    std::array<std::int32_t, 64> quant_{};
    bool inter_ = false;
    int mvx_ = 0, mvy_ = 0;
    std::span<const std::uint8_t> bits_;
    std::size_t bitPos_ = 0;
    std::array<std::int32_t, 256> luma_{};
    std::array<std::int32_t, 64> cb_{}, cr_{};
    mutable std::vector<std::uint8_t> shown_;  // Fine: the smoothed copy of the last picture
};

} // namespace ts2::movie
