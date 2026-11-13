#include "audio/names.hpp"

#include "formats/hash.hpp"

#include <array>
#include <cctype>

namespace ts2::audio {

namespace {

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

struct Word {
    std::string_view word;
    int id;
};
constexpr std::array<Word, 19> kWords{{
    {"destsurfacetype", 1}, {"sourcesurfacetype", 2}, {"cleaningskill", 3}, {"cookingskill", 4},
    {"socialskill", 5}, {"repairskill", 6}, {"gardeningskill", 7}, {"musicskill", 8},
    {"creativeskill", 9}, {"literacyskill", 10}, {"physicalskill", 11}, {"logicskill", 12},
    {"objectquality", 14}, {"objectqualityeffect", 15}, {"destfootstep", 16}, {"sourcefootstep", 17},
    {"vox", 18}, {"stitcher", 19}, {"alias", 20},
}};

constexpr std::uint32_t bit(int id) {
    return std::uint32_t{1} << id;
}

// The age letter: 'n' for pets, else by age in years.
char ageLetter(const Speaker& s) {
    if (s.species != 0) {
        return 'n';
    }
    const int age = s.age > 18 && s.personData0xAF != 0 ? 18 : s.age;
    return age < 2 ? 'b' : age < 3 ? 'p' : age < 16 ? 'c' : age < 19 ? 't' : age > 50 ? 'e' : 'a';
}

// The voice letter: 'g' for ghosts, two voices only for ages 19 to 50.
char voiceLetter(const Speaker& s) {
    if ((s.ghostFlags & 1) != 0) {
        return 'g';
    }
    if (s.species != 0 || s.personData0xAF != 0) {
        return 'a';
    }
    constexpr std::array<int, 5> kUpTo{1, 2, 15, 18, 50};
    constexpr std::array<int, 6> kVoices{1, 1, 1, 1, 2, 1};
    std::size_t i = 0;
    while (i < kUpTo.size() && s.age > kUpTo[i]) {
        ++i;
    }
    // The remainder keeps the voice's sign, as the game's does.
    return static_cast<char>('a' + s.voice % kVoices[i]);
}

// The sex letter for people, the species letter for pets.
char genderLetter(const Speaker& s) {
    switch (s.species) {
    case 0: return s.gender == 0 ? 'm' : s.gender == 1 ? 'f' : '?';
    case 1: return 'd';
    case 2: return 'l';
    case 3: return 'k';
    default: return '?';
    }
}

void append(std::string& name, const std::optional<std::string>& suffix) {
    if (suffix) {
        name += '_';
        name += *suffix;
    }
}

} // namespace

SoundKey soundKey(std::string_view name) {
    const std::string low = lower(name);
    return {0xFF000000u | hash::crc24(low), hash::crc32(low)};
}

std::uint32_t modifierMask(std::string_view list) {
    std::uint32_t mask = 0;
    while (!list.empty()) {
        const std::size_t comma = list.find(',');
        const std::string word = lower(trim(list.substr(0, comma)));
        for (const Word& w : kWords) {
            if (word == w.word) {
                mask |= bit(w.id);
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return mask;
}

std::uint32_t eventMask(std::string_view name, std::uint32_t listed) {
    std::uint32_t mask = listed | bit(modifier::kAlias);
    if (name.starts_with("vox")) {
        mask |= bit(modifier::kVox);
    }
    return mask;
}

std::string voxSuffix(const Speaker& speaker) {
    const char age = ageLetter(speaker);
    if (speaker.age < 3) {
        return {age};
    }
    if (speaker.age < 16) {
        return {age, voiceLetter(speaker)};
    }
    return {age, genderLetter(speaker), voiceLetter(speaker)};
}

AliasMap aliasMap(const std::map<std::string, std::string>& aliases) {
    AliasMap out;
    for (const auto& [from, to] : aliases) {
        out.emplace(hash::crc32(lower(from)), to);
    }
    return out;
}

std::string trackName(std::string_view event, std::uint32_t mask, const NameInputs& inputs, const AliasMap& aliases) {
    std::string name(event);
    for (int id = 0; id < 32; ++id) {
        if ((mask & bit(id)) == 0) {
            continue;
        }
        switch (id) {
        case modifier::kDestSurface: append(name, inputs.destSurface); break;
        case modifier::kSourceSurface: append(name, inputs.sourceSurface); break;
        case modifier::kObjectQuality: append(name, inputs.quality); break;
        case modifier::kVox:
            if (inputs.speaker) {
                append(name, voxSuffix(*inputs.speaker));
            }
            break;
        case modifier::kAlias:
            if (const auto it = aliases.find(hash::crc32(lower(name))); it != aliases.end()) {
                name = it->second;
            }
            break;
        default: break;
        }
    }
    return name;
}

} // namespace ts2::audio
