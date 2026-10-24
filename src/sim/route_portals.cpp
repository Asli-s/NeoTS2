#include "sim/route_portals.hpp"

#include "formats/objd.hpp"
#include "sim/route_goals.hpp"
#include "sim/snap.hpp"

#include <algorithm>
#include <map>

namespace ts2::sim {

namespace {

constexpr std::uint16_t kModelBones = 0x90;
constexpr std::uint32_t kTargetNamed = 1u << 21;  // the slot's name entry names the slot it leads to
constexpr std::size_t kLeadField = 0x13;
constexpr std::uint16_t kPortalFunction = 15;  // OBJf entry
constexpr std::uint16_t kFirstStairType = 10, kLastStairType = 12;
constexpr std::uint16_t kStairPortalType = 12;  // a modular stair's top part
constexpr std::size_t kNextDownField = 0x50;    // the next part down a modular stair
constexpr int kMostStairParts = 64;             // broken links could make a loop

struct Entry {
    std::string name;
    StairPortal::End end;
};

bool hasPortalFunction(CodeSource& code, const ObjectType& type) {
    const auto* functions = code.objectFunctions(type);
    return functions != nullptr && kPortalFunction < functions->size() && (*functions)[kPortalFunction].action != 0;
}

} // namespace

std::string partnerSlotName(std::string_view name) {
    static constexpr std::pair<std::string_view, std::string_view> kSwaps[] = {
        {"_bottom", "_top"}, {"_top", "_bottom"}, {"_nw", "_se"}, {"_se", "_nw"}, {"_ne", "_sw"},
        {"_sw", "_ne"},      {"_n", "_s"},        {"_s", "_n"},   {"_e", "_w"},   {"_w", "_e"},
    };
    for (const auto& [from, to] : kSwaps) {
        if (name.size() > from.size() && name.substr(name.size() - from.size()) == from) {
            return std::string(name.substr(0, name.size() - from.size())) + std::string(to);
        }
    }
    return {};
}

std::vector<StairPortal> stairPortals(const World& world, CodeSource& code) {
    // The stairs' parts by their lead, in id order.
    std::map<std::int16_t, std::vector<const SimObject*>> stairs;
    world.nextAfter(std::numeric_limits<std::int16_t>::min(), [&](const SimObject& o) {
        if (o.isPerson() || o.removed || o.x == kOffLot || o.y == kOffLot) {
            return false;
        }
        const ObjectType* type = code.objectType(o.definition.guid());
        const std::uint16_t kind = type != nullptr ? type->definition.field(objd::field::kType) : 0;
        if (kind >= kFirstStairType && kind <= kLastStairType) {
            stairs[o.data[kLeadField] != 0 ? o.data[kLeadField] : o.id].push_back(&o);
        }
        return false;
    });
    std::vector<StairPortal> out;
    // A modular stair's top part brings its last part down (the chain of object data 0x50).
    const auto lastDown = [&](const SimObject& top) {
        const SimObject* at = &top;
        for (int i = 0; i < kMostStairParts && at->data[kNextDownField] != 0; ++i) {
            const SimObject* next = world.find(at->data[kNextDownField]);
            if (next == nullptr || next == &top) {
                break;
            }
            at = next;
        }
        return at;
    };
    for (auto& [lead, group] : stairs) {
        std::sort(group.begin(), group.end(), [](const SimObject* a, const SimObject* b) { return a->id < b->id; });
        std::vector<const SimObject*> parts;
        for (const SimObject* part : group) {
            parts.push_back(part);
            const ObjectType* type = code.objectType(part->definition.guid());
            if (type != nullptr && type->definition.field(objd::field::kType) == kStairPortalType) {
                if (const SimObject* end = lastDown(*part); end != part) {
                    parts.push_back(end);
                }
            }
        }
        std::vector<Entry> entries;
        std::vector<std::pair<std::string, std::string>> pairs;
        const auto known = [&](const std::string& name) {
            return std::any_of(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; });
        };
        for (const SimObject* part : parts) {
            const ObjectType* type = code.objectType(part->definition.guid());
            if (type == nullptr) {
                continue;
            }
            for (std::size_t i = 0; i < type->routingSlots.size(); ++i) {
                const slot::Entry& slot = type->routingSlots[i];
                if (!isBoneSlot(slot) || slot.bone < 0) {
                    continue;
                }
                const auto text = code.string(type->group, kModelBones, static_cast<std::size_t>(slot.bone));
                if (!text) {
                    continue;
                }
                const auto names = slotJointNames(*text);
                const std::string& own = names.front();
                const bool targetNamed = (slot.routingFlags & kTargetNamed) != 0;
                if (own.empty() || (known(own) && !targetNamed)) {
                    continue;
                }
                if (!known(own)) {
                    const auto joint = modelJoint(world, *part, own);
                    const auto spot = joint ? routingSlotTarget(*part, slot, joint) : std::nullopt;
                    if (!spot) {
                        continue;
                    }
                    entries.push_back({own, {static_cast<float>(spot->x) / 65536.0f, static_cast<float>(spot->y) / 65536.0f, part->level,
                                             part->id, static_cast<std::int32_t>(i), spot->facing}});
                }
                if (targetNamed) {
                    if (names.size() > 1 && !names[1].empty()) {
                        pairs.emplace_back(own, names[1]);
                    }
                } else if (auto partner = partnerSlotName(own); !partner.empty()) {
                    pairs.emplace_back(own, std::move(partner));
                }
            }
        }
        const auto find = [&](const std::string& name) -> const Entry* {
            for (const Entry& e : entries) {
                if (e.name == name) {
                    return &e;
                }
            }
            return nullptr;
        };
        for (const auto& [a, b] : pairs) {
            const Entry* ea = find(a);
            const Entry* eb = find(b);
            if (ea == nullptr || eb == nullptr || ea == eb) {
                continue;
            }
            const SimObject* part = world.find(ea->end.part);
            const ObjectType* type = part != nullptr ? code.objectType(part->definition.guid()) : nullptr;
            if (type == nullptr || !hasPortalFunction(code, *type)) {
                continue;
            }
            // The end whose slot is the part's first is where the way starts. With neither, it goes both ways.
            StairPortal p;
            p.owner = lead;
            p.from = eb->end.slot == 0 && ea->end.slot != 0 ? eb->end : ea->end;
            p.to = eb->end.slot == 0 && ea->end.slot != 0 ? ea->end : eb->end;
            p.twoWay = ea->end.slot != 0 && eb->end.slot != 0;
            const bool again = std::any_of(out.begin(), out.end(), [&](const StairPortal& q) {
                return q.owner == p.owner && q.from.part == p.from.part && q.from.slot == p.from.slot && q.to.part == p.to.part &&
                       q.to.slot == p.to.slot;
            });
            if (!again) {
                out.push_back(p);
            }
        }
    }
    return out;
}

} // namespace ts2::sim
