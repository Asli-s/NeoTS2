#pragma once

#include "content/video_capture.hpp"
#include "formats/avi_writer.hpp"
#include "formats/tga.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <thread>

namespace app {

// A Video Capture recording: pictures handed to it are encoded as JPEG and written to an AVI file on
// a thread of its own, so the game doesn't wait for them. Each picture stands for as many frames as
// the time it covers asks for, so the video keeps real time when the game draws fewer frames than
// it records. The file is finished when the recorder is let go of.
class VideoRecorder {
public:
    // Starts writing `file`. Throws when it can't be made.
    // With `soundRate` (the game's sound output rate), the video records sound too, as the settings ask.
    VideoRecorder(const std::filesystem::path& file, const ts2::content::VideoCaptureSettings& settings, std::optional<int> soundRate = std::nullopt);
    ~VideoRecorder();
    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    const ts2::content::VideoCaptureSettings& settings() const { return settings_; }
    const std::filesystem::path& file() const { return file_; }

    // How many frames the video should have after `seconds` of recording.
    std::uint64_t framesBy(double seconds) const;
    // The frames handed over so far (written or waiting).
    std::uint64_t framesGiven() const { return given_; }
    // Whether pictures wait to be encoded: a picture handed over now would wait behind them.
    bool busy() const;
    // Hands over a picture of the video's size standing for `frames` frames.
    void add(ts2::tga::Pixels picture, std::uint32_t frames);
    // Repeats the last picture for `frames` more frames (when the game drew none since).
    void repeat(std::uint32_t frames);

    // The game's sound as it plays: interleaved stereo floats at the sound's rate. Called from the
    // sound device's thread; it is made mono at the video's rate and written between frames.
    void addSound(const float* stereo, std::size_t frames);
    bool recordsSound() const { return soundDivisor_ > 0; }

    // Stops, writes what waits and finishes the file. The frames written.
    std::uint64_t finish();

private:
    struct Job {
        ts2::tga::Pixels picture;  // empty: repeat the last frame
        std::uint32_t frames = 1;
    };
    void run();

    std::filesystem::path file_;
    ts2::content::VideoCaptureSettings settings_;
    std::ofstream out_;
    std::unique_ptr<ts2::avi::MjpegWriter> writer_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Job> jobs_;
    bool stopping_ = false, finished_ = false;
    std::uint64_t given_ = 0;
    // Sound waiting to be written, and the average being made of the next video-rate sample.
    int soundDivisor_ = 0;
    std::vector<std::int16_t> sound_;
    float soundSum_ = 0;
    int soundCount_ = 0;
    std::thread worker_;
};

} // namespace app
