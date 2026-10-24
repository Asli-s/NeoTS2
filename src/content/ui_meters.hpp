#pragma once

#include "formats/tga.hpp"

#include <array>
#include <cstdint>
#include <deque>

namespace ts2::content {

// How the game's live panels colour their meters from gradient pictures.

// The row of a gradient picture `rows` tall that a meter shows for `value` within [min, max]
// (the needs bars run -100..100 down a picture whose rows are whole bars).
int meterRow(int value, int min, int max, int rows);

// The row of the mood gradient (`rows` tall, best mood at the top) for a mood of -100..100.
int moodRow(int mood, int rows);

// A colour of a picture (RGB), black outside it.
std::array<std::uint8_t, 3> pixel(const tga::Pixels& picture, std::uint32_t x, std::uint32_t y);

// A needs bar's fill picture painted with one row of the bar gradient: every row takes that row's
// colours, and the fill keeps its own alpha (the bar's rounded shape).
tga::Pixels paintBar(const tga::Pixels& fill, const tga::Pixels& gradient, int row);

// The same for a meter that picks a column (the mood meter, the aspiration bars): every column
// takes the gradient column, rows below the gradient keep the fill's pixels, and the fill's alpha
// then scales all four channels.
tga::Pixels paintColumns(const tga::Pixels& fill, const tga::Pixels& gradient, int column);

// How fast a need is moving, as the needs bars' arrows show it. The panel adds the need's value
// every half second. The trend keeps the last 10 values and, for each, their average (rounded
// half away from zero). The arrows are the newest average minus the oldest: none for 0, one for
// up to 2, two for up to 4, three beyond. They are negative for a falling need.
class NeedTrend {
public:
    void add(int value);
    int arrows() const;
    void clear();

private:
    static constexpr std::size_t kKept = 10;
    std::deque<int> values_, averages_;
    int sum_ = 0;
};

// What the mood meter shows: the mood, or full while the aspiration score is platinum (75 and up).
int moodMeterValue(int mood, int aspirationScore);
bool isPlatinum(int aspirationScore);

// How far a Sim is through its life stage, as the age bar counts it: the stage lasts its Age
// Duration (person data 0x9C) plus one day, and the days left (0x5B) plus one are at most that.
struct AgeProgress {
    int lived = 0;
    int length = 1;
};
AgeProgress ageProgress(std::int16_t ageDuration, std::int16_t ageDaysLeft);

// Which of the live panel's age pictures a life stage (person data 0x3A) shows: 0 toddler,
// 1 child, 2 teen, 3 adult, 4 elder, or -1 for babies, who have none.
int ageStage(std::int16_t lifeStage);

// The age display's tooltip: an entry (counted from 1) of Live text table 0x83 and the number put
// in its "%d". Entry 0 is for none (babies). Elders show their age in days: person data 0x5C
// (`daysBefore`) plus the days lived as an elder. Others show the days left of their stage, with
// its own text for the last day. PlantSim toddlers (person data 0x54 bit 0x100) become adults.
struct AgeTip {
    std::uint16_t entry = 0;
    int days = 0;
};
AgeTip ageTip(std::int16_t lifeStage, bool plantSim, std::int16_t ageDuration, std::int16_t ageDaysLeft, std::int16_t daysBefore);

// How many of a bar's `width` columns are filled for `lived` of `length`.
std::uint32_t barFill(int lived, int length, std::uint32_t width);

// The game's stepped bars (personality, fitness): the value is kept within [min, max] and cut down
// to a whole step (max / steps). The bar then fills floor(size / (max - min) x (value - min)) of
// its `size` pixels, at most size - 1.
int barPixels(int value, int min, int max, int steps, int size);

// The zodiac sign (1..12, Aries first) a personality points to, as the game works it out when a
// Sim's saved sign (person data 0x46) is not 1..12: the sign whose traits are nearest to outgoing,
// nice, playful, neat and active (person data 6, 2, 5, 7, 3, in hundredths). Of equals, the first.
int nearestZodiacSign(int outgoing, int nice, int playful, int neat, int active);

// A picture placed at (x, y) on a transparent one of `width` x `height` (the parts outside are cut).
tga::Pixels placed(const tga::Pixels& picture, std::uint32_t width, std::uint32_t height, int x, int y);

// The largest portrait the functions below work on (the game's are 256). Larger ones are refused.
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

// A picture in grey, as the game shows what can't be had: each pixel's grey is 0.3 of its red,
// 0.6 of its green and 0.1 of its blue (alpha kept).
tga::Pixels greyed(tga::Pixels picture);

// A picture's shape (its alpha) filled with one colour.
tga::Pixels fillShape(const tga::Pixels& shape, std::array<std::uint8_t, 3> colour);

} // namespace ts2::content
