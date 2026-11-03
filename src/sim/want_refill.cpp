#include "sim/want_refill.hpp"

#include "diag/log.hpp"
#include "sim/clock.hpp"
#include "sim/code_source.hpp"
#include "sim/motives.hpp"
#include "sim/vm.hpp"
#include "sim/want_history.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

using content::WantArgument;
using content::WantDefinition;
using want_trees::AdKind;
using want_trees::Node;
using want_trees::Tree;

constexpr std::uint32_t kWantsControllerGuid = 0x6D9E58F0, kWantsCheckTreesGuid = 0x8D92DD2E;
constexpr std::uint32_t kSecondaryAspirationToken = 0x53D08989;
constexpr std::size_t kLifeStage = 0x3A, kAspiration = 0x2E, kAspirationScore = 0xA2;
constexpr std::size_t kCategoryField = 0x2F;
// Past this many records in all, adding one drops the oldest record of the same want, so a Sim
// satisfying wants without end can't use up memory.
constexpr std::size_t kMaxHistoryRecords = 20000;
// A history record flag: the trees don't follow records that have it.
constexpr std::uint8_t kSkippedRecord = 0x10;
constexpr std::uint8_t kHeld = 0x01;
// Where the fear places start in WantsState::slotNodes.
constexpr std::size_t kFearSlots = 1000;
// Candidates and the records they come from stop at this many, so a broken history can't make a
// refill without end.
constexpr std::size_t kMaxItems = 200000;
// One sim hour in history time.
constexpr std::int32_t kHour = 3600;

// The history's clock at the wants clock's time. Moving it by at least `step` makes each new
// record's time later than the last.
std::int32_t historyNow(WantHistory& history, std::int32_t clock, std::int32_t step) {
    history.time += std::max(clock - history.lastClock, step);
    history.lastClock = clock;
    return history.time;
}

std::int32_t categoryOf(CodeSource& code, std::uint32_t guid) {
    const ObjectType* type = guid != 0 ? code.objectType(guid) : nullptr;
    return type != nullptr ? type->definition.field(kCategoryField) : -1;
}

// The node's subject: its want's argument kind. A want without one whose event names a Sim or an
// object has a Sim or an object as its subject.
WantArgument subjectOf(const WantDefinition& def) {
    if (def.argument == WantArgument::None) {
        if (def.eventRequiresSim) {
            return WantArgument::Sim;
        }
        if (def.eventRequiresObject) {
            return WantArgument::Object;
        }
    }
    return def.argument;
}

// The node's lifetime and frequency in sim seconds. The game never reads the file's frequency,
// so a node that regenerates takes its lifetime as its frequency instead.
std::pair<std::int32_t, std::int32_t> lifeAndFrequency(const Node& node) {
    const std::int64_t hours = std::min<std::int64_t>(node.lifetime, INT32_MAX / kHour);
    const std::int32_t life = hours > 0 ? static_cast<std::int32_t>(hours * kHour) : 0;
    return node.regeneration && life > 0 ? std::pair{0, life} : std::pair{life, 0};
}

// The aspiration points a want of this node gives: the node's override when set, else its definition's.
std::int32_t pointsOf(const Node& node, const WantDefinition& def) { return node.scoreOverride.value_or(def.score); }

// A history record, or a candidate made from one, with where it sits in the trees. `parent`
// indexes the item it was reached from, -1 for none.
struct Item {
    swaf::Want want;
    const Tree* tree = nullptr;
    std::int32_t node = -1;
    std::int32_t parent = -1;
};

// Whether a history record is an event of this node's want, by the node's subject.
bool nodeMatches(const Node& node, const WantDefinition& def, const swaf::Want& event, CodeSource& code) {
    if (event.guid != node.wantId) {
        return false;
    }
    const auto objectField = static_cast<std::int32_t>(node.objectField);
    const auto integerField = static_cast<std::int32_t>(node.integerField);
    switch (subjectOf(def)) {
    case WantArgument::None:
    case WantArgument::Sim:
        break;
    case WantArgument::Object:
        if (objectField != 0 && (event.argument == 0 || event.argument != objectField)) {
            return false;
        }
        break;
    case WantArgument::Category:
        if (objectField != 0) {
            if (event.type == static_cast<std::uint8_t>(WantArgument::Object)) {
                if (categoryOf(code, static_cast<std::uint32_t>(event.argument)) != objectField) {
                    return false;
                }
            } else if (event.type == static_cast<std::uint8_t>(WantArgument::Category)) {
                return event.argument == objectField;
            }
        }
        break;
    case WantArgument::Skill:
        return (objectField == 0 || static_cast<std::int16_t>(event.argument) == static_cast<std::int16_t>(objectField)) &&
               (integerField == 0 || event.value == integerField);
    case WantArgument::Career:
    case WantArgument::Badge:
        return (objectField == 0 || event.argument == objectField) && (integerField == 0 || event.value == integerField);
    }
    return wantCompare(def.op, event.value, integerField);
}

