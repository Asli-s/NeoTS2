#include "job_panel.hpp"

#include "content/ui_meters.hpp"
#include "content/ui_text.hpp"

#include <algorithm>
#include <optional>

namespace app {

namespace {

constexpr std::uint32_t kImages = 0x499DB772;
constexpr std::uint32_t kLiveText = 0x7F96C284;

// The skill rows (windows 0x4BDEFF01 on), the person data each shows and the career table of the
// skill each job level needs: cooking, mechanical, charisma, body, logic, creativity, cleaning.
constexpr std::uint32_t kFirstSkill = 0x4BDEFF01;
constexpr std::size_t kSkills[] = {0x0A, 0x0C, 0x0B, 0x11, 0x12, 0x0F, 0x09};
constexpr std::uint16_t kSkillNeeds[] = {0x1004, 0x1005, 0x1007, 0x1006, 0x1009, 0x1008, 0x100B};
constexpr std::uint32_t kSkillFill = 3, kSkillMarker = 5;
constexpr std::uint32_t kSkillPicture = 0xAB201945;
constexpr int kSkillMax = 1000, kSkillSteps = 10;

// The pages: toddler, child, teen, adult (and elders without a pension), elder with a pension,
// unemployed, young adult in college and graduated. Then the performance bar and the friends a
// promotion needs.
constexpr std::uint32_t kToddlerPage = 0xA1, kChildPage = 0xA2, kTeenPage = 0xA3, kAdultPage = 0xA4, kElderPage = 0xA9;
constexpr std::uint32_t kUnemployedPage = 0xA5, kCollegePage = 0xAA, kGraduatePage = 0xAB;
constexpr std::uint32_t kPerformance = 0xA7, kFriends = 0xA8;
constexpr std::uint32_t kPages[] = {kToddlerPage, kChildPage, kTeenPage, kAdultPage, kElderPage, kUnemployedPage, kCollegePage, kGraduatePage};
// The teen's parts: school without a job, school, job, and the school and job buttons.
// The elder's parts: retired without a job, retired, job, and the retired and job buttons.
constexpr std::uint32_t kTeenParts[] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5};
constexpr std::uint32_t kElderParts[] = {0xC1, 0xC2, 0xC3, 0xC4, 0xC5};

// The skills page and the talent badges page, and the buttons that pick them.
constexpr std::uint32_t kSkillsPage = 0xA6, kBadgesPage = 0xAE, kSkillsButton = 0x506E0940, kBadgesButton = 0x506E0941;

// The backgrounds: expanded (wants open) and minimized, each with its skills tab.
constexpr std::uint32_t kExpanded[] = {0x8CB609AB, 0xD06E0E91, 0x306E0EF1};
constexpr std::uint32_t kMinimized[] = {0x8CB609AC, 0xD06E0E90, 0x306E0EF0};

// A job's windows.
constexpr std::uint32_t kJobTitle = 0x77E74B47, kSalary = 0x47E74B6D, kJobHours = 0x27E74B64, kVacation = 0xEC2CFCFD;
constexpr std::uint32_t kJobIcon = 0x4C2D86DF;
// The game's thumbnail service shows a career or school as picture 1 of its object's group (2 on the
// teen's and elder's school and job buttons).
constexpr std::uint32_t kPagePicture = 1, kButtonPicture = 2, kMaxPictureSide = 256;
// School's windows.
constexpr std::uint32_t kSchoolName = 0x0DB4B9D5, kGrade = 0xEC2D3BBA, kSchoolHours = 0xAC2D3A85, kSchoolIcon = 0x0DB4B9D6;
// The week: the days (Sunday first), the marker under today, and their holder.
constexpr std::uint32_t kDays = 0x47E74B6E, kFirstDay = 0x0C1FC411, kToday = 0x0C1FC421;
// Retirement: the pension.
constexpr std::uint32_t kPension = 0x2D0A50A6;
// College: its info and the school year.
constexpr std::uint32_t kCollegeInfo = 0x2EA70169, kSchoolYear = 0x2EA70172;
constexpr std::uint32_t kCollegeUnread[] = {0x2EA70170, 0x2EA70171, 0x2EA70173, 0x2EA70174, 0x2EA70175, 0x2EA70176, 0x2EA70180};

// The performance bar's fill and gradient (a column a value).
constexpr std::uint32_t kPerformanceFill = 3, kPerformancePicture = 0xAB201875, kPerformanceGradient = 0xAB201876;

// Person data.
constexpr std::size_t kJobLevel = 0x39, kLifeStage = 0x3A, kSpecies = 0xBA, kGender = 0x41;
constexpr std::size_t kCareerLow = 0x59, kCareerHigh = 0x5A, kSchoolLow = 0x6B, kSchoolHigh = 0x6C;
constexpr std::size_t kJobPerformance = 0x3F, kGradePoints = 0x38, kVacationDays = 0x45, kPensionAmount = 0x7B;
constexpr std::size_t kOwnBusiness = 0xC3, kHourlyPay = 0xC4;
constexpr std::size_t kInCollege = 0xAF, kCollegeFlags = 0xAD, kSemester = 0xAE;
// Life stages.
constexpr std::int16_t kToddler = 2, kChild = 3, kTeen = 16, kAdult = 19, kElderAbove = 50;
// The Sim global the day marker follows (0 Monday).
constexpr std::size_t kWeekdayGlobal = 0x22;

// Career tables: pay, start hour, hours, work days (bit 0 Monday), the number of levels.
constexpr std::uint16_t kPay = 0x1000, kStart = 0x1001, kHours = 0x1002, kWorkDays = 0x101A, kLevels = 0x1019;
// Live text tables (1-based entries): job texts, vacation, school texts, college years.
constexpr std::uint16_t kJobTexts = 0x89, kVacationText = 0x88, kSchoolTexts = 0x8B, kCollegeTexts = 0xC8;
constexpr std::size_t kHoursText = 0x0C, kHoursTip = 0x0E, kSchoolHoursTip = 0x12;

std::int16_t at(std::span<const std::int16_t> data, std::size_t index) { return index < data.size() ? data[index] : 0; }

std::uint32_t guid(std::span<const std::int16_t> data, std::size_t low, std::size_t high) {
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(at(data, high))) << 16 | static_cast<std::uint16_t>(at(data, low));
}

} // namespace

