#include "audio_options_panel.hpp"

#include "content/install.hpp"
#include "content/user_props.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kAudioOptions = 0x49060F01;
constexpr std::uint32_t kMenuButton = 0x0000A242;
// Voices, effects, music, ambience.
constexpr std::array<std::uint32_t, 4> kSliders{0x00000B31, 0x00000B01, 0x00000B41, 0x00000B21};
constexpr std::array<std::uint32_t, 4> kMutes{0x00000010, 0x00000011, 0x00000012, 0x00000013};
// Drawn but not wired up.
constexpr std::uint32_t kQuality = 0x00000B11;
// Track and thumb pictures named by the panel's script.
constexpr std::uint32_t kPictures = 0x499DB772, kTrack = 0xE9F50515, kThumb = 0xE9F50510;
// Track and thumb are drawn in three slices, caps a quarter of the 36 px width. The thumb picture stacks
// four frames: disabled, normal, hover, pressed.
constexpr int kPictureWidth = 36, kBarHeight = 12, kCap = kPictureWidth / 4;
constexpr int kNormalFrame = 1, kHoverFrame = 2, kPressedFrame = 3;
// Scroll-bar values from the script: volumes 0-100 with 5 shown, quality 1-4 with 1 shown. The thumb is
// shown/rows of the track, at least two caps.
constexpr int kVolumeRows = 105, kVolumeShown = 5, kQualityRows = 4, kQualityShown = 1;
constexpr std::array<const char*, 4> kVolumeKeys{"VOXVolume", "FXVolume", "MusicVolume", "AmbienceVolume"};
constexpr std::array<const char*, 4> kMuteKeys{"SoundMasterGroupVoxEnabled", "SoundMasterGroupFXEnabled",
                                               "SoundMasterGroupMusicEnabled", "SoundMasterGroupAmbienceEnabled"};
constexpr std::string_view kNotWorking = "Not working in NeoTS2 yet. The choice is saved, but changes nothing.";

bool readBool(const std::string& text, bool& out) {
    if (text == "true") {
        out = true;
        return true;
    }
    if (text == "false") {
        out = false;
        return true;
    }
    return false;
}

} // namespace

void AudioOptionsPanel::readSettings(const std::filesystem::path& install, const std::filesystem::path& userFolder) {
    userFolder_ = userFolder;
    saved_ = ts2::content::readUserProps(userFolder).values;
    defaults_.clear();
    if (const auto file = ts2::content::newestDataFile(ts2::content::findProducts(install), "Res/Config/globalProps.xml")) {
        defaults_ = ts2::content::readPropsFile(*file).values;
    }
    for (std::size_t i = 0; i < volume_.size(); ++i) {
        volume_[i] = static_cast<float>(std::clamp(number(kVolumeKeys[i], 100), 0, 100)) / 100.0f;
        muted_[i] = !flag(kMuteKeys[i], true);
    }
}

void AudioOptionsPanel::takeSettings(const AudioOptionsPanel& other) {
    userFolder_ = other.userFolder_;
    saved_ = other.saved_;
    defaults_ = other.defaults_;
    volume_ = other.volume_;
    muted_ = other.muted_;
    showValues();
}

int AudioOptionsPanel::number(const char* key, int otherwise) const {
    if (const auto it = saved_.find(key); it != saved_.end()) {
        return std::atoi(it->second.c_str());
    }
    if (const auto it = defaults_.find(key); it != defaults_.end()) {
        return std::atoi(it->second.c_str());
    }
    return otherwise;
}

bool AudioOptionsPanel::flag(const char* key, bool otherwise) const {
    bool value = otherwise;
    if (const auto it = saved_.find(key); it != saved_.end() && readBool(it->second, value)) {
        return value;
    }
    if (const auto it = defaults_.find(key); it != defaults_.end() && readBool(it->second, value)) {
        return value;
    }
    return otherwise;
}

int AudioOptionsPanel::addPicture(int parent, std::uint32_t group, std::uint32_t instance, int w, int h) {
    const ts2::ui::Window window{
        {{"iid", "IGZWinGen"},
         {"winflag_visible", "yes"},
         {"winflag_ignoremouse", "yes"},
         {"paint", "yes"},
         {"area", std::format("(0,0,{},{})", w, h)},
         {"image", std::format("{{{:08x},{:08x}}}", group, instance)}},
        {}};
    return screen_.addWindows({window}, 0, 0, parent);
}