// The argument kind, argument and value of a node's want reached from `parent` (an index into
// `items`, -1 for none). With inheritSubject the argument comes from the nearest record above of the
// same kind. The node's objectField and integerField then set the argument and the value.
void buildArgument(swaf::Want& out, const Node& node, WantArgument subject, std::int32_t parent, const std::vector<Item>& items,
                   const World& world) {
    out.type = static_cast<std::uint8_t>(subject);
    out.argument = 0;
    out.value = 0;
    if (node.inheritSubject) {
        for (std::int32_t at = parent; at >= 0; at = items[static_cast<std::size_t>(at)].parent) {
            const Item& above = items[static_cast<std::size_t>(at)];
            const auto kind = static_cast<WantArgument>(above.want.type);
            if (subject == WantArgument::Category) {
                const Node& n = above.tree->nodes[static_cast<std::size_t>(above.node)];
                const WantDefinition* def = world.wantDefinition(n.wantId);
                if (def != nullptr && subjectOf(*def) == WantArgument::Category) {
                    out.argument = static_cast<std::int32_t>(n.objectField);
                    break;
                }
                continue;
            }
            if (kind != subject || (subject == WantArgument::Object && above.want.argument == 0)) {
                continue;
            }
            const bool shortArgument = subject == WantArgument::Sim || subject == WantArgument::Skill;
            out.argument = shortArgument ? static_cast<std::int16_t>(above.want.argument) : above.want.argument;
            break;
        }
    }
    if (subject != WantArgument::None && subject != WantArgument::Sim && node.objectField != 0) {
        out.argument = subject == WantArgument::Skill ? static_cast<std::int16_t>(node.objectField) : static_cast<std::int32_t>(node.objectField);
    }
    if (node.integerField != 0) {
        out.value = static_cast<std::int16_t>(node.integerField);
    }
}

// Whether `less` names the same want as `more` but leaves out an argument or value `more` has.
bool lessSpecific(const swaf::Want& less, const swaf::Want& more) {
    return less.guid == more.guid && less.type == more.type &&
           ((less.argument == 0 && more.argument != 0) || (less.value == 0 && more.value != 0));
}

// Whether two wants are the same: Sim, GUID, argument kind, then the argument and value by kind.
bool sameWant(const swaf::Want& a, const swaf::Want& b) {
    if (a.id != b.id || a.guid != b.guid || a.type != b.type) {
        return false;
    }
    switch (static_cast<WantArgument>(a.type)) {
    case WantArgument::None:
        return a.value == b.value;
    case WantArgument::Sim:
    case WantArgument::Skill:
        return static_cast<std::int16_t>(a.argument) == static_cast<std::int16_t>(b.argument) && a.value == b.value;
    case WantArgument::Object:
    case WantArgument::Category:
    case WantArgument::Career:
    case WantArgument::Badge:
        return a.argument == b.argument && a.value == b.value;
    }
    return false;
}

std::int32_t points(const swaf::Want& w) { return w.tail.size() > 1 ? w.tail[1] : 0; }
std::int32_t influence(const swaf::Want& w) { return w.tail.size() > 2 ? w.tail[2] : 0; }

// The straight rise from 0 at `low` to 1 at `high`.
float ramp(std::int32_t low, std::int32_t high, std::int32_t x) {
    if (x < low) {
        return 0;
    }
    if (x > high) {
        return 1;
    }
    if (low == high) {
        return x == high ? 1.0f : 0.0f;
    }
    return static_cast<float>(x - low) / static_cast<float>(high - low);
}

// The Sim whose wants are being made, and what the scores read about it.
class Refill {
public:
    Refill(const WantContext& context, std::int16_t neighbor)
        : c_(context), world_(context.world), neighbor_(neighbor), history_(wantHistory(world_, neighbor)) {
        sim_ = world_.nextAfter(0, [&](const SimObject& o) { return o.isPerson() && world_.neighborIdOf(o) == neighbor; });
        now_ = historyNow(history_, world_.wantsState().clock, 0);
    }

