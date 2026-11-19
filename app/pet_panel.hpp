#pragma once

#include "age_meter.hpp"
#include "mood_meter.hpp"
#include "needs_panel.hpp"
#include "relationships_panel.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace app {

// The live panel the game shows for a selected pet (UI script 0xFEED2000): the portrait, the age
// picture by species, the mood meter and four small buttons that open the needs, relationships,
// job and Petology beside them. It has no wants and no aspiration. It opens with no sub-panel and
// remembers the last one apart from the Sims' panel.
class PetPanel {
public:
    enum class Sub { None, Needs, Relationships, Job, Petology };

    // `panel` is the loaded pet panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);

    // Handles a click on one of the panel's own buttons. Returns whether it was one.
    bool clicked(int window);

    // Shows the pet's needs: its life stage and species (person data 0x3A and 0xBA) and motives.
    void setNeeds(std::int16_t sim, std::int16_t lifeStage, std::int16_t species, const std::array<int, 16>& motives) {
        needs_.show(sim, lifeStage, species, motives, sub_ == Sub::Needs);
    }
    // Shows the pet's mood (-100..100) on the mood meter.
    void setMood(int mood) { mood_.showMood(mood); }
    // Shows the pet's age picture with `lived` of its stage's `length` days gone, and its tooltip.
    void setAge(std::int16_t species, std::int16_t lifeStage, int lived, int length, const ts2::content::AgeTip& tip) {
        age_.showStage(ts2::content::petAgeWindow(species, lifeStage), lived, length, tip);
    }
    // Shows the pet's job: the group for its age, with or without a job (`hasJob`: person data 0x39
    // names a career that exists), and the Learned Commands for grown pets.
    void setJob(std::span<const std::int16_t> personData, bool hasJob);
    // Shows the pet's Petology: its personality (person data 0xC7) and zodiac sign (0x46).
    void setPetology(std::span<const std::int16_t> personData);
    Sub sub() const { return sub_; }
    // Whether the relationships are open (their list is worth making).
    bool showsRelationships() const { return sub_ == Sub::Relationships; }
    // Shows the pet's relationships. `species` is its person data 0xBA.
    void setRelationships(std::vector<RelationshipsPanel::Person> people, std::int16_t species) {
        relationships_.show(std::move(people), species);
    }

private:
    void layout();
    void selectTab(int tab);

    ts2::ui::Screen* screen_ = nullptr;
    int panel_ = -1;
    // The sub-panels' contents, by Sub after None.
    std::array<int, 4> contents_{-1, -1, -1, -1};
    Sub sub_ = Sub::None;
    int zodiac_ = -1;  // the sign Petology shows
    NeedsPanel needs_;
    MoodMeter mood_;
    AgeMeter age_;
    RelationshipsPanel relationships_;
};

} // namespace app
