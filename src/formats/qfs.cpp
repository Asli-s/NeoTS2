#include "formats/qfs.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace ts2::qfs {

namespace {

constexpr std::size_t kHeaderSize = 9;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("QFS: " + message);
}

} // namespace

bool isCompressed(std::span<const std::uint8_t> data) {
    return data.size() >= kHeaderSize && data[4] == 0x10 && data[5] == 0xFB;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> in) {
    if (!isCompressed(in)) {
        fail("missing 0x10FB signature");
    }

    const std::size_t outSize = static_cast<std::size_t>(in[6]) << 16
                              | static_cast<std::size_t>(in[7]) << 8
                              | static_cast<std::size_t>(in[8]);
    std::vector<std::uint8_t> out(outSize);

    std::size_t src = kHeaderSize;
    std::size_t dst = 0;

    auto need = [&](std::size_t count) {
        if (count > in.size() - src) {
            fail("compressed data ends too early");
        }
    };

    auto copyLiteral = [&](std::size_t count) {
        need(count);
        if (count > outSize - dst) {
            fail("literal run overflows output");
        }
        std::copy_n(in.begin() + src, count, out.begin() + dst);
        src += count;
        dst += count;
    };

    auto copyBackReference = [&](std::size_t offset, std::size_t count) {
        if (offset > dst) {
            fail("back-reference points before start of output");
        }
        if (count > outSize - dst) {
            fail("back-reference overflows output");
        }
        for (std::size_t i = 0; i < count; ++i, ++dst) {
            out[dst] = out[dst - offset];
        }
    };

    while (dst < outSize) {
        need(1);
        const std::uint8_t b0 = in[src++];

        std::size_t literal = 0;
        std::size_t copy = 0;
        std::size_t offset = 0;

        if (b0 < 0x80) {
            need(1);
            const std::uint8_t b1 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x1C) >> 2) + 3;
            offset = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            need(2);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            literal = (b1 & 0xC0) >> 6;
            copy = (b0 & 0x3F) + 4;
            offset = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            need(3);
            const std::uint8_t b1 = in[src++];
            const std::uint8_t b2 = in[src++];
            const std::uint8_t b3 = in[src++];
            literal = b0 & 0x03;
            copy = ((b0 & 0x0C) << 6) + b3 + 5;
            offset = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            literal = ((b0 & 0x1F) << 2) + 4;
        } else {
            copyLiteral(b0 & 0x03);
            break;
        }

        copyLiteral(literal);
        if (copy > 0) {
            copyBackReference(offset, copy);
        }
    }

    if (dst != outSize) {
        fail("stream ended after " + std::to_string(dst) + " of " +
             std::to_string(outSize) + " bytes");
    }
    return out;
}

namespace {

// The game's search: the last position of each 3-byte hash, then a chain to earlier positions with
// the same hash, within a window of 0x1FFFF bytes. Matches run to 0x404 bytes.
constexpr std::size_t kHashSize = 0x10000;
constexpr std::size_t kChainSize = 0x20000;
constexpr std::int64_t kWindow = 0x1FFFF;
constexpr std::size_t kMaxMatch = 0x404;
constexpr std::size_t kFastSearchAbove = 250000;
constexpr std::size_t kMaxLiteralRun = 0x70;

class Encoder {
public:
    Encoder(std::span<const std::uint8_t> in, bool fast) : in_(in), fast_(fast), head_(kHashSize, -1), chain_(kChainSize, -1) {}