    void run(int level) {
        if (world_.wantTrees() == nullptr || !world_.hasWantDefinitions()) {
            return;
        }
        makeCandidates();
        for (Candidate& candidate : candidates_) {
            candidate.score = score(candidate.item);
        }
        std::stable_sort(candidates_.begin(), candidates_.end(), [](const Candidate& a, const Candidate& b) { return b.score < a.score; });
        auto& wants = world_.neighborWants(neighbor_);
        auto& fears = world_.neighborFears(neighbor_);
        const auto oldWants = wants, oldFears = fears;
        std::size_t wantCount = keep(wants, level <= 1);
        std::size_t fearCount = keep(fears, level <= 2);
        for (const Candidate& candidate : candidates_) {
            if (!(candidate.score > 0)) {
                continue;
            }
            const WantDefinition* def = world_.wantDefinition(candidate.item.want.guid);
            if (def == nullptr) {
                continue;
            }
            const bool fear = def->isFear;
            auto& list = fear ? fears : wants;
            std::size_t& count = fear ? fearCount : wantCount;
            if (count <= list.size()) {
                place(candidate.item, list, count, fear ? oldFears : oldWants);
            }
        }
    }

private:
    struct Candidate {
        Item item;
        float score = 0;
    };

    std::int16_t personValue(std::size_t index) const {
        if (sim_ != nullptr) {
            return index < sim_->personData.size() ? sim_->personData[index] : 0;
        }
        return world_.neighborPersonValue(neighbor_, index);
    }

    Item makeItem(const Tree& tree, std::int32_t node, std::int32_t parent) const {
        const Node& n = tree.nodes[static_cast<std::size_t>(node)];
        const WantDefinition* def = world_.wantDefinition(n.wantId);
        Item item;
        item.tree = &tree;
        item.node = node;
        item.parent = parent;
        item.want.version = 10;
        item.want.id = static_cast<std::uint16_t>(neighbor_);
        item.want.guid = n.wantId;
        if (def != nullptr) {
            buildArgument(item.want, n, subjectOf(*def), parent, items_, world_);
            item.want.tail = {0, pointsOf(n, *def), def->influence};
        }
        return item;
    }

    // The records of the node's want in the history that the node matches.
    void events(const Node& node, std::vector<const swaf::Want*>& out) const {
        out.clear();
        const WantDefinition* def = world_.wantDefinition(node.wantId);
        const auto found = history_.records.find(node.wantId);
        if (def == nullptr || found == history_.records.end()) {
            return;
        }
        for (const auto& record : found->second) {
            if (!(record.flags & kSkippedRecord) && nodeMatches(node, *def, record, c_.code)) {
                out.push_back(&record);
            }
        }
    }

    // Every tree's root becomes a candidate when it is visible and its want is not in the history
    // (or it regenerates). Each history record of a root's want leads to the root's children: a
    // child becomes a candidate while the record is younger than the child's lifetime, unless
    // the child's own want is in the history and it doesn't regenerate. A child's own records lead
    // on to its children the same way.
    void makeCandidates() {
        std::vector<const swaf::Want*> found;
        for (const Tree& tree : *world_.wantTrees()) {
            const Node& root = tree.nodes.front();
            if (world_.wantDefinition(root.wantId) == nullptr) {
                continue;
            }
            events(root, found);
            if ((found.empty() || root.regeneration) && root.visible) {
                if (candidates_.size() >= kMaxItems) {
                    diag::warnOnce("wants: a refill made too many candidates and stops there");
                    return;
                }
                candidates_.push_back({makeItem(tree, 0, -1)});
            }
            const std::size_t first = items_.size();
            for (const swaf::Want* event : found) {
                if (items_.size() >= kMaxItems) {
                    diag::warnOnce("wants: a refill made too many candidates and stops there");
                    return;
                }
                items_.push_back({*event, &tree, 0, -1});
            }
            for (std::size_t at = first; at < items_.size(); ++at) {
                const Item parent = items_[at];
                const Node& from = tree.nodes[static_cast<std::size_t>(parent.node)];
                for (const std::int32_t child : from.children) {
                    const Node& node = tree.nodes[static_cast<std::size_t>(child)];
                    events(node, found);
                    std::erase_if(found, [&](const swaf::Want* e) { return !inheritsFrom(*e, parent.want); });
                    for (const swaf::Want* event : found) {
                        if (items_.size() >= kMaxItems) {
                            diag::warnOnce("wants: a refill made too many candidates and stops there");
                            return;
                        }
                        items_.push_back({*event, &tree, child, static_cast<std::int32_t>(at)});
                    }
                    if (!found.empty() && !node.regeneration) {
                        continue;
                    }
                    const std::int64_t age = std::int64_t{now_} - recordTime(parent.want);
                    const auto life = lifeAndFrequency(node).first;
                    if (age < 0 || (life > 0 && age >= life) || !node.visible) {
                        continue;
                    }
                    if (candidates_.size() >= kMaxItems) {
                        diag::warnOnce("wants: a refill made too many candidates and stops there");
                        return;
                    }
                    candidates_.push_back({makeItem(tree, child, static_cast<std::int32_t>(at))});
                }
            }
        }
    }

