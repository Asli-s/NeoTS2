#include "sim/family_picture.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <memory>
#include <set>

namespace ts2::sim {

namespace {

using famt::TieType;

// Kinds of Sim, which pick the slot queue and pose prefix. Human young adults count as adults.
enum Kind : int {
    kAdult = 0, kChild = 1, kToddler = 2, kTeen = 3, kElder = 4,
    kDog = 5, kDogElder = 6, kLargeDog = 7, kLargeDogElder = 8, kCat = 9, kCatElder = 10,
    kPuppy = 11, kKitten = 12, kBaby = 13, kKinds = 14,
};

// Life stages by person data 0x3A: elders over 50, adults over 18, teens over 15, children over 2,
// toddlers at 2 and babies below.
Kind kindOf(const FamilyPictureSim& s) {
    const int age = s.age;
    switch (s.species) {
        case 0:
            return age > 50 ? kElder : age > 18 ? kAdult : age > 15 ? kTeen : age > 2 ? kChild : age > 1 ? kToddler : kBaby;
        case 1:
            return age > 50 ? kDogElder : age > 18 ? kDog : kPuppy;
        case 2:
            return age > 50 ? kLargeDogElder : age > 18 ? kLargeDog : kPuppy;
        case 3:
            return age > 50 ? kCatElder : age > 18 ? kCat : kKitten;
        default:
            return kAdult;
    }
}

bool isHuman(const FamilyPictureSim& s) { return s.species == 0; }
bool isBaby(const FamilyPictureSim& s) { return isHuman(s) && s.age >= 0 && s.age <= 1; }

// The per-Sim table is a hash table of 193 buckets by neighbor id, walked bucket by bucket.
constexpr int kTableBuckets = 193;

struct Entry {
    FamilyPictureSim sim;
    Kind kind = kAdult;
    int slot = 0;
    std::string animation;
    bool disgruntled = false;
    bool grouped = false;
};

// Sims tied by spouse, child and sibling ties, with the couple or first adult who holds its babies.
struct Group {
    Entry* holderA = nullptr;
    Entry* holderB = nullptr;
    std::array<std::vector<Entry*>, kKinds> byKind;
};

class Arranger {
public:
    Arranger(const std::vector<FamilyPictureSim>& members, const FamilyPictureFacts& facts) : facts_(facts) {
        entries_.reserve(members.size());
        for (const auto& m : members) {
            entries_.push_back({m, kindOf(m)});
        }
        for (auto& e : entries_) {
            byKind_[e.kind].push_back(&e);
            byId_[e.sim.id] = &e;
        }
        // Walk order: bucket by bucket. Two Sims sharing a bucket would need the chain order.
        std::set<int> buckets;
        for (auto& e : entries_) {
            if (!buckets.insert(static_cast<std::uint16_t>(e.sim.id) % kTableBuckets).second) {
                fail("two household members share a bucket of the per-Sim table");
            }
            tableOrder_.push_back(&e);
        }
        std::stable_sort(tableOrder_.begin(), tableOrder_.end(), [](const Entry* a, const Entry* b) {
            return static_cast<std::uint16_t>(a->sim.id) % kTableBuckets < static_cast<std::uint16_t>(b->sim.id) % kTableBuckets;
        });
    }

    FamilyPicture run() {
        buildGroups();
        fillQueues();
        assignSlots();
        for (Entry* e : tableOrder_) {
            soloPose(*e);
        }
        FamilyPicture out;
        out.camera = camera();
        for (const auto& e : entries_) {
            // simMark_10 is in no game package, so a Sim given slot 10 is left out, as the game leaves it.
            if (e.slot >= 1 && e.slot <= 9 && !e.animation.empty()) {
                out.sims.push_back({e.sim.id, e.slot, e.animation});
            }
        }
        out.unknown = unknown_;
        return out;
    }

private:
    void fail(const std::string& why) {
        if (unknown_.empty()) {
            unknown_ = why;
        }
    }

    Entry* find(std::int16_t id) {
        const auto it = byId_.find(id);
        return it == byId_.end() ? nullptr : it->second;
    }

    std::optional<std::int16_t> firstTie(std::int16_t id, TieType type) const {
        const auto list = facts_.ties ? facts_.ties(id, type) : std::vector<std::int16_t>{};
        return list.empty() ? std::nullopt : std::optional(list.front());
    }

