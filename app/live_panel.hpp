#pragma once

#include "age_meter.hpp"
#include "job_panel.hpp"
#include "aspiration_bars.hpp"
#include "mood_meter.hpp"
#include "needs_panel.hpp"
#include "relationships_panel.hpp"
#include "rewards_panel.hpp"
#include "simology_panel.hpp"
#include "wants_panel.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace app {

// The live panel's two parts that open and close on their own: wants and fears (the aspiration
// button) and the sub-panel the small buttons pick (needs, relationships, skills and career,
// Simology, or inventory and rewards). The panel's frame
// (caps, tiles, button column, mood meter) is laid out the way the game lays it out for each state.
class LivePanel {
public:
    enum class Sub { None, Needs, Relationships, Job, Simology, Rewards };

    // `panel` is the loaded live panel script's root window.
    void attach(ts2::ui::Screen& screen, int panel);

    // Handles a click on one of the panel's own buttons. Returns whether it was one.
    bool clicked(int window);

    // Shows the aspiration button for the selected Sim's Aspiration person data (one flag set).
    void setAspiration(std::int16_t aspiration);
    // Shows the selected Sim's needs (life stage and motives as whole numbers).
    void setNeeds(std::int16_t sim, std::int16_t lifeStage, const std::array<int, 16>& motives) {
        needs_.show(sim, lifeStage, motives, sub_ == Sub::Needs);
    }
    // Shows the selected Sim's wants and fears in its want and fear slots.
    void setWants(const std::vector<WantsPanel::Want>& wants, const std::vector<WantsPanel::Want>& fears, std::size_t wantSlots,
                  std::size_t fearSlots) {
        wants_.show(wants, fears, wantSlots, fearSlots);
    }
    // Shows the selected Sim's mood and aspiration score (person data 0xA0) on the mood meter.
    void setMood(int mood, int aspirationScore) { mood_.show(mood, aspirationScore); }
    // Shows the selected Sim's aspiration score (person data 0xA0), lifetime aspiration happiness
    // (0xD8) and influence (the bar's value at a level 0..4) on the bars beside the portrait.
    void setAspirationBars(int aspirationScore, int lifetimeHappiness, int influence, int influenceLevel) {
        aspirationBars_.show(aspirationScore, lifetimeHappiness, influence, influenceLevel);
    }
    // Shows the selected Sim's life stage, how far through it the Sim is and the age tooltip.
    void setAge(std::int16_t lifeStage, int lived, int length, const ts2::content::AgeTip& tip) { age_.show(lifeStage, lived, length, tip); }
    // Shows the selected Sim's Simology: its life stage and person data.
    void setSimology(std::int16_t lifeStage, std::span<const std::int16_t> personData, std::span<const std::int16_t> secretLots) {
        simology_.show(lifeStage, personData, secretLots);
    }
    // Shows the selected Sim's skills and career from its person data.
    void setJob(std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, ts2::sim::CodeSource& code) {
        job_.show(personData, globals, code);
    }
    void setBadges(std::span<const std::int16_t> personData, int badges) { job_.showBadges(personData, badges); }
    // Whether Simology's memories are open, and showing them.
    bool showsMemories() const { return sub_ == Sub::Simology && simology_.showsMemories(); }
    void setMemories(std::vector<MemoriesPage::Memory> memories) { simology_.showMemories(std::move(memories)); }
    // Shows the selected Sim's inventory and rewards.
    void setRewards(const RewardsPanel::Sim& sim) { rewards_.show(sim); }
    // The sub-panel that is open, and closing it.
    Sub sub() const { return sub_; }
    void closeSub() {
        sub_ = Sub::None;
        layout();
    }
    // Whether the relationships are open (their list is worth making).
    bool showsRelationships() const { return sub_ == Sub::Relationships; }
    // Shows the selected Sim's relationships. `species` is its person data 0xBA.
    void setRelationships(std::vector<RelationshipsPanel::Person> people, std::int16_t species) {
        relationships_.show(std::move(people), species);
    }
    // The mouse is at (x, y): a want's or memory's popup closes when it leaves the wants or memories.
    void mouseMoved(int x, int y) {
        if (wantsOpen_) {
            wants_.mouseAt(x, y);
        } else {
            wants_.hidePopup();
        }
        if (sub_ == Sub::Simology) {
            simology_.mouseMoved(x, y);
        }
    }
    // The want or fear whose icon `window` is part of, if any.
    std::optional<WantsPanel::SlotRef> wantAt(int window) const { return wantsOpen_ ? wants_.slotOf(window) : std::nullopt; }
    // Refills every want icon on the next setWants.
    void refreshWants() { wants_.refresh(); }

private:
    void layout();

    ts2::ui::Screen* screen_ = nullptr;
    int panel_ = -1;
    int wantsContent_ = -1, needsContent_ = -1, relationshipsContent_ = -1, jobContent_ = -1, simologyContent_ = -1,
        rewardsContent_ = -1;
    // The first lot opens with the wants and the needs open, as in the game. Later lots keep the
    // panel as it was left.
    bool wantsOpen_ = true;
    Sub sub_ = Sub::Needs;
    NeedsPanel needs_;
    MoodMeter mood_;
    AspirationBars aspirationBars_;
    AgeMeter age_;
    WantsPanel wants_;
    SimologyPanel simology_;
    RewardsPanel rewards_;
    JobPanel job_;
    RelationshipsPanel relationships_;
};

} // namespace app
