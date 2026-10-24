#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ts2::content {

// Animate primitives name an animation by an entry of a string list in the object's code. Each object
// keeps parallel lists for the ages of Sims, the same entry meaning the same action in each: adult,
// child, toddler, teen, elder and baby. A Sim plays its own age's entry when it names an animation
// the game has, else the adult one.
namespace animation_list {
constexpr std::uint8_t kAdult = 0x81, kChild = 0x82, kSocial = 0x83, kObject = 0x86, kToddler = 0x89, kTeen = 0x8A,
                       kElder = 0x8B, kBaby = 0x91;
}

// The list a Sim of this age (person data "age": 0 baby, 2 toddler, 3-15 child, 16-18 teen,
// 19-50 adult, over 50 elder) reads for an animation from `list`: its age's list for the adult
// list, any other list as it is.
std::uint8_t ageAnimationList(std::uint8_t list, std::int16_t age);

// Social animations (list 0x83) are named for who plays them with whom: "<actor>2<partner>-" before
// the entry, each a person's age letter (e elder, a adult, t teen, c child, p toddler, b baby), as in
// "a2t-" for an adult with a teen.
char socialAgeLetter(std::int16_t age);
std::string socialPrefix(std::int16_t actorAge, std::int16_t partnerAge);

// The age value Sims of an age data age flag (SimLook::age) have in their person data.
std::int16_t ageValue(std::uint32_t ageFlag);

// The animation an entry names: the entry without its kind ("a-idle-...-breatheloop(idle)" names
// "a-idle-...-breatheloop").
std::string animationName(std::string_view entry);

// What kind of animation an entry is, from the word in its "(...)" suffix (any case), as Animate Stop's category
// stops pick them. An entry without one is kNoCategory (the game makes it Normal, or Carry when its
// clip has a target of type 2). An unknown word is kUnknownCategory.
namespace animation_category {
constexpr std::uint8_t kUnknownCategory = 0, kNoCategory = 1, kNormal = 2, kCarry = 3, kIdle = 4, kGesture = 5, kReaction = 6,
                       kFacial = 7, kFacialIdle = 8, kReceptivity = 9;
}
std::uint8_t animationCategory(std::string_view entry);

// "Idle - System": the code whose lists hold the Sims' standing idles. Entry 0x25 is each age's
// plain standing breathing loop.
constexpr std::uint32_t kIdleSystemGroup = 0x7F05CC2C;
constexpr std::uint16_t kStandingIdleEntry = 0x25;

} // namespace ts2::content