int JobPanel::ObjectData::value(std::uint16_t table, std::size_t index) const {
    const auto* values = type ? code->constants(type->group, table) : nullptr;
    return values && index < values->size() ? (*values)[index] : 0;
}

std::string JobPanel::ObjectData::text(std::uint16_t table, std::size_t index) const {
    return type ? code->string(type->group, table, index).value_or("") : "";
}

void JobPanel::attach(ts2::ui::Screen& screen, int content) {
    screen_ = &screen;
    content_ = content;
    for (std::size_t i = 0; i < skills_.size(); ++i) {
        const int row = screen.find(content, kFirstSkill + static_cast<std::uint32_t>(i));
        skills_[i].fill = screen.find(row, kSkillFill);
        skills_[i].marker = screen.find(row, kSkillMarker);
        screen.setVisible(skills_[i].marker, false);
        rowWidth_ = row >= 0 ? screen.width(row) : rowWidth_;
    }
    width_ = skills_[0].fill >= 0 ? screen.width(skills_[0].fill) : 0;
    height_ = skills_[0].fill >= 0 ? screen.height(skills_[0].fill) : 0;
    picture_ = screen.data().image(kImages, kSkillPicture).value_or(ts2::tga::Pixels{});
    performanceFill_ = screen.data().image(kImages, kPerformancePicture).value_or(ts2::tga::Pixels{});
    performanceGradient_ = screen.data().image(kImages, kPerformanceGradient).value_or(ts2::tga::Pixels{});
    // The family friends a promotion needs are not worked out yet.
    screen.setVisible(screen.find(content, kFriends), false);
    showSkillsPage(true);
    for (const std::uint32_t page : {kCollegePage, kGraduatePage}) {
        for (const std::uint32_t id : kCollegeUnread) {
            screen.setVisible(screen.find(screen.find(content, page), id), false);
        }
    }
}

