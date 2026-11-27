#pragma once

#include "lot_life.hpp"

#include "scene/pick.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace app {

// Where a ray from `eye` along `ray` first meets the ground of `lot`, as a distance in units of `ray`,
// none when it misses the lot. Ground with no surface (over a pool) is passed through unless `poolTops`
// is set, which stops the ray at the ground's height there.
std::optional<float> groundHit(const ts2::scene::Scene& scene, const ts2::content::Lot& lot, const std::array<float, 3>& eye,
                               const std::array<float, 3>& ray, bool poolTops = false);

// Buy mode's hand. A click lifts an object, which then follows the mouse from tile centre to tile
// centre. "," and "." or a drag turn it, and the next click or letting go puts it down. Over a
// refused spot it follows the mouse unsnapped, drawn red. A catalog item comes in as a new object
// and is bought as it is put down. Not done yet: slots and wall objects.
class BuyHand {
public:
    struct FloorPoint {
        std::array<float, 3> point{};  // in the scene
        int storey = 1;
    };
    // The floor point under the mouse as the game's hand finds it: the first floor, storey grid or
    // ground the ray from the camera hits among the shown instances, passing through anything else
    // (objects, walls, roofs and pool water), so over empty air upstairs it is the ground below. With
    // `lot`, the ground is found by groundHit, `poolTops` as there.
    [[nodiscard]] static std::optional<FloorPoint> floorPoint(const ts2::scene::Scene& scene, std::span<const std::uint8_t> shown,
                                                              const ts2::scene::Picker& picker, const std::array<float, 3>& eye,
                                                              const std::array<float, 3>& ray, int groundLevel,
                                                              const ts2::content::Lot* lot = nullptr, bool poolTops = false);

    bool carrying() const { return carried_ != 0; }
    std::uint32_t carried() const { return carried_; }
    // Whether the held object is a catalog item not bought yet.
    bool buying() const { return buying_ != 0; }
    // Whether the spot under the mouse failed the placement test last time it moved.
    bool badSpot() const { return bad_; }

