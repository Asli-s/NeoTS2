#include "sim/find_best_interaction.hpp"

#include "diag/log.hpp"
#include "sim/advertisement.hpp"
#include "sim/advertisers.hpp"
#include "sim/happiness.hpp"
#include "sim/pie_menu.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>
#include <vector>

namespace ts2::sim {

namespace {

constexpr std::size_t kAgePersonData = 0x3A;
constexpr std::size_t kYoungAdultPersonData = 0xAF;
constexpr std::size_t kVisitorPersonData = 0x20;
constexpr std::size_t kAutonomyLevelPersonData = 0x24;
constexpr std::size_t kSittingPersonData = 0x00;
constexpr std::size_t kLastAutonomyObjectPersonData = 0x35;
constexpr std::size_t kSpeciesPersonData = 0xBA;
constexpr std::size_t kNeighborIdPersonData = 0x1F;
constexpr std::size_t kTreeTableEntryField = 0x14;
constexpr std::size_t kLockoutField = 0x19;
constexpr std::size_t kRoomField = 0x1D;
constexpr std::size_t kSkipFlagsField = 0x38;
constexpr std::size_t kPlacementFlags2Field = 0x52;
constexpr std::size_t kDisableInteractionsField = 0x57;

constexpr std::uint32_t kAgeFlags = 0x3F013E51;
constexpr std::uint32_t kDebugLine = 0x80;
constexpr std::uint32_t kOnSelf = 0x00100000;
constexpr std::uint32_t kPreferredByFurious = 0x00800000;  // a flag bit, leakcheck: ok
// The person data Free Will's check reads (see findBestInteraction).
constexpr std::size_t kPersonTypePersonData = 0x20, kSelectionFlagsPersonData = 0x9E, kPersonFlagsData = 0x54;
constexpr std::int16_t kSelectable = 0x01, kFreeWillCheckFlag = 0x800;
// Tokens in a Sim's neighbor inventory that the social bias reads. "Token - Furious" holds the
// other Sim's neighbor id in property 0 and how furious in property 2. "Token - Attraction Marker"
// holds the attraction in property 0 and the other Sim's neighbor id in property 1.
constexpr std::uint32_t kFuriousToken = 0xCF505AB0;
constexpr std::uint32_t kAttractionToken = 0x2F4D6811;
// The line names its own queue icon (an object type and picture).
constexpr std::uint32_t kOwnIconFlag = 0x00400000;  // a flag bit, leakcheck: ok
constexpr float kLowestScore = 0.00001f;
constexpr std::uint16_t kMaxTree = 0x7FFF;
constexpr std::uint32_t kMaxId = 0x7FFF;

struct Candidate {
    std::int16_t object = 0;
    std::uint16_t interaction = 0;
    float score = 0;
    float after = 0;
    bool checked = false;  // its check tree already ran and passed
    InteractionIcon icon;  // its queue icon (see FoundInteraction::icon)
    std::string name;      // see FoundInteraction::name
    std::array<std::int16_t, 4> params{};
};

// A Sim's age kind for the advertiser list (13 for babies, which nothing advertises to, pets 5-12)
// and the age flag its interaction lines need. Another species counts as a human adult.
std::pair<std::size_t, std::uint32_t> ageKind(const SimObject& sim) {
    // The line flags of the pet kinds 5-12 (see petKind).
    constexpr std::array<std::uint32_t, 8> kPetFlags{0x1000, 0x4000000, 0x10000000, 0x20000000,
                                                     0x2000, 0x8000000, 0x1000000, 0x2000000};
    if (sim.personData[kSpeciesPersonData] != 0) {
        const auto kind = petKind(sim);
        return kind ? std::pair{*kind, kPetFlags[*kind - kFirstPetKind]} : std::pair<std::size_t, std::uint32_t>{0, 0x40};
    }
    const std::int16_t age = sim.personData[kAgePersonData];
    if (age > 50) return {4, 0x400};
    if (age > 18 || sim.personData[kYoungAdultPersonData] != 0) return {0, 0x40};
    if (age > 15) return {3, 0x800};
    if (age > 2) return {1, 0x10};
    if (age == 2) return {2, 0x200};
    return {13, 0x10000};
}

bool inUse(const SimObject& object) {
    return object.data[kLockoutField] > 0 || object.data[kDisableInteractionsField] > 0 ||
           (object.data[kPlacementFlags2Field] & 4) != 0;
}

bool placed(const SimObject& object) {
    return !object.removed && object.x != kOffLot;
}

float distance(const SimObject& from, const SimObject& to, float floorCost) {
    const double dx = (static_cast<double>(to.x) - from.x) / 65536.0;
    const double dy = (static_cast<double>(to.y) - from.y) / 65536.0;
    const double dz = static_cast<double>(to.level - from.level) * floorCost;
    return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz));
}

