#include "formats/avi_writer.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ts2::avi {

namespace {

void u16(std::ostream& out, std::uint16_t v) {
    const char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
    out.write(b, 2);
}
void u32(std::ostream& out, std::uint32_t v) {
    const char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    out.write(b, 4);
}
void fourcc(std::ostream& out, const char* code) { out.write(code, 4); }

// Writes `v` at `at`, then carries on where the stream was.
void patch(std::ostream& out, std::streampos at, std::uint32_t v) {
    const std::streampos here = out.tellp();
    out.seekp(at);
    u32(out, v);
    out.seekp(here);
}

std::uint32_t distance(std::streampos from, std::streampos to) { return static_cast<std::uint32_t>(to - from); }

constexpr std::uint32_t kHasIndex = 0x10, kInterleaved = 0x100, kKeyFrame = 0x10;
constexpr std::uint32_t kAvihBytes = 56, kStrhBytes = 56, kBitmapInfoBytes = 40, kWaveFormatBytes = 18;
constexpr std::uint16_t kPcm = 1, kBitsPerSample = 16;

// A stream header: its kind and handler, scale and rate (rate / scale units a second), the place its
// length goes, its suggested buffer, its sample size and frame (left, top, right, bottom).
void streamHeader(std::ostream& out, const char* kind, const char* handler, std::uint32_t scale, std::uint32_t rate, std::streampos* lengthAt,
                  std::streampos* bufferAt, std::uint32_t sampleSize, std::uint16_t width, std::uint16_t height) {
    fourcc(out, "strh");
    u32(out, kStrhBytes);
    fourcc(out, kind);
    fourcc(out, handler);
    u32(out, 0);  // flags
    u16(out, 0);  // priority
    u16(out, 0);  // language
    u32(out, 0);  // initial frames
    u32(out, scale);
    u32(out, rate);
    u32(out, 0);  // start
    *lengthAt = out.tellp();
    u32(out, 0);
    if (bufferAt != nullptr) {
        *bufferAt = out.tellp();
    }
    u32(out, 0);
    u32(out, 0xFFFFFFFF);  // quality: the default
    u32(out, sampleSize);
    u16(out, 0);
    u16(out, 0);
    u16(out, width);
    u16(out, height);
}

} // namespace

MjpegWriter::MjpegWriter(std::ostream& out, std::uint32_t width, std::uint32_t height, std::uint32_t fps, std::optional<AudioFormat> audio)
    : out_(out), width_(width), height_(height), fps_(std::max<std::uint32_t>(fps, 1)), audio_(audio) {
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        throw std::invalid_argument("avi: a video needs a size from 1 to 65535 pixels a side");
    }
    if (audio_ && (audio_->channels == 0 || audio_->channels > 8 || audio_->rate == 0 || audio_->rate > 384000)) {
        throw std::invalid_argument("avi: sound needs 1 to 8 channels and a rate up to 384000");
    }
    start_ = out_.tellp();
    fourcc(out_, "RIFF");
    u32(out_, 0);  // the file's size, at finish
    fourcc(out_, "AVI ");
    const std::uint32_t videoStrl = 4 + 8 + kStrhBytes + 8 + kBitmapInfoBytes;
    const std::uint32_t audioStrl = audio_ ? 4 + 8 + kStrhBytes + 8 + kWaveFormatBytes : 0;
    const std::uint32_t hdrl = 4 + 8 + kAvihBytes + 8 + videoStrl + (audio_ ? 8 + audioStrl : 0);
    fourcc(out_, "LIST");
    u32(out_, hdrl);
    fourcc(out_, "hdrl");
    fourcc(out_, "avih");
    u32(out_, kAvihBytes);
    u32(out_, 1000000 / fps_);  // microseconds a frame
    maxBytesAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, 0);  // padding granularity
    u32(out_, kHasIndex | (audio_ ? kInterleaved : 0));
    totalFramesAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, 0);  // initial frames
    u32(out_, audio_ ? 2 : 1);  // streams
    bufferAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, width_);
    u32(out_, height_);
    for (int i = 0; i < 4; ++i) {
        u32(out_, 0);
    }
    // The video: Motion JPEG frames, fps a second.
    fourcc(out_, "LIST");
    u32(out_, videoStrl);
    fourcc(out_, "strl");
    streamHeader(out_, "vids", "MJPG", 1, fps_, &lengthAt_, &streamBufferAt_, 0, static_cast<std::uint16_t>(width_),
                 static_cast<std::uint16_t>(height_));
    fourcc(out_, "strf");
    u32(out_, kBitmapInfoBytes);
    u32(out_, kBitmapInfoBytes);
    u32(out_, width_);
    u32(out_, height_);
    u16(out_, 1);   // planes
    u16(out_, 24);  // bits a pixel
    fourcc(out_, "MJPG");
    u32(out_, width_ * height_ * 3);
    for (int i = 0; i < 4; ++i) {
        u32(out_, 0);
    }
    // The sound: PCM, counted in sample frames (a block each).
    if (audio_) {
        const std::uint16_t block = static_cast<std::uint16_t>(audio_->channels * kBitsPerSample / 8);
        fourcc(out_, "LIST");
        u32(out_, audioStrl);
        fourcc(out_, "strl");
        streamHeader(out_, "auds", "\0\0\0\0", block, audio_->rate * block, &audioLengthAt_, nullptr, block, 0, 0);
        fourcc(out_, "strf");
        u32(out_, kWaveFormatBytes);
        u16(out_, kPcm);
        u16(out_, audio_->channels);
        u32(out_, audio_->rate);
        u32(out_, audio_->rate * block);  // bytes a second
        u16(out_, block);
        u16(out_, kBitsPerSample);
        u16(out_, 0);  // no extra bytes
    }
    moviAt_ = out_.tellp();
    fourcc(out_, "LIST");
    u32(out_, 0);  // the frames' size, at finish
    fourcc(out_, "movi");
}