    // Puts `e` in group `g` unless it is in one, and says whether it was put there.
    bool join(Entry& e, Group& g) {
        if (e.grouped) {
            return false;
        }
        e.grouped = true;
        g.byKind[e.kind].push_back(&e);
        groupOf_[e.sim.id] = &g;
        return true;
    }

    // Gathers into `id`'s group its spouse, children and siblings in the picture, and theirs.
    void gather(std::int16_t id, int depth) {
        // A tie chain can't be longer than the household, which also stops a loop of ties.
        if (depth > static_cast<int>(entries_.size())) {
            return;
        }
        Group* g = groupOf_.contains(id) ? groupOf_[id] : nullptr;
        if (g == nullptr) {
            groups_.push_back(std::make_unique<Group>());
            g = groups_.back().get();
            if (Entry* e = find(id)) {
                join(*e, *g);
            }
        }
        const auto follow = [&](std::int16_t other) {
            if (Entry* o = find(other); o != nullptr && join(*o, *g)) {
                gather(other, depth + 1);
            }
        };
        if (const auto spouse = firstTie(id, TieType::Spouse)) {
            follow(*spouse);
        }
        for (const auto type : {TieType::Child, TieType::Sibling}) {
            for (const std::int16_t other : facts_.ties ? facts_.ties(id, type) : std::vector<std::int16_t>{}) {
                follow(other);
            }
        }
    }

    void gatherKind(Kind kind) {
        for (Entry* e : byKind_[kind]) {
            if (!e->grouped) {
                gather(e->sim.id, 0);
            }
        }
    }

    // Couples: a human aged 19 to 50 whose spouse is in the picture, walking the table from its start
    // again after each couple found.
    void findCouples() {
        std::vector<Entry*> left = tableOrder_;
        for (std::size_t i = 0; i < left.size();) {
            Entry& e = *left[i];
            const auto spouse = isHuman(e.sim) && e.sim.age > 18 && e.sim.age <= 50 ? firstTie(e.sim.id, TieType::Spouse) : std::nullopt;
            Entry* partner = spouse ? find(*spouse) : nullptr;
            if (partner == nullptr) {
                ++i;
                continue;
            }
            couples_.push_back({&e, partner});
            std::erase(left, &e);
            std::erase(left, partner);
            i = 0;
        }
    }

    void buildGroups() {
        gatherKind(kElder);
        gatherKind(kAdult);
        findCouples();
        for (const auto& [a, b] : couples_) {
            if (Group* g = groupOf_.contains(a->sim.id) ? groupOf_[a->sim.id] : nullptr) {
                g->holderA = a;
                g->holderB = b;
            }
        }
        for (Entry* e : byKind_[kAdult]) {
            if (Group* g = groupOf_.contains(e->sim.id) ? groupOf_[e->sim.id] : nullptr; g != nullptr && g->holderA == nullptr) {
                g->holderA = e;
            }
        }
        for (const Kind k : {kTeen, kToddler, kChild, kBaby}) {
            gatherKind(k);
        }
        for (int k = kDog; k <= kKitten; ++k) {
            gatherKind(static_cast<Kind>(k));
        }
    }

    std::size_t count(std::initializer_list<Kind> kinds) const {
        std::size_t n = 0;
        for (const Kind k : kinds) {
            n += byKind_[k].size();
        }
        return n;
    }
    std::size_t petCount() const { return count({kDog, kDogElder, kLargeDog, kLargeDogElder, kCat, kCatElder, kPuppy, kKitten}); }
    std::size_t standingCount() const { return count({kAdult, kTeen, kElder}); }
    std::size_t youngCount() const { return count({kChild, kToddler}); }

    bool taken(int slot) const {
        for (const auto& q : {coupleSlots_, standingSlots_, youngSlots_, petSlots_}) {
            if (std::find(q.begin(), q.end(), slot) != q.end()) {
                return true;
            }
        }
        return false;
    }

    // Each queue takes, per Sim it serves, the next of its candidate slots no queue holds yet.
    void fillFrom(std::vector<int>& queue, std::size_t sims, std::initializer_list<int> candidates, bool young) {
        const std::vector<int> c(candidates);
        std::size_t next = 0;
        for (std::size_t i = 0; i < sims; ++i) {
            while (next < c.size() && taken(c[next])) {
                ++next;
            }
            if (next < c.size()) {
                queue.push_back(c[next++]);
            } else if (young) {
                fail("a child or toddler with every candidate slot taken");
            }
        }
    }