// Autonomy remembers a pick on a multi-tile object by its lead, so every part counts as the same.
std::int16_t rememberedAs(const SimObject& object) {
    return object.data[kLeadTileField] > 0 ? object.data[kLeadTileField] : object.id;
}

// Forgets the picks whose time is up and keeps the `count` latest of the rest.
void forgetOldPicks(std::vector<AutonomyMemory>& memory, std::uint64_t tick, int count) {
    std::erase_if(memory, [tick](const AutonomyMemory& m) { return m.until < tick; });
    std::stable_sort(memory.begin(), memory.end(), [](const AutonomyMemory& a, const AutonomyMemory& b) { return a.until > b.until; });
    if (memory.size() > static_cast<std::size_t>(std::max(0, count))) {
        memory.resize(static_cast<std::size_t>(std::max(0, count)));
    }
}

class Search {
public:
    explicit Search(const InteractionSearch& s) : s_(s), visitor_(s.me.personData[kVisitorPersonData] != 0) {}

    std::optional<FoundInteraction> run() {
        auto& me = s_.me;
        curves_ = scoreCurves(s_.code, me, me.group);
        if (!curves_) {
            return std::nullopt;
        }
        motives_ = scoredMotives(me);
        noteInfluences();
        human_ = me.personData[kSpeciesPersonData] == 0;
        now_ = happiness(*curves_, motives_, human_);
        const auto [kind, ageFlag] = ageKind(me);
        ageFlag_ = ageFlag;
        origin_ = &me;
        room_ = s_.temps.empty() ? 0 : s_.temps[0];
        if (option(0x20)) {
            if (SimObject* o = s_.world.find(s_.temps.size() > 1 ? s_.temps[1] : 0); o != nullptr && placed(*o)) {
                origin_ = o;
            }
        }

        const Advertisers ads = Advertisers::build(s_.world, s_.code, kind);
        const auto counts = ads.counts(kind);
        const auto given = static_cast<std::uint16_t>(s_.operands[0] | s_.operands[1] << 8);
        std::uint16_t used = 0;
        // A later pass that finds no pick keeps the earlier one.
        std::optional<Candidate> pick;
        for (bool first = true;; first = false) {
            const bool automatic = given == 0 || (!first && option(0x01));
            if (!first && !automatic) {
                break;
            }
            const std::uint16_t mask = automatic
                ? worstMotives(motives_, human_, counts, used, static_cast<std::size_t>(std::max(0, s_.tunings.worstMotives)))
                : given;
            if (mask == 0) {
                break;
            }
            used = static_cast<std::uint16_t>(used | mask);
            gather(ads, kind, mask);
            scoreOffers();
            if (auto found = choose(first)) {
                pick = candidates_[*found];
            }
            if (pick && checks_ >= s_.tunings.maxChecks) {
                break;
            }
        }
        if (s_.scored != nullptr) {
            for (const Candidate& c : candidates_) {
                s_.scored->push_back({c.object, c.interaction, c.score, c.after, c.checked});
            }
        }
        if (!pick) {
            return std::nullopt;
        }
        const Candidate& best = *pick;
        if (me.personData[kSittingPersonData] != 0 && best.score < s_.tunings.sittingMinimum) {
            return std::nullopt;
        }
        return FoundInteraction{best.object, best.interaction, best.score, best.after, best.checked, best.icon, best.name, best.params};
    }

private:
    bool option(std::uint8_t bit) const { return (s_.operands[3] & bit) != 0; }