void MjpegWriter::chunk(const char* id, std::span<const std::uint8_t> data) {
    if (finished_) {
        throw std::logic_error("avi: something added after the video was finished");
    }
    // The index counts from the "movi" code.
    IndexEntry entry{{id[0], id[1], id[2], id[3]}, distance(moviAt_ + std::streamoff(8), out_.tellp()), static_cast<std::uint32_t>(data.size())};
    fourcc(out_, id);
    u32(out_, entry.size);
    out_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (data.size() % 2 != 0) {
        out_.put(0);  // chunks start on even bytes
    }
    index_.push_back(entry);
    largest_ = std::max(largest_, entry.size);
}

void MjpegWriter::addFrame(std::span<const std::uint8_t> jpeg) {
    chunk("00dc", jpeg);
    ++frames_;
}

void MjpegWriter::addAudio(std::span<const std::int16_t> samples) {
    if (!audio_ || samples.empty()) {
        if (finished_) {
            throw std::logic_error("avi: something added after the video was finished");
        }
        return;
    }
    const std::size_t whole = samples.size() / audio_->channels * audio_->channels;
    // Little-endian, as the file keeps them.
    std::vector<std::uint8_t> bytes(whole * 2);
    for (std::size_t i = 0; i < whole; ++i) {
        const auto v = static_cast<std::uint16_t>(samples[i]);
        bytes[i * 2] = static_cast<std::uint8_t>(v);
        bytes[i * 2 + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    chunk("01wb", bytes);
    audioFrames_ += whole / audio_->channels;
}

void MjpegWriter::finish() {
    if (finished_) {
        return;
    }
    finished_ = true;
    const std::streampos moviEnd = out_.tellp();
    fourcc(out_, "idx1");
    u32(out_, static_cast<std::uint32_t>(index_.size() * 16));
    for (const auto& e : index_) {
        out_.write(e.id, 4);
        u32(out_, kKeyFrame);
        u32(out_, e.offset);
        u32(out_, e.size);
    }
    const std::streampos end = out_.tellp();
    patch(out_, start_ + std::streamoff(4), distance(start_ + std::streamoff(8), end));
    patch(out_, moviAt_ + std::streamoff(4), distance(moviAt_ + std::streamoff(8), moviEnd));
    patch(out_, totalFramesAt_, frames_);
    patch(out_, lengthAt_, frames_);
    if (audio_) {
        patch(out_, audioLengthAt_, static_cast<std::uint32_t>(audioFrames_));
    }
    patch(out_, bufferAt_, largest_ + 8);
    patch(out_, streamBufferAt_, largest_ + 8);
    patch(out_, maxBytesAt_, largest_ * fps_);
    out_.flush();
}

} // namespace ts2::avi
