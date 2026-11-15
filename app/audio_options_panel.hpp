#pragma once

#include "options_menu.hpp"
#include "ui/screen.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <map>
#include <string>

namespace app {

// The Audio Options panel (script 0x49060F01): a volume slider and mute box per mixer group, saved to
// Config/userProps.xml under the game's own keys as they change. The Audio Quality slider does nothing yet.
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

    // A thumb `shown` of `rows` long on a track `width` wide, never shorter than its two caps.
    static int thumbLength(int width, int rows, int shown, int cap) { return std::min(width, std::max(shown * width / rows, 2 * cap)); }

    // True when the click was on the Audio button or the panel.
    bool clicked(int window);
    // A press sets a level and a drag keeps setting it.
    bool mouseDown(int x, int y);
    void mouseMove(int x, int y);
    void mouseUp(int x, int y);
    bool holdsMouse() const { return dragging_ >= 0; }
    bool takeChanged();

    std::array<float, 4> volumes() const { return volume_; }
    std::array<bool, 4> mutes() const { return muted_; }

private:
    void showValues();
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
    std::array<int, 4> sliders_{-1, -1, -1, -1};
    std::array<std::array<int, 3>, 4> thumbs_{};  // each thumb's left cap, middle and right cap
    std::array<int, 4> thumbLength_{};
    std::array<int, 4> muteButtons_{-1, -1, -1, -1};
    std::array<float, 4> volume_{1, 1, 0.4f, 1};
    std::array<bool, 4> muted_{};
    std::filesystem::path userFolder_;
    std::map<std::string, std::string> saved_;     // userProps.xml's settings
    std::map<std::string, std::string> defaults_;  // globalProps.xml's settings
    int dragging_ = -1, hover_ = -1;
    int grab_ = 0;  // how far into the thumb a drag took it
    bool changed_ = false;
};

} // namespace app