    void gather(const Advertisers& ads, std::size_t kind, std::uint16_t mask) {
        for (std::size_t motive = 0; motive < kMotiveCount; ++motive) {
            if ((mask & (1u << motive)) == 0) {
                continue;
            }
            for (const Advertiser& ad : ads.advertising(kind, motive)) {
                SimObject* object = s_.world.find(ad.object);
                if (object == nullptr || !mayUse(*object)) {
                    continue;
                }
                const auto table = interactionsOf(s_.code, *object).table;
                if (!table) {
                    continue;
                }
                // The game walks an object's lines last first.
                for (auto it = ad.lines.rbegin(); it != ad.lines.rend(); ++it) {
                    if (*it < table->interactions.size()) {
                        consider(*object, *table, table->interactions[*it]);
                    }
                }
            }
        }
    }

    bool mayUse(const SimObject& object) const {
        if (!placed(object) || inUse(object) || (object.data[kSkipFlagsField] & 5) != 0) {
            return false;
        }
        return !option(0x02) || object.data[kRoomField] == room_;
    }

    void consider(SimObject& object, const ttab::InteractionTable& table, const ttab::Interaction& advertising) {
        const SimObject& me = s_.me;
        if ((advertising.flags & kAgeFlags & ageFlag_) == 0 || (advertising.flags & kDebugLine) != 0 ||
            advertising.autonomy > static_cast<std::uint32_t>(std::max<std::int16_t>(0, me.personData[kAutonomyLevelPersonData]))) {
            return;
        }
        // The candidate is named by the line's id. The line it runs and scores is the first with it.
        // Ids above 0x7FFF don't fit my object data 0x14, so scripts couldn't run them.
        if (advertising.stringIndex > kMaxId) {
            return;
        }
        const auto id = static_cast<std::uint16_t>(advertising.stringIndex);
        if (!seen_.insert({object.id, id}).second) {
            return;
        }
        const ttab::Interaction* line = ttab::find(table, id);
        if (line == nullptr || line->action == 0 || line->action > kMaxTree) {
            return;
        }
        if (object.id == me.id && (line->flags & kOnSelf) == 0) {
            return;
        }
        // A check tree that changes the line runs first, and the line is scored as it left it. The
        // candidate's queue icon is the object, or the object type the line names with flag
        // 0x400000, as that tree left it (one run later, at the pick, no longer counts).
        ttab::Interaction changed;
        const ttab::Interaction* scoredLine = line;
        bool checked = false;
        InteractionIcon icon;
        if (line->flags & kOwnIconFlag) {
            icon.guid = line->iconGuid;
            icon.index = line->iconIndex;
        }
        if (s_.checkFirst(object, *line)) {
            // The lines it offers are kept even when the check fails. A line whose check offered lines
            // is not a candidate itself: only what it offered is.
            std::vector<AutonomyOffer> offered;
            const bool passed = runCheck(object, *line, changed, icon, offered);
            const bool offeredLines = !offered.empty();
            for (auto& o : offered) {
                offers_.push_back({object.id, id, std::move(o)});
            }
            if (!passed || offeredLines) {
                return;
            }
            scoredLine = &changed;
            checked = true;
        }
        score(object, id, *scoredLine, Candidate{object.id, id, 0, 0, checked, icon, {}, {}});
    }

    // Scores a candidate by `line`'s advertisements and keeps it when the score is high enough.
    void score(const SimObject& object, std::uint16_t id, const ttab::Interaction& line, Candidate c) {
        const float after = happiness(*curves_, advertisedMotives(line, s_.me, motives_), human_);
        const float fall = attenuationOf(line, s_.tunings, visitor_) * distance(*origin_, object, s_.tunings.floorCost) + 1.0f;
        const float value = (after - now_) / fall * remembered(object, id, line) * socialBias(object, line) * petBias(object);
        const float minimum = visitor_ ? s_.tunings.visitorMinimum : s_.tunings.familyMinimum;
        if (value >= minimum && value >= kLowestScore) {
            c.interaction = id;
            c.score = value;
            c.after = after;
            candidates_.push_back(std::move(c));
        }
    }

