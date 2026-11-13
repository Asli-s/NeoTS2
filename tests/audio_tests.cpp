#include "test_support.hpp"

#include "audio/xa.hpp"
#include "formats/anim.hpp"
#include "formats/hash.hpp"

#include <cstdint>
#include <vector>

namespace {

std::vector<std::uint8_t> xaResource(std::uint32_t pcmBytes, std::uint16_t channels, std::uint32_t sampleRate) {
    std::vector<std::uint8_t> data(24, 0);
    data[0] = 'X';
    data[1] = 'A';
    data[4] = static_cast<std::uint8_t>(pcmBytes);
    data[5] = static_cast<std::uint8_t>(pcmBytes >> 8);
    data[6] = static_cast<std::uint8_t>(pcmBytes >> 16);
    data[7] = static_cast<std::uint8_t>(pcmBytes >> 24);
    data[8] = 1;  // PCM
    data[10] = static_cast<std::uint8_t>(channels);
    data[11] = static_cast<std::uint8_t>(channels >> 8);
    data[12] = static_cast<std::uint8_t>(sampleRate);
    data[13] = static_cast<std::uint8_t>(sampleRate >> 8);
    data[14] = static_cast<std::uint8_t>(sampleRate >> 16);
    data[15] = static_cast<std::uint8_t>(sampleRate >> 24);
    data[22] = 16;  // bits
    data[23] = 0;
    return data;
}

void soundNamesHashAsTheGameFilesThem() {
    // Values checked against the base game's Sfx1.package: a track of this name is filed here.
    const auto key = ts2::hash::scenegraphKey("door_open");
    CHECK(key.instance == 0xFFF17AC7u);
    CHECK(key.instanceHigh == 0x85519390u);
}

void xaDecodesItsNibbles() {
    auto data = xaResource(56, 1, 32000);  // one block: 28 samples
    data.resize(data.size() + 15, 0);
    data[24] = 0x00;    // predictor 0, shift 8
    data[25] = 0x10;    // first nibble 1, second 0
    const ts2::audio::Pcm pcm = ts2::audio::decodeXa(data);
    CHECK(pcm.sampleRate == 32000);
    CHECK(pcm.channels == 1);
    CHECK(pcm.samples.size() == 28);
    CHECK(pcm.samples[0] == 4096);
    CHECK(pcm.samples[1] == 0);
    CHECK(pcm.samples[2] == 0);
}

void xaTrimsToTheHeaderSize() {
    auto data = xaResource(28, 1, 32000);  // half a block: 14 samples
    data.resize(data.size() + 15, 0);
    data[24] = 0x00;
    data[25] = 0x11;
    const ts2::audio::Pcm pcm = ts2::audio::decodeXa(data);
    CHECK(pcm.samples.size() == 14);
    CHECK(pcm.samples[0] == 4096);
    CHECK(pcm.samples[1] == 4096);
}

void xaRejectsWhatIsNotXa() {
    bool threw = false;
    try {
        const std::vector<std::uint8_t> data(24, 0);
        (void)ts2::audio::decodeXa(data);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void animationSoundMarksNameTheirSound() {
    const ts2::anim::Event sound{100, ts2::anim::kSoundEvent, "sound_name=toilet_flush; x=1"};
    CHECK(ts2::anim::soundEventName(sound) == std::optional<std::string>("toilet_flush"));
    const ts2::anim::Event event{100, ts2::anim::kSimAnticsEvent, "event=42"};
    CHECK(!ts2::anim::soundEventName(event));
}

} // namespace

std::vector<Test> audioTests() {
    return {
        {"sound names hash as the game files them", soundNamesHashAsTheGameFilesThem},
        {"XA decodes its nibbles", xaDecodesItsNibbles},
        {"XA trims to the header size", xaTrimsToTheHeaderSize},
        {"XA rejects what is not XA", xaRejectsWhatIsNotXa},
        {"animation sound marks name their sound", animationSoundMarksNameTheirSound},
    };
}
