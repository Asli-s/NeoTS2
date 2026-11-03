#include "ui/animated_bitmap.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <string_view>

namespace ts2::ui {

namespace {

bool sameText(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

AnimatedBitmap::AnimatedBitmap(std::span<const std::string> constants, int pictureWidth, int frameWidth) {
    frames_ = frameWidth > 0 ? pictureWidth / frameWidth : 0;
    for (const std::string& line : constants) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string_view name(line.data(), equals), value(line.data() + equals + 1, line.size() - equals - 1);
        std::uint32_t number = 0;
        const bool isNumber = std::from_chars(value.data(), value.data() + value.size(), number).ec == std::errc{};
        // "true" turns a switch on and anything else turns it off.
        const bool on = sameText(value, "true");
        if (name == "kType") {
            type_ = value == "kAlphaFade" ? Type::AlphaFade : value == "kPixelStrip" ? Type::PixelStrip : Type::Strip;
        } else if (name == "kRandomStartFrame") {
            random_ = on;
        } else if (name == "kPingPong") {
            pingPong_ = on;
        } else if (name == "kHideWhenDone") {
            hideWhenDone_ = on;
        } else if (!isNumber) {
            continue;
        } else if (name == "kFrameRate") {
            frameRate_ = number;
        } else if (name == "kDuration") {
            duration_ = number;
        } else if (name == "kLoopCount") {
            loops_ = number;
        } else if (name == "kDelay") {
            delay_ = number;
        } else if (name == "kFirstFrameHold") {
            firstHold_ = number;
        } else if (name == "kLastFrameHold") {
            lastHold_ = number;
        } else if (name == "kFrameCount") {
            frames_ = static_cast<int>(std::min<std::uint32_t>(number, 1u << 16));
        } else if (name == "kStartFrameOffset") {
            startOffset_ = number;
        }
    }
    // A step lasts 1000 / kFrameRate ms, cut to whole ms. What the game does without a frame rate
    // is not known yet. Here the timeline stays on the step it started on.
    stepMs_ = frameRate_ > 0 ? static_cast<std::uint32_t>(std::floor(1000.0f / static_cast<float>(frameRate_))) : 0;
    if (stepMs_ > 0) {
        firstSteps_ = firstHold_ / stepMs_;
        lastSteps_ = lastHold_ / stepMs_;
    }
    // Played back and forth, each hold is passed twice.
    if (pingPong_) {
        firstSteps_ /= 2;
        lastSteps_ /= 2;
    }
    steps_ = static_cast<std::uint32_t>(std::max(frames_, 0)) + firstSteps_ + lastSteps_;
}

bool AnimatedBitmap::play(std::uint32_t random) {
    if (frames_ < 2) {
        return false;
    }
    start_ = (random_ ? random % static_cast<std::uint32_t>(frames_) : 0) + firstSteps_ + startOffset_;
    return true;
}

AnimatedBitmap::State AnimatedBitmap::at(std::uint32_t ms) const {
    // Until the delay has passed it shows the step it started on.
    if (ms <= delay_ || steps_ < 2) {
        return {false, pictureFrame(start_)};
    }
    ms -= delay_;
    bool done = false;
    if (duration_ != 0 && ms >= duration_) {
        done = true;
        ms = duration_;
    }
    const std::uint32_t step = (stepMs_ > 0 ? ms / stepMs_ : 0) + start_;
    std::uint32_t shown = 0, loops = 0;
    if (pingPong_) {
        // There and back is one loop, the end steps shown once each way.
        const std::uint32_t leg = steps_ - 1;
        shown = (step / leg) % 2 == 0 ? step % leg : steps_ - 1 - step % leg;
        loops = step / leg / 2;
    } else {
        shown = step % steps_;
        loops = step / steps_;
    }
    if (loops_ != 0 && loops >= loops_) {
        done = true;
        shown = pingPong_ ? start_ : steps_ - lastSteps_;
    }
    return {done, pictureFrame(shown)};
}

int AnimatedBitmap::pictureFrame(std::uint32_t step) const {
    if (step <= firstSteps_) {
        return 0;
    }
    if (step >= steps_ - lastSteps_) {
        return frames_ - 1;
    }
    return static_cast<int>(step - firstSteps_);
}

int AnimatedBitmap::alpha(int frame) const {
    if (frames_ < 2) {
        return 255;
    }
    return static_cast<int>(std::floor(static_cast<float>(frame) / static_cast<float>(frames_ - 1) * 255.0f));
}

} // namespace ts2::ui