    // How much more a pet likes a line on a Sim when its relationship with that Sim has more than one
    // value and bit 0x800 of value 1 set: 1 + global constant 0x15B entry 11 / 100. Otherwise 1, also
    // for humans and on objects that aren't Sims.
    float petBias(const SimObject& object) const {
        constexpr std::uint16_t kPetConstants = 0x15B;
        constexpr std::size_t kPetBiasConstant = 11, kRelationshipFlags = 1;
        constexpr std::int32_t kBiasFlag = 0x800;
        if (human_ || !object.isPerson()) {
            return 1.0f;
        }
        const auto* values = s_.world.neighborRelationshipValues(s_.me.personData[kNeighborIdPersonData],
                                                                 object.personData[kNeighborIdPersonData]);
        if (values == nullptr || values->size() <= kRelationshipFlags || ((*values)[kRelationshipFlags] & kBiasFlag) == 0) {
            return 1.0f;
        }
        const auto* constants = s_.code.constants(kGlobalGroup, kPetConstants);
        if (constants == nullptr || constants->size() <= kPetBiasConstant) {
            return 1.0f;
        }
        return static_cast<float>(static_cast<double>((*constants)[kPetBiasConstant]) * static_cast<double>(0.01f) + 1.0);
    }

    // The Sims I am furious at and the places of those I'm furious at or attracted to, from the tokens
    // in my neighbor inventory, as they stand when the search starts. Only Sims on the lot count.
    void noteInfluences() {
        const std::int16_t mine = s_.me.personData[kNeighborIdPersonData];
        if (s_.tunings.biasOff || mine <= 0) {
            return;
        }
        const Inventory* inventory = s_.world.inventory(InventoryKind::Neighbor, mine);
        if (inventory == nullptr) {
            return;
        }
        for (const auto& token : inventory->singular) {
            if (token->guid == kFuriousToken && token->properties.size() > 1 && token->properties[0] != mine) {
                furious_.insert(token->properties[0]);
            }
        }
        const AutonomyTunings& t = s_.tunings;
        for (const auto& token : inventory->singular) {
            const auto& p = token->properties;
            if (token->guid == kFuriousToken && p.size() > 2) {
                addInfluence(p[0], -static_cast<float>(p[2]) * t.furiousWeight, t.furiousStart, t.furiousEnd, true);
            } else if (token->guid == kAttractionToken && p.size() > 1 && p[1] != mine && !furious_.contains(p[1])) {
                addInfluence(p[1], static_cast<float>(p[0]) * t.attractionWeight, t.attractionStart, t.attractionEnd, false);
            }
        }
    }

    // A source at the place of the Sim whose neighbor id is `neighbor`: `weight` hundredths, in full up
    // to `start` tiles away (at least 1) and falling to nothing at `end` (at least `start`).
    void addInfluence(std::int16_t neighbor, float weight, float start, float end, bool furious) {
        const auto ids = s_.world.idsIncludingRemoved([neighbor](const SimObject& o) {
            return !o.removed && o.isPerson() && o.personData[kNeighborIdPersonData] == neighbor;
        });
        const SimObject* sim = ids.empty() ? nullptr : s_.world.find(ids.front());
        if (sim == nullptr || !placed(*sim)) {
            return;
        }
        influences_.push_back(Influence{static_cast<double>(sim->x) / 65536.0, static_cast<double>(sim->y) / 65536.0, sim->level,
                                        sim->data[kRoomField], weight * 0.01f, std::max(1.0f, start), std::max(start, end), furious});
    }

