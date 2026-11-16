#include "test_support.hpp"

#include "../app/audio_options_panel.hpp"
#include "audio/audio_config.hpp"
#include "audio/bank.hpp"
#include "audio/manager.hpp"
#include "audio/mix.hpp"
#include "audio/mp3.hpp"
#include "audio/music.hpp"
#include "audio/song_settings.hpp"
#include "audio/song_titles.hpp"
#include "audio/mp3.hpp"
#include "audio/names.hpp"
#include "audio/spx.hpp"
#include "audio/track.hpp"
#include "audio/xa.hpp"
#include "formats/anim.hpp"

#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <set>
#include <vector>

namespace {

using namespace ts2::audio;

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

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
    return data;
}

void soundNamesHashAsTheGameFilesThem() {
    // Misc.package files the "events" set under these, and the base game's Sfx1.package a track
    // named door_open.
    CHECK(soundKey("events") == (SoundKey{0xFFD6C8B9u, 0xC48BBB17u}));
    CHECK(soundKey("aliases") == (SoundKey{0xFF1EBA43u, 0x3CF64D4Bu}));
    CHECK(soundKey("door_open") == (SoundKey{0xFFF17AC7u, 0x85519390u}));
    // Event names keep their capitals, the tracks are filed under the lowercase name (Voice4.package).
    CHECK(soundKey("vox_soc_agree_medA_afa") == (SoundKey{0xFFB1439Fu, 0x264D263Bu}));
}

void modifierListsBecomeMasks() {
    CHECK(modifierMask("destfootstep,sourcefootstep") == ((1u << 16) | (1u << 17)));
    CHECK(modifierMask("sourcefootstep, destfootstep") == ((1u << 16) | (1u << 17)));
    CHECK(modifierMask("lowlifescore") == 0);
    CHECK(modifierMask("objectquality,destsurfacetype") == ((1u << 14) | (1u << 1)));
    // The alias always runs, vox on every name starting with "vox".
    CHECK(eventMask("door_open", 0) == 1u << 20);
    CHECK(eventMask("vox_laugh", 0) == ((1u << 20) | (1u << 18)));
}

void voxSuffixesFollowAgeSexAndVoice() {
    CHECK(voxSuffix({17, 1, 0, 0, 0, 0}) == "tfa");  // a teen girl
    CHECK(voxSuffix({10, 0, 0, 0, 0, 0}) == "ca");   // a child: no sex letter
    CHECK(voxSuffix({2, 0, 0, 0, 0, 0}) == "p");     // a toddler: the age letter alone
    CHECK(voxSuffix({1, 0, 0, 0, 0, 0}) == "b");
    CHECK(voxSuffix({30, 1, 0, 1, 0, 0}) == "afb");  // adults have two voices
    CHECK(voxSuffix({30, 0, 0, 2, 0, 0}) == "ama");
    CHECK(voxSuffix({60, 0, 0, 1, 0, 0}) == "ema");  // elders have one
    CHECK(voxSuffix({30, 0, 1, 1, 0, 0}) == "amg");  // a ghost
    CHECK(voxSuffix({30, 0, 0, 1, 1, 0}) == "tma");  // person data 0xAF keeps an adult at teen age
    CHECK(voxSuffix({30, 0, 0, 1, 0, 1}) == "nda");  // a dog
    CHECK(voxSuffix({30, 0, 0, 1, 0, 3}) == "nka");  // a cat
}

void trackNamesAppendInBitOrderThenAlias() {
    NameInputs inputs;
    inputs.destSurface = "wood";
    inputs.sourceSurface = "glass";
    inputs.quality = "cheap";
    const std::uint32_t mask = modifierMask("objectquality,sourcesurfacetype,destsurfacetype");
    CHECK(trackName("glass_put", eventMask("glass_put", mask), inputs, {}) == "glass_put_wood_glass_cheap");
    // A modifier without its value leaves the name as it is, the others still run.
    inputs.sourceSurface.reset();
    CHECK(trackName("glass_put", eventMask("glass_put", mask), inputs, {}) == "glass_put_wood_cheap");
    // The alias replaces the full name once, from Misc.package's own example.
    std::map<std::string, std::string> aliases{{"cine_vox_age_teen_adultmT_tfa", "cine_vox_age_teen_adultfT_tfa"}};
    NameInputs teen;
    teen.speaker = Speaker{17, 1, 0, 0, 0, 0};
    CHECK(trackName("cine_vox_age_teen_adultmT", eventMask("cine_vox_age_teen_adultmT", modifierMask("vox")), teen, aliasMap(aliases)) ==
          "cine_vox_age_teen_adultfT_tfa");
    // No suffix for an object that is not a person.
    CHECK(trackName("vox_hello", eventMask("vox_hello", 0), {}, {}) == "vox_hello");
}

void distanceCurveIsTheGames() {
    CHECK(distanceGain(1, 2, 64, 1.2f) == 1);
    CHECK(distanceGain(65, 2, 64, 1.2f) == 0);
    CHECK(near(distanceGain(4, 2, 64, 1.2f), 1 / 2.2f));
    CHECK(near(distanceGain(10, 2, 64, 1.2f), 1 / (4 * 1.2f + 1)));
    CHECK(distanceGain(100, 2, 64, 0) == 1);
}

