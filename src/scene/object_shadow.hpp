#pragma once
// Which shadow the game gives a lot object with its Shadows option at High. Each object asks for a
// kind when its model is set up, the room it stands in decides between the indoor and outdoor
// choice, and the kind decides whether the model's ground and wall shadow groups (groundshadow,
// wallshadow and the four directional wall shadows) are drawn.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ts2::scene::objectshadow {

enum class Kind : std::uint8_t {
    Model,  // no shadow of its own: the model's shadow groups are drawn as the model has them
    Quad,   // the model's shadow groups
    Sun,    // a picture of the model drawn from the sun, laid on the ground under it
    Live,   // a picture redrawn every frame, as Sims get (not drawn by the engine yet)
    None,   // nothing, and the model's shadow groups are hidden
};

// What the game reads from an object to choose its shadow.
struct Facts {
    int type = 0;               // OBJD field 9, the object type
    bool drawable = false;      // its model has something to draw and isn't "nullModel"
    bool hasShadowGroups = false;
    std::int16_t data8 = 0;     // object data 8, flags
    std::int16_t data13 = 0;    // object data 0x0D, wall placement flags
    std::int16_t data68 = 0;    // object data 0x44, the shadow wanted
};

// Whether a model's mesh group is one of the six shadow groups.
bool isShadowGroup(const std::string& group);

// The kind the object asks for. Sun is only a request: resolved() turns it into what is drawn.
Kind requested(const Facts& facts);

// The kind drawn for an object standing `outdoors` (in room 0).
Kind resolved(Kind requested, bool outdoors, const Facts& facts);

// Whether the model's shadow groups are drawn.
inline bool showsShadowGroups(Kind kind) {
    return kind == Kind::Model || kind == Kind::Quad;
}

// A model drawn from the sun at one facing. Every object of that model and facing shares it.
struct Picture {
    int mesh = -1;   // the scene mesh of the model at rest
    int facing = 0;  // eighths of a turn counterclockwise
    // The model's own space, turned to its facing, to the picture's clip space: x and y -1..1 across
    // the picture (y up), z 0..1 along the sun's way. Row-major, for column vectors.
    std::array<float, 16> camera{};
    int size = 0;  // texels, square: 32, 64 or 128
    // The box around the model turned to its facing, in its own space.
    std::array<float, 3> low{}, high{};
};

// An object laying its picture on the floor and ground around it.
struct Place {
    std::size_t picture = 0;
    std::array<float, 16> camera{};   // world to the picture's clip space
    std::array<float, 4> rect{};      // where it may fall: min x, max x, min y, max y
    int level = 0;                    // the storey it falls on, as the lot's height grid counts them
};

struct Layout {
    std::vector<Picture> pictures;
    std::vector<Place> places;
};

} // namespace ts2::scene::objectshadow
