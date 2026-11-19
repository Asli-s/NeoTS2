#include "pet_panel.hpp"

#include "content/ui_meters.hpp"

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
// The sub-panels' scripts, by Sub after None: the needs and relationships are the Sims' own, the
// job and Petology are the pets'.
constexpr std::uint32_t kSubScripts[] = {0x49064905, 0x49065002, 0xFEED2006, 0xFEED2008};
// The small buttons, by Sub after None.
constexpr std::uint32_t kButtons[] = {0x0D8AB291, 0x17DEFBF5, 0x57DEFBF4, 0x47DEFBF2};
constexpr std::uint32_t kButtonHolder = 0x31D5BA3A, kMoodPanel = 0x49064801;
// Clicking the mood meter opens and closes the needs.
constexpr std::uint32_t kMoodButton = 0x49064809;
// The frame: the sub-panel's holder, its background tile and end cap, and the end cap and piece
// shown in their place with no sub-panel open.
constexpr std::uint32_t kSubHolder = 0xA94E97EA, kSubTile = 0xA94E97F5, kEndCapSub = 0xA94E97EB;
constexpr std::uint32_t kEndCap = 0xD152176B, kNoSubPiece = 0x4906480B;
// The age pictures: puppy, grown dog and elder dog, then kitten, grown cat and elder cat.
constexpr std::uint32_t kAgeWindows[] = {0xBD8A9F0B, 0xBD8A9F0E, 0xBD8A9F0F, 0xAD8A9F0B, 0xAD8A9F0E, 0xAD8A9F0F};

constexpr std::size_t kLifeStage = 0x3A, kJob = 0x39, kZodiac = 0x46, kPension = 0x7B, kSpecies = 0xBA, kPersonality = 0xC7;
// The job's groups: puppies and kittens, a career, retired and no job, with the no job texts for
// grown pets and for elders, and the performance bar and the Learned Commands.
constexpr std::uint32_t kJobYoung = 0xA1, kJobCareer = 0xA4, kJobRetired = 0xA9, kJobNone = 0xA5;
constexpr std::uint32_t kNoJobText = 0x71ECC381, kTooOldText = 0x71ECC38B, kSpectralText = 0x71ECC38C;
constexpr std::uint32_t kPerformance = 0xA7, kLearnedCommands = 0xD002;
// Petology's tab buttons and their pages: personality, interests, learned behaviors and memories.
constexpr std::uint32_t kTabButtons[] = {0xB1, 0xB2, 0xB3, 0xB4};
constexpr std::uint32_t kTabPages[] = {0x10000000, 0x20000000, 0x30000000, 0x40000000};
// The personality bars, a pip each side and one in the middle. Bar k shows person data 0xC7 bit
// 2k on the left (Genius, Hyper, Independent, Aggressive, Pigpen) and bit 2k + 1 on the right, and
// the middle pip when neither is set.
constexpr std::uint32_t kPersonalityBars = 0x1000, kLeftPip = 1, kMiddlePip = 2, kRightPip = 3;
// The zodiac picture and the button carrying its tip, the pictures by sign, Aries first, and the
// signs' names (Live text table 0x90).
constexpr std::uint32_t kZodiacWindow = 0x1006, kZodiacTip = 0x1007;
constexpr std::uint32_t kZodiacPictures[] = {0xAB202525, 0xAB202505, 0xAB202515, 0xAB202540, 0xAB202530, 0xAB202535,
                                             0xAB202520, 0xAB202545, 0xAB202510, 0xAB202550, 0xAB202555, 0xAB202500};
constexpr std::uint32_t kImages = 0x499DB772, kLiveText = 0x7F96C284, kSignNames = 0x90;

std::int16_t at(std::span<const std::int16_t> data, std::size_t index) { return index < data.size() ? data[index] : 0; }

} // namespace

void PetPanel::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    panel_ = panel;
    const int holder = screen.find(panel, kSubHolder);
    for (std::size_t i = 0; i < contents_.size(); ++i) {
        contents_[i] = screen.load(kScripts, kSubScripts[i], 0, 0, holder);
    }
    needs_.attach(screen, contents_[0], true);
    relationships_.attach(screen, contents_[1]);
    mood_.attach(screen, panel);
    age_.attach(screen, panel, kAgeWindows);
    screen.setVisible(screen.find(panel, kButtonHolder), true);
    layout();
}

bool PetPanel::clicked(int window) {
    if (screen_ == nullptr) {
        return false;
    }
    const std::uint32_t id = screen_->id(window);
    Sub picked = Sub::None;
    if (id == kMoodButton) {
        picked = Sub::Needs;
    } else {
        const int buttons = screen_->find(panel_, kButtonHolder);
        for (std::size_t i = 0; i < std::size(kButtons); ++i) {
            if (id == kButtons[i] && screen_->parent(window) == buttons) {
                picked = static_cast<Sub>(i + 1);
            }
        }
    }
    for (std::size_t i = 0; i < std::size(kTabButtons); ++i) {
        if (id == kTabButtons[i] && screen_->parent(window) == contents_[3]) {
            selectTab(static_cast<int>(i) + 1);
            return true;
        }
    }
    if (picked == Sub::None) {
        return relationships_.clicked(window);
    }
    // A button opens its sub-panel, or closes it when it is open.
    sub_ = sub_ == picked ? Sub::None : picked;
    layout();
    return true;
}