void listenerAxesDropSeparately() {
    // A camera looking north, level: side is east, the listener 1 above the target.
    const Listener listener = cameraListener({0, -10, 0}, {0, 0, 0}, {0, 0, 1}, 1, 0, 0.4f);
    CHECK(near(listener.side[0], 1) && near(listener.forward[1], 1) && near(listener.up[2], 1));
    CHECK(near(listener.position[2], 1));
    CHECK(near(listener.ears[1], -6) && near(listener.ears[2], 0.4f));
    // Four tiles east and four north: each axis drops on its own, the up axis hardly at all.
    const float gain = axisGain(listener, {4, 4, 1}, 2, 64, 1.2f, 1, 0.001f);
    CHECK(near(gain, (1 / 2.2f) * (1 / 2.2f)));
    const float above = axisGain(listener, {0, 0, 9}, 2, 64, 1.2f, 1, 0.001f);
    CHECK(near(above, 1 / ((8.0f / 2 - 1) * 1.2f * 0.001f + 1)));
}

void panIsEqualPowerWithScreenRightPositive() {
    const Listener listener = cameraListener({0, -10, 0}, {0, 0, 0}, {0, 0, 1}, 0, 0, 1);
    CHECK(near(azimuth(listener, {5, 0, 0}), std::numbers::pi_v<float> / 2));
    const EarGains right = panGains(std::numbers::pi_v<float> / 2);
    CHECK(near(right.right, 1) && near(right.left, 0));
    const EarGains front = panGains(0);
    CHECK(near(front.left, 0.70710678f) && near(front.right, 0.70710678f));
    // A stereo source never pans: each ear gets the same power wherever it is.
    for (float a = -3; a <= 3; a += 0.5f) {
        const auto channels = channelGains(a, 2);
        CHECK(near(channels[0].left * channels[0].left + channels[1].left * channels[1].left, 1));
        CHECK(near(channels[0].right * channels[0].right + channels[1].right * channels[1].right, 1));
    }
    // Straight ahead its left channel is in the left ear only.
    const auto ahead = channelGains(0, 2);
    CHECK(near(ahead[0].left, 1) && near(ahead[0].right, 0) && near(ahead[1].right, 1));
}

void playChanceComparesTheRoll() {
    CHECK(chanceRoll(0) == 0);
    CHECK(chanceRoll(32767) == 100);
    CHECK(chanceRoll(16383) == 49);
    // The game plays when the chance is at least the roll: a roll of 52 against 50 is dropped.
    CHECK(!chancePlays(50, 52));
    CHECK(chancePlays(50, 50));
    CHECK(chancePlays(0, 100));
    AudioRandom a(7), b(7);
    CHECK(a.next() == b.next());
}

void effectivePriorityFadesOverTheLastQuarterSecond() {
    CHECK(near(effectivePriority(100, 0.5f, 1000), 50));
    CHECK(near(effectivePriority(100, 1, 100), 40));
}

TrackSettings oneShot(float priority = 100) {
    TrackSettings s;
    s.priority = priority;
    return s;
}

Request request(std::uint32_t track, std::int16_t object, Group group = Group::Sfx) {
    Request r;
    r.track = {track, 1};
    r.event = r.track;
    r.object = object;
    r.group = group;
    return r;
}

void voiceLimitStealsOnlyLowerPriorities() {
    const std::vector<SampleKey> samples{{1, 1}};
    Tuning tuning;
    tuning.maxSoundsByGroup[0] = {1, 1, 1};
    Manager manager;
    manager.setTuning(tuning);
    // One more than the limit plays, as the count is taken before the new sound.
    CHECK(manager.start(request(1, 1), oneShot(50), samples, 0));
    CHECK(manager.start(request(2, 2), oneShot(100), samples, 0));
    // Full: the lowest one-shot is stolen only for a strictly higher priority.
    Dropped why = Dropped::None;
    CHECK(!manager.start(request(3, 3), oneShot(50), samples, 0, &why));
    CHECK(why == Dropped::NoVoice);
    CHECK(manager.start(request(4, 4), oneShot(80), samples, 0));
    (void)manager.update(0);
    CHECK(manager.instances().size() == 2);
    // Loops are never stolen.
    Manager loops;
    loops.setTuning(tuning);
    TrackSettings loop = oneShot(10);
    loop.looping = true;
    CHECK(loops.start(request(1, 1), loop, samples, 0));
    CHECK(loops.start(request(2, 2), loop, samples, 0));
    CHECK(!loops.start(request(3, 3), oneShot(1000), samples, 0));
}

void pauseDropsOneShotsAndHoldsLoops() {
    const std::vector<SampleKey> samples{{1, 1}};
    Manager manager;
    manager.setGamePaused(true);
    Dropped why = Dropped::None;
    CHECK(!manager.start(request(1, 1), oneShot(), samples, 0, &why));
    CHECK(why == Dropped::GroupPaused);
    TrackSettings loop;
    loop.looping = true;
    const auto id = manager.start(request(2, 1), loop, samples, 0);
    CHECK(id);
    CHECK(manager.start(request(3, 0, Group::Ui), oneShot(), samples, 0));
    auto outputs = manager.update(16);
    for (const auto& output : outputs) {
        CHECK(output.advancing == (output.id != *id));
    }
    // Unpaused, the loop goes on.
    manager.setGamePaused(false);
    outputs = manager.update(16);
    CHECK(outputs.size() == 2 && outputs[0].advancing);
}

