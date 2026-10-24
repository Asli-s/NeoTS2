#include "live_panel.hpp"

#include <initializer_list>
#include <utility>

namespace app {

namespace {

constexpr std::uint32_t kScripts = 0xA99D8A11;
constexpr std::uint32_t kWantsScript = 0x49064900, kNeedsScript = 0x49064905, kRelationshipsScript = 0x49065002;
constexpr std::uint32_t kJobScript = 0x49065003, kSimologyScript = 0x49064820, kRewardsScript = 0x49064800;

// The frame's pieces: each comes in a version for wants closed and one for wants open.
constexpr std::uint32_t kFrontCapClosed = 0xA94E97F3, kFrontCapOpen = 0xA94E97F1;
constexpr std::uint32_t kEndCapClosed = 0xA94E97F2, kEndCapClosedSub = 0xA94E97EB;
constexpr std::uint32_t kEndCapOpen = 0xA94E97E6, kEndCapOpenSub = 0xA94E97EC;
constexpr std::uint32_t kWantsClosedOverlay = 0xA94E97F7, kWantsOpenOverlay = 0xA94E97F6, kWantsPopout = 0x2E9CAC56;
constexpr std::uint32_t kWantsTile = 0xA94E97E5, kSubTile = 0xA94E97F5;
constexpr std::uint32_t kWantsHolder = 0xA94E97E7, kSubHolder = 0xA94E97EA;
constexpr std::uint32_t kMoodClosed = 0x49064801, kMoodOpen = 0x49064802;
constexpr std::uint32_t kButtonsClosed = 0xA94E97E8, kButtonsOpen = 0xA94E97F4;
constexpr std::uint32_t kAspirations = 0xCD8B8B10;
// The small buttons, in both button columns.
constexpr std::uint32_t kNeedsButton = 0x0D8AB291, kRelationshipsButton = 0x17DEFBF5, kJobButton = 0x57DEFBF4,
                        kSimologyButton = 0x47DEFBF2;
// The last slot holds the inventory (rewards) button for family Sims, or the roommate
// satisfaction button for roommates. The script shows the roommate one by default.
constexpr std::uint32_t kInventoryButton = 0x8B85BA4E, kRoommateButton = 0x8B85BADD;

// Where the frame starts its right-hand part (x within the panel), for wants closed and open.
constexpr int kClosedFrontX = 316, kClosedStartX = 343;
constexpr int kOpenFrontX = 500, kOpenStartX = 524;
constexpr int kFrameY = 136;
// A sub-panel's bottom sits on the panel's bottom, as the game places needs and Simology.
constexpr int kPanelBottom = 280;
// A sub-panel's end cap sits this far left of its content's right edge.
constexpr int kEndCapInset = 35;

// The aspiration buttons by Aspiration flag (bit 0 first): Romance, Family, Fortune, (unused),
// Popularity, Knowledge, Grow Up, Pleasure, Grilled Cheese.
constexpr std::uint32_t kAspirationButtons[] = {0xCD8B8B13, 0xCD8B8B11, 0xCD8B8B14, 0,          0xCD8B8B15,
                                                0xCD8B8B12, 0xCD8B8B16, 0xCD8B8B17, 0xCD8B8B18};

} // namespace

void LivePanel::attach(ts2::ui::Screen& screen, int panel) {
    screen_ = &screen;
    panel_ = panel;
    wantsContent_ = screen.load(kScripts, kWantsScript, 0, 0, screen.find(panel, kWantsHolder));
    wants_.attach(screen, wantsContent_);
    needsContent_ = screen.load(kScripts, kNeedsScript, 0, 0, screen.find(panel, kSubHolder));
    needs_.attach(screen, needsContent_);
    mood_.attach(screen, panel);
    aspirationBars_.attach(screen, panel);
    age_.attach(screen, panel);
    relationshipsContent_ = screen.load(kScripts, kRelationshipsScript, 0, 0, screen.find(panel, kSubHolder));
    jobContent_ = screen.load(kScripts, kJobScript, 0, 0, screen.find(panel, kSubHolder));
    simologyContent_ = screen.load(kScripts, kSimologyScript, 0, 0, screen.find(panel, kSubHolder));
    simology_.attach(screen, simologyContent_);
    rewardsContent_ = screen.load(kScripts, kRewardsScript, 0, 0, screen.find(panel, kSubHolder));
    rewards_.attach(screen, rewardsContent_);
    job_.attach(screen, jobContent_);
    relationships_.attach(screen, relationshipsContent_);
    screen.setVisible(screen.find(panel, kAspirations), true);
    screen.setVisible(screen.find(panel, kWantsPopout), false);
    // Only family Sims are selectable so far.
    for (const std::uint32_t column : {kButtonsClosed, kButtonsOpen}) {
        const int c = screen.find(panel, column);
        screen.setVisible(screen.find(c, kInventoryButton), true);
        screen.setVisible(screen.find(c, kRoommateButton), false);
    }
    layout();
}

bool LivePanel::clicked(int window) {
    if (screen_ == nullptr) {
        return false;
    }
    const std::uint32_t id = screen_->id(window);
    auto toggle = [&](Sub sub) { sub_ = sub_ == sub ? Sub::None : sub; };
    if (screen_->parent(window) == screen_->find(panel_, kAspirations)) {
        wantsOpen_ = !wantsOpen_;
    } else if (id == kNeedsButton) {
        toggle(Sub::Needs);
    } else if (id == kRelationshipsButton) {
        toggle(Sub::Relationships);
    } else if (id == kJobButton) {
        toggle(Sub::Job);
    } else if (id == kSimologyButton) {
        toggle(Sub::Simology);
    } else if (id == kInventoryButton) {
        toggle(Sub::Rewards);
    } else {
        return (wantsOpen_ && wants_.clicked(window)) || simology_.clicked(window) || job_.clicked(window) || relationships_.clicked(window) ||
               rewards_.clicked(window);
    }
    layout();
    return true;
}

void LivePanel::setAspiration(std::int16_t aspiration) {
    if (screen_ == nullptr) {
        return;
    }
    for (int bit = 0; bit < 9; ++bit) {
        if (kAspirationButtons[bit] != 0) {
            screen_->setVisible(screen_->find(panel_, kAspirationButtons[bit]), (aspiration & (1 << bit)) != 0);
        }
    }
}

void LivePanel::layout() {
    auto& s = *screen_;
    auto show = [&](std::uint32_t id, bool visible) { s.setVisible(s.find(panel_, id), visible); };
    auto place = [&](std::uint32_t id, int x, int y) { s.move(s.find(panel_, id), x, y); };

    // Wants and fears.
    const int front = wantsOpen_ ? kOpenFrontX : kClosedFrontX, start = wantsOpen_ ? kOpenStartX : kClosedStartX;
    show(kWantsClosedOverlay, !wantsOpen_);
    show(kWantsOpenOverlay, wantsOpen_);
    show(kWantsHolder, wantsOpen_);
    place(kWantsHolder, 333, 92);
    // The tile is the wants' background, from the closed front cap's place to the open one.
    show(kWantsTile, wantsOpen_);
    place(kWantsTile, kClosedFrontX, kFrameY);
    s.resize(s.find(panel_, kWantsTile), kOpenFrontX - kClosedFrontX, s.height(s.find(panel_, kWantsTile)));
    show(kFrontCapClosed, !wantsOpen_);
    show(kFrontCapOpen, wantsOpen_);
    place(wantsOpen_ ? kFrontCapOpen : kFrontCapClosed, front, kFrameY);
    for (const auto [closed, open] : {std::pair{kButtonsClosed, kButtonsOpen}, std::pair{kMoodClosed, kMoodOpen}}) {
        show(closed, !wantsOpen_);
        show(open, wantsOpen_);
        place(wantsOpen_ ? open : closed, front, kFrameY);
    }
    // The mood meter shows only beside the needs or no sub-panel.
    if (sub_ != Sub::None && sub_ != Sub::Needs) {
        show(kMoodClosed, false);
        show(kMoodOpen, false);
    }

    // The sub-panel, after the wants: its holder, tile and end cap.
    int content = -1;
    switch (sub_) {
    case Sub::None: break;
    case Sub::Needs: content = needsContent_; break;
    case Sub::Relationships: content = relationshipsContent_; break;
    case Sub::Job: content = jobContent_; break;
    case Sub::Simology: content = simologyContent_; break;
    case Sub::Rewards: content = rewardsContent_; break;
    }
    s.setVisible(needsContent_, sub_ == Sub::Needs);
    s.setVisible(simologyContent_, sub_ == Sub::Simology);
    s.setVisible(rewardsContent_, sub_ == Sub::Rewards);
    s.setVisible(jobContent_, sub_ == Sub::Job);
    job_.setExpanded(wantsOpen_);
    s.setVisible(relationshipsContent_, sub_ == Sub::Relationships);
    relationships_.setMaximized(wantsOpen_);
    simology_.setMaximized(wantsOpen_);
    rewards_.setMaximized(wantsOpen_);
    show(kSubHolder, content >= 0);
    show(kSubTile, content >= 0);
    place(kSubTile, start, kFrameY);
    if (content >= 0) {
        place(kSubHolder, start - 10, kPanelBottom - s.height(content));
    }
    const std::uint32_t endCap = wantsOpen_ ? (content >= 0 ? kEndCapOpenSub : kEndCapOpen) : (content >= 0 ? kEndCapClosedSub : kEndCapClosed);
    for (const std::uint32_t cap : {kEndCapClosed, kEndCapClosedSub, kEndCapOpen, kEndCapOpenSub}) {
        show(cap, cap == endCap);
    }
    const int endX = content >= 0 ? start - 10 + s.width(content) - kEndCapInset : start;
    place(endCap, endX, kFrameY);
    // The sub-panel's background tile runs up to its end cap.
    s.resize(s.find(panel_, kSubTile), endX - start, s.height(s.find(panel_, kSubTile)));

    // The aspiration button shows the close arrow while wants are open.
    for (const int button : s.children(s.find(panel_, kAspirations))) {
        s.setChecked(button, wantsOpen_);
    }
    // The small buttons show which sub-panel is open.
    for (const std::uint32_t column : {kButtonsClosed, kButtonsOpen}) {
        const int c = s.find(panel_, column);
        s.setChecked(s.find(c, kNeedsButton), sub_ == Sub::Needs);
        s.setChecked(s.find(c, kRelationshipsButton), sub_ == Sub::Relationships);
        s.setChecked(s.find(c, kJobButton), sub_ == Sub::Job);
        s.setChecked(s.find(c, kSimologyButton), sub_ == Sub::Simology);
        s.setChecked(s.find(c, kInventoryButton), sub_ == Sub::Rewards);
    }
}

} // namespace app
