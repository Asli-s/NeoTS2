// Sound: the clip decoders, hit lists, track settings, the mixer and 3D volume.
#include "test_support.hpp"

#include "audio/decode.hpp"
#include "audio/mixer.hpp"
#include "audio/sound_bank.hpp"
#include "formats/anim.hpp"

#include <speex/speex.h>

#include <cmath>
#include <set>

namespace {

using ts2::audio::Category;
using ts2::audio::ClipKey;
using ts2::audio::Pcm;

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::vector<std::uint8_t> xaHeader(std::uint16_t channels, std::uint32_t rate, std::uint32_t decodedBytes) {
    std::vector<std::uint8_t> b{'X', 'A', 0, 0};
    put32(b, decodedBytes);
    put16(b, 1);
    put16(b, channels);
    put32(b, rate);
    put32(b, rate * 2 * channels);
    put16(b, static_cast<std::uint16_t>(2 * channels));
    put16(b, 16);
    return b;
}

// Maxis XA: a block's head byte picks the filter (high nibble) and shift (low nibble + 8); the sum
// of the scaled nibble and the prediction is scaled down by 256. Filter 0 predicts nothing, so a
// nibble n with shift 8 is n * 4096. Filter 1 adds 240/256 of the last sample.
void xaBlocksDecodeAsTheGame() {
    auto b = xaHeader(1, 22050, 2 * 56);
    b.push_back(0x00);  // filter 0, shift 8
    b.push_back(0x1F);  // 1, then -1
    for (int i = 0; i < 13; ++i) {
        b.push_back(0x11);  // 1, 1 ...
    }
    b.push_back(0x10);  // filter 1, shift 8
    for (int i = 0; i < 14; ++i) {
        b.push_back(0x00);  // nothing new: each sample is 240/256 of the one before
    }
    const auto pcm = ts2::audio::decodeXa(b);
    CHECK(pcm && pcm->rate == 22050 && pcm->channels == 1 && pcm->frames() == 56);
    CHECK(pcm->samples[0] == 4096 && pcm->samples[1] == -4096 && pcm->samples[2] == 4096 && pcm->samples[27] == 4096);
    CHECK(pcm->samples[28] == (4096 * 240 + 128) / 256);
    CHECK(pcm->samples[29] == (pcm->samples[28] * 240 + 128) / 256);
    // Stereo: both head bytes first, then the channels' bytes taken in turn, left first.
    auto s = xaHeader(2, 32000, 4 * 28);
    s.push_back(0x00);  // left: filter 0, shift 8
    s.push_back(0x01);  // right: filter 0, shift 9
    for (int i = 0; i < 14; ++i) {
        s.push_back(0x1F);  // left: 1, -1 -> 4096, -4096
        s.push_back(0x11);  // right: 1, 1 -> 2048s
    }
    const auto stereo = ts2::audio::decodeXa(s);
    CHECK(stereo && stereo->channels == 2 && stereo->frames() == 28);
    CHECK(stereo->samples[0] == 4096 && stereo->samples[1] == 2048 && stereo->samples[2] == -4096 && stereo->samples[3] == 2048);
    CHECK(stereo->samples[2 * 26] == 4096 && stereo->samples[2 * 27] == -4096 && stereo->samples[2 * 27 + 1] == 2048);
    // Broken clips decode to nothing, or only as far as their blocks go.
    CHECK(!ts2::audio::decodeXa(std::vector<std::uint8_t>{'X', 'A', 0}));
    auto noChannels = xaHeader(0, 22050, 0);
    CHECK(!ts2::audio::decodeXa(noChannels));
    auto cut = xaHeader(1, 22050, 1000000);
    cut.push_back(0);
    CHECK(ts2::audio::decodeXa(cut) && ts2::audio::decodeXa(cut)->frames() == 0);
}

// The game's Speex clips: "SPX1", version 1, a count, the mode (2 ultra-wideband) and the frame
// size, then packets of a length byte and one encoded frame. A tone encoded so decodes back.
void spxClipsDecode() {
    void* encoder = speex_encoder_init(&speex_uwb_mode);
    int frameSize = 0;
    speex_encoder_ctl(encoder, SPEEX_GET_FRAME_SIZE, &frameSize);
    int quality = 8;
    speex_encoder_ctl(encoder, SPEEX_SET_QUALITY, &quality);
    SpeexBits bits;
    speex_bits_init(&bits);
    std::vector<std::uint8_t> clip{'S', 'P', 'X', '1', 1};
    put32(clip, 0);
    put32(clip, 2);
    put16(clip, static_cast<std::uint16_t>(frameSize));
    constexpr int kFrames = 20;
    std::vector<spx_int16_t> frame(static_cast<std::size_t>(frameSize));
    for (int f = 0; f < kFrames; ++f) {
        for (int i = 0; i < frameSize; ++i) {
            frame[static_cast<std::size_t>(i)] = static_cast<spx_int16_t>(8000 * std::sin(2 * 3.14159265 * 440 * (f * frameSize + i) / 32000.0));
        }
        speex_bits_reset(&bits);
        speex_encode_int(encoder, frame.data(), &bits);
        char packet[256];
        const int n = speex_bits_write(&bits, packet, sizeof packet);
        clip.push_back(static_cast<std::uint8_t>(n));
        clip.insert(clip.end(), packet, packet + n);
    }
    speex_bits_destroy(&bits);
    speex_encoder_destroy(encoder);
    const auto pcm = ts2::audio::decodeClip(clip);
    CHECK(pcm && pcm->rate == 32000 && pcm->channels == 1 && pcm->frames() == static_cast<std::size_t>(kFrames * frameSize));
    // Past the codec's start-up the tone is back: loud, and far from noise.
    double sum = 0;
    for (std::size_t i = pcm->frames() / 2; i < pcm->frames(); ++i) {
        sum += static_cast<double>(pcm->samples[i]) * pcm->samples[i];
    }
    const double rms = std::sqrt(sum / static_cast<double>(pcm->frames() / 2));
    CHECK(rms > 3000 && rms < 8000);
    auto wrongVersion = clip;
    wrongVersion[4] = 2;
    CHECK(!ts2::audio::decodeSpx(wrongVersion));
    auto wrongMode = clip;
    wrongMode[9] = 7;
    CHECK(!ts2::audio::decodeSpx(wrongMode));
    CHECK(!ts2::audio::decodeClip(std::vector<std::uint8_t>{'?', '?', '?', '?'}));
    CHECK(!ts2::audio::decodeMp3(std::vector<std::uint8_t>(64, 0xFF)));
}

// A hit list: version 0x38 lists instance and high instance, version 1 instances only.
void hitListsAreRead() {
    std::vector<std::uint8_t> v38;
    put32(v38, 0x38);
    put32(v38, 2);
    put32(v38, 0xFF000001);
    put32(v38, 0x11);
    put32(v38, 0xFF000002);
    put32(v38, 0x22);
    CHECK((ts2::audio::SoundBank::readHitList(v38) == std::vector<ClipKey>{{0xFF000001, 0x11}, {0xFF000002, 0x22}}));
    v38[4] = 9;  // says more than it holds
    CHECK(ts2::audio::SoundBank::readHitList(v38).size() == 2);
    std::vector<std::uint8_t> v1;
    put32(v1, 1);
    put32(v1, 2);
    put32(v1, 0xFFA511AD);
    put32(v1, 0xFFA9885B);
    CHECK((ts2::audio::SoundBank::readHitList(v1) == std::vector<ClipKey>{{0xFFA511AD, 0}, {0xFFA9885B, 0}}));
    CHECK(ts2::audio::SoundBank::readHitList(std::vector<std::uint8_t>{1, 2, 3}).empty());
}

// The game's choice from a hit list never plays a clip again until a third of the list (plus one)
// has played since, and a list of one plays its clip each time.
void hitListsDontRepeatSoon() {
    std::vector<ClipKey> clips;
    for (std::uint32_t i = 1; i <= 6; ++i) {
        clips.push_back({i, 0});
    }
    ts2::audio::SoundBank::HitList list(clips);
    std::mt19937 random(1);
    std::vector<std::uint32_t> played;
    std::set<std::uint32_t> seen;
    for (int i = 0; i < 300; ++i) {
        played.push_back(list.choose(random).instance);
        seen.insert(played.back());
    }
    CHECK(seen.size() == 6);
    for (std::size_t i = 0; i + 2 < played.size(); ++i) {  // window 6/3+1 = 3: none within 3 picks
        CHECK(played[i] != played[i + 1] && played[i] != played[i + 2]);
    }
    ts2::audio::SoundBank::HitList one({{7, 0}});
    CHECK(one.choose(random).instance == 7 && one.choose(random).instance == 7);
    ts2::audio::SoundBank::HitList none({});
    CHECK(none.choose(random).instance == 0);
}

// A track's settings, by HitLab's property keys, with the game's defaults for the rest.
void trackSettingsAreRead() {
    const std::string xml =
        "<?xml version=\"1.0\"?><cGZPropertySetUint32>"
        "<AnyUint32 key=\"0xff099db2\" type=\"0xeb61e4f7\">23077</AnyUint32>"
        "<AnyUint32 key=\"0xff53dbf3\" type=\"0xeb61e4f7\">512</AnyUint32>"
        "<AnyUint32 key=\"0xffcc156f\" type=\"0xeb61e4f7\">1</AnyUint32>"
        "<AnyUint32 key=\"0xFFAAB623\" type=\"0xeb61e4f7\">2</AnyUint32>"
        "<AnyUint32 key=\"0xffa6a614\" type=\"0xeb61e4f7\">18</AnyUint32>"
        "<AnyUint32 key=\"0xfff7b8df\" type=\"0xeb61e4f7\">1</AnyUint32>"
        "<AnyUint32 key=\"0xff8dcfd8\" type=\"0xeb61e4f7\">4294967294</AnyUint32>"
        "<AnyUint32 key=\"0xff3ec880\" type=\"0xeb61e4f7\">1024</AnyUint32>"
        "<AnyString key=\"0xff1cdf6a\" type=\"0x0b8bea18\">Track</AnyString>"
        "</cGZPropertySetUint32>";
    const auto t = ts2::audio::SoundBank::readTrack(std::span(reinterpret_cast<const std::uint8_t*>(xml.data()), xml.size()));
    CHECK(t.pitchHz == 23077 && t.volume == 0.5f && t.is3d && t.minDistance == 2 && t.maxDistance == 18 && t.looped);
    CHECK(t.pitchRandomMin == -2 && t.pan == 1.0f && t.priority == 32 && t.fadeOutMs == 100 && t.rolloff == 1);
    const auto defaults = ts2::audio::SoundBank::readTrack(std::span<const std::uint8_t>());
    CHECK(defaults.volume == 1 && !defaults.is3d && defaults.minDistance == 4 && defaults.maxDistance == 32 && !defaults.looped);
}

std::shared_ptr<const Pcm> constant(int rate, std::size_t frames, std::int16_t value) {
    auto pcm = std::make_shared<Pcm>();
    pcm->rate = rate;
    pcm->channels = 1;
    pcm->samples.assign(frames, value);
    return pcm;
}

// The mixer plays a clip to its end (or round again when looped) through an equal-power pan,
// times its volume, its category's and the master volume, and fades a stop out.
void mixerPlaysAndStops() {
    ts2::audio::Mixer mixer(8000, 2);
    std::vector<float> out(2 * 200);
    ts2::audio::PlayRequest r;
    r.pcm = constant(8000, 100, 16384);
    r.fadeInSeconds = 0;
    const auto id = mixer.play(r);
    CHECK(id != 0 && mixer.playing(id));
    mixer.mix(out.data(), 200);
    CHECK(std::abs(out[0] - 0.5f * 0.70710678f) < 1e-4f && std::abs(out[1] - out[0]) < 1e-6f);
    CHECK(out[2 * 150] == 0.0f && !mixer.playing(id));  // ended after its 100 frames
    // Hard right; half the effects volume.
    r.pan = 1;
    mixer.setCategoryVolume(Category::Effects, 0.5f);
    mixer.play(r);
    mixer.mix(out.data(), 10);
    CHECK(std::abs(out[0]) < 1e-6f && std::abs(out[1] - 0.25f) < 1e-4f);
    mixer.stopAll(false);
    // A looped clip goes round; a stop fades it over its fade-out time, then it ends.
    mixer.setCategoryVolume(Category::Effects, 1);
    r.pan = 0;
    r.loop = true;
    r.fadeOutSeconds = 50.0f / 8000.0f;  // 50 frames
    const auto loop = mixer.play(r);
    mixer.mix(out.data(), 200);
    CHECK(out[2 * 199] > 0.3f);
    mixer.stop(loop);
    CHECK(!mixer.playing(loop));
    mixer.mix(out.data(), 100);
    CHECK(out[2 * 10] > 0.0f && out[2 * 10] < 0.35f && out[2 * 80] == 0.0f && mixer.voicesPlaying() == 0);
    // With every voice busy, a lower priority waits and a higher one takes the lowest's place.
    r.loop = true;
    r.priority = 50;
    mixer.play(r);
    mixer.play(r);
    r.priority = 10;
    CHECK(mixer.play(r) == 0);
    r.priority = 90;
    CHECK(mixer.play(r) != 0 && mixer.voicesPlaying() == 2);
    // Paused, a category holds; nothing without a clip.
    mixer.setPaused(Category::Effects, true);
    mixer.mix(out.data(), 10);
    CHECK(out[0] == 0.0f);
    CHECK(mixer.play({}) == 0);
}

// The game's DirectSound distance model: full volume to the minimum distance, then falling as
// min / (min + rolloff * (d - min)), held past the maximum.
void soundsFallOffWithDistance() {
    CHECK(ts2::audio::spatial(1, 0, 4, 32, 1).gain == 1.0f);
    CHECK(std::abs(ts2::audio::spatial(8, 0, 4, 32, 1).gain - 0.5f) < 1e-6f);
    CHECK(std::abs(ts2::audio::spatial(100, 0, 4, 32, 1).gain - 4.0f / 32.0f) < 1e-6f);
    CHECK(std::abs(ts2::audio::spatial(8, 0, 4, 32, 2).gain - 4.0f / 12.0f) < 1e-6f);
    CHECK(ts2::audio::spatial(20, 1, 4, 32, 1).pan == 1.0f && ts2::audio::spatial(2, 1, 4, 32, 1).pan == 0.5f);
}

// An animation's sound event names its sound on a sound_name line.
void animationSoundEventsNameTheirSound() {
    ts2::anim::Event e;
    e.type = ts2::anim::kSoundEvent;
    e.text = "sound_name=Barstool_sit_boost\r\nsound_source=center_of_object\r\nbone_tag=\r\n";
    CHECK(ts2::anim::soundEventName(e) == "Barstool_sit_boost");
    e.text = "sound_name=\nsound_source=center_of_object\n";
    CHECK(!ts2::anim::soundEventName(e));
    e.text = "other_sound_name=x\nsound_name=y";
    CHECK(ts2::anim::soundEventName(e) == "y");
    e.type = ts2::anim::kSimAnticsEvent;
    CHECK(!ts2::anim::soundEventName(e));
}

} // namespace

std::vector<Test> audioTests() {
    return {
        {"XA blocks decode as the game", xaBlocksDecodeAsTheGame},
        {"SPX clips decode", spxClipsDecode},
        {"hit lists are read", hitListsAreRead},
        {"hit lists don't repeat soon", hitListsDontRepeatSoon},
        {"track settings are read", trackSettingsAreRead},
        {"the mixer plays and stops", mixerPlaysAndStops},
        {"sounds fall off with distance", soundsFallOffWithDistance},
        {"animation sound events name their sound", animationSoundEventsNameTheirSound},
    };
}
