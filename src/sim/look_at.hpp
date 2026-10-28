#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ts2::sim {

// A head's look in radians about the look frame, the head's own at rest (forward +Z, up +Y): pitch
// (negative is up), yaw (positive toward +X) and roll.
struct LookAngles {
    float pitch = 0, yaw = 0, roll = 0;
};
// A direction in the look frame as angles, as the game takes them for a Sim's head.
LookAngles lookAngles(const std::array<float, 3>& direction);
// Clamps the angles to the head's cone: pitch -51 to 36 degrees, yaw and roll 70 and 40 either way.
// True when any had to be clamped.
bool clampLook(LookAngles& angles);

// A Look At's states, as person data 0x2B and the look_state events give them.
enum LookState : int {
    kLookNone = 9900,
    kLookTurningTo = 9901,
    kLookHolding = 9902,
    kLookTimeUp = 9903,     // its duration ran out, turning back
    kLookEndedEarly = 9904, // stopped, target lost or out of the cone, turning back
    kLookDone = 9905,
    kLookCancelled = 9906,  // replaced by a newer look, or the head can't look
};

// A straight line of weight over a look's time (ms since it started): `from` until `begin`, then
// toward `to` over `length` ms, then `to`.
struct LookEnvelope {
    float from = 0, to = 0, begin = 0, length = 0;
    float at(float ms) const;
    float end() const { return begin + length; }
};

// One Look At: its settings from the primitive, and how far it has got.
struct LookRequest {
    int id = 0;                      // the owner's, to tell its changes apart
    double startMs = 0;              // on the world's clock
    float towards = 0, away = 0;     // turn speeds, radians a second
    bool forever = false;            // held until stopped, as asked
    bool untilStopped = false;       // its hold left is unending (set from `forever` when it begins)
    float remainingMs = 0;           // of its hold, counted down only while holding
    bool noEarlyExit = false;        // keeps looking at the cone's edge when the target is outside it
    int state = kLookNone;           // kLookNone until the head first takes it
    std::optional<LookEnvelope> envelope;
    float weight = 1;                // the head turn's weight when it has no envelope
    // Its weight at `nowMs`: the head IK's share in the head's turn.
    float weightAt(double nowMs) const;
};

// Where the head is as the look updates, in the rig's space.
struct LookPose {
    std::array<float, 4> neck{0, 0, 0, 1};  // the neck's turn
    std::array<float, 3> head{};            // the head joint
    std::array<float, 3> eye{};             // head_grip's offset from the head, as the head's local place
    std::array<float, 4> headTurn{0, 0, 0, 1};  // the head's own turn under the neck
    std::array<float, 4> headRest{0, 0, 0, 1};  // the head's turn at rest
    std::array<float, 3> target{};          // the point looked at
};

// A Sim's head looking at things: turns its look toward the newest request's target at the request's
// speed, easing in and out, holds it, and turns back to straight ahead. The head turn's target and
// pole for the head IK, in the rig's space.
class LookController {
public:
    struct Change {
        int request = 0;  // its LookRequest::id
        int state = 0;
        double atMs = 0;
        // As the look_state event gives them: how far through the look it is in percent (0 for a look
        // held until stopped) and its hold left in ms (-1 until stopped).
        int percent = 0, holdMs = 0;
    };
    // Makes `request` the one the head follows from the next update. The one it follows now is
    // cancelled then. A request whose speeds aren't above 0 is left out (the primitive refuses them).
    void begin(const LookRequest& request);
    // The Look At primitive's STOP: the current look turns back at `away` radians a second.
    void stop(double nowMs, float away);
    // Advances to `nowMs`. Appends each state the current request (or a replaced one) takes.
    void update(double nowMs, const LookPose& pose, std::vector<Change>& changes);
    // The target lost (gone, or in another room): the current look turns back.
    void lose(double nowMs);
    // The request the next update follows: the newest one given, or the one followed, 0 for none.
    int following() const;
    // Whether an update has anything to do: a look to follow, or states still to tell.
    bool busy() const { return following() != 0 || !stopChanges_.empty(); }
    // The request the head follows, if any (state below kLookDone).
    const LookRequest* current() const;
    // The head IK's target and pole, set by the last update while there is a current request.
    std::array<float, 3> target() const { return target_; }
    std::array<float, 3> pole() const { return pole_; }

private:
    void setState(LookRequest& r, int state, double nowMs, std::vector<Change>& changes);

    std::optional<LookRequest> request_, incoming_;
    std::vector<Change> stopChanges_;  // told at the next update
    bool started_ = false;            // a request has been taken since the last look from idle
    double lastMs_ = 0;
    bool updated_ = false;
    std::array<float, 4> look_{0, 0, 0, 1}, start_{0, 0, 0, 1}, rest_{0, 0, 0, 1};
    std::array<float, 3> euler_{};    // the look as Euler angles (x, y, z), x the pitch
    bool insideCone_ = false;
    float estimate_ = 2147483648.0f; // ms the current look has left, as last reckoned
    float speed_ = 0;                 // radians a second
    float scaleTo_ = 1, scaleAway_ = 1;
    float ramp_ = 25, rampAngle_ = 0; // ms, radians
    std::array<float, 3> target_{}, pole_{};
};

// A Sim's look (Look At): the object looked at (-1 the camera), its point (slot | kind << 12), the
// look's state (9900 none to 9906) and its hold left in ms (-1 until stopped).
constexpr std::size_t kLookTargetPersonData = 0x29, kLookPointPersonData = 0x2A, kLookStatePersonData = 0x2B,
                      kLookHoldPersonData = 0x2C;

// What a Look At looks at, and the tree told of its states.
struct LookTarget {
    int request = 0;                 // its LookRequest::id
    std::int16_t object = -1;        // -1: the camera
    std::uint8_t pointKind = 0;      // 0 3/4 up its bounding box, 1 a targeting slot, 2 a routing slot, 3 a containment slot
    std::uint8_t slot = 0;
    bool checksRoom = true;          // ends when it and the Sim are in different rooms
    std::uint16_t eventTree = 0;     // 0: none
    std::uint32_t codeOwner = 0;
    std::int16_t stackObject = 0;
};

// The Look At primitive's operands:
//   byte 0     flags: 0x01 STOP, 0x02 no early exit, 0x04 including the spine, 0x08 turn-towards speed
//              from temp 1, 0x10 duration from temp 0, 0x20 ignoring rooms, 0x40 ignoring the cone,
//              0x80 at the camera
//   byte 1, bytes 2-3  the object looked at (owner, data)
//   byte 4, 5  turn-towards and turn-away speeds, in 2-degrees-a-second steps. Byte 15 flag 0x02 takes
//              the turn-away speed from temp 1, 0x01 from temp 2 (degrees a second)
//   bytes 6-7  how long it holds, in ms (-1: until stopped)
//   byte 8     the slot number, byte 14 the point: 0 3/4 up its bounding box, 1 targeting slot,
//              2 routing slot, 3 containment slot
//   bytes 9-10 the tree told of its states (0: none)
//   byte 15    flags: 0x04 not hurryable
struct LookAtOperands {
    std::uint8_t flags = 0, flags2 = 0;
    std::uint8_t owner = 0;
    std::uint16_t data = 0;
    std::uint8_t towards = 0, away = 0;
    std::int16_t duration = 0;
    std::uint8_t slot = 0, pointKind = 0;
    std::uint16_t eventTree = 0;
};
LookAtOperands lookAtOperands(std::span<const std::uint8_t> operands);

} // namespace ts2::sim