    std::vector<std::uint8_t> run() {
        std::size_t remaining = in_.size();
        while (remaining > 0) {
            const std::size_t maxLength = std::min(kMaxMatch, remaining);
            const std::size_t hash = hashAt(pos_);
            const std::int64_t lowest = std::max<std::int64_t>(static_cast<std::int64_t>(pos_) - kWindow, 0);
            std::int64_t candidate = head_[hash];
            if (candidate < lowest) {
                takeLiteral(hash);
                --remaining;
                continue;
            }
            std::size_t bestLength = 2;
            std::size_t bestCost = 2;
            std::size_t bestOffset = 0;
            while (true) {
                const auto from = static_cast<std::size_t>(candidate);
                // The game tests the byte just past the best match first. At the end of the input
                // that byte lies past it, but then no longer match can follow anyway.
                if (pos_ + bestLength < in_.size() && in_[pos_ + bestLength] == in_[from + bestLength]) {
                    std::size_t length = 0;
                    while (length < maxLength && in_[pos_ + length] == in_[from + length]) {
                        ++length;
                    }
                    if (bestLength < length) {
                        const std::size_t offset = pos_ - 1 - from;
                        std::size_t cost = 3;
                        if (offset < 0x400 && length < 0xB) {
                            cost = 2;
                        } else if (offset > 0x3FFF || length > 0x43) {
                            cost = 4;
                        }
                        // A match is taken when it saves more bytes than the best so far. Signed,
                        // as a 3-byte match far back costs more than it saves.
                        if (static_cast<std::int64_t>(bestLength) - static_cast<std::int64_t>(bestCost) <
                            static_cast<std::int64_t>(length) - static_cast<std::int64_t>(cost)) {
                            bestLength = length;
                            bestCost = cost;
                            bestOffset = offset;
                            if (length > kMaxMatch - 1) {
                                break;
                            }
                        }
                    }
                }
                candidate = chain_[from & (kChainSize - 1)];
                if (candidate < lowest) {
                    break;
                }
            }
            if (bestLength <= bestCost) {
                takeLiteral(hash);
                --remaining;
                continue;
            }
            emitMatch(bestLength, bestCost, bestOffset);
            if (fast_) {
                remember(pos_, hash);
            } else {
                for (std::size_t i = 0; i < bestLength; ++i) {
                    remember(pos_ + i, hashAt(pos_ + i));
                }
            }
            pos_ += bestLength;
            remaining -= bestLength;
            literalStart_ = pos_;
        }
        flushLiterals();
        out_.push_back(static_cast<std::uint8_t>(0xFC + literals_));
        appendLiterals(literals_);
        return std::move(out_);
    }

private:
    // Reads past the end count as 0. Only the last two positions read there, and they can only
    // be literals, so the value does not change the output.
    std::uint8_t at(std::size_t i) const { return i < in_.size() ? in_[i] : 0; }

    std::size_t hashAt(std::size_t i) const {
        return (static_cast<std::size_t>(at(i + 1)) << 4) ^ (static_cast<std::size_t>(at(i)) << 8 | at(i + 2));
    }

    void remember(std::size_t position, std::size_t hash) {
        chain_[position & (kChainSize - 1)] = head_[hash];
        head_[hash] = static_cast<std::int64_t>(position);
    }

    void takeLiteral(std::size_t hash) {
        ++literals_;
        remember(pos_, hash);
        ++pos_;
    }

    void appendLiterals(std::size_t count) {
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(literalStart_),
                    in_.begin() + static_cast<std::ptrdiff_t>(literalStart_ + count));
        literalStart_ += count;
    }

    // Literals go out in runs that are a multiple of 4, up to 0x70 bytes. Up to 3 stay to ride on
    // the next match or the end marker.
    void flushLiterals() {
        while (literals_ > 3) {
            const std::size_t run = std::min(literals_ & ~std::size_t{3}, kMaxLiteralRun);
            literals_ -= run;
            out_.push_back(static_cast<std::uint8_t>(0xE0 + run / 4 - 1));
            appendLiterals(run);
        }
    }

    void emitMatch(std::size_t length, std::size_t cost, std::size_t offset) {
        flushLiterals();
        const auto lit = static_cast<std::uint8_t>(literals_);
        if (cost == 2) {
            out_.push_back(static_cast<std::uint8_t>(((offset >> 3) & 0xE0) + (length - 3) * 4 + lit));
            out_.push_back(static_cast<std::uint8_t>(offset));
        } else if (cost == 3) {
            out_.push_back(static_cast<std::uint8_t>(0x80 + length - 4));
            out_.push_back(static_cast<std::uint8_t>((offset >> 8) + lit * 0x40));
            out_.push_back(static_cast<std::uint8_t>(offset));
        } else {
            out_.push_back(static_cast<std::uint8_t>(0xC0 + (((length - 5) >> 6) & 0xFC) + ((offset >> 12) & 0xF0) + lit));
            out_.push_back(static_cast<std::uint8_t>(offset >> 8));
            out_.push_back(static_cast<std::uint8_t>(offset));
            out_.push_back(static_cast<std::uint8_t>(length - 5));
        }
        appendLiterals(literals_);
        literals_ = 0;
    }

    std::span<const std::uint8_t> in_;
    bool fast_;
    std::vector<std::int64_t> head_;
    std::vector<std::int64_t> chain_;
    std::vector<std::uint8_t> out_;
    std::size_t pos_ = 0;
    std::size_t literalStart_ = 0;
    std::size_t literals_ = 0;
};

} // namespace

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> data) {
    if (data.size() > kMaxInput) {
        fail("input of " + std::to_string(data.size()) + " bytes is too large to compress");
    }
    const std::vector<std::uint8_t> stream = Encoder(data, data.size() > kFastSearchAbove).run();
    const std::size_t total = 4 + 5 + stream.size();
    std::vector<std::uint8_t> out;
    out.reserve(total);
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>(total >> shift));
    }
    out.push_back(0x10);
    out.push_back(0xFB);
    out.push_back(static_cast<std::uint8_t>(data.size() >> 16));
    out.push_back(static_cast<std::uint8_t>(data.size() >> 8));
    out.push_back(static_cast<std::uint8_t>(data.size()));
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

} // namespace ts2::qfs