    // Whether a child's record goes with the parent's: when both have an argument of the same
    // kind it must be the same, but a record of an object want without one goes with any.
    static bool inheritsFrom(const swaf::Want& event, const swaf::Want& parent) {
        if (event.type != parent.type || parent.argument == 0) {
            return true;
        }
        if (event.argument == 0 && parent.type == static_cast<std::uint8_t>(WantArgument::Object)) {
            return true;
        }
        return event.argument == parent.argument;
    }

    float factor(AdKind kind, const Node& node, const swaf::Want& want) const {
        const want_trees::Ad& ad = node.ads[static_cast<std::size_t>(kind)];
        auto ticked = [&](std::string_view name) { return std::find(ad.ticked.begin(), ad.ticked.end(), name) != ad.ticked.end(); };
        auto ranged = [&](std::int32_t x) { return ad.min <= ad.max ? ramp(ad.min, ad.max, x) : 1 - ramp(ad.max, ad.min, x); };
        switch (kind) {
        case AdKind::Age: {
            static constexpr std::pair<int, std::string_view> kAges[] = {{0, "Adult"}, {1, "Child"}, {2, "Toddler"}, {3, "Teen"}, {4, "Elder"}, {13, "Baby"}};
            const int code = wantAgeCode(personValue(kLifeStage));
            for (const auto& [value, name] : kAges) {
                if (value == code) {
                    return ticked(name) ? 1.0f : 0.0f;
                }
            }
            return 0;
        }
        case AdKind::Aspiration: {
            static constexpr std::pair<int, std::string_view> kAspirations[] = {
                {0, "None"},        {1, "Romance"},    {2, "Family"},         {4, "Fortune"},       {0x10, "Popularity"},
                {0x20, "Knowledge"}, {0x40, "GrowUp"}, {0x80, "PleasureSeeker"}, {0x100, "GrilledCheese"}};
            auto has = [&](int value) {
                for (const auto& [v, name] : kAspirations) {
                    if (v == value) {
                        return ticked(name);
                    }
                }
                return false;
            };
            int aspiration = sim_ != nullptr || world_.neighbor(neighbor_) != nullptr ? personValue(kAspiration) : 4;
            if (aspiration == 0) {
                return 0;
            }
            const int secondary = secondaryAspiration();
            if (secondary > 0) {
                aspiration ^= secondary;
            }
            if (has(aspiration)) {
                return 1;
            }
            const WantDefinition* def = world_.wantDefinition(node.wantId);
            if (secondary > 0 && def != nullptr && pointsOf(node, *def) >= 0 && has(secondary)) {
                // The "Aspirations" constants (BCON 0x13E) 0x2D and 0x2E, in percent.
                return (secondary & 0x100) ? 0.55f : 0.75f;
            }
            return 0;
        }
        case AdKind::Mood:
            return ranged(sim_ != nullptr ? static_cast<std::int32_t>(sim_->motives[motive::kMood]) : 0);
        case AdKind::Personality: {
            // Each choice counts how far its trait is from the middle (500) on its own side.
            static constexpr struct {
                std::string_view name;
                std::size_t field;
                int sign;
            } kTraits[] = {{"Nice", 2, 1},     {"Mean", 2, -1},    {"Active", 3, 1}, {"Lazy", 3, -1},  {"Playful", 5, 1},
                           {"Serious", 5, -1}, {"Outgoing", 6, 1}, {"Shy", 6, -1},   {"Neat", 7, 1},   {"Sloppy", 7, -1}};
            float sum = 0;
            int count = 0;
            for (const auto& trait : kTraits) {
                if (ticked(trait.name)) {
                    sum += std::clamp(static_cast<float>(trait.sign * (personValue(trait.field) - 500)) * 0.002f, 0.0f, 1.0f);
                    ++count;
                }
            }
            return count > 0 ? sum / static_cast<float>(count) : 0.0f;
        }
        case AdKind::Relationship: {
            if (want.type != static_cast<std::uint8_t>(WantArgument::Sim) || want.argument == 0) {
                return ranged(0);
            }
            const auto* values = world_.neighborRelationshipValues(neighbor_, static_cast<std::int16_t>(want.argument));
            return ranged(values != nullptr && values->size() > 2 ? static_cast<std::int16_t>((*values)[2]) : 0);
        }
        case AdKind::AspirationScore:
            return ranged(aspirationLevel());
        case AdKind::PersonData: {
            constexpr std::int32_t kLastField = 0xE7;
            if (ad.index < 1 || ad.index > kLastField) {
                return 0;
            }
            return ranged(personValue(static_cast<std::size_t>(ad.index - 1)));
        }
        }
        return 0;
    }

