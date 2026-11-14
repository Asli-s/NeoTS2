// Video Capture: the AVI files it writes and the settings it records with.
#include "test_support.hpp"

#include "content/video_capture.hpp"
#include "formats/avi_writer.hpp"
#include "formats/jpeg.hpp"

#include <sstream>

namespace {

std::uint32_t le32(const std::string& s, std::size_t at) {
    return static_cast<std::uint8_t>(s[at]) | static_cast<std::uint8_t>(s[at + 1]) << 8 | static_cast<std::uint8_t>(s[at + 2]) << 16 |
           static_cast<std::uint32_t>(static_cast<std::uint8_t>(s[at + 3])) << 24;
}

void aviHoldsTheFrames() {
    std::ostringstream out;
    ts2::avi::MjpegWriter writer(out, 16, 8, 15);
    ts2::tga::Pixels picture{16, 8, std::vector<std::uint8_t>(16 * 8 * 4, 200)};
    const auto frame = ts2::jpeg::encode(picture, 75);
    const std::vector<std::uint8_t> odd(frame.begin(), frame.end() - (frame.size() % 2 == 0 ? 1 : 0));  // an odd size is padded
    writer.addFrame(frame);
    writer.addFrame(odd);
    writer.addFrame(frame);
    writer.finish();
    const std::string s = out.str();
    CHECK(s.substr(0, 4) == "RIFF" && s.substr(8, 4) == "AVI " && le32(s, 4) == s.size() - 8);
    // The header's frame counts, size and rate.
    const auto avih = s.find("avih");
    CHECK(avih != std::string::npos && le32(s, avih + 8) == 1000000 / 15 && le32(s, avih + 8 + 16) == 3 && le32(s, avih + 8 + 32) == 16 &&
          le32(s, avih + 8 + 36) == 8);
    const auto strh = s.find("strh");
    CHECK(strh != std::string::npos && s.substr(strh + 8, 8) == "vidsMJPG" && le32(s, strh + 8 + 24) == 15 && le32(s, strh + 8 + 32) == 3);
    // The frames' list and the index pointing at each frame.
    const auto movi = s.find("movi");
    const auto idx1 = s.find("idx1");
    CHECK(movi != std::string::npos && idx1 != std::string::npos && le32(s, movi - 4) == idx1 - movi);
    CHECK(le32(s, idx1 + 4) == 3 * 16);
    for (std::size_t i = 0; i < 3; ++i) {
        const std::size_t entry = idx1 + 8 + i * 16;
        const std::size_t chunk = movi + le32(s, entry + 8);
        CHECK(s.substr(entry, 4) == "00dc" && le32(s, entry + 4) == 0x10);
        CHECK(s.substr(chunk, 4) == "00dc" && le32(s, chunk + 4) == le32(s, entry + 12) && chunk % 2 == 0);
        CHECK(static_cast<std::uint8_t>(s[chunk + 8]) == 0xFF && static_cast<std::uint8_t>(s[chunk + 9]) == 0xD8);
    }
    CHECK(le32(s, idx1 + 8 + 16 + 12) == odd.size());
    // Nothing can follow the end.
    bool refused = false;
    try {
        writer.addFrame(frame);
    } catch (const std::exception&) {
        refused = true;
    }
    CHECK(refused);
}

void videoSettingsFollowTheOptions() {
    ts2::content::UserProps saved, defaults;
    defaults.values = {{"videoCaptureSize", "1"}, {"videoCaptureQuality", "1"}, {"videoCaptureMaxRecordingSeconds", "60"}};
    auto v = ts2::content::videoCaptureSettings(saved, defaults);
    CHECK(v.width == 320 && v.height == 240 && v.fps == 15 && v.maxSeconds == 60);
    saved.values = {{"videoCaptureSize", "2"}, {"videoCaptureQuality", "2"}, {"videoCaptureMaxRecordingSeconds", "15"}};
    v = ts2::content::videoCaptureSettings(saved, defaults);
    CHECK(v.width == 640 && v.height == 480 && v.fps == 30 && v.maxSeconds == 15);
    saved.values = {{"videoCaptureSize", "0"}, {"videoCaptureQuality", "0"}, {"videoCaptureMaxRecordingSeconds", "999999"}};
    v = ts2::content::videoCaptureSettings(saved, defaults);
    CHECK(v.width == 128 && v.height == 128 && v.fps == 10 && v.maxSeconds == 3600);
    saved.values = {{"videoCaptureSize", "x"}};
    v = ts2::content::videoCaptureSettings(saved, {});
    CHECK(v.width == 320 && v.fps == 15 && v.maxSeconds == 60);
    CHECK(ts2::content::nextMovieFile("no such folder").filename() == "movie0001.avi");
}

} // namespace

std::vector<Test> videoTests() {
    return {
        {"a video capture's AVI file holds its frames", aviHoldsTheFrames},
        {"video capture follows the game options", videoSettingsFollowTheOptions},
    };
}