void PetPanel::selectTab(int tab) {
    auto& s = *screen_;
    for (std::size_t i = 0; i < std::size(kTabButtons); ++i) {
        const bool shown = static_cast<int>(i) + 1 == tab;
        s.setChecked(s.find(contents_[3], kTabButtons[i]), shown);
        s.setVisible(s.find(contents_[3], kTabPages[i]), shown);
    }
}

void PetPanel::setJob(std::span<const std::int16_t> personData, bool hasJob) {
    if (screen_ == nullptr) {
        return;
    }
    auto& s = *screen_;
    const int job = contents_[2];
    auto show = [&](std::uint32_t id, bool visible) { s.setVisible(s.find(job, id), visible); };
    // Only dogs and cats have an age group here.
    const std::int16_t species = at(personData, kSpecies);
    const int group = species >= 1 && species <= 3 ? ts2::content::petAgeGroup(at(personData, kLifeStage)) : -1;
    const bool young = group == 0, adult = group == 1, elder = group == 2;
    const bool pension = at(personData, kPension) > 0;
    const bool career = adult || (elder && !pension);
    show(kJobYoung, young);
    show(kJobRetired, elder && pension);
    show(kJobCareer, career && hasJob);
    show(kJobNone, career && !hasJob);
    show(kPerformance, career && hasJob);
    // Spectral cats have their own text, not shown yet as telling them apart isn't known.
    show(kNoJobText, adult);
    show(kTooOldText, elder);
    show(kSpectralText, false);
    show(kLearnedCommands, !young);
}

void PetPanel::setPetology(std::span<const std::int16_t> personData) {
    if (screen_ == nullptr) {
        return;
    }
    auto& s = *screen_;
    const int petology = contents_[3];
    const auto traits = static_cast<std::uint16_t>(at(personData, kPersonality));
    for (std::uint32_t k = 0; k < 5; ++k) {
        const int bar = s.find(petology, kPersonalityBars + k);
        const bool left = (traits >> (2 * k) & 1) != 0, right = (traits >> (2 * k + 1) & 1) != 0;
        s.setVisible(s.find(bar, kLeftPip), left);
        s.setVisible(s.find(bar, kRightPip), right);
        s.setVisible(s.find(bar, kMiddlePip), !left && !right);
    }
    // The sign is shown as saved. One outside 1..12 has no picture and no tip.
    const int sign = at(personData, kZodiac);
    if (sign != zodiac_) {
        zodiac_ = sign;
        const bool known = sign >= 1 && sign <= 12;
        const int picture = s.find(petology, kZodiacWindow);
        const auto image = known ? s.data().image(kImages, kZodiacPictures[sign - 1]) : std::nullopt;
        s.setVisible(picture, image.has_value());
        if (image) {
            s.setImage(picture, *image);
        }
        s.setTip(s.find(petology, kZodiacTip), known ? s.data().text(kLiveText, kSignNames << 16 | static_cast<std::uint32_t>(sign)) : std::string());
    }
}

void PetPanel::layout() {
    auto& s = *screen_;
    auto part = [&](std::uint32_t id) { return s.find(panel_, id); };
    const int open = sub_ == Sub::None ? -1 : static_cast<int>(sub_) - 1;
    const int content = open >= 0 ? contents_[static_cast<std::size_t>(open)] : -1;
    for (std::size_t i = 0; i < contents_.size(); ++i) {
        s.setVisible(contents_[i], static_cast<int>(i) == open);
        s.setChecked(s.find(part(kButtonHolder), kButtons[i]), static_cast<int>(i) == open);
    }
    // The mood meter shows only beside the needs or no sub-panel.
    s.setVisible(part(kMoodPanel), sub_ == Sub::None || sub_ == Sub::Needs);
    // The holder is as big as the open sub-panel, which stands on the panel's bottom.
    const int holder = part(kSubHolder);
    s.setVisible(holder, content >= 0);
    if (content >= 0) {
        s.move(content, 0, 0);
        s.resize(holder, s.width(content), s.height(content));
    }
    s.move(holder, s.x(holder), s.height(panel_) - s.height(holder));
    // With a sub-panel open, its end cap closes the holder's right edge and the tile runs up to it.
    const int capSub = part(kEndCapSub), cap = part(kEndCap), tile = part(kSubTile);
    s.setVisible(capSub, content >= 0);
    s.setVisible(tile, content >= 0);
    s.setVisible(cap, content < 0);
    s.setVisible(part(kNoSubPiece), content < 0);
    if (content >= 0) {
        s.move(capSub, s.x(holder) + s.width(holder) - s.width(capSub), s.y(capSub));
        s.resize(tile, s.x(capSub) - s.x(tile), s.height(tile));
    }
    // The panel ends where its end piece does.
    const int end = content >= 0 ? capSub : cap;
    s.resize(panel_, s.x(end) + s.width(end), s.height(panel_));
}

} // namespace app