    void fillQueues() {
        const std::size_t n = couples_.size();
        if (n == 1) {
            coupleSlots_ = {2, 3};
        } else if (n > 4) {
            fail("more than four couples");
        } else {
            constexpr int kPairs[4][2] = {{1, 2}, {3, 4}, {7, 8}, {5, 6}};
            for (std::size_t i = 0; i < n; ++i) {
                coupleSlots_.push_back(kPairs[i][0]);
                coupleSlots_.push_back(kPairs[i][1]);
            }
        }
        const std::size_t standing = standingCount() >= 2 * n ? standingCount() - 2 * n : 0;
        fillFrom(standingSlots_, standing, {2, 3, 1, 4, 5, 6, 8, 7}, false);
        fillFrom(petSlots_, petCount(), {7, 6, 8, 5, 9, 10}, false);
        fillFrom(youngSlots_, youngCount(), {7, 8, 6, 4, 5, 1, 3, 2}, true);
    }

    static int pop(std::vector<int>& queue) {
        const int slot = queue.front();
        queue.erase(queue.begin());
        return slot;
    }

    // Gives a Sim without a slot the next one of its queue: pets', under-16s' or standing Sims'.
    void assign(Entry* e) {
        if (e == nullptr || e->slot != 0) {
            return;
        }
        auto& queue = !isHuman(e->sim) ? petSlots_ : e->sim.age < 16 ? youngSlots_ : standingSlots_;
        if (queue.empty()) {
            fail("a Sim whose slot queue is empty");
            return;
        }
        e->slot = pop(queue);
    }

    // `holder` holds `held` (a baby in one arm, else a toddler in both). The held Sim shares its slot.
    void hold(Entry* holder, Entry* held, bool rightArm) {
        if (holder == nullptr || holder->slot == 0 || held == nullptr) {
            return;
        }
        held->slot = holder->slot;
        if (isBaby(held->sim)) {
            holder->animation = rightArm ? "A2b-portraitPose-holdRightArm_anim" : "A2b-portraitPose-holdLeftArm_anim";
            held->animation = rightArm ? "b2a-portraitPose-holdRightArm_anim" : "b2a-portraitPose-holdLeftArm_anim";
        } else {
            holder->animation = "a2p-portraitPose-holdInArms_anim";
            held->animation = "p2a-portraitPose-holdInArms_anim";
        }
    }

    // A group's babies held by its couple (`b` set) or by one holder.
    void holdBabies(Entry* a, Entry* b, const std::vector<Entry*>& babies) {
        if (a == nullptr || babies.empty()) {
            return;
        }
        // A couple holds up to four babies, and none when there are more.
        if (b != nullptr && babies.size() > 4) {
            return;
        }
        if (b != nullptr && babies.size() == 1) {
            a->animation = "a2a-couple-simA-portraitPose-coupleHoldingBaby_anim";
            b->animation = "a2b-couple-simB-portraitPose-coupleHoldingBaby_anim";
            babies[0]->animation = "b2a-couple-simB-portraitPose-coupleHoldingBaby_anim";
            babies[0]->slot = b->slot;
            return;
        }
        if (b != nullptr && babies.size() == 2) {
            hold(a, babies[0], false);
            hold(b, babies[1], true);
            return;
        }
        if (b == nullptr && babies.size() == 1) {
            hold(a, babies[0], false);
            return;
        }
        fail("one Sim holding two babies");
    }