void groupVolumesMutesAndDippingScaleTheMix() {
    const std::vector<SampleKey> samples{{1, 1}};
    const float centre = std::sqrt(0.5f);
    Manager manager;
    CHECK(manager.start(request(1, 1), oneShot(), samples, 0));
    auto out = manager.update(0);
    CHECK(out.size() == 1 && near(out[0].mono.left, centre));
    manager.setGroupVolume(Group::Sfx, 0.5f);
    out = manager.update(0);
    CHECK(near(out[0].mono.left, centre * 0.5f));
    manager.setGroupMuted(Group::Sfx, true);
    out = manager.update(0);
    CHECK(near(out[0].mono.left, 0));
    manager.setGroupMuted(Group::Sfx, false);
    // --mute silences everything without losing the levels.
    manager.setMuted(true);
    out = manager.update(0);
    CHECK(near(out[0].mono.left, 0));
    manager.setMuted(false);
    out = manager.update(0);
    CHECK(near(out[0].mono.left, centre * 0.5f));
}

void cinematicDippingFadesAndComesBack() {
    Tuning tuning;
    tuning.sfxDippingVolume = 0.2f;
    tuning.voxDippingVolume = 0.3f;
    tuning.musicDippingVolume = 0.1f;
    tuning.dippingStartMs = 500;
    tuning.dippingStopMs = 5000;
    Manager manager;
    manager.setTuning(tuning);
    const std::vector<SampleKey> samples{{1, 1}};
    CHECK(manager.start(request(1, 1), oneShot(), samples, 0));
    const float centre = std::sqrt(0.5f);
    manager.setDipping(true);
    // Half of the start time takes the effects half way down to 0.2.
    auto out = manager.update(250);
    CHECK(near(out[0].mono.left, centre * 0.6f));
    out = manager.update(250);
    CHECK(near(out[0].mono.left, centre * 0.2f));
    // Coming back takes the stop time.
    manager.setDipping(false);
    out = manager.update(2500);
    CHECK(near(out[0].mono.left, centre * 0.6f));
    out = manager.update(2500);
    CHECK(near(out[0].mono.left, centre));
    // The stereo dips with the music, and ambience doesn't dip at all.
    Manager groups;
    groups.setTuning(tuning);
    Request stereo = request(2, 2);
    stereo.group = Group::Stereo;
    Request ambience = request(3, 3);
    ambience.group = Group::Ambience;
    CHECK(groups.start(stereo, oneShot(), samples, 0) && groups.start(ambience, oneShot(), samples, 0));
    groups.setDipping(true);
    out = groups.update(500);
    CHECK(out.size() == 2 && near(out[0].mono.left, centre * 0.1f) && near(out[1].mono.left, centre));
}

void musicTuningReadsItsValues() {
    Tuning tuning;
    readMusicTuning({{"defaultmusicvolume", ".7"}, {"musicpriority", "128"}, {"trackfadeouttime", "3000"},
                     {"musicmindistance", "5"}, {"musicmaxdistance", "64"}},
                    tuning);
    readManagerTuning({{"2dMusicVolumeScalar", "0.75"}}, tuning);
    CHECK(near(tuning.musicVolume, 0.7f) && near(tuning.music2dScalar, 0.75f) && near(tuning.musicPriority, 128));
    CHECK(tuning.musicFadeOutMs == 3000 && near(tuning.musicMinDistance, 5) && near(tuning.musicMaxDistance, 64));
}

void songSettingsReadAndWriteTheirFile() {
    // Three songs of a station, the middle one off, as a station folder's file holds them.
    const std::vector<std::uint8_t> file{0x62, 0x48, 0xEA, 0x2B, 0x02, 0x00, 0x03, 0x00, 0x00, 0x00,
                                         0xE1, 0x08, 0xA9, 0xCB, 0x08, 0x72, 0x19, 0xFF, 0x01,
                                         0xE1, 0x08, 0xA9, 0xCB, 0x79, 0x2D, 0x37, 0xFF, 0x00,
                                         0xE1, 0x08, 0xA9, 0xCB, 0x8D, 0xB8, 0x4A, 0xFF, 0x01};
    const auto settings = parseSongSettings(file);
    CHECK(settings && settings->songs.size() == 3);
    CHECK(settings->enabled(0xFF197208) && !settings->enabled(0xFF372D79) && settings->enabled(0xFF4AB88D));
    CHECK(settings->enabled(0xFF000001));  // a song the file does not list plays
    CHECK(writeSongSettings(*settings) == file);
    // A cut-short file, extra bytes, another magic or another type are refused.
    CHECK(!parseSongSettings(std::span(file).first(file.size() - 1)));
    auto longer = file;
    longer.push_back(0);
    CHECK(!parseSongSettings(longer));
    auto other = file;
    other[0] = 0;
    CHECK(!parseSongSettings(other));
    other = file;
    other[10] = 0;
    CHECK(!parseSongSettings(other) && !parseSongSettings({}));
}

