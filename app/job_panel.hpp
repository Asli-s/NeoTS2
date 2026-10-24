#pragma once

#include "formats/tga.hpp"
#include "sim/code_source.hpp"
#include "ui/screen.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace app {

// The live panel's skills and career: the Sim's seven skills, and a page for its life stage
// (toddler, school, job or unemployed, college, retirement) with a performance bar.
class JobPanel {
public:
    // `content` is the loaded skills and career script's root window.
    void attach(ts2::ui::Screen& screen, int content);

    // Handles a click on the teen's or elder's school, job or retirement buttons. Returns whether it
    // was one.
    bool clicked(int window);

    // Shows the expanded background (wants open) or the minimized one.
    void setExpanded(bool expanded);

    // Shows a Sim from its person data, the Sim globals (the day of the week) and the game's objects
    // (career and school tables and strings).
    void show(std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, ts2::sim::CodeSource& code);

private:
    // An object's constant table (BCON) and string table, by its GUID.
    struct ObjectData {
        ts2::sim::CodeSource* code = nullptr;
        const ts2::sim::ObjectType* type = nullptr;
        int value(std::uint16_t table, std::size_t index) const;
        std::string text(std::uint16_t table, std::size_t index) const;
    };

    void showSkillsPage(bool skills);
    void showSkills(std::span<const std::int16_t> personData);
    void showMarkers(std::span<const std::int16_t> personData, const ObjectData* career);
    void showJob(int page, std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, const ObjectData& career);
    void showSchool(int page, std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, const ObjectData& school);
    void showRetired(int page, std::span<const std::int16_t> personData);
    void showCollege(int page, std::span<const std::int16_t> personData);
    void showDays(int page, int mask, std::span<const std::int16_t> globals);
    void showPerformance(int value, int min, int max);
    void setText(int page, std::uint32_t id, const std::string& text);
    // `hideWithout`: the window is hidden when there is no picture (else it stays as it is).
    void showIcon(int window, const ObjectData& object, std::uint32_t instance, bool hideWithout = true);
    std::string liveText(std::uint16_t table, std::size_t index);

    struct Skill {
        int fill = -1, marker = -1;
        int pixels = -1;  // how much of the bar is shown
    };

    ts2::ui::Screen* screen_ = nullptr;
    int content_ = -1;
    std::array<Skill, 7> skills_;
    int width_ = 0, height_ = 0;  // a skill bar's fill
    int rowWidth_ = 0;            // a skill row
    ts2::tga::Pixels picture_;
    int teenMode_ = 1, elderMode_ = 3;  // teen: 1 school, 2 job. Elder: 3 retired, 4 job
    int performanceColumn_ = -1;
    ts2::tga::Pixels performanceFill_, performanceGradient_;
    // Career and school pictures by group and instance (nullopt: none), and what each icon shows.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::optional<ts2::tga::Pixels>> pictures_;
    std::map<int, std::pair<std::uint32_t, std::uint32_t>> iconShown_;
};

} // namespace app
