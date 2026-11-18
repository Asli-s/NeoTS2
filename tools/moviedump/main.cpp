// Shows what a game .movie file holds and decodes it: a frame as raw RGBA (as the game draws it, or
// with --fine as NeoTS2 does), or its sound as a WAV.
#include "audio/movie_sound.hpp"
#include "formats/movie.hpp"

#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::vector<std::uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put16(std::ofstream& out, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    out.write(b, 2);
}

void put32(std::ofstream& out, std::uint32_t v) {
    put16(out, static_cast<std::uint16_t>(v));
    put16(out, static_cast<std::uint16_t>(v >> 16));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: moviedump <file.movie> [--fine] [--frame <n> <out.rgba>] [--wav <out.wav>]\n");
        return 1;
    }
    try {
        const std::vector<std::uint8_t> file = readFile(argv[1]);
        const ts2::movie::Movie movie = ts2::movie::read(file);
        int keys = 0, predicted = 0, passing = 0;
        for (const auto& f : movie.frames) {
            (f.kind == ts2::movie::FrameKind::Key ? keys : f.kind == ts2::movie::FrameKind::Predicted ? predicted : passing)++;
        }
        std::printf("%u x %u, %u ms a frame, %zu frames (%d key, %d predicted, %d passing)\n", movie.width, movie.height, movie.frameMs,
                    movie.frames.size(), keys, predicted, passing);
        std::printf("sound: %u channels, %u Hz, %u samples, %zu blocks\n", movie.sound.channels, movie.sound.sampleRate,
                    movie.sound.samples, movie.sound.blocks.size());

        bool fine = false;
        for (int i = 2; i < argc; ++i) {
            fine = fine || std::string_view(argv[i]) == "--fine";
        }
        ts2::movie::VideoDecoder decoder(fine ? ts2::movie::Quality::Fine : ts2::movie::Quality::Game);
        long wanted = -1;
        std::string out;
        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view(argv[i]) == "--frame" && i + 2 < argc) {
                wanted = std::stol(argv[i + 1]);
                out = argv[i + 2];
            }
        }
        int broken = 0;
        for (std::size_t n = 0; n < movie.frames.size(); ++n) {
            if (!decoder.decode(file, movie, movie.frames[n])) {
                ++broken;
            }
            if (static_cast<long>(n) == wanted) {
                std::vector<std::uint8_t> rgba;
                decoder.rgba(rgba);
                std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
            }
        }
        std::printf("frames that failed to decode: %d\n", broken);

        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string_view(argv[i]) != "--wav") {
                continue;
            }
            const ts2::audio::Pcm pcm = ts2::audio::decodeMovieSound(file, movie.sound);
            std::ofstream wav(argv[i + 1], std::ios::binary);
            const auto bytes = static_cast<std::uint32_t>(pcm.samples.size() * 2);
            wav.write("RIFF", 4);
            put32(wav, 36 + bytes);
            wav.write("WAVEfmt ", 8);
            put32(wav, 16);
            put16(wav, 1);
            put16(wav, pcm.channels);
            put32(wav, pcm.sampleRate);
            put32(wav, pcm.sampleRate * pcm.channels * 2);
            put16(wav, static_cast<std::uint16_t>(pcm.channels * 2));
            put16(wav, 16);
            wav.write("data", 4);
            put32(wav, bytes);
            wav.write(reinterpret_cast<const char*>(pcm.samples.data()), static_cast<std::streamsize>(bytes));
            std::printf("sound: %.2f s decoded\n", pcm.seconds());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "moviedump: %s\n", e.what());
        return 1;
    }
    return 0;
}
