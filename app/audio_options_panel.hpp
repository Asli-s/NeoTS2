#pragma once

#include "audio/system.hpp"
#include "options_list.hpp"
#include "options_menu.hpp"
#include "ui/screen.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace app {

// The Audio Options panel (script 0x49060F01): a volume slider and mute box per mixer group, the Audio Quality
// slider and the Speakers choice, saved to Config/userProps.xml under the game's own keys as they change. Its
// tabs list the stations and their songs, where a song can be turned off (see ts2::audio::System::setSongOn).
class AudioOptionsPanel {
public:
    AudioOptionsPanel(ts2::ui::Screen& screen, OptionsMenu& menu) : screen_(screen), menu_(menu) {}

    // Reads saved levels and mutes, falling back to the install's globalProps.xml.
    void readSettings(const std::filesystem::path& install, const std::filesystem::path& userFolder);
    // Copies the other panel's settings, so both stay the same after a change.
    void takeSettings(const AudioOptionsPanel& other);
    void load();
    void remove();
    void show(bool show);
    bool shown() const { return menu_.isOpen(panel_); }
    // The sound system whose music the tabs list. Without one the lists are empty.
    void setMusic(ts2::audio::System* music) { music_ = music; }
    // Called each frame with the time in milliseconds: repeats a held press on the song list's bar.
    void tick(std::uint64_t ms);
    bool wheel(int x, int y, int delta);

    // A thumb `shown` of `rows` long on a track `width` wide, never shorter than its two caps.
    static int thumbLength(int width, int rows, int shown, int cap) { return std::min(width, std::max(shown * width / rows, 2 * cap)); }

    // True when the click was on the Audio button or the panel.
    bool clicked(int window);
    // A press sets a level and a drag keeps setting it.
    bool mouseDown(int x, int y);
    void mouseMove(int x, int y);
    void mouseUp(int x, int y);
    bool holdsMouse() const { return dragging_ >= 0 || songs_.holdsMouse(); }
    bool takeChanged();

    std::array<float, 4> volumes() const { return {volume_[0], volume_[1], volume_[2], volume_[3]}; }
    // The Audio Quality: 1 Low, 2 Medium, 3 High (the game's SoundQuality setting). The mixer's performance
    // level is one less.
    int quality() const { return 1 + static_cast<int>(std::lround(volume_[4] * 2)); }
    std::array<bool, 4> mutes() const { return muted_; }

private:
    void showValues();
    // The tab's windows, its stations and the songs of the station picked.
    void showTab();
    void showStations();
    void placeStations();
    void showSongs();
    void pickStation(int index);
    void setFromX(int group, int x);
    void saveVolume(int group);
    void saveMute(int group);
    int addPicture(int parent, std::uint32_t group, std::uint32_t instance, int w, int h);
    int thumbX(int group) const;
    bool onThumb(int group, int x, int y) const;
    void showFrame(int group);
    std::array<int, 3> addSlider(int slider, int rows, int shown, int& length);
    int number(const char* key, int otherwise) const;
    bool flag(const char* key, bool otherwise) const;

    ts2::ui::Screen& screen_;
    OptionsMenu& menu_;
    int panel_ = -1;
    // The four volume sliders, then the Audio Quality one: its "volume" is where the thumb sits, 0, 0.5 or 1.
    std::array<int, 5> sliders_{-1, -1, -1, -1, -1};
    std::array<std::array<int, 3>, 5> thumbs_{};  // each thumb's left cap, middle and right cap
    std::array<int, 5> thumbLength_{};
    std::array<int, 4> muteButtons_{-1, -1, -1, -1};
    std::array<float, 5> volume_{1, 1, 0.4f, 1, 0.5f};
    std::array<int, 3> speakerButtons_{-1, -1, -1};
    int speakers_ = 0;  // 0 stereo, 1 4.0, 2 5.1
    std::array<bool, 4> muted_{};
    std::filesystem::path userFolder_;
    std::map<std::string, std::string> saved_;     // userProps.xml's settings
    std::map<std::string, std::string> defaults_;  // globalProps.xml's settings
    int dragging_ = -1, hover_ = -1;
    int grab_ = 0;  // how far into the thumb a drag took it
    bool changed_ = false;

    ts2::audio::System* music_ = nullptr;
    OptionsList songs_;
    int tab_ = 0;  // 0 the stereos' stations, then Buy, Build, Neighborhood and Create a Sim
    std::array<int, 5> tabButtons_{-1, -1, -1, -1, -1};
    int stereoPanel_ = -1, modePanel_ = -1, stereoOverlay_ = -1, modeOverlay_ = -1;
    int songBox_ = -1, songBar_ = -1, stationBox_ = -1, stationTemplate_ = -1;
    std::array<int, 2> stationArrows_{-1, -1};
    std::vector<int> stationButtons_;
    std::vector<std::string> stations_;
    int stationTop_ = 0, station_ = 0;
    std::vector<ts2::audio::MusicSong> songList_;
    std::uint64_t now_ = 0;
};

} // namespace app