void songTitlesAreFoundByTheHashOfTheDefaultText() {
    ts2::str::StringTable table;
    for (const auto& [language, text] : std::vector<std::pair<int, const char*>>{
             {1, "Techno"}, {1, "Banana Blaster"}, {3, "Techno"}, {3, "La guerre des bananes"}}) {
        ts2::str::Entry entry;
        entry.language = static_cast<std::uint8_t>(language);
        entry.value = text;
        table.entries.push_back(entry);
    }
    SongTitles titles;
    titles.add(table);
    // 0xFF82AD9E is CRC24 of "Banana Blaster", the id its song has.
    CHECK(titles.title(0xFF82AD9E, 1) && *titles.title(0xFF82AD9E, 1) == "Banana Blaster");
    CHECK(*titles.title(0xFF82AD9E, 3) == "La guerre des bananes");
    CHECK(*titles.title(0xFF82AD9E, 9) == "Banana Blaster");  // no text in that language
    CHECK(titles.title(0xFF000001, 1) == nullptr);
    // The station's own name is a row too: pop's id is the hash of "pop".
    ts2::str::StringTable pop;
    ts2::str::Entry name;
    name.value = "Pop";
    pop.entries.push_back(name);
    titles.add(pop);
    CHECK(titles.title(0xFF23C3BF, 1) && *titles.title(0xFF23C3BF, 1) == "Pop");
}

void playlistPlaysEverySongBeforeRepeatingAndNeverTwiceInARow() {
    std::vector<SampleKey> songs;
    for (std::uint32_t i = 1; i <= 5; ++i) {
        songs.push_back({i, 0});
    }
    Playlist playlist(songs, 7);
    std::vector<std::uint32_t> heard;
    for (int i = 0; i < 20; ++i) {
        const SampleKey next = playlist.upcoming();
        const SampleKey taken = playlist.take();
        CHECK(next.instance == taken.instance);
        heard.push_back(taken.instance);
    }
    for (std::size_t round = 0; round < 4; ++round) {
        std::set<std::uint32_t> once(heard.begin() + static_cast<std::ptrdiff_t>(round * 5),
                                     heard.begin() + static_cast<std::ptrdiff_t>(round * 5 + 5));
        CHECK(once.size() == 5);
    }
    for (std::size_t i = 1; i < heard.size(); ++i) {
        CHECK(heard[i] != heard[i - 1]);
    }
    CHECK(Playlist({}, 1).empty());
}

void oneSoundPausesAndFadesOut() {
    const std::vector<SampleKey> samples{{1, 1}};
    const float centre = std::sqrt(0.5f);
    Manager manager;
    const auto id = manager.start(request(1, 0, Group::Music), oneShot(), samples, 0);
    CHECK(id.has_value());
    // A pause with no fade is silent and holds at once, and resuming brings it back at once.
    manager.pause(*id, true, 0);
    auto out = manager.update(10);
    CHECK(out.size() == 1 && near(out[0].mono.left, 0) && !out[0].advancing);
    manager.pause(*id, false, 0);
    out = manager.update(10);
    CHECK(near(out[0].mono.left, centre) && out[0].advancing);
    // A fade-out takes its time, then the sound is gone.
    manager.fadeOut(*id, 1000);
    out = manager.update(500);
    CHECK(out.size() == 1 && near(out[0].mono.left, centre * 0.5f, 1e-3f));
    out = manager.update(600);
    CHECK(out.empty() && manager.find(*id) == nullptr);
}

void biquadFiltersPassTheirBand() {
    // A tone of one frequency through a filter, as the loudness of what is left once it has settled.
    const auto level = [](Biquad filter, float hz) {
        float peak = 0;
        for (int i = 0; i < 4800; ++i) {
            const float out = filter.process(std::sin(2 * std::numbers::pi_v<float> * hz * static_cast<float>(i) / 48000.0f));
            if (i > 2400) {
                peak = std::max(peak, std::fabs(out));
            }
        }
        return peak;
    };
    // The cheap stereo's band: low-pass 8500 Hz, high-pass 3500 Hz.
    CHECK(level(Biquad::lowpass(8500, 48000), 200) > 0.98f);
    CHECK(near(level(Biquad::lowpass(8500, 48000), 8500), 0.7071f, 0.02f));
    CHECK(level(Biquad::lowpass(8500, 48000), 20000) < 0.2f);
    CHECK(level(Biquad::highpass(3500, 48000), 200) < 0.01f);
    CHECK(near(level(Biquad::highpass(3500, 48000), 3500), 0.7071f, 0.02f));
    CHECK(level(Biquad::highpass(3500, 48000), 15000) > 0.95f);
    // A cutoff of 0 leaves the sound as it is.
    CHECK(near(level(Biquad::lowpass(0, 48000), 1000), 1, 1e-4f) && near(level(Biquad::highpass(0, 48000), 1000), 1, 1e-4f));
}

