#include "audio_options_panel.hpp"

#include "content/install.hpp"
#include "content/user_props.hpp"
#include "diag/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11, kAudioOptions = 0x49060F01;
constexpr std::uint32_t kMenuButton = 0x0000A242;
// Voices, effects, music, ambience, then Audio Quality.
constexpr std::array<std::uint32_t, 5> kSliders{0x00000B31, 0x00000B01, 0x00000B41, 0x00000B21, 0x00000B11};
constexpr std::size_t kQualitySlider = 4;
constexpr std::array<std::uint32_t, 4> kMutes{0x00000010, 0x00000011, 0x00000012, 0x00000013};
// The Speakers radios (Stereo, 4.0, 5.1) and the value each saves as SoundOutputMode.
constexpr std::array<std::uint32_t, 3> kSpeakers{0x00000A01, 0x00000A03, 0x00000A04};
constexpr std::array<const char*, 3> kSpeakerModes{"stereo", "4.0", "5.1"};
// Track and thumb pictures named by the panel's script.
constexpr std::uint32_t kPictures = 0x499DB772, kTrack = 0xE9F50515, kThumb = 0xE9F50510;
// Track and thumb are drawn in three slices, caps a quarter of the 36 px width. The thumb picture stacks
// four frames: disabled, normal, hover, pressed.
constexpr int kPictureWidth = 36, kBarHeight = 12, kCap = kPictureWidth / 4;
constexpr int kNormalFrame = 1, kHoverFrame = 2, kPressedFrame = 3;
// Scroll-bar values from the script: volumes 0-100 with 5 shown, quality 1-4 with 1 shown, so quality has
// three places (1, 2 and 3: Low, Medium and High, the numbers Graphics Rules gives them). The thumb is
// shown/rows of the track, at least two caps.
constexpr int kVolumeRows = 105, kVolumeShown = 5, kQualityRows = 3, kQualityShown = 1;
// Audio Quality is the game's SoundQuality option, saved as its setting number like the other options.
constexpr const char* kQualityKey = "optionSoundQuality";
constexpr std::array<const char*, 4> kVolumeKeys{"VOXVolume", "FXVolume", "MusicVolume", "AmbienceVolume"};
constexpr std::array<const char*, 4> kMuteKeys{"SoundMasterGroupVoxEnabled", "SoundMasterGroupFXEnabled",
                                               "SoundMasterGroupMusicEnabled", "SoundMasterGroupAmbienceEnabled"};
// The tabs: the stereos, Buy, Build, Neighborhood and Create a Sim, and the modes' own stations.
constexpr std::array<std::uint32_t, 5> kTabs{0x00000D06, 0x00000D02, 0x00000D03, 0x00000D04, 0x00000D05};
constexpr std::array<const char*, 5> kTabStations{"", "buy", "build", "nhood", "cas"};
constexpr std::uint32_t kStereoPanel = 0x200, kModePanel = 0x100, kStereoOverlay = 0xCC, kModeOverlay = 0xDD;
constexpr std::uint32_t kSongBox = 0x0CC3BA9B, kSongList = 0x297, kSongBar = 0x299, kStationBox = 0x400, kStationTemplate = 0x201;
constexpr std::array<std::uint32_t, 2> kStationArrows{0x2AA, 0x2BB};
// The Custom Radio Stations button: one at the stations' top left on the stereos' tab, the other at the
// song list's top right on the other tabs, each shown only on its own tabs.
constexpr std::array<std::uint32_t, 2> kCustomStations{0xD07, 0xD08};
// The song list sits 107 px further left under the modes' smaller panel, as their overlay does.
constexpr int kStereoListX = 277, kModeListX = 170;
// The station buttons stand one button's height apart, as many as the box (215 high) holds.
constexpr int kStationFirstY = 11, kStationPitch = 20, kStationRows = 10, kStationWidth = 116, kStationHeight = 20;
constexpr std::uint32_t kFirstStationId = 0x00B00000;  // window id, leakcheck: ok
constexpr std::string_view kStillStereo = "NeoTS2 plays in stereo for now. The choice is saved.";