    // The secondary aspiration: value 0 of the Sim's token 0x53D08989, -1 without one.
    int secondaryAspiration() const {
        const Inventory* inventory = world_.inventory(InventoryKind::Neighbor, neighbor_);
        if (inventory != nullptr) {
            for (const auto* list : {&inventory->singular, &inventory->counted}) {
                for (const auto& token : *list) {
                    if (token->guid == kSecondaryAspirationToken && !token->properties.empty()) {
                        return token->properties[0];
                    }
                }
            }
        }
        return -1;
    }

    // The aspiration score (person data 0xA2, in tens) on -100..100 by the age's levels: -100 at
    // rock bottom, -50, 0 at neutral, 25, 50, 75 and 100 at perfect, straight between them.
    std::int32_t aspirationLevel() const {
        static constexpr std::int32_t kLevels[7] = {-100, -50, 0, 25, 50, 75, 100};
        const auto& levels = world_.wantDefinitions()->aspirationLevels;
        const auto found = levels.find(wantAgeCode(personValue(kLifeStage)));
        if (found == levels.end()) {
            return 0;
        }
        const auto& t = found->second;
        const std::int32_t s = std::clamp(static_cast<std::int32_t>(personValue(kAspirationScore)) * 10, -327680, 327670);
        std::size_t band = 0;
        for (std::size_t k = 1; k <= 5; ++k) {
            if (s >= t[k]) {
                band = k;
            }
        }
        const std::int32_t span = t[band + 1] - t[band];
        const float f = span != 0 ? std::clamp(static_cast<float>(s - t[band]) / static_cast<float>(span), 0.0f, 1.0f) : 0.0f;
        const float value = static_cast<float>(kLevels[band]) + static_cast<float>(kLevels[band + 1] - kLevels[band]) * f;
        return std::clamp(static_cast<std::int32_t>(std::lround(value)), -100, 100);
    }

    // A candidate's score: between the node's least and most by its weighted factors, times how far
    // its lockout has worn off since the record it was reached from.
    float score(const Item& item) const {
        const Node& node = item.tree->nodes[static_cast<std::size_t>(item.node)];
        float lockout = 1;
        if (item.parent >= 0) {
            const swaf::Want& from = items_[static_cast<std::size_t>(item.parent)].want;
            if (recordTime(from) != 0) {
                const std::int64_t elapsed = std::int64_t{now_} - recordTime(from);
                const auto [life, frequency] = lifeAndFrequency(node);
                if (life > 0 && elapsed > life) {
                    lockout = 0;
                } else if (frequency > 0) {
                    lockout = static_cast<float>(std::max<std::int64_t>(frequency - elapsed % frequency, 0)) / static_cast<float>(frequency);
                } else if (life > 0) {
                    lockout = static_cast<float>(std::max<std::int64_t>(life - elapsed, 0)) / static_cast<float>(life);
                }
            }
        }
        // In 64 bits, so seven weights from a broken file can't overflow.
        std::int64_t sum = 0, weights = 0;
        for (std::size_t i = 0; i < want_trees::kAdCount; ++i) {
            const want_trees::Ad& ad = node.ads[i];
            const float f = factor(static_cast<AdKind>(i), node, item.want);
            if (ad.superZero && !(f > 0)) {
                return 0;
            }
            // The game adds up the weighted factors as whole numbers.
            sum = static_cast<std::int64_t>(static_cast<double>(ad.weight) * f + static_cast<double>(sum));
            weights += ad.weight;
        }
        const float t = weights > 0 ? static_cast<float>(sum) / static_cast<float>(weights) : 1.0f;
        const float least = static_cast<float>(node.minScore);
        return (least + (static_cast<float>(node.maxScore) - least) * t) * lockout;
    }

