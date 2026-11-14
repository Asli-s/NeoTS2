#include "video_recorder.hpp"

#include "diag/log.hpp"
#include "formats/jpeg.hpp"

#include <cmath>
#include <stdexcept>

namespace app {

namespace {

// Pictures waiting to be encoded at most: past this the game hands over no new one (the last is
// repeated instead), so a slow encoder can't hold the game's memory.
constexpr std::size_t kMostWaiting = 4;

} // namespace

VideoRecorder::VideoRecorder(const std::filesystem::path& file, const ts2::content::VideoCaptureSettings& settings)
    : file_(file), settings_(settings), out_(file, std::ios::binary | std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("the video file can't be made");
    }
    writer_ = std::make_unique<ts2::avi::MjpegWriter>(out_, settings_.width, settings_.height, settings_.fps);
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

void VideoRecorder::run() {
    std::vector<std::uint8_t> last;
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return;  // stopping, and nothing waits
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        if (!job.picture.rgba.empty()) {
            auto encoded = ts2::jpeg::encode(job.picture, settings_.jpegQuality);
            if (!encoded.empty()) {
                last = std::move(encoded);
            }
        }
        if (last.empty()) {
            continue;  // nothing to repeat yet
        }
        for (std::uint32_t i = 0; i < job.frames; ++i) {
            writer_->addFrame(last);
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