// A song's row, as the game builds it from its row script (UI script 0x49060F04, the one for every
// screen size but 800 x 600): 19 high with 5 between rows, as many as fit, spread over the list's
// height. The bar is the panel script's own (0x299), 9 past the list's right end and 15 below its
// top, 241 high.
constexpr std::uint32_t kSongRowScript = 0x49060F04;
OptionsList::RowStyle songRows() {
    OptionsList::RowStyle style;
    style.layout = ListBoxRows::Layout{19, 5, 0, 0, std::array<int, 3>{286, 15, 241}};
    style.scriptGroup = kScripts;
    style.scriptInstance = kSongRowScript;
    return style;
}

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
    for (std::size_t i = 0; i < kVolumeKeys.size(); ++i) {
        volume_[i] = static_cast<float>(std::clamp(number(kVolumeKeys[i], 100), 0, 100)) / 100.0f;
        muted_[i] = !flag(kMuteKeys[i], true);
    }
    // Without a saved quality it stays at what globalProps.xml's AudioPerformance (0 to 2) says.
    const int quality = std::clamp(number(kQualityKey, number("AudioPerformance", 1) + 1), 1, 3);
    volume_[kQualitySlider] = static_cast<float>(quality - 1) / 2.0f;
    speakers_ = 0;
    if (const auto it = saved_.find("SoundOutputMode"); it != saved_.end()) {
        for (std::size_t i = 0; i < kSpeakerModes.size(); ++i) {
            if (it->second == kSpeakerModes[i]) {
                speakers_ = static_cast<int>(i);
            }
        }
    }
}

void AudioOptionsPanel::takeSettings(const AudioOptionsPanel& other) {
    userFolder_ = other.userFolder_;
    saved_ = other.saved_;
    defaults_ = other.defaults_;
    volume_ = other.volume_;
    muted_ = other.muted_;
    speakers_ = other.speakers_;
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
        if (i < muteButtons_.size()) {
            muteButtons_[i] = screen_.find(panel_, kMutes[i]);
        }
        if (sliders_[i] < 0) {
            continue;
        }
        const bool quality = i == kQualitySlider;
        thumbs_[i] = addSlider(sliders_[i], quality ? kQualityRows : kVolumeRows, quality ? kQualityShown : kVolumeShown, thumbLength_[i]);
    }
    for (std::size_t i = 0; i < speakerButtons_.size(); ++i) {
        speakerButtons_[i] = screen_.find(panel_, kSpeakers[i]);
        if (i > 0 && speakerButtons_[i] >= 0) {
            screen_.setTip(speakerButtons_[i], std::string(kStillStereo));
        }
    }
    for (std::size_t i = 0; i < tabButtons_.size(); ++i) {
        tabButtons_[i] = screen_.find(panel_, kTabs[i]);
    }
    stereoPanel_ = screen_.find(panel_, kStereoPanel);
    modePanel_ = screen_.find(panel_, kModePanel);
    stereoOverlay_ = screen_.find(panel_, kStereoOverlay);
    modeOverlay_ = screen_.find(panel_, kModeOverlay);
    songBox_ = screen_.find(panel_, kSongBox);
    songBar_ = screen_.find(panel_, kSongBar);
    stationBox_ = screen_.find(panel_, kStationBox);
    stationTemplate_ = screen_.find(panel_, kStationTemplate);
    stationArrows_ = {screen_.find(panel_, kStationArrows[0]), screen_.find(panel_, kStationArrows[1])};
    customStations_ = {screen_.find(panel_, kCustomStations[0]), screen_.find(panel_, kCustomStations[1])};
    // The script's bar and button are only samples: the lists make their own, as the game's code does.
    for (const int sample : {songBar_, stationTemplate_}) {
        if (sample >= 0) {
            screen_.setVisible(sample, false);
        }
    }
    const int list = screen_.find(panel_, kSongList);
    if (list >= 0) {
        songs_.attach(screen_, list, songRows());
        songs_.setMulti(true);
    }
    tab_ = 0;
    showValues();
}

void AudioOptionsPanel::remove() {
    if (panel_ >= 0) {
        menu_.close();
        screen_.remove(panel_);
        panel_ = -1;
    }
    sliders_ = {-1, -1, -1, -1, -1};
    thumbs_ = {};
    tabButtons_ = {-1, -1, -1, -1, -1};
    stereoPanel_ = modePanel_ = stereoOverlay_ = modeOverlay_ = songBox_ = songBar_ = stationBox_ = stationTemplate_ = -1;
    stationArrows_ = {-1, -1};
    customStations_ = {-1, -1};
    stationButtons_.clear();
    stations_.clear();
    songList_.clear();
    hover_ = -1;
    dragging_ = -1;
    muteButtons_ = {-1, -1, -1, -1};
    speakerButtons_ = {-1, -1, -1};
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
        if (i < muteButtons_.size() && muteButtons_[i] >= 0) {
            screen_.setChecked(muteButtons_[i], muted_[i]);
        }
    }
    for (std::size_t i = 0; i < speakerButtons_.size(); ++i) {
        if (speakerButtons_[i] >= 0) {
            screen_.setChecked(speakerButtons_[i], static_cast<int>(i) == speakers_);
        }
    }
}