    // Keeps the held wants and those still valid, or (forced) only the held ones. Returns how many stay.
    std::size_t keep(std::vector<swaf::Want>& list, bool forced) {
        std::size_t kept = 0;
        for (std::size_t at = 0; at < list.size(); ++at) {
            auto& want = list[at];
            if (want.guid == 0) {
                continue;
            }
            if (!(want.flags & kHeld) && (forced || !valid(want))) {
                want = {};
                slotNodes().erase(slotKey(list, at));
            } else {
                ++kept;
            }
        }
        return kept;
    }

    // Whether a want may be offered: its packs are installed, a category want's category has an
    // object worth its value, and its definition's checkTree (if any) is true for it.
    bool valid(const swaf::Want& want) {
        const WantDefinition* def = world_.wantDefinition(want.guid);
        if (def == nullptr || (def->gameVersionFlags & c_.installedPacks) == 0) {
            return false;
        }
        if (want.type == static_cast<std::uint8_t>(WantArgument::Category)) {
            const auto& values = categoryValues();
            const auto found = values.find(static_cast<std::uint16_t>(want.argument));
            if (found == values.end()) {
                return false;
            }
            if (def->op == content::WantOperator::GreaterThan && !(found->second > want.value)) {
                return false;
            }
            if (def->op == content::WantOperator::GreaterThanOrEquals && !(found->second >= want.value)) {
                return false;
            }
        }
        return checkTree(*def, want);
    }

    const std::map<std::uint16_t, std::int16_t>& categoryValues() {
        auto& values = world_.wantsState().categoryValues;
        if (!values) {
            values.emplace();
            for (const std::uint32_t guid : c_.code.objectTypeGuids()) {
                const ObjectType* type = c_.code.objectType(guid);
                if (type == nullptr) {
                    continue;
                }
                const auto field = [&](std::size_t i) { return static_cast<std::int16_t>(type->definition.field(i)); };
                if (field(kCategoryField) == 0 || field(0x27) == 0 || !(field(0x45) > 0 || (field(0x28) > 0 && field(0x5E) > 0))) {
                    continue;
                }
                auto [it, fresh] = values->try_emplace(static_cast<std::uint16_t>(field(kCategoryField)), field(0x12));
                if (!fresh) {
                    it->second = std::max(it->second, field(0x12));
                }
            }
        }
        return *values;
    }

    // The checkTree, one of the "Wants - Check Trees" object's found by name, run at once on the
    // lot's "Wants - Controller" with the want as its params. True without either object or the tree.
    bool checkTree(const WantDefinition& def, const swaf::Want& want) {
        if (def.checkTree.empty()) {
            return true;
        }
        for (const auto& [key, result] : checked_) {
            if (sameWant(key, want)) {
                return result;
            }
        }
        SimObject* controller = world_.nextAfter(0, [](const SimObject& s) { return s.definition.guid() == kWantsControllerGuid; });
        const ObjectType* checkTrees = c_.code.objectType(kWantsCheckTreesGuid);
        if (controller == nullptr || checkTrees == nullptr) {
            return true;
        }
        std::optional<std::uint16_t> tree;
        for (const std::uint32_t g : {checkTrees->group, c_.code.semiGlobalGroup(checkTrees->group), kGlobalGroup}) {
            if ((tree = c_.code.behaviorByName(g, def.checkTree))) {
                break;
            }
        }
        if (!tree) {
            return true;
        }
        const auto kind = static_cast<WantArgument>(want.type);
        const std::int16_t simArgument = kind == WantArgument::Sim ? static_cast<std::int16_t>(want.argument) : 0;
        const std::int32_t longArgument =
            kind == WantArgument::Object || kind == WantArgument::Career || kind == WantArgument::Badge ? want.argument : 0;
        const std::array<std::int16_t, 8> params{neighbor_,
                                                 static_cast<std::int16_t>(want.guid & 0xFFFF),
                                                 static_cast<std::int16_t>(want.guid >> 16),
                                                 simArgument,
                                                 static_cast<std::int16_t>(longArgument & 0xFFFF),
                                                 static_cast<std::int16_t>(static_cast<std::uint32_t>(longArgument) >> 16),
                                                 static_cast<std::int16_t>(want.argument),
                                                 want.value};
        const RunResult r = c_.vm.runOnObjectNow(*controller, checkTrees->group, *tree, controller->id, params);
        const bool result = r.outcome == Outcome::True;
        checked_.emplace_back(want, result);
        return result;
    }

