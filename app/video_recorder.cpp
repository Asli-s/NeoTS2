#include "video_recorder.hpp"

#include "diag/log.hpp"
#include "formats/jpeg.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace app {

namespace {

// Pictures waiting to be encoded at most: past this the game hands over no new one (the last is
// repeated instead), so a slow encoder can't hold the game's memory.
constexpr std::size_t kMostWaiting = 4;

} // namespace

VideoRecorder::VideoRecorder(const std::filesystem::path& file, const ts2::content::VideoCaptureSettings& settings, std::optional<int> soundRate)
    : file_(file), settings_(settings), out_(file, std::ios::binary | std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("the video file can't be made");
    }
    // Sound when the options ask for it and there is any: mono at the sound's rate over the divisor.
    std::optional<ts2::avi::AudioFormat> audio;
    if (settings_.sound && soundRate && *soundRate > 0 && settings_.soundRateDivisor > 0) {
        soundDivisor_ = settings_.soundRateDivisor;
        audio = ts2::avi::AudioFormat{1, static_cast<std::uint32_t>(*soundRate / soundDivisor_)};
    }
    writer_ = std::make_unique<ts2::avi::MjpegWriter>(out_, settings_.width, settings_.height, settings_.fps, audio);
    worker_ = std::thread([this] { run(); });
}

VideoRecorder::~VideoRecorder() { finish(); }

std::uint64_t VideoRecorder::framesBy(double seconds) const {
    return static_cast<std::uint64_t>(std::floor(std::max(seconds, 0.0) * settings_.fps)) + 1;
}

bool VideoRecorder::busy() const {
    std::lock_guard lock(mutex_);
    return jobs_.size() >= kMostWaiting;
}

void VideoRecorder::add(ts2::tga::Pixels picture, std::uint32_t frames) {
    if (frames == 0) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back({std::move(picture), frames});
        given_ += frames;
    }
    wake_.notify_one();
}

void VideoRecorder::repeat(std::uint32_t frames) { add({}, frames); }

void VideoRecorder::addSound(const float* stereo, std::size_t frames) {
    if (soundDivisor_ <= 0 || stereo == nullptr) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (finished_) {
        return;
    }
    // Each video-rate sample the average of `soundDivisor_` frames, both channels together.
    for (std::size_t i = 0; i < frames; ++i) {
        soundSum_ += (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f;
        if (++soundCount_ == soundDivisor_) {
            const float v = std::clamp(soundSum_ / static_cast<float>(soundDivisor_), -1.0f, 1.0f);
            sound_.push_back(static_cast<std::int16_t>(std::lround(v * 32767.0f)));
            soundSum_ = 0;
            soundCount_ = 0;
        }
    }
}

void VideoRecorder::run() {
    std::vector<std::uint8_t> last;
    std::vector<std::int16_t> sound;
    for (;;) {
        Job job;
        bool done = false;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            done = jobs_.empty();  // stopping, and nothing waits
            if (!done) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            // The sound so far goes in after this picture's frames, so the two stay together.
            sound.swap(sound_);
            sound_.clear();
        }
        if (!job.picture.rgba.empty()) {
            auto encoded = ts2::jpeg::encode(job.picture, settings_.jpegQuality);
            if (!encoded.empty()) {
                last = std::move(encoded);
            }
        }
        if (!done && !last.empty()) {
            for (std::uint32_t i = 0; i < job.frames; ++i) {
                writer_->addFrame(last);
            }
        }
        writer_->addAudio(sound);
        sound.clear();
        if (done) {
            return;
        }
    }
}

std::uint64_t VideoRecorder::finish() {
    {
        std::lock_guard lock(mutex_);
        if (finished_) {
            return writer_->frames();
        }
        finished_ = true;
        stopping_ = true;
    }
    wake_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    try {
        writer_->finish();
        out_.close();
    } catch (const std::exception& ex) {
        ts2::diag::error(std::string("video capture: the video couldn't be finished: ") + ex.what());
    }
    return writer_->frames();
}

} // namespace app
