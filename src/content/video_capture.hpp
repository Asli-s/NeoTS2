#pragma once

#include "content/user_props.hpp"

#include <cstdint>
#include <filesystem>

namespace ts2::content {

// How Video Capture (V) records, from the game options the game saves: Video Capture Size
// (videoCaptureSize: 0 small, 1 medium, 2 large), Video Capture Quality (videoCaptureQuality: 0 low,
// 1 medium, 2 high, 3 high uncompressed) and Maximum Video Recording Time
// (videoCaptureMaxRecordingSeconds). The sizes and frame rates are the game's (Config/VideoCapture.ini:
// small 128 x 128, medium 320 x 240, large 640 x 480; 10, 15 and 30 frames a second). The game
// encodes with On2's VP6; NeoTS2 writes Motion JPEG, the quality picking how finely.
struct VideoCaptureSettings {
    std::uint32_t width = 320, height = 240, fps = 15;
    int jpegQuality = 75;
    std::uint32_t maxSeconds = 60;
    // Video Sound Capture (videoCaptureAudioOn): the game's sound recorded too, mono at a quarter of
    // the sound's own rate (VideoCapture.ini's AudioStereoOutput=0 and AudioRateDivisor=4).
    bool sound = true;
    int soundRateDivisor = 4;
};

// The settings from the user's saved options, else the install's defaults (globalProps.xml).
VideoCaptureSettings videoCaptureSettings(const UserProps& saved, const UserProps& defaults);

// The next free name for a recording in `folder`, as the game names them: movie0001.avi and on.
std::filesystem::path nextMovieFile(const std::filesystem::path& folder);

} // namespace ts2::content