bool JobPanel::clicked(int window) {
    if (screen_ == nullptr) {
        return false;
    }
    const std::uint32_t id = screen_->id(window);
    const int parent = screen_->parent(window);
    if (parent == content_ && (id == kSkillsButton || id == kBadgesButton)) {
        showSkillsPage(id == kSkillsButton);
        return true;
    }
    if (parent == screen_->find(content_, kTeenPage) && (id == kTeenParts[3] || id == kTeenParts[4])) {
        teenMode_ = id == kTeenParts[3] ? 1 : 2;
        return true;
    }
    if (parent == screen_->find(content_, kElderPage) && (id == kElderParts[3] || id == kElderParts[4])) {
        elderMode_ = id == kElderParts[3] ? 3 : 4;
        return true;
    }
    return false;
}

void JobPanel::showSkillsPage(bool skills) {
    // The badges page's contents are not filled yet.
    screen_->setVisible(screen_->find(content_, kSkillsPage), skills);
    screen_->setVisible(screen_->find(content_, kBadgesPage), !skills);
    screen_->setChecked(screen_->find(content_, kSkillsButton), skills);
    screen_->setChecked(screen_->find(content_, kBadgesButton), !skills);
}

void JobPanel::setExpanded(bool expanded) {
    if (screen_ == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < std::size(kExpanded); ++i) {
        screen_->setVisible(screen_->find(content_, kExpanded[i]), expanded);
        screen_->setVisible(screen_->find(content_, kMinimized[i]), !expanded);
    }
}

