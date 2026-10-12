#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pieces of sending a crash report that aren't platform code: the zip the report travels in,
// and reading and writing the web requests' bodies. Sending itself is in app/report_upload.cpp.
namespace ts2::diag {

// BugSplat's crash database for NeoTS2.
constexpr std::string_view kReportDatabase = "NeoTS2";
constexpr std::string_view kReportApp = "NeoTS2";

struct ZipFile {
    std::string name;  // inside the zip
    std::vector<std::uint8_t> data;
};

// A zip of the files, stored as they are (not compressed). Empty when a file is too large for a
// plain zip (4 GB) or there are no files.
std::vector<std::uint8_t> zip(const std::vector<ZipFile>& files);

// The CRC-32 zips use.
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);

// The value of a string field in a JSON object (`{"url":"https:\/\/..."}`), escapes undone;
// empty when it isn't there.
std::string jsonString(std::string_view json, std::string_view key);

// A multipart/form-data body of plain fields, split by `boundary`.
std::string formBody(const std::vector<std::pair<std::string, std::string>>& fields, std::string_view boundary);

} // namespace ts2::diag