    // The pose of a couple standing side by side: chosen by its GUID in the slots up to 3, else by
    // slot. One partner without the in-love flag (relationship variable 1, bit 1) towards
    // the other stands alone and disgruntled. Both without it pose disgruntled together.
    void couplePose(Entry& a, Entry& b) {
        if (a.slot == 0 || b.slot == 0) {
            return;
        }
        const auto noLove = [&](const Entry& from, const Entry& to) {
            const auto r = facts_.relationship ? facts_.relationship(from.sim.id, to.sim.id) : std::nullopt;
            return r && (r->size() < 2 || ((*r)[1] & 2) == 0);
        };
        if (facts_.relationship && facts_.relationship(a.sim.id, b.sim.id) && facts_.relationship(b.sim.id, a.sim.id)) {
            a.disgruntled = noLove(a, b);
            b.disgruntled = noLove(b, a);
        }
        const bool both = a.disgruntled && b.disgruntled;
        if (a.slot + 1 == b.slot) {
            const std::uint32_t r = a.sim.guid % 3;
            if (a.slot <= 3 && r < 2) {
                // GUID % 3 = 1 gives kissCheek and 0 bunnyEars. See SV-28 in docs/open-questions.md.
                const char* pose = r == 1 ? "kissCheek" : "bunnyEars";
                a.animation = std::format("a2a-couple-simA-portraitPose-{}_anim", pose);
                b.animation = std::format("a2a-couple-simB-portraitPose-{}_anim", pose);
            } else {
                const int s = a.slot;
                const char* pose = s == 3 ? "armOnShoulder" : s == 5 ? "kneelingArmOnShoulder" : s == 7 ? "kneelingSquatBeside" : "armBehindBack";
                const char* mood = both ? "Disgruntled" : "";
                a.animation = std::format("a2a-couple-simA-portraitPose{}-{}_anim", mood, pose);
                b.animation = std::format("a2a-couple-simB-portraitPose{}-{}_anim", mood, pose);
            }
        }
        if ((a.disgruntled || b.disgruntled) && !both) {
            Entry& sulky = a.disgruntled ? a : b;
            Entry& other = a.disgruntled ? b : a;
            sulky.animation = std::format("a-solo-portraitPoseDisgruntled-slot0{}_anim", sulky.slot);
            other.animation = std::format("a-solo-{}portraitPose-slot0{}_anim", other.sim.gender == 1 ? "female-" : "male-", other.slot);
        }
    }

    void assignSlots() {
        for (const auto& [a, b] : couples_) {
            if (coupleSlots_.size() < 2) {
                fail("a couple without slots");
                break;
            }
            a->slot = pop(coupleSlots_);
            b->slot = pop(coupleSlots_);
            if (Group* g = groupOf_.contains(a->sim.id) ? groupOf_[a->sim.id] : nullptr) {
                holdBabies(a, b, g->byKind[kBaby]);
            }
            if (a->animation.empty() && b->animation.empty()) {
                couplePose(*a, *b);
            }
        }
        for (Entry* e : tableOrder_) {
            if (!isHuman(e->sim)) {
                assign(e);
            }
        }
        for (const auto& g : groups_) {
            if (!g->byKind[kBaby].empty()) {
                Entry* a = g->holderA;
                Entry* b = g->holderB;
                const bool aFree = a != nullptr && a->slot == 0;
                const bool bFree = b != nullptr && b->slot == 0;
                if (aFree || bFree) {
                    Entry* holder = a != nullptr ? a : b;
                    assign(holder);
                    holdBabies(holder, nullptr, g->byKind[kBaby]);
                }
            }
            // A toddler is held by the first adult of its group without a slot, else elder, else teen.
            for (Entry* toddler : g->byKind[kToddler]) {
                bool held = false;
                for (const Kind k : {kAdult, kElder, kTeen}) {
                    for (Entry* h : g->byKind[k]) {
                        if (h->slot == 0) {
                            assign(h);
                            hold(h, toddler, false);
                            held = true;
                            break;
                        }
                    }
                    if (held) {
                        break;
                    }
                }
            }
        }
        for (Entry* e : tableOrder_) {
            assign(e);
        }
    }

    // A teen is disgruntled when its daily relationship (variable 0) to its father or mother in the
    // picture is 0 or less.
    bool teenSulks(const Entry& teen) const {
        bool sulks = teen.disgruntled;
        for (const auto type : {TieType::Father, TieType::Mother}) {
            const auto parent = firstTie(teen.sim.id, type);
            if (parent && byId_.contains(*parent) && facts_.relationship) {
                if (const auto r = facts_.relationship(teen.sim.id, *parent)) {
                    sulks = sulks || r->empty() || (*r)[0] <= 0;
                }
            }
        }
        return sulks;
    }