void JobPanel::show(std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, ts2::sim::CodeSource& code) {
    if (screen_ == nullptr) {
        return;
    }
    auto& s = *screen_;
    const bool person = at(personData, kSpecies) == 0;
    const std::int16_t stage = at(personData, kLifeStage);
    const bool toddler = person && stage == kToddler, child = person && stage >= kChild && stage < kTeen;
    const bool teen = person && stage >= kTeen && stage < kAdult, adult = person && stage >= kAdult && stage <= kElderAbove;
    const bool elder = person && stage > kElderAbove, youngAdult = adult && at(personData, kInCollege) != 0;
    const bool pension = elder && at(personData, kPensionAmount) >= 1;
    const bool graduated = (at(personData, kCollegeFlags) & 0x40) != 0;

    ObjectData career{&code, nullptr}, school{&code, nullptr};
    if (at(personData, kJobLevel) != 0) {
        career.type = code.objectType(guid(personData, kCareerLow, kCareerHigh));
    }
    school.type = code.objectType(guid(personData, kSchoolLow, kSchoolHigh));
    const bool job = career.type != nullptr;

    // Adults and elders without a pension see the job page, or the unemployed one.
    const bool jobPage = (adult && !youngAdult) || (elder && !pension);
    std::uint32_t page = 0;
    if (toddler) page = kToddlerPage;
    else if (child) page = kChildPage;
    else if (teen) page = kTeenPage;
    else if (youngAdult) page = graduated ? kGraduatePage : kCollegePage;
    else if (jobPage) page = job ? kAdultPage : kUnemployedPage;
    else if (pension) page = kElderPage;
    for (const std::uint32_t p : kPages) {
        s.setVisible(s.find(content_, p), p == page);
    }

    // The performance bar shows a job's performance (-100..100) or the school grade (0..13).
    struct Performance {
        int value, min, max;
    };
    std::optional<Performance> performance;
    const ObjectData* markerCareer = nullptr;
    if (child) {
        showSchool(s.find(content_, kChildPage), personData, globals, school);
        performance = Performance{at(personData, kGradePoints), 0, 13};
    } else if (teen) {
        const int p = s.find(content_, kTeenPage);
        const bool atJob = job && teenMode_ == 2;
        s.setVisible(s.find(p, kTeenParts[0]), !job);
        s.setVisible(s.find(p, kTeenParts[1]), job && !atJob);
        s.setVisible(s.find(p, kTeenParts[2]), atJob);
        s.setVisible(s.find(p, kTeenParts[3]), job);
        s.setVisible(s.find(p, kTeenParts[4]), job);
        s.setChecked(s.find(p, kTeenParts[3]), !atJob);
        s.setChecked(s.find(p, kTeenParts[4]), atJob);
        if (job) {
            showIcon(s.find(p, kTeenParts[3]), school, kButtonPicture, false);
            showIcon(s.find(p, kTeenParts[4]), career, kButtonPicture, false);
        }
        if (atJob) {
            showJob(s.find(p, kTeenParts[2]), personData, globals, career);
            markerCareer = &career;
            performance = Performance{at(personData, kJobPerformance), -100, 100};
        } else {
            showSchool(s.find(p, job ? kTeenParts[1] : kTeenParts[0]), personData, globals, school);
            performance = Performance{at(personData, kGradePoints), 0, 13};
        }
    } else if (page == kAdultPage) {
        showJob(s.find(content_, kAdultPage), personData, globals, career);
        markerCareer = &career;
        performance = Performance{at(personData, kJobPerformance), -100, 100};
    } else if (page == kElderPage) {
        const int p = s.find(content_, kElderPage);
        const bool atJob = job && elderMode_ == 4;
        s.setVisible(s.find(p, kElderParts[0]), !job);
        s.setVisible(s.find(p, kElderParts[1]), job && !atJob);
        s.setVisible(s.find(p, kElderParts[2]), atJob);
        s.setVisible(s.find(p, kElderParts[3]), job);
        s.setVisible(s.find(p, kElderParts[4]), job);
        s.setChecked(s.find(p, kElderParts[3]), !atJob);
        s.setChecked(s.find(p, kElderParts[4]), atJob);
        if (job) {
            showIcon(s.find(p, kElderParts[4]), career, kButtonPicture, false);
        }
        if (atJob) {
            showJob(s.find(p, kElderParts[2]), personData, globals, career);
            markerCareer = &career;
            performance = Performance{at(personData, kJobPerformance), -100, 100};
        } else {
            showRetired(s.find(p, job ? kElderParts[1] : kElderParts[0]), personData);
        }
    } else if (youngAdult) {
        showCollege(s.find(content_, page), personData);
    }

    s.setVisible(s.find(content_, kPerformance), performance.has_value());
    if (performance) {
        showPerformance(performance->value, performance->min, performance->max);
    }
    // The game leaves the skills as they were for babies and animals.
    if (person && stage >= kToddler) {
        showSkills(personData);
        showMarkers(personData, markerCareer);
    }
}

void JobPanel::showSkills(std::span<const std::int16_t> personData) {
    for (std::size_t i = 0; i < skills_.size(); ++i) {
        Skill& skill = skills_[i];
        const int pixels = ts2::content::barPixels(at(personData, kSkills[i]), 0, kSkillMax, kSkillSteps, width_);
        if (skill.fill < 0 || pixels == skill.pixels) {
            continue;
        }
        skill.pixels = pixels;
        screen_->setVisible(skill.fill, pixels > 0);
        if (pixels > 0) {
            screen_->setImage(skill.fill, ts2::content::placed(picture_, static_cast<std::uint32_t>(pixels), static_cast<std::uint32_t>(height_), 0, 0));
            screen_->resize(skill.fill, pixels, height_);
        }
    }
}