void dippingTuningReadsItsValues() {
    Tuning tuning;
    readDippingTuning({{"sfxdippingvolume", "0.2"}, {"voxdippingvolume", "0.3"}, {"musicdippingvolume", "0.1"},
                       {"dippingstarttime", "0.5"}, {"dippingstoptime", "5"}},
                      tuning);
    CHECK(near(tuning.sfxDippingVolume, 0.2f) && near(tuning.voxDippingVolume, 0.3f) && near(tuning.musicDippingVolume, 0.1f));
    CHECK(tuning.dippingStartMs == 500 && tuning.dippingStopMs == 5000);
    // A broken value leaves the default.
    Tuning fallback;
    readDippingTuning({{"sfxdippingvolume", "x"}}, fallback);
    CHECK(near(fallback.sfxDippingVolume, 0.5f) && fallback.dippingStopMs == 5000);
}

// 0.2 s of a 440 Hz tone, stereo 44.1 kHz at 32 kbit/s, made with ffmpeg.
const std::uint8_t kTinyMp3[] = {
    0xff, 0xfb, 0x10, 0x64, 0x00, 0x00, 0x00, 0x79, 0x06, 0xd3, 0x85, 0x30, 0x00, 0x0a, 0x00, 0x00,
    0x0d, 0x20, 0xa0, 0x00, 0x01, 0x04, 0x08, 0x33, 0x4a, 0x19, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x34,
    0x83, 0x00, 0x00, 0x00, 0x12, 0xc4, 0xb3, 0x38, 0xce, 0x04, 0x00, 0x40, 0x1a, 0x13, 0x1f, 0xbb,
    0x6f, 0x87, 0x87, 0x97, 0x98, 0x43, 0xc2, 0x50, 0x15, 0xa5, 0x60, 0x0a, 0x30, 0xd6, 0x01, 0x86,
    0x83, 0xc4, 0x5d, 0x1b, 0x61, 0x75, 0xc6, 0x86, 0xaf, 0xff, 0x7c, 0x28, 0x0f, 0x80, 0x83, 0x5c,
    0x2a, 0x0a, 0xfd, 0x8a, 0x70, 0x06, 0xe0, 0x00, 0x63, 0x8c, 0x66, 0x63, 0x46, 0x3f, 0xa3, 0x8c,
    0x56, 0x1d, 0x95, 0x53, 0x53, 0x65, 0x28, 0x1e, 0xff, 0xfb, 0x12, 0x64, 0x0a, 0x03, 0xf0, 0x8f,
    0x06, 0xd5, 0xa7, 0x60, 0x00, 0x08, 0x00, 0x00, 0x0d, 0x20, 0xe0, 0x00, 0x01, 0x02, 0x44, 0x1d,
    0x58, 0x84, 0xe1, 0x82, 0x60, 0x00, 0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0xf0, 0x00, 0xf0, 0x46,
    0x14, 0x95, 0x82, 0x26, 0x9c, 0x76, 0x00, 0xf9, 0x2d, 0x61, 0xda, 0xfe, 0x69, 0x69, 0xd0, 0x72,
    0x21, 0x06, 0x34, 0x4f, 0x03, 0x43, 0x04, 0x65, 0xa4, 0xa4, 0x90, 0x05, 0x1d, 0x20, 0x98, 0x02,
    0x92, 0x37, 0x28, 0x30, 0xd9, 0x12, 0x2d, 0x80, 0x08, 0x59, 0x91, 0xab, 0x4d, 0x29, 0x74, 0x84,
    0x40, 0xe6, 0x83, 0xee, 0x7f, 0x0e, 0xcb, 0x88, 0xd1, 0x10, 0x12, 0x59, 0x28, 0x1a, 0xf0, 0x00,
    0xf0, 0xff, 0xfb, 0x10, 0x64, 0x1a, 0x83, 0xf0, 0x88, 0x07, 0x50, 0x01, 0x9b, 0x30, 0x98, 0x00,
    0x00, 0x0d, 0x20, 0x00, 0x00, 0x01, 0x01, 0xbc, 0x1b, 0x57, 0x00, 0x60, 0xc0, 0xb0, 0x00, 0x00,
    0x34, 0x80, 0x00, 0x00, 0x04, 0x4c, 0x01, 0x2b, 0x06, 0x25, 0x51, 0xd8, 0x0d, 0x91, 0xa8, 0x5d,
    0x1a, 0x70, 0x0d, 0x30, 0x00, 0x32, 0x94, 0x0e, 0xc7, 0x82, 0x9e, 0x5e, 0xd8, 0x81, 0x2b, 0x00,
    0x89, 0x3c, 0x9a, 0x00, 0x4d, 0x04, 0x86, 0xa3, 0x24, 0x0a, 0x9c, 0x5e, 0xe1, 0x82, 0x04, 0x62,
    0x08, 0x55, 0x70, 0x0d, 0x30, 0x00, 0x2b, 0x12, 0x0f, 0x47, 0x42, 0xff, 0x5e, 0xb9, 0x05, 0x65,
    0x02, 0x14, 0x4d, 0x81, 0x9f, 0x00, 0x0b, 0x1c, 0x02, 0xff, 0xfb, 0x12, 0x64, 0x2d, 0x03, 0xf0,
    0x87, 0x07, 0x50, 0x03, 0x79, 0x31, 0x08, 0x00, 0x00, 0x0d, 0x20, 0x00, 0x00, 0x01, 0x02, 0x18,
    0x1b, 0x5a, 0x83, 0x61, 0x22, 0x60, 0x00, 0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0x56, 0x00, 0x5a,
    0x4d, 0xd8, 0x03, 0x64, 0x70, 0x5d, 0x1a, 0xca, 0xac, 0xa9, 0xd1, 0x15, 0x04, 0x32, 0xae, 0x23,
    0xa4, 0x27, 0x24, 0xec, 0x48, 0x02, 0x89, 0x87, 0x60, 0x11, 0x15, 0xf0, 0x00, 0x9e, 0x21, 0x97,
    0x08, 0x21, 0x66, 0x45, 0xab, 0x47, 0x22, 0xa1, 0x80, 0x53, 0x27, 0xe7, 0x32, 0x40, 0x57, 0x28,
    0xdd, 0x44, 0x29, 0x2f, 0xce, 0xdb, 0xe8, 0x91, 0x87, 0x02, 0x19, 0xb9, 0x12, 0x9d, 0x6e, 0xbc,
    0xae, 0x37, 0xff, 0xfb, 0x10, 0x64, 0x3e, 0x83, 0xf0, 0x84, 0x07, 0x56, 0x21, 0x18, 0x18, 0x98,
    0x00, 0x00, 0x0d, 0x20, 0x00, 0x00, 0x01, 0x01, 0xf4, 0x1b, 0x42, 0x04, 0xed, 0x22, 0x60, 0x00,
    0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0x1b, 0xcd, 0x30, 0x03, 0x8c, 0xd6, 0xe7, 0x14, 0x12, 0x2b,
    0xfc, 0x6e, 0xf8, 0x28, 0x41, 0x8b, 0x09, 0xcf, 0xf7, 0x72, 0xc4, 0x34, 0xff, 0x7f, 0x8e, 0x40,
    0xa0, 0x39, 0x00, 0xc5, 0x80, 0xb2, 0x5c, 0x10, 0x96, 0x3d, 0xc1, 0xf1, 0x29, 0xde, 0x85, 0x4c,
    0x41, 0x4d, 0x45, 0x33, 0x2e, 0x31, 0x30, 0x30, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xff, 0xfb, 0x12, 0x64, 0x50, 0x03,
    0xf0, 0x83, 0x07, 0x56, 0x21, 0x18, 0x18, 0x98, 0x00, 0x00, 0x0d, 0x20, 0x00, 0x00, 0x01, 0x02,
    0x08, 0x1b, 0x58, 0x81, 0x61, 0x22, 0x60, 0x00, 0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0xff, 0xfb, 0x10, 0x64, 0x62, 0x03, 0xf0, 0x7d, 0x06, 0xd0, 0x83, 0x79, 0x31,
    0x08, 0x00, 0x00, 0x0d, 0x20, 0x00, 0x00, 0x01, 0x01, 0xa0, 0x1b, 0x5f, 0x00, 0x3c, 0xc0, 0xb0,
    0x00, 0x00, 0x34, 0x80, 0x00, 0x00, 0x04, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xff, 0xfb, 0x12, 0x64, 0x75,
    0x8f, 0xf0, 0x7c, 0x06, 0xd0, 0x81, 0x9b, 0x30, 0x98, 0x00, 0x00, 0x0d, 0x20, 0x00, 0x00, 0x01,
    0x02, 0x08, 0x1b, 0x48, 0x15, 0xa0, 0x00, 0x20, 0x00, 0x00, 0x34, 0x82, 0x80, 0x00, 0x04, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0xff, 0xfb, 0x10, 0x64, 0x88, 0x00, 0x01, 0x34, 0x0e, 0x51, 0x86, 0x68,
    0x40, 0x00, 0x00, 0x00, 0x0d, 0x20, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x01, 0xa4, 0x1c, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x34, 0x83, 0x80, 0x00, 0x04, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
};

