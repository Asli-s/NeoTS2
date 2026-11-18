#include "test_support.hpp"

#include "audio/movie_sound.hpp"
#include "content/broadcasts.hpp"
#include "formats/movie.hpp"
#include "sim/tv_links.hpp"
#include "sim/world.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ts2::movie;

// Bits written high first into little-endian 16-bit words, as movie frames hold them.
class Bits {
public:
    void put(std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            bits_.push_back(static_cast<std::uint8_t>(value >> i & 1));
        }
    }
    std::vector<std::uint8_t> bytes() const {
        std::vector<std::uint8_t> out;
        for (std::size_t w = 0; w < bits_.size(); w += 16) {
            std::uint16_t word = 0;
            for (std::size_t i = 0; i < 16; ++i) {
                word = static_cast<std::uint16_t>(word << 1 | (w + i < bits_.size() ? bits_[w + i] : 0));
            }
            out.push_back(static_cast<std::uint8_t>(word));
            out.push_back(static_cast<std::uint8_t>(word >> 8));
        }
        return out;
    }

private:
    std::vector<std::uint8_t> bits_;
};

void putLe(std::vector<std::uint8_t>& out, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

void chunk(std::vector<std::uint8_t>& file, const char* tag, const std::vector<std::uint8_t>& data) {
    file.insert(file.end(), tag, tag + 4);
    putLe(file, static_cast<std::uint32_t>(data.size() + 8), 4);
    file.insert(file.end(), data.begin(), data.end());
}

// A 16 x 16 frame chunk's data: the 16-byte header (66 ms, quantiser scale 2) and the bits.
std::vector<std::uint8_t> frame(const Bits& bits, std::uint16_t side = 16) {
    std::vector<std::uint8_t> data(16, 0);
    data[6] = 66;
    data[8] = static_cast<std::uint8_t>(side);
    data[9] = static_cast<std::uint8_t>(side >> 8);
    data[10] = static_cast<std::uint8_t>(side);
    data[11] = static_cast<std::uint8_t>(side >> 8);
    data[13] = 2;
    const auto b = bits.bytes();
    data.insert(data.end(), b.begin(), b.end());
    return data;
}

// A key frame whose blocks hold only a DC: luma `y`, chroma 0.
Bits flatKey(int y) {
    Bits b;
    for (int block = 0; block < 6; ++block) {
        b.put(static_cast<std::uint32_t>(block < 4 ? y : 0) & 0xFF, 8);
        b.put(0b10, 2);  // end of block
    }
    return b;
}

void movieReadsItsChunks() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(40)));
    chunk(file, "XXXX", {1, 2, 3});
    Bits all;
    all.put(1, 1);
    chunk(file, "MADm", frame(all));
    chunk(file, "MADe", frame(all));
    const Movie movie = read(file);
    CHECK(movie.width == 16 && movie.height == 16 && movie.frameMs == 66);
    CHECK(movie.frames.size() == 3);
    CHECK(movie.frames[0].kind == FrameKind::Key && movie.frames[1].kind == FrameKind::Predicted && movie.frames[2].kind == FrameKind::Passing);
    CHECK(movie.sound.channels == 0);
}

void movieRejectsBadFiles() {
    auto throws = [](const std::vector<std::uint8_t>& file) {
        try {
            (void)read(file);
        } catch (const std::runtime_error&) {
            return true;
        }
        return false;
    };
    CHECK(throws({}));
    std::vector<std::uint8_t> odd;
    chunk(odd, "MADk", frame(flatKey(0), 24));  // not whole macroblocks
    CHECK(throws(odd));
    std::vector<std::uint8_t> cut;
    chunk(cut, "MADk", frame(flatKey(0)));
    cut[4] = 0xFF;  // a size past the end leaves no frames
    CHECK(throws(cut));
}

// A DC of 40 is 40 * 32768 in the block, whose byte 2 is 20: the 7-bit sample 84. Chroma 0 is 64.
// The game's tables make that a grey of 169.
void keyFrameDecodesItsDc() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(40)));
    const Movie movie = read(file);
    VideoDecoder decoder;
    CHECK(decoder.decode(file, movie, movie.frames[0]));
    const auto& packed = decoder.packed();
    CHECK(packed.size() == 16 * 16 * 2);
    CHECK(packed[0] == 64 && packed[1] == 64 && packed[2] == 84 && packed[3] == 84);
    std::vector<std::uint8_t> rgba;
    decoder.rgba(rgba);
    CHECK(rgba.size() == 16 * 16 * 4);
    CHECK(rgba[0] == 169 && rgba[1] == 169 && rgba[2] == 169 && rgba[3] == 255);
    CHECK(rgba[rgba.size() - 4] == 169);
}

