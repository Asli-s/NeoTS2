#pragma once

#include "formats/tga.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::cursor {

// A mouse cursor from a Windows .cur file: its picture and the point of it that is the mouse's position.
struct Cursor {
    tga::Pixels pixels;
    std::uint32_t hotX = 0, hotY = 0;
};

// An animated cursor from a Windows .ani file: its pictures, and the steps that show them in turn.
// The animation loops.
struct AnimatedCursor {
    std::vector<Cursor> frames;
    struct Step {
        std::uint32_t frame = 0;    // which picture, always one of frames
        std::uint32_t jiffies = 1;  // how long it shows, in sixtieths of a second (at least 1)
    };
    std::vector<Step> steps;
};

// Cursors are small: anything wider or taller than this is a broken file.
constexpr std::uint32_t kMaxSide = 256;
// Limits for animated cursors, far above the game's (56 pictures in 63 steps). They keep a broken
// file's pictures to 32 MB at most.
constexpr std::uint32_t kMaxFrames = 128, kMaxSteps = 1024;

// The first picture of a .cur file (1, 4, 8, 24 or 32 bits per pixel, uncompressed). A pixel the
// file's mask leaves out is transparent. Empty for other kinds or broken data.
[[nodiscard]] std::optional<Cursor> decode(std::span<const std::uint8_t> data);

// An .ani file whose pictures are .cur files, each read as decode() reads one. Without a sequence
// the pictures show in order, and without per-step times each shows for the file's one rate. Empty
// for other kinds or broken data.
[[nodiscard]] std::optional<AnimatedCursor> decodeAnimated(std::span<const std::uint8_t> data);

// Plays an animated cursor's steps as Windows plays an .ani file: each for its own time, looping.
class CursorPlayback {
public:
    // Starts again from the first step at the next call to step().
    void restart();
    // The step shown at `seconds` (any steady clock). After a stall of over a second (a slow frame)
    // the step shown starts again rather than racing through the ones missed.
    [[nodiscard]] std::size_t step(std::span<const AnimatedCursor::Step> steps, double seconds);

private:
    std::size_t step_ = 0;
    std::optional<double> stepEnds_;  // when the step shown ends, on step()'s clock
};

} // namespace ts2::cursor