    // The social bias of a candidate on `object`: the sources on its level and in its room, summed by
    // their falloff at its place, give s. The bias is (s + 1) to the positive tuning when s is not
    // negative, else (1 / (1 - s)) to the negative tuning, at least 0.0001. A line flagged 0x800000
    // ("preferred by furious") on a Sim I am furious at is worth `preferred furious` times that, and
    // its sum leaves out the furious sources.
    float socialBias(const SimObject& object, const ttab::Interaction& line) const {
        const AutonomyTunings& t = s_.tunings;
        if (t.biasOff || !placed(object)) {
            return 1.0f;
        }
        float factor = 1.0f;
        bool atFurious = false;
        if ((line.flags & kPreferredByFurious) && object.isPerson() && furious_.contains(object.personData[kNeighborIdPersonData])) {
            atFurious = true;
            factor = t.preferredFurious;
        }
        const double x = static_cast<double>(object.x) / 65536.0, y = static_cast<double>(object.y) / 65536.0;
        double sum = 0;
        for (const Influence& source : influences_) {
            if (source.level != object.level || source.room != object.data[kRoomField] || (atFurious && source.furious)) {
                continue;
            }
            const double d = std::hypot(x - source.x, y - source.y);
            const double share = d <= source.start ? 1.0 : d < source.end ? (source.end - d) / (source.end - source.start) : 0.0;
            sum += share * source.weight;
        }
        if (sum >= 0) {
            return factor * static_cast<float>(std::pow(sum + 1.0, t.biasPositive));
        }
        return std::max(0.0001f, factor * static_cast<float>(std::pow(1.0 / (1.0 - sum), t.biasNegative)));
    }

    // The line's memory multiplier while I remember picking it on this object, else 1.
    float remembered(const SimObject& object, std::uint16_t id, const ttab::Interaction& line) const {
        const std::int16_t key = rememberedAs(object);
        const std::uint64_t tick = s_.world.ticks();
        for (const AutonomyMemory& m : s_.me.autonomyMemory) {
            if (m.object == key && m.interaction == id && tick < m.until) {
                return line.memoryMultiplier;
            }
        }
        return 1.0f;
    }

    // Every line offered so far, as a candidate already checked, with its own name and params.
    void scoreOffers() {
        for (const Offered& o : offers_) {
            if (const SimObject* object = s_.world.find(o.object)) {
                score(*object, o.interaction, o.offer.line,
                      Candidate{o.object, o.interaction, 0, 0, true, o.offer.icon, o.offer.name, o.offer.params});
            }
        }
    }

    bool runCheck(SimObject& object, const ttab::Interaction& line, ttab::Interaction& changed, InteractionIcon& icon,
                  std::vector<AutonomyOffer>& offered) {
        ++checks_;
        if (s_.mayStart && !s_.mayStart(object, line)) {
            return false;
        }
        // A check tree hands back the line as it left it, so the line is copied here only without one.
        if (line.test == 0) {
            changed = line;
            return true;
        }
        return s_.check(object, line, changed, icon, offered);
    }