void mp3DecodesToPcmAndRejectsGarbage() {
    const Pcm pcm = decodeMp3(kTinyMp3);
    CHECK(pcm.sampleRate == 44100 && pcm.channels == 2);
    CHECK(pcm.seconds() > 0.15 && pcm.seconds() < 0.35);
    int peak = 0;
    for (const std::int16_t s : pcm.samples) {
        peak = std::max(peak, std::abs(static_cast<int>(s)));
    }
    CHECK(peak > 1000);
    const std::uint8_t garbage[64] = {1, 2, 3};
    bool threw = false;
    try {
        (void)decodeMp3(garbage);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

void spxReadsItsHeaderAndRejectsBadResources() {
    const auto fails = [](std::span<const std::uint8_t> data) {
        try {
            (void)decodeSpx(data);
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(fails({}));
    std::vector<std::uint8_t> stereo(15, 0);
    stereo[0] = 'S', stereo[1] = 'P', stereo[2] = 'X', stereo[3] = '1';
    stereo[4] = 2;
    CHECK(fails(stereo));
    // A valid header with no frames is an empty sound at the mode's own rate.
    std::vector<std::uint8_t> header(15, 0);
    header[0] = 'S', header[1] = 'P', header[2] = 'X', header[3] = '1';
    header[4] = 1;
    header[5] = 0x80, header[6] = 0x02;  // 640 decoded bytes
    header[9] = 2;                       // ultra-wideband, 32 kHz
    header[13] = 0x80, header[14] = 0x02;  // 640 samples a frame
    const Pcm pcm = decodeSpx(header);
    CHECK(pcm.sampleRate == 32000 && pcm.channels == 1 && pcm.samples.empty());
    // A frame whose length runs past the resource is a broken file.
    std::vector<std::uint8_t> cut = header;
    cut.push_back(40);
    CHECK(fails(cut));
    // A frame that doesn't decode still gives a frame of samples, as the game plays on past it.
    std::vector<std::uint8_t> garbage = header;
    garbage[5] = 0x00, garbage[6] = 0x05;  // 1280 decoded bytes, one frame
    garbage.push_back(3);
    garbage.insert(garbage.end(), {0xFF, 0xFF, 0xFF});
    CHECK(decodeSpx(garbage).samples.size() == 640);
}

void sliderThumbsAreTheirShareOfTheTrack() {
    // The volume bars (0 to 100, 5 shown) get the two caps; Audio Quality (1 to 4, 1 shown) a third.
    CHECK(app::AudioOptionsPanel::thumbLength(236, 105, 5, 9) == 18);
    CHECK(app::AudioOptionsPanel::thumbLength(236, 3, 1, 9) == 78);
    CHECK(app::AudioOptionsPanel::thumbLength(10, 105, 5, 9) == 10);
}

void oneInstanceRules() {
    const std::vector<SampleKey> samples{{1, 1}};
    Manager manager;
    TrackSettings single;
    single.instanceMode = 2;
    CHECK(manager.start(request(1, 5), single, samples, 0));
    Dropped why = Dropped::None;
    CHECK(!manager.start(request(1, 5), single, samples, 0, &why));
    CHECK(why == Dropped::AlreadyPlaying);
    CHECK(manager.start(request(1, 6), single, samples, 0));
    // At fast speed a voice line waits for nothing else of its object.
    manager.setSpeedIndex(3);
    CHECK(!manager.start(request(9, 5, Group::Voice), oneShot(), samples, 0, &why));
    CHECK(why == Dropped::VoiceAtSpeed);
    manager.setSpeedIndex(2);
    CHECK(manager.start(request(9, 5, Group::Voice), oneShot(), samples, 0));
}

void stopsMatchEventAndObject() {
    const std::vector<SampleKey> samples{{1, 1}};
    Manager manager;
    Request r = request(1, 5);
    r.event = soundKey("tub_fill");
    CHECK(manager.start(r, oneShot(), samples, 0));
    manager.stopEvent(soundKey("tub_fill"), 6);
    CHECK(manager.update(0).size() == 1);
    manager.stopEvent(soundKey("tub_fill"), 5);
    CHECK(manager.update(0).empty());
}

void trackSettingsReadTheirKeys() {
    const TrackSettings s = trackSettings({{"0xfff7b8df", "1"}, {"0xff595ffd", "500"}, {"0xffcc156f", "1"}, {"0xffaab623", "3"},
                                           {"0xffa6a614", "16"}, {"0xff53dbf3", "512"}, {"0xffe1cf85", "50"}, {"0xff303b1b", "2"}});
    CHECK(s.looping && s.positional && s.fadeOutMs == 500);
    CHECK(s.minDistance == 3 && s.maxDistance == 16);
    CHECK(near(s.volume, 0.5f) && s.playChance == 50 && s.instanceMode == 2);
    // The fade-out time alone is not a loop.
    CHECK(!trackSettings({{"0xff595ffd", "500"}}).looping);
    CHECK(trackSettings({}).priority == 100 && trackSettings({}).maxDistance == 64);
}

void hitListsReadTheirSamples() {
    std::vector<std::uint8_t> list{0x38, 0, 0, 0, 1, 0, 0, 0, 0x28, 0x08, 0x62, 0xFF, 0xCE, 0xF3, 0x15, 0x75};
    const auto samples = hitListSamples(list);
    CHECK(samples.size() == 1 && samples[0].instance == 0xFF620828u && samples[0].instanceHigh == 0x7515F3CEu);
    list[4] = 2;  // a count past the resource
    CHECK(hitListSamples(list).empty());
}

void packedSegmentsKeepTheFilesOrder() {
    const auto paths = packedSegments("[Config]\r\nx = 1\r\n[PackedDBSegments]\r\n; comment\r\n"
                                      "0x00000002 = 0x086d4a89;%DataDirectory%Res\\Sound\\Sfx1.package\r\n"
                                      "0x00000001 = 0x086d4a89;%DataDirectory%Res\\Sound\\Misc.package\r\n"
                                      "0x0000001f = 0x086d4a89;%DataDirectory%res\\Locale\\%Language%\\Sound\\Misc.package\r\n"
                                      ";;0x00000041 = 0x086d4a89;%DataDirectory%Res\\Sound\\HipHop.package\r\n"
                                      "[ResourceFactories]\r\n0x0b8d821a = 0x5b945038;%DataDirectory%Res\\Sound\\No.package\r\n");
    CHECK(paths == (std::vector<std::string>{"res/sound/sfx1.package", "res/sound/misc.package"}));
}

void firstPackageWinsAndLastEntryInOne() {
    const auto entry = [](std::uint32_t group, std::uint32_t instance, std::uint32_t offset) {
        ts2::dbpf::IndexEntry e;
        e.key = {0x0B9EB87E, group, instance, 7};
        e.offset = offset;
        return e;
    };
    KeyMap map;
    const std::vector<ts2::dbpf::IndexEntry> first{entry(1, 10, 100), entry(2, 10, 200), entry(1, 11, 300)};
    const std::vector<ts2::dbpf::IndexEntry> second{entry(3, 10, 400), entry(3, 12, 500)};
    addSegment(map, 0, first, 0x0B9EB87E);
    addSegment(map, 1, second, 0x0B9EB87E);
    CHECK(map.size() == 3);
    CHECK(map.at(mapKey(10, 7)).entry.offset == 200);  // the group is not part of the key
    CHECK(map.at(mapKey(12, 7)).package == 1);
}

void xaDecodesItsNibbles() {
    auto data = xaResource(56, 1, 32000);  // one block: 28 samples
    data.resize(data.size() + 15, 0);
    data[24] = 0x00;  // predictor 0, shift 8
    data[25] = 0x10;  // first nibble 1, second 0
    const Pcm pcm = decodeXa(data);
    CHECK(pcm.sampleRate == 32000);
    CHECK(pcm.channels == 1);
    CHECK(pcm.samples.size() == 28);
    CHECK(pcm.samples[0] == 4096);
    CHECK(pcm.samples[1] == 0);
}

void xaTrimsToTheHeaderSize() {
    auto data = xaResource(28, 1, 32000);  // half a block: 14 samples
    data.resize(data.size() + 15, 0);
    data[25] = 0x11;
    const Pcm pcm = decodeXa(data);
    CHECK(pcm.samples.size() == 14);
    CHECK(pcm.samples[0] == 4096 && pcm.samples[1] == 4096);
}

void xaRejectsWhatIsNotXa() {
    bool threw = false;
    try {
        const std::vector<std::uint8_t> data(24, 0);
        (void)decodeXa(data);
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
        {"sound modifier lists become masks", modifierListsBecomeMasks},
        {"vox suffixes follow age, sex and voice", voxSuffixesFollowAgeSexAndVoice},
        {"track names append in bit order, then alias", trackNamesAppendInBitOrderThenAlias},
        {"the sound distance curve is the game's", distanceCurveIsTheGames},
        {"listener axes drop separately", listenerAxesDropSeparately},
        {"sound pan is equal power, screen right positive", panIsEqualPowerWithScreenRightPositive},
        {"play chance compares the roll", playChanceComparesTheRoll},
        {"effective priority fades over the last quarter second", effectivePriorityFadesOverTheLastQuarterSecond},
        {"voice limit steals only lower priorities", voiceLimitStealsOnlyLowerPriorities},
        {"pause drops one-shots and holds loops", pauseDropsOneShotsAndHoldsLoops},
        {"group volumes, mutes and dipping scale the mix", groupVolumesMutesAndDippingScaleTheMix},
        {"cinematic dipping fades and comes back", cinematicDippingFadesAndComesBack},
        {"dipping tuning reads its values", dippingTuningReadsItsValues},
        {"biquad filters pass their band", biquadFiltersPassTheirBand},
        {"music tuning reads its values", musicTuningReadsItsValues},
        {"song settings read and write their file", songSettingsReadAndWriteTheirFile},
        {"song titles are found by the hash of the default text", songTitlesAreFoundByTheHashOfTheDefaultText},
        {"a playlist plays every song before repeating", playlistPlaysEverySongBeforeRepeatingAndNeverTwiceInARow},
        {"one sound pauses and fades out", oneSoundPausesAndFadesOut},
        {"SPX reads its header and rejects bad resources", spxReadsItsHeaderAndRejectsBadResources},
        {"MP3 decodes to PCM and rejects garbage", mp3DecodesToPcmAndRejectsGarbage},
        {"slider thumbs are their share of the track", sliderThumbsAreTheirShareOfTheTrack},
        {"one instance rules", oneInstanceRules},
        {"sound stops match event and object", stopsMatchEventAndObject},
        {"track settings read their keys", trackSettingsReadTheirKeys},
        {"hit lists read their samples", hitListsReadTheirSamples},
        {"packed segments keep the file's order", packedSegmentsKeepTheFilesOrder},
        {"first sound package wins, last entry in one", firstPackageWinsAndLastEntryInOne},
        {"XA decodes its nibbles", xaDecodesItsNibbles},
        {"XA trims to the header size", xaTrimsToTheHeaderSize},
        {"XA rejects what is not XA", xaRejectsWhatIsNotXa},
        {"animation sound marks name their sound", animationSoundMarksNameTheirSound},
    };
}
