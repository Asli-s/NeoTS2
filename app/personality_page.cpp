#include "personality_page.hpp"

#include "content/ui_meters.hpp"

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;

// Each trait bar's window, the person data it shows and the one the Sim was born with.
struct Trait {
    std::uint32_t window;
    std::size_t value, born;
};
constexpr Trait kTraits[] = {{0x1001, 7, 0x2F},   // neat
                             {0x1003, 6, 0x32},   // outgoing
                             {0x1005, 3, 0x31},   // active
                             {0x1004, 5, 0x33},   // playful
                             {0x1002, 2, 0x30}};  // nice
constexpr std::uint32_t kBornLine = 0xAA, kFill = 3;
constexpr std::uint32_t kTraitPicture = 0xAB200480, kBornPicture = 0xAB200495;
constexpr int kTraitMax = 1000, kTraitSteps = 10;

// Fitness: the bar shows 1000 - person data 0x52 a point at a time, from the bottom up. The body
// icons follow bits of person data 0x51.
constexpr std::uint32_t kFitnessBar = 0x1008, kFitnessPicture = 0xAB204015;
constexpr std::size_t kFitness = 0x52, kBody = 0x51;
constexpr std::uint32_t kTopIcon = 0x1009, kMiddleIcon = 0x100A, kBottomIcon = 0x100B;

// The zodiac sign (person data 0x46) and its pictures, Aries first.
constexpr std::uint32_t kZodiacWindow = 0x1006;
constexpr std::size_t kZodiac = 0x46;
constexpr std::uint32_t kZodiacPictures[] = {0xAB202525, 0xAB202505, 0xAB202515, 0xAB202540, 0xAB202530, 0xAB202535,
                                             0xAB202520, 0xAB202545, 0xAB202510, 0xAB202550, 0xAB202555, 0xAB202500};

int child(const ts2::ui::Screen& screen, int window, std::uint32_t id) {
    if (window < 0) {
        return -1;
    }
    for (const int c : screen.children(window)) {
        if (screen.id(c) == id) {
            return c;
        }
    }
    return -1;
}

std::int16_t at(std::span<const std::int16_t> data, std::size_t index) { return index < data.size() ? data[index] : 0; }

} // namespace

void PersonalityPage::attach(ts2::ui::Screen& screen, int page) {
    screen_ = &screen;
    page_ = page;
    for (std::size_t i = 0; i < traits_.size(); ++i) {
        const int window = screen.find(page, kTraits[i].window);
        traits_[i] = MeterBar(screen, child(screen, window, kFill), kTraitPicture, false);
        born_[i] = MeterBar(screen, child(screen, child(screen, window, kBornLine), kFill), kBornPicture, false);
    }
    fitness_ = MeterBar(screen, child(screen, screen.find(page, kFitnessBar), kFill), kFitnessPicture, true);
}

void PersonalityPage::show(std::span<const std::int16_t> personData) {
    if (screen_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < traits_.size(); ++i) {
        traits_[i].show(at(personData, kTraits[i].value), kTraitSteps);
        born_[i].show(at(personData, kTraits[i].born), kTraitSteps);
    }
    fitness_.show(kTraitMax - at(personData, kFitness), kTraitMax);
    const int body = at(personData, kBody);
    screen_->setVisible(screen_->find(page_, kTopIcon), (body & 0x10) != 0);
    screen_->setVisible(screen_->find(page_, kBottomIcon), (body & 0x01) != 0);
    screen_->setVisible(screen_->find(page_, kMiddleIcon), (body & 0x11) == 0);

    // The game works the sign out from the traits when the saved one is not 1..12, and saves it.
    // Here it is only shown.
    int sign = at(personData, kZodiac);
    if (sign < 1 || sign > 12) {
        sign = ts2::content::nearestZodiacSign(at(personData, 6), at(personData, 2), at(personData, 5), at(personData, 7), at(personData, 3));
    }
    if (sign != zodiac_) {
        const int window = screen_->find(page_, kZodiacWindow);
        if (auto picture = screen_->data().image(kImages, kZodiacPictures[sign - 1])) {
            screen_->setImage(window, std::move(*picture));
        }
        zodiac_ = sign;
    }
}

} // namespace app
