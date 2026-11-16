#pragma once

// Song titles. A title table lists the same rows in every language. A row's text in the default
// language, hashed, is the id of the song it names, and the text in the player's language is the title.

#include "formats/str.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>

namespace ts2::audio {

class SongTitles {
public:
    // Files each row of the table under the id its default-language text hashes to, as written and
    // in lowercase.
    void add(const str::StringTable& table);
    // The title in `language`, the default language's when the table has none. Null for an unknown id.
    [[nodiscard]] const std::string* title(std::uint32_t id, std::uint8_t language) const;
    [[nodiscard]] std::size_t size() const { return titles_.size(); }

private:
    std::unordered_map<std::uint32_t, std::map<std::uint8_t, std::string>> titles_;
};

} // namespace ts2::audio