void JobPanel::showMarkers(std::span<const std::int16_t> personData, const ObjectData* career) {
    // Each skill's marker sits on the pip the next job level needs.
    const int level = at(personData, kJobLevel);
    const bool next = career != nullptr && level < career->value(kLevels, 0);
    const int pip = rowWidth_ / kSkillSteps;
    for (std::size_t i = 0; i < skills_.size(); ++i) {
        int x = -1;
        if (next) {
            const int needed = career->value(kSkillNeeds[i], static_cast<std::size_t>(level) + 1);
            x = (needed / (kSkillMax / kSkillSteps) - 1) * pip;
        }
        const bool shown = x >= 0 && x <= rowWidth_ - pip;
        screen_->setVisible(skills_[i].marker, shown);
        if (shown) {
            screen_->move(skills_[i].marker, x, screen_->y(skills_[i].marker));
        }
    }
}

void JobPanel::showJob(int page, std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, const ObjectData& career) {
    const int level = at(personData, kJobLevel);
    const int levels = career.value(kLevels, 0);
    showIcon(screen_->find(page, kJobIcon), career, kPagePicture);
    // Titles come in pairs (title, description) a level, women's after all the men's.
    const bool female = at(personData, kGender) == 1;
    setText(page, kJobTitle, career.text(0x93, static_cast<std::size_t>(std::max(0, 2 * level - 1 + (female ? 2 * levels : 0)))));
    const int start = career.value(kStart, static_cast<std::size_t>(level));
    const int hours = career.value(kHours, static_cast<std::size_t>(level));
    const bool ownBusiness = at(personData, kOwnBusiness) != 0;
    const int pay = ownBusiness ? hours * at(personData, kHourlyPay) : career.value(kPay, static_cast<std::size_t>(level));
    setText(page, kSalary, ts2::content::moneyText(pay));
    setText(page, kJobHours, ts2::content::formatted(liveText(kJobTexts, kHoursText),
                                                     {ts2::content::hourText(start), ts2::content::hourText(start + hours)}));
    screen_->setTip(screen_->find(page, kJobHours),
                    ts2::content::formatted(liveText(kJobTexts, kHoursTip), {ts2::content::hourText(start), ts2::content::hourText(start + hours),
                                                                             ts2::content::hourText(start == 0 ? 23 : start - 1)}));
    // Vacation days are kept in hundredths. Its text for one's own business is not read.
    const int vacation = std::max(0, at(personData, kVacationDays) / 100);
    screen_->setVisible(screen_->find(page, kVacation), !ownBusiness);
    setText(page, kVacation, ts2::content::formatted(liveText(kVacationText, 0x0C), {std::to_string(vacation)}));
    showDays(page, career.value(kWorkDays, static_cast<std::size_t>(level)), globals);
}

void JobPanel::showSchool(int page, std::span<const std::int16_t> personData, std::span<const std::int16_t> globals, const ObjectData& school) {
    // Children read the school's first entries, teens the second.
    const std::size_t entry = at(personData, kLifeStage) >= kTeen ? 1 : 0;
    setText(page, kSchoolName, school.text(0x93, 0));
    showIcon(screen_->find(page, kSchoolIcon), school, kPagePicture);
    // Grades 1..13 are F up to A+ (entries 0x12 down to 6). Grade 0 counts as 1.
    const int grade = at(personData, kGradePoints);
    const std::size_t gradeText = grade <= 1 ? 0x12 : grade >= 13 ? 6 : static_cast<std::size_t>(0x13 - grade);
    setText(page, kGrade, liveText(kSchoolTexts, gradeText));
    const int start = school.value(kStart, entry), hours = school.value(kHours, entry);
    setText(page, kSchoolHours, ts2::content::formatted(liveText(kJobTexts, kHoursText),
                                                        {ts2::content::hourText(start), ts2::content::hourText(start + hours)}));
    screen_->setTip(screen_->find(page, kSchoolHours),
                    ts2::content::formatted(liveText(kJobTexts, kSchoolHoursTip), {ts2::content::hourText(start), ts2::content::hourText(start + hours),
                                                                                   ts2::content::hourText(start == 0 ? 23 : start - 1)}));
    showDays(page, school.value(kWorkDays, entry), globals);
}