// "1": every block predicted. A zero vector and an add of +1 for luma ("1" and 5 bits 0) and 0 for
// chroma copy the frame before and brighten its luma by one step.
void predictedFrameCopiesAndAdds() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(40)));
    Bits b;
    b.put(1, 1);
    b.put(0, 1);  // vector x
    b.put(0, 1);  // vector y
    for (int block = 0; block < 6; ++block) {
        if (block < 4) {
            b.put(0b100000, 6);
        } else {
            b.put(0, 1);
        }
    }
    chunk(file, "MADe", frame(b));
    chunk(file, "MADm", frame(b));
    const Movie movie = read(file);
    VideoDecoder decoder;
    CHECK(decoder.decode(file, movie, movie.frames[0]));
    CHECK(decoder.decode(file, movie, movie.frames[1]));
    CHECK(decoder.packed()[2] == 85 && decoder.packed()[0] == 64);
    // The passing frame was not kept, so the next one builds on the key frame again.
    CHECK(decoder.decode(file, movie, movie.frames[2]));
    CHECK(decoder.packed()[2] == 85);
}

void fineFrameKeepsFullPrecision() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(41)));
    Bits b;
    b.put(1, 1);
    b.put(0, 1);
    b.put(0, 1);
    for (int block = 0; block < 6; ++block) {
        if (block < 4) {
            b.put(0b100000, 6);  // +1, two 8-bit steps
        } else {
            b.put(0, 1);
        }
    }
    chunk(file, "MADm", frame(b));
    const Movie movie = read(file);
    VideoDecoder decoder(Quality::Fine);
    CHECK(decoder.decode(file, movie, movie.frames[0]));
    // A DC of 41 is the 8-bit sample 41 + 128, where the game's 7 bits keep only 168.
    CHECK(decoder.packed().size() == 16 * 16 * 3 / 2 && decoder.packed()[0] == 169 && decoder.packed()[16 * 16] == 128);
    std::vector<std::uint8_t> rgba;
    decoder.rgba(rgba);
    CHECK(rgba.size() == 16 * 16 * 4 && rgba[0] == 169 && rgba[1] == 169 && rgba[2] == 169 && rgba[3] == 255);
    CHECK(decoder.decode(file, movie, movie.frames[1]));
    CHECK(decoder.packed()[0] == 171);
}

void brokenFrameStopsSafely() {
    std::vector<std::uint8_t> file;
    Bits b;
    b.put(0, 8);
    for (int i = 0; i < 70; ++i) {
        b.put(0b011, 3);  // run 1, level 1: past the 64th coefficient
        b.put(0, 1);
    }
    chunk(file, "MADk", frame(b));
    const Movie movie = read(file);
    VideoDecoder decoder;
    CHECK(!decoder.decode(file, movie, movie.frames[0]));
    // Nothing but zeros: no code the table knows.
    std::vector<std::uint8_t> zeros;
    chunk(zeros, "MADk", frame(Bits{}));
    const Movie empty = read(zeros);
    CHECK(!decoder.decode(zeros, empty, empty.frames[0]));
}

std::vector<std::uint8_t> soundHeader(int channels) {
    return {'P', 'T', 0, 0, 0x06, 0x01, 0x65, 0xFD, 0x80, 0x01, 0x01, 0x82, 0x01, static_cast<std::uint8_t>(channels), 0xFF};
}

// One mono block: 4 bytes of starting samples the game skips, then one frame. Predictor 1 (240, 0)
// with shift 12: the first nibble 1 gives (1 << 28 >> 20) >> 8 = 1, and each later 0 gives
// previous * 240 >> 8.
std::vector<std::uint8_t> soundBlock(std::uint8_t header, std::uint8_t firstByte) {
    std::vector<std::uint8_t> data;
    putLe(data, 28, 4);
    putLe(data, 0, 4);
    putLe(data, 0x7FFF7FFF, 4);
    data.push_back(header);
    data.push_back(firstByte);
    data.insert(data.end(), 13, 0);
    return data;
}

void movieSoundDecodesAndCarriesHistory() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(0)));
    chunk(file, "SCHl", soundHeader(1));
    chunk(file, "SCDl", soundBlock(0x1C, 0x70));  // 7 << 28 >> 20 = 1792, >> 8 = 7
    chunk(file, "SCDl", soundBlock(0x1C, 0x00));
    const Movie movie = read(file);
    CHECK(movie.sound.channels == 1 && movie.sound.sampleRate == 22050);
    const auto pcm = ts2::audio::decodeMovieSound(file, movie.sound);
    CHECK(pcm.samples.size() == 56);
    CHECK(pcm.samples[0] == 7);
    CHECK(pcm.samples[1] == 6);  // 7 * 240 >> 8
    // The second block goes on from the first block's last samples, not from its own start.
    CHECK(pcm.samples[28] == (pcm.samples[27] * 240 >> 8));
}

