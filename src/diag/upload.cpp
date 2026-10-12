#include "diag/upload.hpp"

#include <array>
#include <limits>

namespace ts2::diag {

namespace {

void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

std::array<std::uint32_t, 256> crcTable() {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        t[i] = c;
    }
    return t;
}

} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    static const auto table = crcTable();
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> zip(const std::vector<ZipFile>& files) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    std::uint64_t total = 0;
    for (const auto& f : files) {
        total += f.data.size() + f.name.size() * 2 + 128;
        if (f.data.size() >= kMax || f.name.size() > 0xFFFF) {
            return {};
        }
    }
    if (files.empty() || files.size() > 0xFFFF || total >= kMax) {
        return {};
    }
    std::vector<std::uint8_t> out, central;
    out.reserve(static_cast<std::size_t>(total));
    for (const auto& f : files) {
        const auto offset = static_cast<std::uint32_t>(out.size());
        const std::uint32_t crc = crc32(f.data.data(), f.data.size());
        const auto size = static_cast<std::uint32_t>(f.data.size());
        const auto nameSize = static_cast<std::uint32_t>(f.name.size());
        // Local file header: stored (method 0), no date, sizes known up front.
        put32(out, 0x04034B50);
        put16(out, 20);  // version needed
        put16(out, 0);   // flags
        put16(out, 0);   // stored
        put16(out, 0);   // time
        put16(out, 0x21);  // date: 1980-01-01
        put32(out, crc);
        put32(out, size);
        put32(out, size);
        put16(out, nameSize);
        put16(out, 0);
        out.insert(out.end(), f.name.begin(), f.name.end());
        out.insert(out.end(), f.data.begin(), f.data.end());
        // Its central directory entry.
        put32(central, 0x02014B50);
        put16(central, 20);  // made by
        put16(central, 20);  // needed
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, size);
        put32(central, size);
        put16(central, nameSize);
        put16(central, 0);  // extra
        put16(central, 0);  // comment
        put16(central, 0);  // disk
        put16(central, 0);  // internal attributes
        put32(central, 0);  // external attributes
        put32(central, offset);
        central.insert(central.end(), f.name.begin(), f.name.end());
    }
    const auto centralStart = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054B50);  // end of central directory
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(files.size()));
    put16(out, static_cast<std::uint32_t>(files.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, centralStart);
    put16(out, 0);
    return out;
}

std::string jsonString(std::string_view json, std::string_view key) {
    const std::string quoted = "\"" + std::string(key) + "\"";
    for (std::size_t at = json.find(quoted); at != std::string_view::npos; at = json.find(quoted, at + 1)) {
        std::size_t i = at + quoted.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
            ++i;
        }
        if (i >= json.size() || json[i] != ':') {
            continue;  // the key's text inside a value
        }
        ++i;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\r' || json[i] == '\n')) {
            ++i;
        }
        if (i >= json.size() || json[i] != '"') {
            return {};  // not a string
        }
        std::string value;
        for (++i; i < json.size() && json[i] != '"'; ++i) {
            if (json[i] != '\\' || i + 1 >= json.size()) {
                value += json[i];
                continue;
            }
            const char e = json[++i];
            if (e == 'u' && i + 4 < json.size()) {
                unsigned code = 0;
                for (int k = 1; k <= 4; ++k) {
                    const char h = json[i + k];
                    code = code * 16 + static_cast<unsigned>(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : 0);
                }
                i += 4;
                // Links only hold plain characters; anything else is left out.
                if (code < 0x80) {
                    value += static_cast<char>(code);
                }
            } else {
                value += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b' : e == 'f' ? '\f' : e;
            }
        }
        return i < json.size() ? value : std::string();
    }
    return {};
}

std::string formBody(const std::vector<std::pair<std::string, std::string>>& fields, std::string_view boundary) {
    std::string body;
    for (const auto& [name, value] : fields) {
        body += "--" + std::string(boundary) + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n" + value + "\r\n";
    }
    body += "--" + std::string(boundary) + "--\r\n";
    return body;
}

} // namespace ts2::diag
