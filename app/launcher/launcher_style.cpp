#include "launcher/launcher_style.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstddef>

extern const unsigned char notoSansRegular[];
extern const std::size_t notoSansRegularSize;
extern const unsigned char notoSansSemiBold[];
extern const std::size_t notoSansSemiBoldSize;

namespace ts2::launcher {

namespace {

// Text sizes in pixels at 100% display scale, large enough for every player to read easily.
constexpr float kTextSize = 20.0f;
constexpr float kHeadingSize = 24.0f;

// The letters folder names and paths may hold: Latin with its accents, Greek, Cyrillic, general
// punctuation and the ™ in "The Sims™ 2". Noto Sans has no Chinese, Japanese, Korean, Arabic,
// Hebrew or Thai letters, so paths written in those show as question marks until a font for them
// is added.
constexpr ImWchar kGlyphs[] = {
    0x0020, 0x024F,
    0x0370, 0x03FF,
    0x0400, 0x052F,
    0x2000, 0x206F,
    0x2122, 0x2122,
    0,
};

ImFont* addFont(const unsigned char* data, std::size_t size, float pixels) {
    ImFontConfig config;
    // The font lives in the exe for the whole run, so ImGui must not free it.
    config.FontDataOwnedByAtlas = false;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), static_cast<int>(size), pixels,
                                                      &config, kGlyphs);
}

} // namespace

LauncherFonts applyStyle(float displayScale) {
    const float scale = std::clamp(displayScale, 1.0f, 4.0f);
    LauncherFonts fonts;
    fonts.text = addFont(notoSansRegular, notoSansRegularSize, kTextSize * scale);
    fonts.heading = addFont(notoSansSemiBold, notoSansSemiBoldSize, kHeadingSize * scale);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(24, 20);
    style.FramePadding = ImVec2(12, 7);
    style.ItemSpacing = ImVec2(12, 10);
    style.ItemInnerSpacing = ImVec2(10, 6);
    style.IndentSpacing = 32;
    style.FrameRounding = 6;
    style.GrabRounding = 6;
    style.SeparatorTextBorderSize = 2;
    style.SeparatorTextPadding = ImVec2(0, 4);
    style.ScaleAllSizes(scale);
    return fonts;
}

} // namespace ts2::launcher
