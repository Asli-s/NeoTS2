#include "content/animation_lists.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ts2::content {

std::uint8_t ageAnimationList(std::uint8_t list, std::int16_t age) {
    if (list != animation_list::kAdult) {
        return list;
    }
    if (age <= 1) {
        return animation_list::kBaby;
    }
    if (age == 2) {
        return animation_list::kToddler;
    }
    if (age <= 15) {
        return animation_list::kChild;
    }
    if (age <= 18) {
        return animation_list::kTeen;
    }
    return age <= 50 ? animation_list::kAdult : animation_list::kElder;
}

std::uint16_t objectAnimationList(std::int16_t species, std::int16_t age) {
    constexpr std::uint16_t kHumanElder = 0x190, kHumanTeen = 0x191, kHumanChild = 0x192, kHumanToddler = 0x193, kHumanBaby = 0x194;
    constexpr std::int16_t kOldestYoung = 18, kOldestAdult = 50;
    switch (species) {
    case 0:
        if (age > kOldestAdult) {
            return kHumanElder;
        }
        if (age > kOldestYoung) {
            return animation_list::kObject;
        }
        return age > 15 ? kHumanTeen : age > 2 ? kHumanChild : age == 2 ? kHumanToddler : kHumanBaby;
    case 1:
        return age > kOldestAdult ? 0x19A : age > kOldestYoung ? 0x195 : 0x197;
    case 2:
        return age > kOldestAdult ? 0x19B : age > kOldestYoung ? 0x199 : 0x197;
    case 3:
        return age > kOldestAdult ? 0x19C : age > kOldestYoung ? 0x196 : 0x198;
    default:
        return animation_list::kObject;
    }
}

char socialAgeLetter(std::int16_t age) {
    if (age > 50) {
        return 'e';
    }
    if (age > 18) {
        return 'a';
    }
    if (age > 15) {
        return 't';
    }
    if (age >= 3) {
        return 'c';
    }
    return age == 2 ? 'p' : 'b';
}

std::string socialPrefix(std::int16_t actorAge, std::int16_t partnerAge) {
    return {socialAgeLetter(actorAge), '2', socialAgeLetter(partnerAge), '-'};
}

std::int16_t ageValue(std::uint32_t ageFlag) {
    switch (ageFlag) {
    case 0x20: return 0;   // baby
    case 0x01: return 2;   // toddler
    case 0x02: return 3;   // child
    case 0x04: return 16;  // teen
    case 0x10: return 51;  // elder
    default: return 19;    // adult, young adult
    }
}

std::uint8_t animationCategory(std::string_view entry) {
    namespace c = animation_category;
    const auto open = entry.rfind('(');
    if (entry.empty() || entry.back() != ')' || open == std::string_view::npos) {
        return c::kNoCategory;
    }
    const std::string_view word = entry.substr(open + 1, entry.size() - open - 2);
    static constexpr std::pair<std::string_view, std::uint8_t> kWords[] = {
        {"animation", c::kNormal}, {"carry", c::kCarry}, {"idle", c::kIdle}, {"gesture", c::kGesture}, {"reaction", c::kReaction},
        {"facial", c::kFacial}, {"facialidle", c::kFacialIdle}, {"facial idle", c::kFacialIdle}, {"receptivity", c::kReceptivity}};
    // The game compares the word ignoring case: "(Carry)" is a carry.
    const auto same = [&](std::string_view name) {
        return word.size() == name.size() && std::equal(word.begin(), word.end(), name.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    };
    for (const auto& [name, category] : kWords) {
        if (same(name)) {
            return category;
        }
    }
    return c::kUnknownCategory;
}

std::string animationName(std::string_view entry) {
    if (!entry.empty() && entry.back() == ')') {
        if (const auto open = entry.rfind('('); open != std::string_view::npos) {
            entry = entry.substr(0, open);
        }
    }
    // Some shipped entries end in a space ("a-soc-talkRespond-side-loop100-disagree-disgust ").
    const auto first = entry.find_first_not_of(' ');
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(entry.substr(first, entry.find_last_not_of(' ') - first + 1));
}

} // namespace ts2::content