void AudioOptionsPanel::show(bool show) {
    if (panel_ < 0 || show == shown()) {
        return;
    }
    if (show) {
        showValues();
        showTab();
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
    for (std::size_t i = 0; i < tabButtons_.size(); ++i) {
        if (window == tabButtons_[i]) {
            tab_ = static_cast<int>(i);
            showTab();
            return true;
        }
    }
    for (std::size_t i = 0; i < stationButtons_.size(); ++i) {
        if (window == stationButtons_[i]) {
            pickStation(static_cast<int>(i));
            return true;
        }
    }
    for (std::size_t i = 0; i < stationArrows_.size(); ++i) {
        if (window == stationArrows_[i]) {
            const int last = std::max(0, static_cast<int>(stations_.size()) - kStationRows);
            stationTop_ = std::clamp(stationTop_ + (i == 0 ? -1 : 1), 0, last);
            placeStations();
            return true;
        }
    }
    if (songs_.clicked(window)) {
        const int row = songs_.toggled();
        if (music_ != nullptr && row >= 0 && row < static_cast<int>(songList_.size()) && station_ < static_cast<int>(stations_.size())) {
            songList_[row].on = songs_.checks()[row];
            music_->setSongOn(stations_[station_], songList_[row].id, songList_[row].on);
        }
        return true;
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
    for (std::size_t i = 0; i < speakerButtons_.size(); ++i) {
        if (window == speakerButtons_[i]) {
            speakers_ = static_cast<int>(i);
            showValues();
            changed_ = true;
            if (!ts2::content::saveUserString(userFolder_, "SoundOutputMode", kSpeakerModes[i])) {
                ts2::diag::warnOnce("audio options: the speaker choice could not be saved to userProps.xml");
            }
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
    if (songs_.mouseDown(x, y, now_)) {
        return true;
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
    songs_.mouseMove(x, y);
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

void AudioOptionsPanel::tick(std::uint64_t ms) {
    now_ = ms;
    if (panel_ >= 0 && shown()) {
        songs_.tick(ms);
    }
}

bool AudioOptionsPanel::wheel(int x, int y, int delta) {
    return panel_ >= 0 && shown() && songs_.wheel(x, y, delta);
}

void AudioOptionsPanel::mouseUp(int x, int y) {
    songs_.mouseUp(x, y);
    const int released = dragging_;
    dragging_ = -1;
    if (released >= 0) {
        saveVolume(released);
        hover_ = onThumb(released, x, y) ? released : -1;
        showFrame(released);
    }
}

// The stereos' tab lists the stations a stereo plays and the songs of the one picked. The other tabs have no
// stations to pick: each lists the songs of its mode's own station.
void AudioOptionsPanel::showTab() {
    const bool stereo = tab_ == 0;
    for (std::size_t i = 0; i < tabButtons_.size(); ++i) {
        if (tabButtons_[i] >= 0) {
            screen_.setChecked(tabButtons_[i], static_cast<int>(i) == tab_);
        }
    }
    for (const int window : {stereoPanel_, stereoOverlay_, stationBox_, stationArrows_[0], stationArrows_[1], customStations_[0]}) {
        if (window >= 0) {
            screen_.setVisible(window, stereo);
        }
    }
    for (const int window : {modePanel_, modeOverlay_, customStations_[1]}) {
        if (window >= 0) {
            screen_.setVisible(window, !stereo);
        }
    }
    if (songBox_ >= 0) {
        screen_.move(songBox_, stereo ? kStereoListX : kModeListX, screen_.position(songBox_)[1] - screen_.position(panel_)[1]);
    }
    const int before = station_ < static_cast<int>(stations_.size()) ? station_ : 0;
    const std::string kept = stations_.empty() ? std::string() : stations_[before];
    stations_ = stereo ? (music_ != nullptr ? music_->musicStations("") : std::vector<std::string>{})
                       : std::vector<std::string>{kTabStations[tab_]};
    station_ = 0;
    for (std::size_t i = 0; stereo && i < stations_.size(); ++i) {
        if (stations_[i] == kept) {
            station_ = static_cast<int>(i);
        }
    }
    stationTop_ = std::clamp(station_ - kStationRows + 1, 0, std::max(0, static_cast<int>(stations_.size()) - kStationRows));
    showStations();
    showSongs();
}

void AudioOptionsPanel::showStations() {
    for (const int button : stationButtons_) {
        screen_.remove(button);
    }
    stationButtons_.clear();
    if (tab_ != 0 || stationBox_ < 0) {
        return;
    }
    for (std::size_t i = 0; i < stations_.size(); ++i) {
        ts2::ui::Window button{{{"iid", "IGZWinBtn"}, {"winflag_visible", "yes"}, {"id", std::format("0x{:08x}", kFirstStationId + i)},
                                {"area", std::format("(0,0,{},{})", kStationWidth, kStationHeight)}, {"image", "{499db772,e9f50546}"},
                                {"style", "toggle"}, {"showcaption", "yes"}, {"font", "OptionsText"}, {"colorfontnormal", "(5,6,113)"},
                                {"colorfonthilited", "(204,214,255)"}, {"gutters", "(8,3,8,3)"}, {"align", "left"}},
                               {}};
        const int made = screen_.addWindows({button}, 0, 0, stationBox_);
        std::string name = music_ != nullptr ? music_->musicTitle(stations_[i]) : std::string();
        if (name.empty()) {
            name = stations_[i];
            name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        }
        screen_.setText(made, name);
        stationButtons_.push_back(made);
    }
    placeStations();
}

void AudioOptionsPanel::placeStations() {
    for (std::size_t i = 0; i < stationButtons_.size(); ++i) {
        const int row = static_cast<int>(i) - stationTop_;
        const bool shown = row >= 0 && row < kStationRows;
        screen_.setVisible(stationButtons_[i], shown);
        if (shown) {
            screen_.move(stationButtons_[i], 0, kStationFirstY + row * kStationPitch);
        }
        screen_.setChecked(stationButtons_[i], static_cast<int>(i) == station_);
    }
}

void AudioOptionsPanel::pickStation(int index) {
    station_ = index;
    placeStations();
    showSongs();
}

void AudioOptionsPanel::showSongs() {
    songList_.clear();
    if (music_ != nullptr && station_ < static_cast<int>(stations_.size())) {
        songList_ = music_->musicSongs(stations_[station_]);
    }
    // The game lists a station's songs by title, whatever order the station plays them in.
    const auto lowered = [](const std::string& text) {
        std::string out = text;
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    };
    std::stable_sort(songList_.begin(), songList_.end(),
                     [&](const ts2::audio::MusicSong& a, const ts2::audio::MusicSong& b) { return lowered(a.title) < lowered(b.title); });
    std::vector<std::string> titles;
    std::vector<bool> checks;
    for (const ts2::audio::MusicSong& song : songList_) {
        titles.push_back(song.title.empty() ? std::format("0x{:08x}", song.id) : song.title);
        checks.push_back(song.on);
    }
    songs_.setRows(titles);
    songs_.setChecks(checks);
}

void AudioOptionsPanel::setFromX(int group, int x) {
    if (group < 0 || group >= static_cast<int>(volume_.size()) || sliders_[group] < 0) {
        return;
    }
    const int left = screen_.position(sliders_[group])[0];
    const int room = std::max(1, screen_.width(sliders_[group]) - thumbLength_[group]);
    const float value = std::clamp(static_cast<float>(x - grab_ - left) / static_cast<float>(room), 0.0f, 1.0f);
    // Volumes go in whole percents, Audio Quality in its three places.
    const float steps = group == static_cast<int>(kQualitySlider) ? 2.0f : 100.0f;
    const float rounded = std::round(value * steps) / steps;
    if (rounded == volume_[group]) {
        return;
    }
    volume_[group] = rounded;
    showValues();
    changed_ = true;
}

void AudioOptionsPanel::saveVolume(int group) {
    if (group == static_cast<int>(kQualitySlider)) {
        if (!ts2::content::saveUserProps(userFolder_, {{kQualityKey, {std::to_string(quality()), false}}})) {
            ts2::diag::warnOnce("audio options: the sound quality could not be saved to userProps.xml");
        }
        return;
    }
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