void JobPanel::showRetired(int page, std::span<const std::int16_t> personData) {
    setText(page, kPension, ts2::content::moneyText(at(personData, kPensionAmount)));
}

void JobPanel::showCollege(int page, std::span<const std::int16_t> personData) {
    // Semesters 1..8 make the years Freshman to Senior (entries 1, 3, 5, 7). The rest of the
    // college page (major, GPA, classes, the grade bar, the graduate's text) is not read yet.
    const int semester = std::clamp<int>(at(personData, kSemester), 1, 8);
    const int year = (semester + 1) / 2;
    const bool graduated = (at(personData, kCollegeFlags) & 0x40) != 0;
    const int info = screen_->find(page, kCollegeInfo);
    screen_->setVisible(screen_->find(info, kSchoolYear), !graduated);
    setText(info, kSchoolYear, liveText(kCollegeTexts, static_cast<std::size_t>(2 * year - 1)));
}

void JobPanel::showDays(int page, int mask, std::span<const std::int16_t> globals) {
    // US English weeks start on Sunday: button i is day d (1 Monday .. 7 Sunday), lit when bit d - 1
    // of the mask is set. The marker sits under today.
    const int days = screen_->find(page, kDays);
    const int today = kWeekdayGlobal < globals.size() ? globals[kWeekdayGlobal] : -1;
    const int marker = screen_->find(days, kToday);
    screen_->setVisible(marker, false);
    for (int i = 0; i < 7; ++i) {
        const int day = i == 0 ? 7 : i;
        const int button = screen_->find(days, kFirstDay + static_cast<std::uint32_t>(i));
        if (button < 0) {
            continue;
        }
        screen_->setEnabled(button, (mask & (1 << (day - 1))) != 0);
        if (day - 1 == today && marker >= 0) {
            screen_->move(marker, screen_->x(button) + (screen_->width(button) - screen_->width(marker)) / 2, screen_->y(marker));
            screen_->setVisible(marker, true);
        }
    }
}

void JobPanel::showPerformance(int value, int min, int max) {
    // Every column of the fill takes one column of the gradient, picked by the value.
    const int column = ts2::content::meterRow(value, min, max, static_cast<int>(performanceGradient_.width));
    if (column == performanceColumn_ || performanceGradient_.width == 0) {
        return;
    }
    performanceColumn_ = column;
    const int bar = screen_->find(content_, kPerformance);
    screen_->setImage(screen_->find(bar, kPerformanceFill), ts2::content::paintColumns(performanceFill_, performanceGradient_, column));
}

void JobPanel::showIcon(int window, const ObjectData& object, std::uint32_t instance, bool hideWithout) {
    if (window < 0) {
        return;
    }
    const std::pair key{object.type ? object.type->group : 0u, instance};
    if (const auto it = iconShown_.find(window); it != iconShown_.end() && it->second == key) {
        return;
    }
    iconShown_[window] = key;
    auto picture = pictures_.find(key);
    if (picture == pictures_.end()) {
        picture = pictures_.emplace(key, object.type ? object.code->image(key.first, instance, kMaxPictureSide) : std::nullopt).first;
    }
    // Without a picture the job icon is hidden, as the game does.
    if (hideWithout) {
        screen_->setVisible(window, picture->second.has_value());
    }
    if (picture->second) {
        screen_->setImage(window, *picture->second);
    }
}

void JobPanel::setText(int page, std::uint32_t id, const std::string& text) {
    const int window = screen_->find(page, id);
    if (window >= 0 && screen_->text(window) != text) {
        screen_->setText(window, text);
    }
}

std::string JobPanel::liveText(std::uint16_t table, std::size_t index) {
    return screen_->data().text(kLiveText, static_cast<std::uint32_t>(table) << 16 | static_cast<std::uint32_t>(index));
}

} // namespace app
