#include "formats/avi_writer.hpp"

#include <algorithm>
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

constexpr std::uint32_t kHasIndex = 0x10, kKeyFrame = 0x10;
constexpr std::uint32_t kAvihBytes = 56, kStrhBytes = 56, kStrfBytes = 40;

} // namespace

MjpegWriter::MjpegWriter(std::ostream& out, std::uint32_t width, std::uint32_t height, std::uint32_t fps)
    : out_(out), width_(width), height_(height), fps_(std::max<std::uint32_t>(fps, 1)) {
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        throw std::invalid_argument("avi: a video needs a size from 1 to 65535 pixels a side");
    }
    start_ = out_.tellp();
    fourcc(out_, "RIFF");
    u32(out_, 0);  // the file's size, at finish
    fourcc(out_, "AVI ");
    const std::uint32_t strl = 4 + 8 + kStrhBytes + 8 + kStrfBytes;
    const std::uint32_t hdrl = 4 + 8 + kAvihBytes + 8 + strl;
    fourcc(out_, "LIST");
    u32(out_, hdrl);
    fourcc(out_, "hdrl");
    fourcc(out_, "avih");
    u32(out_, kAvihBytes);
    u32(out_, 1000000 / fps_);  // microseconds a frame
    maxBytesAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, 0);  // padding granularity
    u32(out_, kHasIndex);
    totalFramesAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, 0);  // initial frames
    u32(out_, 1);  // streams
    bufferAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, width_);
    u32(out_, height_);
    for (int i = 0; i < 4; ++i) {
        u32(out_, 0);
    }
    fourcc(out_, "LIST");
    u32(out_, strl);
    fourcc(out_, "strl");
    fourcc(out_, "strh");
    u32(out_, kStrhBytes);
    fourcc(out_, "vids");
    fourcc(out_, "MJPG");
    u32(out_, 0);  // flags
    u16(out_, 0);  // priority
    u16(out_, 0);  // language
    u32(out_, 0);  // initial frames
    u32(out_, 1);  // scale: rate / scale frames a second
    u32(out_, fps_);
    u32(out_, 0);  // start
    lengthAt_ = out_.tellp();
    u32(out_, 0);
    streamBufferAt_ = out_.tellp();
    u32(out_, 0);
    u32(out_, 0xFFFFFFFF);  // quality: the default
    u32(out_, 0);           // sample size: frames vary
    u16(out_, 0);
    u16(out_, 0);
    u16(out_, static_cast<std::uint16_t>(width_));
    u16(out_, static_cast<std::uint16_t>(height_));
    fourcc(out_, "strf");
    u32(out_, kStrfBytes);
    u32(out_, kStrfBytes);
    u32(out_, width_);
    u32(out_, height_);
    u16(out_, 1);   // planes
    u16(out_, 24);  // bits a pixel
    fourcc(out_, "MJPG");
    u32(out_, width_ * height_ * 3);
    for (int i = 0; i < 4; ++i) {
        u32(out_, 0);
    }
    moviAt_ = out_.tellp();
    fourcc(out_, "LIST");
    u32(out_, 0);  // the frames' size, at finish
    fourcc(out_, "movi");
}

void MjpegWriter::addFrame(std::span<const std::uint8_t> jpeg) {
    if (finished_) {
        throw std::logic_error("avi: a frame added after the video was finished");
    }
    // The index counts from the "movi" code.
    const auto offset = distance(moviAt_ + std::streamoff(8), out_.tellp());
    const auto size = static_cast<std::uint32_t>(jpeg.size());
    fourcc(out_, "00dc");
    u32(out_, size);
    out_.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
    if (size % 2 != 0) {
        out_.put(0);  // chunks start on even bytes
    }
    index_.push_back({offset, size});
    largest_ = std::max(largest_, size);
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
        fourcc(out_, "00dc");
        u32(out_, kKeyFrame);
        u32(out_, e.offset);
        u32(out_, e.size);
    }
    const std::streampos end = out_.tellp();
    patch(out_, start_ + std::streamoff(4), distance(start_ + std::streamoff(8), end));
    patch(out_, moviAt_ + std::streamoff(4), distance(moviAt_ + std::streamoff(8), moviEnd));
    patch(out_, totalFramesAt_, frames());
    patch(out_, lengthAt_, frames());
    patch(out_, bufferAt_, largest_ + 8);
    patch(out_, streamBufferAt_, largest_ + 8);
    patch(out_, maxBytesAt_, largest_ * fps_);
    out_.flush();
}

} // namespace ts2::avi
