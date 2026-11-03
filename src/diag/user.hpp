#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Who the player is on this computer, which logs and crash reports don't name.
namespace ts2::diag {

struct User {
    std::string folder;               // their profile folder (UTF-8)
    std::vector<std::string> names;   // their account name and the profile folder's own name (UTF-8)
};

// This computer's user, from the environment: USERPROFILE and USERNAME on Windows, HOME and USER
// elsewhere. Only reads. Names shorter than 2 characters are left out, since they would match too
// much.
User thisUser();

// The text with the user's folder written as %USERPROFILE%, and each of their names that is a whole
// folder name in a path (right after a slash, as in D:\Sam\Games) as %USERNAME%.
std::string withoutUser(std::string_view text, const User& user);

// The same for a crash dump's bytes, in place and keeping every length, so the dump stays readable:
// each of their names right after a slash, in 8-bit or UTF-16 text, becomes underscores. Returns
// whether anything changed.
bool maskUser(std::span<std::uint8_t> bytes, const User& user);

// maskUser on one of our own dump files, rewritten in place at the same size. A file larger than
// `most` bytes is left alone. False when it can't be read or written.
bool maskUserInFile(const std::filesystem::path& file, const User& user, std::uintmax_t most);

} // namespace ts2::diag