// Builds the track and thumb of one slider; the thumb starts at the left.
std::array<int, 3> AudioOptionsPanel::addSlider(int slider, int rows, int shown, int& length) {
    const int width = screen_.width(slider);
    length = thumbLength(width, rows, shown, kCap);
    const auto slices = [&](std::uint32_t picture, int w, int row) {
        std::array<int, 3> made{};
        const std::array<int, 3> from{0, kCap, kPictureWidth - kCap}, at{0, kCap, w - kCap}, size{kCap, w - 2 * kCap, kCap};
        for (std::size_t i = 0; i < 3; ++i) {
            made[i] = addPicture(slider, kPictures, picture, size[i], kBarHeight);
            const int end = i == 1 ? kPictureWidth - kCap : from[i] + kCap;
            screen_.setImagePart(made[i], {from[i], row * kBarHeight, end, (row + 1) * kBarHeight});
            screen_.move(made[i], at[i], 0);
        }
        return made;
    };
    slices(kTrack, width, 0);
    return slices(kThumb, length, kNormalFrame);
}

void AudioOptionsPanel::load() {
    remove();
    panel_ = screen_.load(kScripts, kAudioOptions, 0, 0, menu_.menu());
    if (panel_ < 0) {
        ts2::diag::warnOnce("audio options: the panel's script is missing, so Audio Options opens nothing");
        return;
    }
    screen_.setVisible(panel_, false);
    menu_.place(panel_);
    for (std::size_t i = 0; i < sliders_.size(); ++i) {
        sliders_[i] = screen_.find(panel_, kSliders[i]);
        muteButtons_[i] = screen_.find(panel_, kMutes[i]);
        if (sliders_[i] < 0) {
            continue;
        }
        thumbs_[i] = addSlider(sliders_[i], kVolumeRows, kVolumeShown, thumbLength_[i]);
    }
    if (const int quality = screen_.find(panel_, kQuality); quality >= 0) {
        int length = 0;
        addSlider(quality, kQualityRows, kQualityShown, length);
        screen_.setTip(quality, std::string(kNotWorking));
    }
    showValues();
}

void AudioOptionsPanel::remove() {
    if (panel_ >= 0) {
        menu_.close();
        screen_.remove(panel_);
        panel_ = -1;
    }
    sliders_ = {-1, -1, -1, -1};
    thumbs_ = {};
    hover_ = -1;
    muteButtons_ = {-1, -1, -1, -1};
}

int AudioOptionsPanel::thumbX(int group) const {
    const int room = std::max(0, screen_.width(sliders_[group]) - thumbLength_[group]);
    return static_cast<int>(std::lround(volume_[group] * room));
}

void AudioOptionsPanel::showFrame(int group) {
    if (sliders_[group] < 0) {
        return;
    }
    const int frame = dragging_ == group ? kPressedFrame : hover_ == group ? kHoverFrame : kNormalFrame;
    const std::array<int, 3> from{0, kCap, kPictureWidth - kCap}, end{kCap, kPictureWidth - kCap, kPictureWidth};
    for (std::size_t i = 0; i < 3; ++i) {
        screen_.setImagePart(thumbs_[group][i], {from[i], frame * kBarHeight, end[i], (frame + 1) * kBarHeight});
    }
}

bool AudioOptionsPanel::onThumb(int group, int x, int y) const {
    const auto at = screen_.position(sliders_[group]);
    const int left = at[0] + thumbX(group);
    return x >= left && x < left + thumbLength_[group] && y >= at[1] && y < at[1] + screen_.height(sliders_[group]);
}

void AudioOptionsPanel::showValues() {
    for (std::size_t i = 0; i < volume_.size(); ++i) {
        if (sliders_[i] >= 0) {
            const int x = thumbX(static_cast<int>(i)), length = thumbLength_[i];
            screen_.move(thumbs_[i][0], x, 0);
            screen_.move(thumbs_[i][1], x + kCap, 0);
            screen_.move(thumbs_[i][2], x + length - kCap, 0);
        }
        if (muteButtons_[i] >= 0) {
            screen_.setChecked(muteButtons_[i], muted_[i]);
        }
    }
}