    // Puts a candidate into the list as the game's placement does: never next to a more specific
    // form of itself, in place of a less specific or shallower form of itself that isn't held,
    // back in its old place if it was there before, else in the first empty place.
    void place(const Item& candidate, std::vector<swaf::Want>& list, std::size_t& count, const std::vector<swaf::Want>& old) {
        const swaf::Want& want = candidate.want;
        const auto op = world_.wantDefinition(want.guid)->op;
        if (list.size() < count ||
            std::any_of(list.begin(), list.end(), [&](const swaf::Want& s) { return s.guid != 0 && lessSpecific(want, s); })) {
            return;
        }
        auto same = std::find_if(list.begin(), list.end(), [&](const swaf::Want& s) { return s.guid != 0 && sameWant(s, want); });
        const bool found = same != list.end();
        const bool ok = valid(want);
        if (ok) {
            auto match = same;
            if (!found) {
                match = std::find_if(list.begin(), list.end(), [&](const swaf::Want& s) {
                    return s.guid != 0 && wantMatches(s, want, op, [&](std::uint32_t g) { return categoryOf(c_.code, g); });
                });
            }
            if (match != list.end()) {
                const auto at = static_cast<std::size_t>(match - list.begin());
                bool replace = lessSpecific(*match, want);
                if (!replace) {
                    const auto shown = slotNodes().find(slotKey(list, at));
                    if (shown != slotNodes().end() && shown->second.first == candidate.tree->id) {
                        replace = shown->second.second < depthOf(candidate);
                    }
                }
                if (replace) {
                    if (!(match->flags & kHeld)) {
                        *match = want;
                        remember(list, at, candidate);
                    }
                    return;
                }
            }
        }
        if (!ok || found) {
            return;
        }
        if (count == list.size()) {
            ++count;
            return;
        }
        swaf::Want current = want;
        for (std::size_t j = 0; j < std::min(list.size(), old.size()); ++j) {
            const swaf::Want& was = old[j];
            if (was.guid == 0 || !sameWant(was, want) || points(was) != points(want) || influence(was) != influence(want)) {
                continue;
            }
            if (list[j].guid == 0) {
                list[j] = was;
                remember(list, j, candidate);
                ++count;
                return;
            }
            // The want moved out of the old place takes its tree node along.
            const auto moved = slotNodes().find(slotKey(list, j));
            std::optional<std::pair<std::uint32_t, std::int32_t>> movedNode;
            if (moved != slotNodes().end()) {
                movedNode = moved->second;
            }
            current = std::exchange(list[j], was);
            remember(list, j, candidate);
            for (std::size_t k = 0; k < list.size(); ++k) {
                if (list[k].guid == 0) {
                    list[k] = current;
                    if (movedNode) {
                        slotNodes()[slotKey(list, k)] = *movedNode;
                    }
                    ++count;
                    return;
                }
            }
            return;
        }
        for (std::size_t j = 0; j < list.size(); ++j) {
            if (list[j].guid == 0) {
                list[j] = current;
                remember(list, j, candidate);
                ++count;
                return;
            }
        }
    }

    static std::int32_t depthOf(const Item& item) {
        std::int32_t depth = 0;
        for (std::int32_t at = item.tree->nodes[static_cast<std::size_t>(item.node)].parent; at >= 0;
             at = item.tree->nodes[static_cast<std::size_t>(at)].parent) {
            ++depth;
        }
        return depth;
    }

    // Each place's tree node is kept, as the game's want keeps it, for the depth test above.
    std::map<std::size_t, std::pair<std::uint32_t, std::int32_t>>& slotNodes() { return world_.wantsState().slotNodes[neighbor_]; }
    std::size_t slotKey(const std::vector<swaf::Want>& list, std::size_t at) {
        return at + (&list == &world_.neighborFears(neighbor_) ? kFearSlots : 0);
    }
    void remember(const std::vector<swaf::Want>& list, std::size_t at, const Item& item) {
        slotNodes()[slotKey(list, at)] = {item.tree->id, depthOf(item)};
    }

    const WantContext& c_;
    World& world_;
    std::int16_t neighbor_;
    WantHistory& history_;
    const SimObject* sim_ = nullptr;
    std::int32_t now_ = 0;
    std::vector<Item> items_;
    std::vector<Candidate> candidates_;
    std::vector<std::pair<swaf::Want, bool>> checked_;
};

} // namespace

int wantAgeCode(std::int16_t lifeStage) {
    if (lifeStage <= 1) {
        return 13;
    }
    if (lifeStage == 2) {
        return 2;
    }
    if (lifeStage <= 15) {
        return 1;
    }
    if (lifeStage <= 18) {
        return 3;
    }
    return lifeStage <= 50 ? 0 : 4;
}

