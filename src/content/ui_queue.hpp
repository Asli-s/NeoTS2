#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::content {

// How the live-mode queue lays out its icons: one icon (UI script 0x0609A1C0) per interaction of the
// selected Sim that has an icon, the running one first, then the waiting ones in queue order.

// Where the row goes and how big an icon is. The place comes from the constants of UI script
// 0xCCD02691. These defaults are the game's own for constants that script lacks.
struct QueueLayout {
    int x = 60, y = 10;  // kLocationX, kLocationY: the first icon's top left on the screen
    int margin = 5;      // kIconMarginX: the gap between two icons of the row
    int width = 51, height = 52;
    // Where a nested icon starts below the bottom of the icon above it (it overlaps when negative).
    int nestedOffset = 0;
};

// One icon's place on the screen and look.
struct QueueSlot {
    int x = 0, y = 0;
    bool nested = false;  // drawn with the nested frame (window 0x200), else the top-level one (0x100)
    // Joined to a nested icon below it: the top-level frame with its nest cutout, or the nested
    // frame's middle piece.
    bool joined = false;
};

// The places of icons whose interactions are nested or not (`nested`, in the queue's order).
// Top-level icons form a row from the row's place, and nested ones form a column under the first icon.
std::vector<QueueSlot> queueSlots(const std::vector<bool>& nested, const QueueLayout& layout);

// One step of an icon sliding to its new place, on one axis, `ms` milliseconds after the last: it
// moves dt x (target - place) x 0.02 / `steps`, at least `delta` pixels, and lands on the target
// once within `delta` of it or when the step would pass it. Returns the new place.
float slideStep(float place, float target, double ms, int steps, float delta);

// The value of a "kName=value" line of a script's constants table. Nullopt when there is none or it
// isn't a whole number.
std::optional<int> uiConstant(const std::vector<std::string>& lines, std::string_view name);

} // namespace ts2::content
