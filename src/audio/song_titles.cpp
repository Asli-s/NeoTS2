#include "audio/song_titles.hpp"

#include "formats/hash.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace ts2::audio {

namespace {

constexpr std::uint8_t kDefaultLanguage = 1;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

void SongTitles::add(const str::StringTable& table) {
    std::map<std::uint8_t, std::vector<const str::Entry*>> byLanguage;
    for (const str::Entry& entry : table.entries) {
        byLanguage[entry.language].push_back(&entry);
    }
    const auto base = byLanguage.find(kDefaultLanguage);
    if (base == byLanguage.end()) {
        return;
    }
    for (std::size_t row = 0; row < base->second.size(); ++row) {
        const std::string& key = base->second[row]->value;
        if (key.empty()) {
            continue;
        }
        for (const std::uint32_t id : {0xFF000000u | hash::crc24(key), 0xFF000000u | hash::crc24(lower(key))}) {
            auto& titles = titles_[id];
            if (!titles.empty()) {
                continue;  // the first table to name a song keeps it
            }
            for (const auto& [language, rows] : byLanguage) {
                if (row < rows.size() && !rows[row]->value.empty()) {
                    titles[language] = rows[row]->value;
                }
            }
        }
    }
}

const std::string* SongTitles::title(std::uint32_t id, std::uint8_t language) const {
    const auto it = titles_.find(id);
    if (it == titles_.end()) {
        return nullptr;
    }
    const auto own = it->second.find(language);
    return &(own != it->second.end() ? own : it->second.find(kDefaultLanguage))->second;
}

} // namespace ts2::audio
