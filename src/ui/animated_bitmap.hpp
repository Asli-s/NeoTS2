#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace ts2::ui {

// The UI's animated bitmap (windows of class 0x4d9ccdb1): a picture played as frames, set up by the
// "kName=value" texts of its "AnimatedBMP Constants Table" child (id 0x8d9df6d4). A strip shows one
// frame of its picture at a time, side by side from the left. An alpha fade shows the whole picture
// at an alpha that rises with the frame.
class AnimatedBitmap {
public:
    enum class Type { Strip = 0, PixelStrip = 1, AlphaFade = 2 };

    // `pictureWidth` and `frameWidth` give the strip's frame count when the table has none.
    // Settings it doesn't know are skipped.
    AnimatedBitmap(std::span<const std::string> constants, int pictureWidth, int frameWidth);

    // Starts it. `random` (0..2^31) picks the first frame when kRandomStartFrame is set. False when
    // it has fewer than 2 frames: the game then leaves it hidden and it never ends.
    bool play(std::uint32_t random);

    // Where it is `ms` after play: whether it has ended, and the frame of the picture it shows.
    struct State {
        bool done = false;
        int frame = 0;
    };
    [[nodiscard]] State at(std::uint32_t ms) const;

    // An alpha fade's alpha (0..255) at a frame.
    [[nodiscard]] int alpha(int frame) const;

    Type type() const { return type_; }
    int frames() const { return frames_; }
    bool hideWhenDone() const { return hideWhenDone_; }

private:
    // The frame of the picture a step of the timeline shows: the first one through the first hold,
    // the last one from the last hold.
    int pictureFrame(std::uint32_t step) const;

    Type type_ = Type::Strip;
    std::uint32_t frameRate_ = 0, duration_ = 0, loops_ = 0, delay_ = 0, firstHold_ = 0, lastHold_ = 0, startOffset_ = 0;
    bool random_ = false, pingPong_ = false, hideWhenDone_ = false;
    int frames_ = 0;  // frames of the picture
    // The timeline in steps of `stepMs_`: the first hold, the frames, then the last hold.
    std::uint32_t stepMs_ = 0, firstSteps_ = 0, lastSteps_ = 0, steps_ = 0, start_ = 0;
};

} // namespace ts2::ui
