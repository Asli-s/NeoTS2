#pragma once

#include "formats/tga.hpp"

#include <array>
#include <cstdint>

namespace ts2::content {

// How the game's live panels colour their meters from gradient pictures.

// The row of a gradient picture `rows` tall that a meter shows for `value` within [min, max]
// (the needs bars run -100..100 down a picture whose rows are whole bars).
int meterRow(int value, int min, int max, int rows);

// The row of the mood gradient (`rows` tall, best mood at the top) for a mood of -100..100.
int moodRow(int mood, int rows);

// A colour of a picture (RGB); black outside it.
std::array<std::uint8_t, 3> pixel(const tga::Pixels& picture, std::uint32_t x, std::uint32_t y);

// A needs bar's fill picture painted with one row of the bar gradient: every row takes that row's
// colours, and the fill keeps its own alpha (the bar's rounded shape).
tga::Pixels paintBar(const tga::Pixels& fill, const tga::Pixels& gradient, int row);

// The same for a meter that picks a column (the mood meter): every column takes the gradient
// column's colours, and the fill keeps its own alpha.
tga::Pixels paintColumns(const tga::Pixels& fill, const tga::Pixels& gradient, int column);

// What the mood meter shows: the mood, or full while the aspiration score is platinum (75 and up).
int moodMeterValue(int mood, int aspirationScore);
bool isPlatinum(int aspirationScore);

// A picture placed at (x, y) on a transparent one of `width` x `height` (the parts outside are cut).
tga::Pixels placed(const tga::Pixels& picture, std::uint32_t width, std::uint32_t height, int x, int y);

// The largest portrait the functions below work on (the game's are 256); larger ones are refused.
constexpr std::uint32_t kMaxPortraitSide = 1024;

// The part of a Sim's portrait the game shows in thumbnails: the face and shoulders.
tga::Pixels portraitCrop(const tga::Pixels& portrait);

// A portrait with premultiplied colour (as drawn over a transparent background) made straight.
tga::Pixels unpremultiplied(tga::Pixels picture);

// A portrait's soft shadow, drawn under it in thumbnails: black, its outline blurred (three box
// blurs 19 pixels wide on a 256-pixel portrait), at most 210 of 255 opaque. Empty for a broken or
// too large portrait.
tga::Pixels portraitSilhouette(const tga::Pixels& portrait);

// A picture scaled to `width` x `height`, each new pixel the average of the area it covers (its
// colour weighted by alpha, so transparent pixels add none). Pictures here have straight alpha.
tga::Pixels scaled(const tga::Pixels& picture, std::uint32_t width, std::uint32_t height);

// A picture cut to a shape of its size: its alpha times the shape's (the picture as it is when the
// sizes differ).
tga::Pixels masked(const tga::Pixels& picture, const tga::Pixels& shape);

// A picture's shape (its alpha) filled with one colour.
tga::Pixels fillShape(const tga::Pixels& shape, std::array<std::uint8_t, 3> colour);

} // namespace ts2::content
