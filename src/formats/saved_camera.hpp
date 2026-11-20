#pragma once

#include "formats/camera_config.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ts2::savedcamera {

// A camera's saved views (resource 0xEC44BDDC), one resource per camera, the instance the low 16
// bits of the camera name's hash. A lot keeps the Live, Build and CameraMan cameras, a
// neighborhood the Neighborhood one. The game writes 64 zero bytes, a kind (4 for the orbiting
// cameras, 1 for the free one), the slot count 7, then each slot: a byte saying whether it holds a
// view, and the view if it does. Floats are kept as their bits so they write back exactly.
//
// Slot 0 is the view the player left. The others hold the views they saved to a key.
constexpr std::uint32_t kType = 0xEC44BDDC;
constexpr std::size_t kSlots = 7;
constexpr std::uint8_t kOrbitKind = 4;
constexpr std::uint8_t kFreeKind = 1;

// The instance a camera's views are filed under: the low 16 bits of the hash of its name in lower
// case, spelled as the camera files spell it ("Live", "Build", "Neighborhood").
std::uint32_t instanceOf(std::string_view cameraName);

// A view of an orbiting camera, 42 bytes.
struct OrbitView {
    // The zoom level the camera is zooming to, counted from the first one the camera file lists.
    std::int32_t zoomStep = 0;
    std::uint8_t byte1 = 0;  // not known yet
    std::uint32_t distance = 0;  // from the camera to the point it circles
    // The orientation the camera is turning to, counted from the first one the camera file lists.
    // Once the turn settles the yaw is that angle.
    std::int32_t orientation = 0;
    std::uint8_t byte2 = 0;  // not known yet
    // The angle around the point, in radians, the camera sitting at its cosine and sine from the
    // point. The camera file gives its orientations as the same angle in degrees.
    std::uint32_t yaw = 0;
    // Where the pitch sits between the zoom level's lowest and highest, 0 to 1, as a camera file's
    // `defaultcamerapitch` gives it. The game uses 0.15 when no file sets it, which is the value a
    // view freshly centred on a Sim keeps.
    std::uint32_t pitchBlend = 0;
    // The point the camera circles, in the lot's own coordinates for a lot camera.
    std::array<std::uint32_t, 3> target{};
    // The Live camera keeps the storey shown here (the ground storey 0, a basement -1) and the
    // wall mode (0 walls down, 1 cutaway, 2 walls up, 3 walls up with the roofs), both -1 when
    // there was no lot view to take them from. Loading the lot shows them again when neither is
    // negative. Other cameras' views sometimes hold values here that look like addresses.
    std::int32_t value1 = 0;
    std::int32_t value2 = 0;
};

// A view of the free camera, 40 bytes.
struct FreeView {
    std::array<std::uint32_t, 3> position{};
    std::array<std::uint32_t, 4> rotation{};  // a quaternion
    std::uint32_t value = 0;  // not known yet
    std::int32_t value1 = 0;  // not known yet
    std::int32_t value2 = 0;  // not known yet
};

struct Record {
    std::uint8_t kind = kOrbitKind;
    std::array<std::optional<OrbitView>, kSlots> orbit;  // for kOrbitKind
    std::array<std::optional<FreeView>, kSlots> free;    // for kFreeKind
};

// Reads a record as the game writes it. Empty for another kind or slot count, a name that is not
// all zeros, a used byte other than 0 or 1, or bytes cut short or left over.
std::optional<Record> readRecord(std::span<const std::uint8_t> data);

std::vector<std::uint8_t> writeRecord(const Record& record);

// Where an orbiting camera is now, in the game's coordinates: the view to save, or the one a load
// brings back.
struct ViewState {
    std::array<float, 3> target{};
    float distance = 0;
    float zoomingTo = 0;  // the distance the zoom is easing to
    float yaw = 0;        // radians
    float pitchBlend = 0;
    std::int32_t storey = -1;    // the Live camera's value1, see OrbitView
    std::int32_t wallMode = -1;  // the Live camera's value2
};

// `loaded` with slot 0 holding the camera's own view, which is what the game saves after the player
// has moved it. The game saves the current distance, yaw and pitch, not the ones they ease to. The
// two flag bytes stay as they were read, so a record written back without the player moving the
// camera matches the game's. Returns `loaded` unchanged for a camera of another kind or one whose
// slot 0 holds no view.
Record viewRecord(const Record& loaded, const camera::Config& rig, const ViewState& state);

// The view in slot 0 as loading a lot brings it back: the distance kept between the camera file's
// nearest and farthest zoom levels, and the zoom easing to that same distance. Empty for a camera
// of another kind, one whose slot 0 holds no view, or a value that is not a finite number.
std::optional<ViewState> savedView(const Record& record, const camera::Config& rig);

// NeoTS2's lot scene has the game's lot x and y swapped. This turns a lot camera's view from one
// to the other, either way: the target's x and y trade places, and the yaw is mirrored to match,
// kept from 0 to 2 pi.
ViewState swappedAxes(ViewState state);

} // namespace ts2::savedcamera
