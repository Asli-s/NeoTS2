// Video Capture (V, or the puck's video button): records the view, without the panels, to a video
// in the user folder's Movies folder, at the size, frame rate and length the game options ask for.
#include "game.hpp"

#include "content/install.hpp"
#include "content/user_props.hpp"
#include "content/video_capture.hpp"
#include "diag/log.hpp"

#include <format>

namespace app {

namespace {

// UIText: the box's title ("Video Capture"), its text ("Your Video Capture is saved in: %s") and OK.
constexpr std::uint32_t kUiText = 0x7FA7DC7C;
constexpr std::uint32_t kVideoTitle = 0x00a9002a, kVideoSaved = 0x00a9002b, kOkText = 0x008E0001;  // string ids, leakcheck: ok
constexpr std::uint32_t kOkButton = 1;

} // namespace

void Game::toggleVideoCapture() {
    if (videoRecorder_) {
        stopVideoCapture(true);
        return;
    }
    if (!(inLot_ || inNeighborhood_) || options_.user.empty()) {
        return;
    }
    // The settings the player chose in the game options, else the install's.
    ts2::content::UserProps defaults;
    if (const auto global = ts2::content::newestDataFile(ts2::content::findProducts(install_), "Res/Config/globalProps.xml")) {
        defaults = ts2::content::readPropsFile(*global);
    }
    const auto settings = ts2::content::videoCaptureSettings(ts2::content::readUserProps(options_.user), defaults);
    const std::filesystem::path folder = options_.user / "Movies";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const auto file = ts2::content::nextMovieFile(folder);
    try {
        const std::optional<int> soundRate = sound_ ? sound_->outputRate() : std::nullopt;
        videoRecorder_ = std::make_unique<VideoRecorder>(file, settings, soundRate);
    } catch (const std::exception& ex) {
        ts2::diag::warning(std::format("video capture: {} can't be written: {}", ts2::diag::utf8(file), ex.what()));
        return;
    }
    // The game's sound goes in as it plays (the sound device's thread hands each mix over).
    if (sound_ && videoRecorder_->recordsSound()) {
        VideoRecorder* recorder = videoRecorder_.get();
        sound_->setCapture([recorder](const float* stereo, std::size_t frames) { recorder->addSound(stereo, frames); });
    }
    videoStartedAt_ = ts2::platform::ticksNs();
    lotUi_.setVideoRecording(true);
    hoodUi_.setVideoRecording(true);
    ts2::diag::step(std::format("video capture: recording {} at {} x {}, {} frames a second, at most {} s, {}", ts2::diag::utf8(file.filename()),
                                settings.width, settings.height, settings.fps, settings.maxSeconds,
                                videoRecorder_->recordsSound() ? "with sound" : settings.sound ? "without sound (no playback device)" : "without sound"));
}

void Game::captureVideoFrame(std::uint64_t now) {
    if (!videoRecorder_) {
        return;
    }
    if (!(inLot_ || inNeighborhood_)) {
        stopVideoCapture(true);
        return;
    }
    const auto& settings = videoRecorder_->settings();
    // A frame begun before the recording started counts as its start.
    const double seconds = now > videoStartedAt_ ? static_cast<double>(now - videoStartedAt_) / 1e9 : 0.0;
    if (seconds >= settings.maxSeconds) {
        stopVideoCapture(true);  // as long as the options let a video be
        return;
    }
    const std::uint64_t due = videoRecorder_->framesBy(seconds);
    const std::uint64_t given = videoRecorder_->framesGiven();
    if (due <= given) {
        return;
    }
    const auto frames = static_cast<std::uint32_t>(due - given);
    if (videoRecorder_->busy()) {
        videoRecorder_->repeat(frames);  // the encoder is behind: the last picture stands for these
        return;
    }
    // The view at the video's size, the panels left out.
    std::vector<ts2::render::Sprite> panels;
    renderer_.showSprites(panels, true);
    auto image = renderer_.drawToImage(viewCamera(), settings.width, settings.height);
    renderer_.showSprites(panels, true);
    if (!image || image->rgba.size() != static_cast<std::size_t>(settings.width) * settings.height * 4) {
        ts2::diag::warnOnce(image ? std::format("video capture: the view was drawn {} x {}, not the video's size", image->width, image->height)
                                  : std::string("video capture: the view couldn't be drawn for the video"));
        videoRecorder_->repeat(frames);
        return;
    }
    ts2::tga::Pixels picture{image->width, image->height, std::move(image->rgba)};
    for (std::size_t i = 3; i < picture.rgba.size(); i += 4) {
        picture.rgba[i] = 255;
    }
    videoRecorder_->add(std::move(picture), frames);
}

void Game::stopVideoCapture(bool tell) {
    if (!videoRecorder_) {
        return;
    }
    if (sound_) {
        sound_->setCapture(nullptr);  // no mix reaches the recorder from here on
    }
    const auto file = videoRecorder_->file();
    const auto frames = videoRecorder_->finish();
    const auto fps = videoRecorder_->settings().fps;
    videoRecorder_.reset();
    lotUi_.setVideoRecording(false);
    hoodUi_.setVideoRecording(false);
    ts2::diag::step(std::format("video capture: saved {} ({} frames, {:.1f} s)", ts2::diag::utf8(file.filename()), frames,
                                static_cast<double>(frames) / fps));
    if (tell) {
        std::string text = data_.text(kUiText, kVideoSaved);
        if (const auto at = text.find("%s"); at != std::string::npos) {
            text.replace(at, 2, ts2::diag::utf8(file));
        }
        noticeBox_.open(screen_, uiWidth_, uiHeight_, data_.text(kUiText, kVideoTitle), text, {{kOkButton, data_.text(kUiText, kOkText)}});
    }
}

} // namespace app