    // What a click did: nothing, a purchase for `cost`, no purchase for lack of funds, a refusal, a
    // lift or a put-down.
    struct Paid {
        enum class What { Nothing, Bought, CantAfford, Refused, Lifted, PutDown } what = What::Nothing;
        std::int32_t cost = 0;
        // A refusal's placement error code, the object its message is about and, for a put-down, the spot.
        int refusal = 0;
        std::uint32_t refusedAt = 0;
        std::optional<ts2::sim::LotSimulation::PlaceRefusal> spot;
        std::uint32_t sound = 0;  // the UI sound of a put-down
    };
    // A left click on the lot with `object` under the mouse (0 for none): lifts it when nothing is
    // held and the hand can lift it, else puts the held one down where it stands, if it can stand there.
    // A catalog item is bought as it is put down, when the household can pay its price.
    Paid click(LotLife& lot, std::uint32_t object);
    // A catalog item picked: the hand drops what it holds and takes a new object of type `guid`.
    // `colours` are the catalog's colour choices for it, which each copy bought gets.
    void buy(LotLife& lot, std::uint32_t guid, std::vector<ts2::content::RecolourChoice> colours = {});
    // Swaps the held catalog copy for one in `colours`, where it was.
    void recolour(LotLife& lot, std::vector<ts2::content::RecolourChoice> colours);
    // Puts the held object back where it was lifted from, or drops a catalog item not bought.
    void cancel(LotLife& lot);
    // Delete, or a click on the held object's panel: sells the held object where the game allows it.
    // Returns what it sold for, none when it wasn't sold.
    std::optional<std::int32_t> sell(LotLife& lot);
    // The sledgehammer on `object`: sells all of it with undo, as the hand does. Returns what it sold
    // for, none when it wasn't sold. What else stops a smash in the game is not known yet.
    std::optional<std::int32_t> smash(LotLife& lot, std::uint32_t object);
    // The mouse moved over `at`: the held object goes to the middle of that tile, or to `at` itself
    // where it can't stand.
    void moved(LotLife& lot, const FloorPoint& at);
    // "," (+1, clockwise) or "." (-1): turns the held object a quarter where it stands.
    void turn(LotLife& lot, int way);
    // Turns the held object to `facing` (object data 1) where it stands, as dragging with the button
    // held does.
    void face(LotLife& lot, std::int16_t facing);
    // The drag turn's facing: the game takes the quarter of the screen the mouse was dragged toward
    // from where the button went down (more than 16 pixels across and down together, else none), and
    // the held object faces that way on screen. `front` gives a facing's front as a screen direction.
    // Also gives the quarter, 0 to 3 going anticlockwise from up and right, for the drag cursor.
    struct DragTurn {
        std::int16_t facing = 0;
        int quarter = 0;
    };
    [[nodiscard]] static std::optional<DragTurn> dragTurn(int dx, int dy, const std::function<std::array<float, 2>(std::int16_t)>& front);
    // Ctrl+Z or the undo arrow takes back the last change, money included, and Ctrl+Y or the redo arrow
    // does it again. Each returns whether it did anything. Neither works while the hand holds an object
    // it lifted (what the game does then is not known yet).
    bool undo(LotLife& lot);
    // Notes a Design tool change to `object`, from colours `before` to `after`, for `cost`.
    void recordRecolour(std::uint32_t object, std::vector<ts2::content::RecolourChoice> before,
                        std::vector<ts2::content::RecolourChoice> after, std::int32_t cost);
    bool redo(LotLife& lot);
    // Notes walls built for `cost`. Undo calls `walls` with false and refunds, redo calls it with true
    // and pays again.
    void recordWalls(std::uint32_t id, std::int32_t cost);
    void setWalls(std::function<void(std::uint32_t id, bool in)> walls) { walls_ = std::move(walls); }
    // Notes floors laid or taken up for `cost` (negative for a refund), undone and redone as walls are.
    void recordFloors(std::uint32_t id, std::int32_t cost);
    void setFloors(std::function<void(std::uint32_t id, bool in)> floors) { floors_ = std::move(floors); }
    // Notes a foundation, deck or stage tile taken out, for `cost`, undone and redone as walls are.
    void recordPlatform(std::uint32_t id, std::int32_t cost);
    void setPlatforms(std::function<void(std::uint32_t id, bool in)> platforms) { platforms_ = std::move(platforms); }
    // Notes wall sides painted for `cost`, undone and redone as walls are.
    void recordPaint(std::uint32_t id, std::int32_t cost);
    void setPaint(std::function<void(std::uint32_t id, bool in)> paint) { paint_ = std::move(paint); }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    // Leaving Buy mode ends the history, and what was sold is deleted for good.
    void clearHistory(LotLife& lot);
    // Whether the hand works for Build mode, where every catalog item it buys gives it another.
    void setInBuild(bool build) { inBuild_ = build; }
    // Ctrl `down` while the hand holds a door or window from the catalog puts it away, and letting go
    // brings a new copy back. Meanwhile a click deletes a door or window instead.
    void stash(LotLife& lot, bool down);
    bool stashed() const { return stashed_; }

private:
    struct Change {
        enum class Kind { Move, Sale, Purchase, Recolour, Walls, Floors, Platform, Paint, Swap } kind = Kind::Move;
        std::uint32_t object = 0;  // for walls, floors, platforms and paint, the id they were recorded with
        LotLife::HandSpot from, to;
        std::int32_t money = 0;  // what a sale brought, or what the change cost
        std::vector<ts2::content::RecolourChoice> before, after;  // a recolour's colours
        std::uint32_t replaced = 0;  // a swap's lifted object, put aside for its other version
    };
    void setBad(LotLife& lot, std::optional<ts2::sim::LotSimulation::PlaceRefusal> refusal);
    // Tests the held object facing `facing` at the tile under the mouse and moves it there, snapped
    // when it fits. With `turnToWalls`, an object hung on walls first turns to the first facing that
    // fits from `facing` on, as it does when carried onto a new tile and after each turn.
    void retest(LotLife& lot, std::int16_t facing, bool turnToWalls = false);
    void record(const Change& change);
    void release(LotLife& lot);
    void takeNew(LotLife& lot, std::optional<LotLife::HandSpot> at);
    // Brings back the lifted object put aside for its other version and drops the new one.
    void unswap(LotLife& lot);

    std::uint32_t carried_ = 0;
    std::uint32_t buying_ = 0;  // the catalog item's GUID while the held object isn't bought
    std::vector<ts2::content::RecolourChoice> colours_;  // the catalog item's colour choices
    std::optional<LotLife::HandSpot> from_;  // where the held object was lifted from
    // The lifted object, put aside where it stood while the hand holds its other version instead.
    std::uint32_t lifted_ = 0;
    std::optional<std::array<int, 3>> lastTile_;  // the tile the mouse was last over, with its storey
    std::optional<FloorPoint> point_;  // where the mouse last was
    bool bad_ = false;
    bool inBuild_ = false;
    bool stashed_ = false;
    std::optional<LotLife::HandSpot> stashedAt_;  // where the copy put away stood
    std::optional<ts2::sim::LotSimulation::PlaceRefusal> refusal_;  // why the spot held over is refused
    std::vector<Change> undo_, redo_;
    std::function<void(std::uint32_t, bool)> walls_;
    std::function<void(std::uint32_t, bool)> floors_;
    std::function<void(std::uint32_t, bool)> platforms_;
    std::function<void(std::uint32_t, bool)> paint_;
};

} // namespace app