bool wantCompare(content::WantOperator op, std::int32_t got, std::int32_t wanted) {
    switch (op) {
    case content::WantOperator::Equals:              return got == wanted;
    case content::WantOperator::LessThan:            return got < wanted;
    case content::WantOperator::GreaterThan:         return got > wanted;
    case content::WantOperator::LessThanOrEquals:    return got <= wanted;
    case content::WantOperator::GreaterThanOrEquals: return got >= wanted;
    case content::WantOperator::None:                return true;
    }
    return true;
}

bool wantMatches(const swaf::Want& want, const swaf::Want& request, content::WantOperator op,
                 const std::function<std::int32_t(std::uint32_t)>& categoryOf) {
    if (want.id != request.id || want.guid != request.guid) {
        return false;
    }
    switch (static_cast<WantArgument>(want.type)) {
    case WantArgument::None:
        break;
    case WantArgument::Sim:
        if (want.argument != 0 && want.argument != request.argument) {
            return false;
        }
        break;
    case WantArgument::Object:
        if (want.argument != 0 && request.argument != 0 && want.argument != request.argument) {
            return false;
        }
        break;
    case WantArgument::Category:
        if (want.argument != 0) {
            if (request.type == static_cast<std::uint8_t>(WantArgument::Object)) {
                if (categoryOf(static_cast<std::uint32_t>(request.argument)) != want.argument) {
                    return false;
                }
            } else if (request.type == static_cast<std::uint8_t>(WantArgument::Category)) {
                return want.argument == request.argument;
            }
        }
        break;
    case WantArgument::Skill:
    case WantArgument::Career:
    case WantArgument::Badge: {
        const bool argumentOk = want.argument == 0 || want.argument == request.argument;
        return want.value == 0 ? argumentOk : argumentOk && want.value == request.value;
    }
    default:
        return false;
    }
    return wantCompare(op, request.value, want.value);
}

WantHistory& wantHistory(World& world, std::int16_t neighbor) {
    WantsState& state = world.wantsState();
    auto found = state.histories.find(neighbor);
    if (found == state.histories.end()) {
        WantHistory history;
        if (const WantHistory* saved = world.savedWantHistory(neighbor)) {
            history = *saved;
            for (auto it = history.records.begin(); it != history.records.end();) {
                it = world.wantDefinition(it->first) == nullptr ? history.records.erase(it) : std::next(it);
            }
        }
        history.lastClock = state.clock;
        found = state.histories.emplace(neighbor, std::move(history)).first;
    }
    return found->second;
}

void addWantHistory(World& world, std::int16_t neighbor, swaf::Want record) {
    WantHistory& history = wantHistory(world, neighbor);
    if (record.tail.empty()) {
        record.tail.push_back(0);
    }
    if (record.tail[0] != 0) {
        history.time = std::max(history.time, record.tail[0]);
    } else {
        record.tail[0] = historyNow(history, world.wantsState().clock, 1);
    }
    std::size_t total = 0;
    for (const auto& [guid, records] : history.records) {
        total += records.size();
    }
    auto& records = history.records[record.guid];
    if (total >= kMaxHistoryRecords && !records.empty()) {
        diag::warnOnce("wants: a Sim's history is past its limit and drops its oldest records");
        records.erase(records.begin());
    }
    records.push_back(std::move(record));
}

void forgetWantPlace(World& world, std::int16_t neighbor, std::size_t place, bool fear) {
    if (const auto it = world.wantsState().slotNodes.find(neighbor); it != world.wantsState().slotNodes.end()) {
        it->second.erase(place + (fear ? kFearSlots : 0));
    }
}

void queueWantRefill(World& world, std::int16_t neighbor, int level) {
    if (neighbor == 0) {
        return;
    }
    auto [it, fresh] = world.wantsState().queue.try_emplace(neighbor, level);
    if (!fresh) {
        it->second = std::min(it->second, level);
    }
}

void stepWants(const WantContext& context) {
    WantsState& state = context.world.wantsState();
    state.clock += 2;
    if (state.queue.empty()) {
        return;
    }
    auto next = state.queue.begin();
    if (const SimObject* selected = context.world.find(context.vm.globals[global::kSelectedPerson])) {
        if (const auto it = state.queue.find(context.world.neighborIdOf(*selected)); it != state.queue.end()) {
            next = it;
        }
    }
    const auto [neighbor, level] = *next;
    state.queue.erase(next);
    if (context.world.neighbor(neighbor) == nullptr) {
        return;
    }
    Refill(context, neighbor).run(level);
}

} // namespace ts2::sim