void movieSoundStopsAtABrokenFrame() {
    std::vector<std::uint8_t> file;
    chunk(file, "MADk", frame(flatKey(0)));
    chunk(file, "SCHl", soundHeader(1));
    chunk(file, "SCDl", soundBlock(0x1C, 0x10));
    chunk(file, "SCDl", soundBlock(0x5C, 0x10));  // predictor 5: the game has four
    const Movie movie = read(file);
    CHECK(ts2::audio::decodeMovieSound(file, movie.sound).samples.size() == 28);
}

void broadcastIniListsItsStations() {
    const auto entries = ts2::content::parseBroadcastIni(
        "[Broadcasts]\r\n0=tvstation_sbn\r\n1=vgs_ssx_attract\r\n2=missing\r\n\r\n[tvstation_sbn]\r\nFolder=Broadcast/TvSBN\r\n"
        "CommercialInterval=60\r\nCommercialFolder=Broadcast/Commercials\r\n\r\n[vgs_ssx_attract]\r\nIsGames=1\r\nFolder=Games/ssx\r\n"
        "File=ssx_Attract.movie\r\n[unlisted]\r\nFolder=x\r\n");
    CHECK(entries.size() == 2);
    CHECK(entries[0].name == "tvstation_sbn" && entries[0].folder == "Broadcast/TvSBN" && entries[0].commercialFolder == "Broadcast/Commercials");
    CHECK(entries[1].isGames && entries[1].file == "ssx_Attract.movie");
    const auto b = ts2::content::Broadcasts::make(
        entries, {{"tvsbn", {"a/TvSBN/one.movie", "b/TvSBN/two.movie"}}, {"commercials", {"c.movie"}}, {"ssx", {"x/SSX/ssx_ATTRACT.movie", "x/SSX/other.movie"}}});
    const auto* sbn = b.find("TVSTATION_SBN");
    CHECK(sbn != nullptr && sbn->shows.size() == 2 && sbn->commercials.size() == 1);
    const auto* ssx = b.find("vgs_ssx_attract");
    CHECK(ssx != nullptr && ssx->shows.size() == 1 && ssx->shows[0].filename() == "ssx_ATTRACT.movie" && ssx->commercials.empty());
    CHECK(b.find("missing") == nullptr);
}

void tvLinksTellScriptsHowTheMovieGoes() {
    ts2::sim::TvLinks links;
    links.setLookup([](std::string_view name) -> std::optional<ts2::sim::TvLinks::Station> {
        if (name == "vgs_ssx_attract") {
            return ts2::sim::TvLinks::Station{};
        }
        if (name == "tvmovie_fetch") {
            return ts2::sim::TvLinks::Station{true};
        }
        return std::nullopt;
    });
    ts2::sim::SimObject tv;
    tv.id = 5;
    links.tune(tv, "TVScreenAnimations_screen_screenoff");
    CHECK(links.links().empty() && tv.attributes[11] == 0);
    links.tune(tv, "VGS_SSX_Attract");
    CHECK(tv.attributes[11] == 1 && links.links().at(5).station == "vgs_ssx_attract");
    links.end("vgs_ssx_attract", [&](std::int16_t id) { return id == 5 ? &tv : nullptr; });
    CHECK(tv.attributes[11] == 2 && links.links().empty());
    links.tune(tv, "tvmovie_fetch");
    links.tune(tv, "tvstation_none");
    CHECK(tv.attributes[11] == -1 && links.links().empty());
}

} // namespace

std::vector<Test> movieTests() {
    return {
        {"movie reads its chunks", movieReadsItsChunks},
        {"movie rejects bad files", movieRejectsBadFiles},
        {"movie key frame decodes its DC to the game's grey", keyFrameDecodesItsDc},
        {"movie predicted frame copies and adds, passing frames are not kept", predictedFrameCopiesAndAdds},
        {"movie fine frame keeps full precision", fineFrameKeepsFullPrecision},
        {"movie broken frame stops safely", brokenFrameStopsSafely},
        {"movie sound decodes and carries its history", movieSoundDecodesAndCarriesHistory},
        {"movie sound stops at a broken frame", movieSoundStopsAtABrokenFrame},
        {"broadcast INI lists its stations", broadcastIniListsItsStations},
        {"TV links tell scripts how the movie goes", tvLinksTellScriptsHowTheMovieGoes},
    };
}