    void soloPose(Entry& e) {
        if (!e.animation.empty() || e.slot == 0) {
            return;
        }
        // The game asks these sets of slots for the poses that fit each place.
        const auto front = [](int s) { return s != 0 && s < 8 && s != 6; };
        const auto childPlace = [](int s) { return s != 0 && s < 8 && s != 4; };
        const auto teenPlace = [](int s) { return s == 1 || s == 2 || s == 5 || s == 6; };
        int n = e.slot;
        if (entries_.size() == 1 && isHuman(e.sim)) {
            // Alone in the picture, a Sim takes a pose by its GUID among the slots like its own.
            const bool want = front(e.slot);
            for (std::uint32_t k = 1; k <= 10; ++k) {
                n = static_cast<int>((e.sim.guid + k) % 10 + 1);
                if (front(n) == want) {
                    break;
                }
            }
        }
        if (n > 10) {
            n = 1;
        }
        static constexpr const char* kPrefix[kKinds] = {"a", "c", "p", "a", "a", "d", "d", "l", "l", "f", "f", "w", "k", "p"};
        const char* prefix = kPrefix[e.kind];
        if (isHuman(e.sim)) {
            const bool grown = e.kind == kAdult || e.kind == kTeen || e.kind == kElder;
            const char* gender = grown ? (e.sim.gender == 1 ? "female-" : "male-") : "";
            // The game writes a human's slot after a fixed 0, unlike a pet's two digits. A lone
            // human's pose number 10 so names "slot010", which no package has. The game then places
            // the Sim without a pose.
            e.animation = std::format("{}-solo-{}portraitPose-slot0{}_anim", prefix, gender, n);
        } else {
            e.animation = std::format("{}-solo-portraitPose-slot{:02}_anim", prefix, n);
        }
        const std::uint32_t r = e.sim.guid % 3;
        if (e.kind == kChild && childPlace(e.slot)) {
            if (r == 0) {
                e.animation = "c-solo-portraitPose-handsOnHead_anim";
            } else if (r == 1) {
                e.animation = "c-solo-portraitPose-handsInAir_anim";
            }
        } else if (e.kind == kTeen) {
            e.disgruntled = teenSulks(e);
            if (e.disgruntled) {
                e.animation = std::format("t-solo-portraitPoseDisgruntled-slot0{}_anim", e.slot);
            } else if (r == 0 && teenPlace(e.slot)) {
                e.animation = "t-solo-portraitPose-pointFront_anim";
            } else if (r == 1 && (teenPlace(e.slot) || e.slot == 7)) {
                e.animation = "t-solo-portraitPose-armsCrossedCool_anim";
            }
        }
    }

    // The camera by household size: F for a few standing Sims with children, D for big households.
    std::string camera() {
        static constexpr const char* kBySize[11] = {"E", "A", "A", "A", "B", "C", "D", "D", "D", "G", "G"};
        const std::size_t n = entries_.size();
        std::string letter;
        if (n >= 11) {
            letter = "D";
        } else if (petCount() == 0) {
            letter = n <= 8 && standingCount() <= 4 && youngCount() > 0 ? "F" : kBySize[n];
        } else if (petCount() == n) {
            letter = "H";
        } else {
            letter = "D";
            fail("a household with pets, whose camera choice is not known yet");
        }
        // Cameras G and H are in no game package.
        if (letter == "G" || letter == "H") {
            fail("camera " + letter + ", which no game package holds");
        }
        return letter;
    }

    const FamilyPictureFacts& facts_;
    std::vector<Entry> entries_;
    std::array<std::vector<Entry*>, kKinds> byKind_;
    std::map<std::int16_t, Entry*> byId_;
    std::vector<Entry*> tableOrder_;
    std::vector<std::unique_ptr<Group>> groups_;
    std::map<std::int16_t, Group*> groupOf_;
    std::vector<std::pair<Entry*, Entry*>> couples_;
    std::vector<int> coupleSlots_, standingSlots_, youngSlots_, petSlots_;
    std::string unknown_;
};

} // namespace

FamilyPicture arrangeFamilyPicture(const std::vector<FamilyPictureSim>& members, const FamilyPictureFacts& facts) {
    // A household holds at most a few Sims. A broken file listing more is left without a picture.
    constexpr std::size_t kMostMembers = 64;
    if (members.empty() || members.size() > kMostMembers) {
        return {"", {}, "an empty or oversized household"};
    }
    return Arranger(members, facts).run();
}

} // namespace ts2::sim