    // The k-th candidate, best first, that passes its check, where k is 0 or, for residents, one of the
    // best `random selection count` at random. Residents draw even when only one can come up, as the
    // game does. Checks not run yet run now, for the candidates up to the pick.
    std::optional<std::size_t> choose(bool firstPass) {
        std::stable_sort(candidates_.begin(), candidates_.end(),
                         [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::size_t k = 0;
        const auto n = std::min<std::size_t>(candidates_.size(), static_cast<std::size_t>(std::max(0, s_.tunings.randomSelection)));
        if (!visitor_ && !candidates_.empty()) {
            k = static_cast<std::size_t>((static_cast<std::uint64_t>(static_cast<std::uint32_t>(s_.rng())) * n) >> 32);
        }
        std::size_t valid = 0;
        for (std::size_t i = 0; i < candidates_.size(); ++i) {
            if (!firstPass && checks_ > s_.tunings.maxChecks) {
                break;
            }
            Candidate& c = candidates_[i];
            if (!c.checked) {
                SimObject* object = s_.world.find(c.object);
                const auto table = object != nullptr ? interactionsOf(s_.code, *object).table : nullptr;
                const ttab::Interaction* line = table ? ttab::find(*table, c.interaction) : nullptr;
                // What this check sets and offers comes too late to count, but a check that offers
                // lines still rules the line out.
                ttab::Interaction changed;
                InteractionIcon unusedIcon = c.icon;
                std::vector<AutonomyOffer> unusedOffers;
                if (line == nullptr || !runCheck(*object, *line, changed, unusedIcon, unusedOffers) || !unusedOffers.empty()) {
                    continue;
                }
            }
            if (valid == k) {
                return i;
            }
            ++valid;
        }
        return std::nullopt;
    }

    const InteractionSearch& s_;
    const bool visitor_;
    std::optional<ScoreCurves> curves_;  // this Sim's, while the search runs
    bool human_ = true;                  // whether I am a human, not a pet
    std::array<float, kMotiveCount> motives_{};
    struct Influence {
        double x, y;  // in tiles
        std::int32_t level;
        std::int16_t room;
        float weight, start, end;
        bool furious;
    };
    std::set<std::int16_t> furious_;  // neighbor ids
    std::vector<Influence> influences_;
    float now_ = 0;
    std::uint32_t ageFlag_ = 0;
    const SimObject* origin_ = nullptr;
    std::int16_t room_ = 0;  // temp 0 as the search began: check trees may change the temps
    std::vector<Candidate> candidates_;
    // The lines check trees offered, with the candidate (object, line id) that offered them.
    struct Offered {
        std::int16_t object = 0;
        std::uint16_t interaction = 0;
        AutonomyOffer offer;
    };
    std::vector<Offered> offers_;
    std::set<std::pair<std::int16_t, std::uint16_t>> seen_;
    int checks_ = 0;
};

// Private trees come from the code owner, semi-global ones from its semi-global group.
std::uint32_t treeGroup(CodeSource& code, std::uint16_t tree, std::uint32_t codeOwner) {
    return tree < 0x1000 ? kGlobalGroup : tree < 0x2000 ? codeOwner : code.semiGlobalGroup(codeOwner);
}

bool assigns(std::uint8_t op) {
    return (op >= 3 && op <= 7) || (op >= 9 && op <= 13) || (op >= 17 && op <= 21);
}

} // namespace

bool CheckTreeKinds::visit(std::uint32_t codeOwner, std::uint16_t tree, int depth) {
    constexpr std::uint16_t kExpression = 0x0002;
    constexpr std::uint16_t kRunFunctionalTree = 0x0014;
    constexpr std::uint16_t kRunTreeByName = 0x001C;
    constexpr std::uint16_t kActionString = 0x0032;
    constexpr std::uint16_t kLua = 0x007E;
    constexpr std::uint16_t kFirstTreeCall = 0x0100;
    // Deeper calls than any game tree makes are taken as changing nothing.
    constexpr int kMaxDepth = 64;
    if (depth > kMaxDepth) {
        diag::warnOnce("autonomy: check trees calling deeper than 64 levels are taken as changing nothing");
        return false;
    }
    const std::uint32_t group = treeGroup(code_, tree, codeOwner);
    const std::pair key{group, tree};
    if (const auto it = known_.find(key); it != known_.end()) {
        return it->second;
    }
    // A tree that calls itself back sees what is known so far, as in the game.
    known_[key] = false;
    const auto code = code_.behavior(group, tree);
    if (!code) {
        return false;
    }
    bool changes = false;
    for (const auto& in : code->instructions) {
        if (in.opcode == kLua || in.opcode == kActionString || in.opcode == kRunFunctionalTree || in.opcode == kRunTreeByName) {
            changes = true;
        } else if (in.opcode == kExpression) {
            const auto x = bhav::decodeExpression(in);
            changes = assigns(x.op) && (x.lhsOwner == 0x0C || x.lhsOwner == 0x1C || x.lhsOwner == 0x1D);
        } else if (in.opcode >= kFirstTreeCall) {
            changes = visit(codeOwner, in.opcode, depth + 1);
        }
        if (changes) {
            break;
        }
    }
    known_[key] = changes;
    return changes;
}

AutonomyTunings AutonomyTunings::from(const fcns::Tunings* tunings) {
    AutonomyTunings t;
    if (tunings == nullptr) {
        return t;
    }
    auto read = [&](const char* name, float& value) {
        if (const auto v = tunings->find(name); v && std::isfinite(*v)) {
            value = *v;
        }
    };
    auto readCount = [&](const char* name, int& value) {
        if (const auto v = tunings->find(name); v && std::isfinite(*v)) {
            value = static_cast<int>(std::clamp(*v, 0.0f, 100000.0f));
        }
    };
    read("min autonomy score for family", t.familyMinimum);
    read("min autonomy score for visitors", t.visitorMinimum);
    read("min autonomy score for sitting", t.sittingMinimum);
    readCount("random selection count", t.randomSelection);
    readCount("random worst motive count", t.worstMotives);
    readCount("max check tree in fba count", t.maxChecks);
    readCount("autonomy memory count", t.memoryCount);
    readCount("max duration autonomy memory ", t.memoryDuration);
    int biasOff = 0;
    readCount("autonomy deactivate autonomy bias", biasOff);
    t.biasOff = biasOff != 0;
    read("autonomy bias positive tuning", t.biasPositive);
    read("autonomy bias negative tuning", t.biasNegative);
    read("autonomy overall furious tuning", t.furiousWeight);
    read("autonomy preferred furious interaction tuning", t.preferredFurious);
    read("autonomy furious falloff start distance", t.furiousStart);
    read("autonomy furious falloff end distance", t.furiousEnd);
    read("autonomy overall attraction tuning", t.attractionWeight);
    read("autonomy attraction falloff start distance", t.attractionStart);
    read("autonomy attraction falloff end distance", t.attractionEnd);
    read("autonomy floor to floor distance cost", t.floorCost);
    read("low attenuation", t.attenuation[0]);
    read("moderate attenuation", t.attenuation[1]);
    read("high attenuation", t.attenuation[2]);
    read("visitor low attenuation", t.visitorAttenuation[0]);
    read("visitor moderate attenuation", t.visitorAttenuation[1]);
    read("visitor high attenuation", t.visitorAttenuation[2]);
    return t;
}

float attenuationOf(const ttab::Interaction& line, const AutonomyTunings& tunings, bool visitor) {
    switch (line.attenuationCode) {
    case 0: return line.attenuation;
    case 1: return 0;
    case 2: return tunings.attenuation[0];
    case 3: return visitor ? tunings.visitorAttenuation[1] : tunings.attenuation[1];
    case 4: return visitor ? tunings.visitorAttenuation[2] : tunings.attenuation[2];
    default: return 0.02f;
    }
}

std::optional<FoundInteraction> findBestInteraction(const InteractionSearch& search) {
    SimObject& me = search.me;
    const std::uint64_t tick = search.world.ticks();
    me.data[kTreeTableEntryField] = -1;
    // With Free Will off the game skips the search for a human Sim that is not an NPC (Person Type
    // 0), is selectable (bit 1 of Selection Flags) and lacks bit 0x800 of person data 0x54, whose
    // meaning is not known yet. Pets keep choosing for themselves.
    const bool stoppedByFreeWill = !search.freeWill && me.personData[kPersonTypePersonData] == 0 &&
                                   (me.personData[kSelectionFlagsPersonData] & kSelectable) != 0 &&
                                   me.personData[kSpeciesPersonData] == 0 && (me.personData[kPersonFlagsData] & kFreeWillCheckFlag) == 0;
    if (stoppedByFreeWill || me.autonomyTick == tick) {
        return std::nullopt;
    }
    me.autonomyTick = tick;
    if (search.operands.size() < 4) {
        return std::nullopt;
    }
    forgetOldPicks(me.autonomyMemory, tick, search.tunings.memoryCount);
    auto found = Search(search).run();
    if (found) {
        if (const SimObject* object = search.world.find(found->object)) {
            const auto until = tick + static_cast<std::uint64_t>(std::max(0, search.tunings.memoryDuration));
            me.autonomyMemory.push_back({rememberedAs(*object), found->interaction, until});
        }
        me.data[kTreeTableEntryField] = static_cast<std::int16_t>(found->interaction);
        me.personData[kLastAutonomyObjectPersonData] = found->object;
        me.foundIcon = found->icon;
        me.foundName = found->name;
        me.foundParams = found->params;
    }
    return found;
}

} // namespace ts2::sim