void AudioOptionsPanel::show(bool show) {
    if (panel_ < 0 || show == shown()) {
        return;
    }
    if (show) {
        showValues();
        menu_.open(panel_, kMenuButton);
    } else {
        menu_.close();
    }
}

bool AudioOptionsPanel::clicked(int window) {
    if (panel_ < 0 || window < 0) {
        return false;
    }
    if (window == menu_.button(kMenuButton)) {
        show(!shown());
        return true;
    }
    if (!shown()) {
        return false;
    }
    for (std::size_t i = 0; i < muteButtons_.size(); ++i) {
        if (window == muteButtons_[i]) {
            muted_[i] = !muted_[i];
            showValues();
            changed_ = true;
            saveMute(static_cast<int>(i));
            return true;
        }
    }
    // The panel takes the clicks it covers, so the world doesn't get them.
    for (int w = window; w >= 0; w = screen_.parent(w)) {
        if (w == panel_) {
            return true;
        }
    }
    return false;
}

bool AudioOptionsPanel::mouseDown(int x, int y) {
    if (panel_ < 0 || !shown()) {
        return false;
    }
    for (std::size_t i = 0; i < sliders_.size(); ++i) {
        if (sliders_[i] < 0) {
            continue;
        }
        const auto at = screen_.position(sliders_[i]);
        if (x >= at[0] && x < at[0] + screen_.width(sliders_[i]) && y >= at[1] && y < at[1] + screen_.height(sliders_[i])) {
            dragging_ = static_cast<int>(i);
            // Pressing the thumb keeps the grab point; pressing the track centres the thumb there.
            grab_ = onThumb(dragging_, x, y) ? x - at[0] - thumbX(dragging_) : thumbLength_[i] / 2;
            setFromX(dragging_, x);
            showFrame(dragging_);
            return true;
        }
    }
    return false;
}

void AudioOptionsPanel::mouseMove(int x, int y) {
    if (dragging_ >= 0) {
        setFromX(dragging_, x);
        return;
    }
    int over = -1;
    for (int i = 0; panel_ >= 0 && shown() && i < static_cast<int>(sliders_.size()); ++i) {
        if (sliders_[i] >= 0 && onThumb(i, x, y)) {
            over = i;
        }
    }
    if (over != hover_) {
        const int before = hover_;
        hover_ = over;
        for (const int g : {before, over}) {
            if (g >= 0) {
                showFrame(g);
            }
        }
    }
}

void AudioOptionsPanel::mouseUp(int x, int y) {
    const int released = dragging_;
    dragging_ = -1;
    if (released >= 0) {
        saveVolume(released);
        hover_ = onThumb(released, x, y) ? released : -1;
        showFrame(released);
    }
}

void AudioOptionsPanel::setFromX(int group, int x) {
    if (group < 0 || group >= static_cast<int>(volume_.size()) || sliders_[group] < 0) {
        return;
    }
    const int left = screen_.position(sliders_[group])[0];
    const int room = std::max(1, screen_.width(sliders_[group]) - thumbLength_[group]);
    const float value = std::clamp(static_cast<float>(x - grab_ - left) / static_cast<float>(room), 0.0f, 1.0f);
    const float rounded = std::round(value * 100.0f) / 100.0f;
    if (rounded == volume_[group]) {
        return;
    }
    volume_[group] = rounded;
    showValues();
    changed_ = true;
}

void AudioOptionsPanel::saveVolume(int group) {
    const std::string value = std::to_string(std::lround(volume_[group] * 100.0f));
    if (!ts2::content::saveUserProps(userFolder_, {{kVolumeKeys[group], {value, false}}})) {
        ts2::diag::warnOnce("audio options: a level could not be saved to userProps.xml");
    }
}

void AudioOptionsPanel::saveMute(int group) {
    const bool on = !muted_[group];
    if (!ts2::content::saveUserProps(userFolder_, {{kMuteKeys[group], {on ? "true" : "false", true}}})) {
        ts2::diag::warnOnce("audio options: a mute could not be saved to userProps.xml");
    }
}

bool AudioOptionsPanel::takeChanged() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

} // namespace app
