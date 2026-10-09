#include "content/install.hpp"
#include "content/resource_index.hpp"
#include "formats/bcon.hpp"
#include "formats/bhav.hpp"
#include "formats/dbpf.hpp"
#include "formats/glob.hpp"
#include "formats/hash.hpp"
#include "formats/objd.hpp"
#include "formats/resource_types.hpp"
#include "formats/str.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using ts2::bhav::Behavior;
using ts2::bhav::OpcodeKind;
using ts2::content::ResourceIndex;
using Resource = ts2::content::ResourceIndex::Resource;

using ts2::types::kObjectDefinition;

constexpr std::uint32_t kGlobalGroup = 0x7FD46CD0;
constexpr std::uint32_t kBehaviorConstants = 0x42434F4E;
constexpr std::uint32_t kSemiGlobalReference = 0x474C4F42;
constexpr std::uint32_t kEditorStringsGroup = 0x7FE59FD0;
constexpr std::uint32_t kGlobalLabels = 0x81;
constexpr std::uint32_t kDataOwners = 0x84;
constexpr std::uint32_t kMotiveLabels = 0x86;
constexpr std::uint32_t kOperatorLabels = 0x88;
constexpr std::uint32_t kPrimitiveNames = 0x8B;
constexpr std::uint32_t kDataLabels = 0x8D;
constexpr std::uint32_t kPersonDataLabels = 0xC8;
constexpr std::uint32_t kAttributeLabels = 0x100;
constexpr std::uint32_t kTreeNames = 0x12F;
constexpr std::uint8_t kConstantsByTempOwner = 0x2F;
constexpr std::uint16_t kRunTreeByName = 0x001C;
constexpr std::uint8_t kUsEnglish = 1;

std::uint64_t groupInstance(std::uint32_t group, std::uint32_t instance) {
    return static_cast<std::uint64_t>(group) << 32 | instance;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string readFileName(ResourceIndex& index, const Resource& r) {
    const std::vector<std::uint8_t> data = index.read(r);
    const std::size_t limit = std::min<std::size_t>(data.size(), 64);
    const auto end = std::find(data.begin(), data.begin() + limit, std::uint8_t{0});
    return std::string(data.begin(), end);
}

struct Catalog {
    std::unordered_map<std::uint64_t, std::string> bhavNames;
    std::unordered_map<std::uint32_t, std::string> objectNames;
    std::unordered_map<std::uint32_t, std::uint32_t> semiGlobalGroups;
    std::unordered_map<std::uint64_t, std::vector<std::int16_t>> constants;
    std::unordered_map<std::uint32_t, std::vector<std::string>> editorTables;
    std::unordered_map<std::uint32_t, std::vector<std::string>> attributeLabels;
    std::unordered_map<std::uint32_t, std::vector<std::string>> treeNames;
    std::map<std::uint32_t, std::size_t> failures;

    const std::string* attributeLabel(std::uint32_t group, std::size_t index) const {
        const auto it = attributeLabels.find(group);
        if (it == attributeLabels.end() || index >= it->second.size() || it->second[index].empty()) {
            return nullptr;
        }
        return &it->second[index];
    }

    std::uint32_t semiGlobalGroupOf(std::uint32_t group) const {
        const auto it = semiGlobalGroups.find(group);
        return it != semiGlobalGroups.end() ? it->second : group;
    }

    const std::string* label(std::uint32_t table, std::size_t index) const {
        const auto it = editorTables.find(table);
        if (it == editorTables.end() || index >= it->second.size() || it->second[index].empty()) {
            return nullptr;
        }
        return &it->second[index];
    }

    std::string primitiveName(std::uint16_t opcode) const {
        const std::string* name = label(kPrimitiveNames, opcode);
        return name ? *name : "(unnamed primitive)";
    }
};

void addToCatalog(Catalog& c, ResourceIndex& index, const Resource& r);

Catalog buildCatalog(ResourceIndex& index) {
    Catalog c;
    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        try {
            addToCatalog(c, index, r);
        } catch (const std::exception&) {
            if (++c.failures[e.key.type] <= 3) {
                std::fprintf(stderr, "catalog: cannot parse %s 0x%08X 0x%08X\n", ts2::types::typeName(e.key.type).c_str(), e.key.group, e.key.instance);
            }
        }
    }
    return c;
}

void addToCatalog(Catalog& c, ResourceIndex& index, const Resource& r) {
    const auto& e = r.entry;
    {
        switch (e.key.type) {
        case ts2::types::kBehavior:
            c.bhavNames.emplace(groupInstance(e.key.group, e.key.instance),
                                readFileName(index, r));
            break;
        case kObjectDefinition:
            if (!c.objectNames.contains(e.key.group)) {
                c.objectNames.emplace(e.key.group, readFileName(index, r));
            }
            break;
        case kSemiGlobalReference:
            c.semiGlobalGroups.emplace(
                e.key.group,
                ts2::hash::groupId(ts2::glob::parse(index.read(r)).semiGlobalName));
            break;
        case kBehaviorConstants:
            c.constants.emplace(groupInstance(e.key.group, e.key.instance),
                                ts2::bcon::parse(index.read(r)).values);
            break;
        case ts2::types::kStringTable:
            if (e.key.group == kEditorStringsGroup) {
                const ts2::str::StringTable table = ts2::str::parse(index.read(r));
                auto& labels = c.editorTables[e.key.instance];
                for (const ts2::str::Entry* entry : table.forLanguage(kUsEnglish)) {
                    labels.push_back(entry->value);
                }
            } else if (e.key.instance == kAttributeLabels || e.key.instance == kTreeNames) {
                const ts2::str::StringTable table = ts2::str::parse(index.read(r));
                auto& labels = e.key.instance == kAttributeLabels ? c.attributeLabels[e.key.group]
                                                                  : c.treeNames[e.key.group];
                for (const ts2::str::Entry* entry : table.forLanguage(kUsEnglish)) {
                    labels.push_back(entry->value);
                }
            }
            break;
        default:
            break;
        }
    }
}

struct BehaviorContext {
    const Catalog& catalog;
    std::uint32_t group;
    bool isSemiGlobal;
};

std::string withLabel(const std::string& prefix, std::uint16_t data, const std::string* label) {
    return label ? prefix + *label : prefix + "#" + std::to_string(data);
}

// A constant reference as the game decodes it: bits 13-14 pick the range (0 own, 1 semi-global,
// 2 global, 3 invalid), bits 7-12 and bit 15 (as 0x40) the table in it, bits 0-6 the value.
struct ConstantRef {
    int range;
    std::uint16_t instance;
    std::uint16_t index;
};

ConstantRef decodeConstant(std::uint16_t data) {
    const int range = (data >> 13) & 3;
    const std::uint16_t table = ((data >> 7) & 0x3F) | ((data >> 9) & 0x40);
    const std::uint16_t bases[] = {0x1000, 0x2000, 0x0100, 0};
    return {range, static_cast<std::uint16_t>(bases[range] + table), static_cast<std::uint16_t>(data & 0x7F)};
}

std::uint32_t constantGroup(const ConstantRef& ref, std::uint32_t group, const Catalog& catalog) {
    return ref.range == 0 ? group : ref.range == 1 ? catalog.semiGlobalGroupOf(group) : kGlobalGroup;
}

std::string constantText(std::uint16_t data, const BehaviorContext& ctx) {
    const ConstantRef ref = decodeConstant(data);
    const std::uint16_t instance = ref.instance;
    const std::uint16_t index = ref.index;
    char text[48];
    if (ref.range == 3) {
        std::snprintf(text, sizeof(text), "const 0x%04X (invalid)", data);
        return text;
    }
    if (ref.range == 0 && ctx.isSemiGlobal) {
        std::snprintf(text, sizeof(text), "const 0x%04X[%u] (of calling object)", instance, index);
        return text;
    }
    const std::uint32_t group = constantGroup(ref, ctx.group, ctx.catalog);
    const auto it = ctx.catalog.constants.find(groupInstance(group, instance));
    if (it == ctx.catalog.constants.end() || index >= it->second.size()) {
        std::snprintf(text, sizeof(text), "const 0x%04X[%u] (not found)", instance, index);
    } else {
        std::snprintf(text, sizeof(text), "const 0x%04X[%u] = %d", instance, index, it->second[index]);
    }
    return text;
}

std::string operandText(std::uint8_t owner, std::uint16_t data, const BehaviorContext& ctx) {
    const Catalog& c = ctx.catalog;
    const std::uint32_t semiGroup = c.semiGlobalGroupOf(ctx.group);
    switch (owner) {
    case 0x00:
        return withLabel("my attribute ", data,
                         ctx.isSemiGlobal ? nullptr : c.attributeLabel(ctx.group, data));
    case 0x01: return "stack obj's attribute #" + std::to_string(data);
    case 0x02: return withLabel("my semi-attribute ", data, c.attributeLabel(semiGroup, data));
    case 0x03: return withLabel("my ", data, c.label(kDataLabels, data));
    case 0x04: return withLabel("stack obj's ", data, c.label(kDataLabels, data));
    case 0x05: return withLabel("stack obj's semi-attribute ", data, c.attributeLabel(semiGroup, data));
    case 0x06: return withLabel("global ", data, c.label(kGlobalLabels, data));
    case 0x07: return std::to_string(static_cast<std::int16_t>(data));
    case 0x08: return "temp " + std::to_string(data);
    case 0x09: return "param " + std::to_string(data);
    case 0x0A: return "stack object id";
    case 0x0E: return withLabel("my motive ", data, c.label(kMotiveLabels, data));
    case 0x0F: return withLabel("stack obj's motive ", data, c.label(kMotiveLabels, data));
    case 0x12: return withLabel("my ", data, c.label(kPersonDataLabels, data));
    case 0x13: return withLabel("stack obj's ", data, c.label(kPersonDataLabels, data));
    case 0x15: return "stack obj's definition field " + std::to_string(data);
    case 0x19: return "local " + std::to_string(data);
    case 0x1A: return constantText(data, ctx);
    default: {
        const std::string* owner_name = c.label(kDataOwners, owner);
        return (owner_name ? "[" + *owner_name + "] " : "[owner " + std::to_string(owner) + "] ") +
               std::to_string(data);
    }
    }
}

std::string expressionText(const ts2::bhav::Instruction& in, const BehaviorContext& ctx) {
    const auto x = ts2::bhav::decodeExpression(in);
    const std::string* op = ctx.catalog.label(kOperatorLabels, x.op);
    return operandText(x.lhsOwner, x.lhsData, ctx) + "  " +
           (op ? *op : "op#" + std::to_string(x.op)) + "  " +
           operandText(x.rhsOwner, x.rhsData, ctx);
}

std::string targetText(std::uint16_t target) {
    switch (target) {
    case ts2::bhav::kTargetTrue:  return "return true";
    case ts2::bhav::kTargetFalse: return "return false";
    case ts2::bhav::kTargetError: return "error";
    default:                      return "-> " + std::to_string(target);
    }
}

std::string opcodeText(std::uint16_t opcode, std::uint32_t ownGroup, const Catalog& catalog) {
    char hex[8];
    std::snprintf(hex, sizeof(hex), "0x%04X", opcode);

    auto callName = [&](std::uint32_t group) -> std::string {
        const auto it = catalog.bhavNames.find(groupInstance(group, opcode));
        return it != catalog.bhavNames.end() ? " \"" + it->second + "\"" : "";
    };

    switch (ts2::bhav::opcodeKind(opcode)) {
    case OpcodeKind::Primitive:
        return std::string(hex) + " " + catalog.primitiveName(opcode);
    case OpcodeKind::Global:
        return std::string(hex) + " call global" + callName(kGlobalGroup);
    case OpcodeKind::Private:
        return std::string(hex) + " call private" + callName(ownGroup);
    case OpcodeKind::SemiGlobal:
        return std::string(hex) + " call semi-global" + callName(catalog.semiGlobalGroupOf(ownGroup));
    }
    return hex;
}

int survey(ResourceIndex& index) {
    struct FormatStats {
        std::size_t total = 0;
        std::size_t exact = 0;
        std::size_t trailing = 0;
        std::size_t failed = 0;
        std::map<std::size_t, std::size_t> trailingSizes;
    };
    std::map<std::uint16_t, FormatStats> byFormat;
    std::map<OpcodeKind, std::size_t> kinds;
    std::map<std::uint16_t, std::size_t> primitives;
    std::size_t instructions = 0;
    std::size_t badTargets = 0;
    std::size_t unreadable = 0;

    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        if (e.key.type != ts2::types::kBehavior) {
            continue;
        }
        std::vector<std::uint8_t> data;
        try {
            data = index.read(r);
        } catch (const std::exception&) {
            ++unreadable;
            continue;
        }
        const std::uint16_t format = data.size() >= 66
            ? static_cast<std::uint16_t>(data[64] | data[65] << 8) : 0;
        FormatStats& s = byFormat[format];
        ++s.total;

        try {
            const Behavior b = ts2::bhav::parse(data);
            if (b.trailingBytes == 0) {
                ++s.exact;
            } else {
                ++s.trailing;
                ++s.trailingSizes[b.trailingBytes];
            }
            for (const auto& in : b.instructions) {
                ++instructions;
                ++kinds[ts2::bhav::opcodeKind(in.opcode)];
                if (in.opcode < 0x100) {
                    ++primitives[in.opcode];
                }
                for (std::uint16_t t : {in.trueTarget, in.falseTarget}) {
                    if (t < 0xFFFC && t >= b.instructions.size()) {
                        ++badTargets;
                    }
                }
            }
        } catch (const std::exception&) {
            ++s.failed;
        }
    }

    std::printf("%-8s %8s %8s %10s %8s\n", "Format", "BHAVs", "Exact", "Trailing", "Failed");
    for (const auto& [format, s] : byFormat) {
        std::printf("0x%04X   %8zu %8zu %10zu %8zu\n", format, s.total, s.exact, s.trailing, s.failed);
        for (const auto& [bytes, n] : s.trailingSizes) {
            std::printf("           %zu BHAVs have %zu extra bytes\n", n, bytes);
        }
    }
    std::printf("\nInstructions      : %zu\n", instructions);
    std::printf("Out-of-range jumps: %zu\n", badTargets);
    std::printf("Unreadable        : %zu\n", unreadable);
    std::printf("Primitive calls   : %zu\n", kinds[OpcodeKind::Primitive]);
    std::printf("Global calls      : %zu\n", kinds[OpcodeKind::Global]);
    std::printf("Private calls     : %zu\n", kinds[OpcodeKind::Private]);
    std::printf("Semi-global calls : %zu\n", kinds[OpcodeKind::SemiGlobal]);

    std::vector<std::pair<std::size_t, std::uint16_t>> top;
    for (const auto& [op, n] : primitives) {
        top.emplace_back(n, op);
    }
    std::sort(top.rbegin(), top.rend());
    const Catalog catalog = buildCatalog(index);
    std::printf("\nMost used primitives:\n");
    for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 15); ++i) {
        std::printf("  0x%02X %-36s %8zu\n", top[i].second,
                    catalog.primitiveName(top[i].second).c_str(), top[i].first);
    }
    return 0;
}

int expressionSurvey(ResourceIndex& index) {
    constexpr std::uint8_t kConstantOwner = 0x1A;
    constexpr std::uint8_t kMaxOperator = 0x15;
    constexpr std::uint8_t kMaxOwner = 0x33;

    const Catalog catalog = buildCatalog(index);

    std::size_t expressions = 0;
    std::size_t badOperator = 0;
    std::size_t badOwner = 0;
    std::size_t nonzeroTail = 0;
    std::map<std::uint8_t, std::size_t> signedByte;
    std::map<std::uint8_t, std::size_t> owners;
    std::map<std::uint8_t, int> maxData;

    struct Candidate {
        std::uint32_t base;
        std::size_t found = 0;
        std::size_t inRange = 0;
    };
    struct RangeClass {
        const char* name;
        std::uint16_t first;
        std::uint16_t last;
        std::vector<Candidate> candidates;
        std::size_t refs = 0;
    };
    RangeClass classes[] = {
        {"0x00-0x3F own group", 0x00, 0x3F, {{0x1000}, {0x2000}, {0x0100}}},
        {"0x40-0x7F semi-global group", 0x40, 0x7F, {{0x2000}, {0x1000}, {0x0100}}},
        {"0x80+     global group", 0x80, 0x1FF, {{0x0100}, {0x1000}, {0x2000}}},
    };
    std::size_t constantRefs = 0;
    std::size_t privateRefs[2][2] = {};
    std::map<std::uint16_t, std::size_t> missingGlobalIds;
    std::size_t decoded[2][3] = {};  // [bit 15][invalid range, table found, index in range]
    std::vector<std::string> outOfRangeSamples;
    std::size_t byTempRefs = 0;
    std::size_t byTempTableFound = 0;
    std::size_t byTempBit15 = 0;
    std::map<std::uint16_t, std::size_t> byTempLow7;
    std::map<int, std::size_t> byTempShift3;
    std::map<int, std::size_t> byTempLow3;
    std::size_t byTempScaled[2][2] = {};
    std::vector<std::string> byTempSamples;

    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        if (e.key.type != ts2::types::kBehavior) {
            continue;
        }
        const Behavior b = ts2::bhav::parse(index.read(r));
        const std::uint32_t groups[] = {e.key.group, catalog.semiGlobalGroupOf(e.key.group),
                                        kGlobalGroup};

        for (const auto& in : b.instructions) {
            if (in.opcode != ts2::bhav::kExpressionOpcode) {
                continue;
            }
            ++expressions;
            const auto x = ts2::bhav::decodeExpression(in);
            ++signedByte[in.operands[4]];
            ++owners[x.lhsOwner];
            ++owners[x.rhsOwner];
            maxData[x.lhsOwner] = std::max<int>(maxData[x.lhsOwner], x.lhsData);
            maxData[x.rhsOwner] = std::max<int>(maxData[x.rhsOwner], x.rhsData);
            badOperator += x.op > kMaxOperator ? 1 : 0;
            badOwner += (x.lhsOwner > kMaxOwner ? 1 : 0) + (x.rhsOwner > kMaxOwner ? 1 : 0);
            if (std::any_of(in.operands.begin() + 8, in.operands.end(),
                            [](std::uint8_t v) { return v != 0; })) {
                ++nonzeroTail;
            }

            for (const auto& [owner, data] : {std::pair{x.lhsOwner, x.lhsData},
                                              std::pair{x.rhsOwner, x.rhsData}}) {
                if (owner == kConstantsByTempOwner) {
                    const std::uint16_t tid = (data >> 7) & 0xFF;
                    const std::uint32_t tgroup = tid < 0x40 ? e.key.group
                                               : tid < 0x80 ? catalog.semiGlobalGroupOf(e.key.group)
                                                            : kGlobalGroup;
                    const std::uint32_t tinst = tid < 0x40 ? 0x1000u + tid
                                              : tid < 0x80 ? 0x2000u + (tid - 0x40)
                                                           : 0x0100u + (tid - 0x80);
                    ++byTempRefs;
                    byTempTableFound += catalog.constants.contains(groupInstance(tgroup, tinst)) ? 1 : 0;
                    ++byTempLow7[data & 0x7F];
                    byTempBit15 += (data & 0x8000) ? 1 : 0;
                    const std::uint32_t asInstance = data >> 3;
                    const bool inOwn = catalog.constants.contains(groupInstance(e.key.group, asInstance));
                    const bool inSemi = catalog.constants.contains(
                        groupInstance(catalog.semiGlobalGroupOf(e.key.group), asInstance));
                    const bool inGlobal = catalog.constants.contains(groupInstance(kGlobalGroup, asInstance));
                    ++byTempShift3[(inOwn ? 1 : 0) + (inSemi ? 2 : 0) + (inGlobal ? 4 : 0)];
                    {
                        const std::uint32_t sid = data >> 3;
                        const std::uint32_t sgroup = sid < 0x400 ? e.key.group
                                                   : sid < 0x800 ? catalog.semiGlobalGroupOf(e.key.group)
                                                                 : kGlobalGroup;
                        const std::uint32_t sinst = sid < 0x400 ? 0x1000u + sid
                                                  : sid < 0x800 ? 0x2000u + (sid - 0x400)
                                                                : 0x0100u + (sid - 0x800);
                        const bool fromSemiBhav = e.key.instance >= 0x2000;
                        ++byTempScaled[fromSemiBhav][catalog.constants.contains(groupInstance(sgroup, sinst))];
                    }
                    ++byTempLow3[data & 7];
                    if (byTempSamples.size() < 10 && (data & 7) == 0) {
                        char text[64];
                        std::snprintf(text, sizeof(text), "data 0x%04X -> instance 0x%04X", data, asInstance);
                        byTempSamples.emplace_back(text);
                    }
                    continue;
                }
                if (owner != kConstantOwner) {
                    continue;
                }
                ++constantRefs;
                const std::uint16_t id = (data >> 7) & 0xFF;
                const std::uint16_t slot = data & 0x7F;
                {
                    const ConstantRef ref = decodeConstant(data);
                    std::size_t* counts = decoded[(data & 0x8000) != 0];
                    const auto it = ref.range == 3 ? catalog.constants.end()
                                                   : catalog.constants.find(groupInstance(
                                                         constantGroup(ref, e.key.group, catalog), ref.instance));
                    counts[0] += ref.range == 3 ? 1 : 0;
                    if (it != catalog.constants.end()) {
                        ++counts[1];
                        counts[2] += ref.index < it->second.size() ? 1 : 0;
                        if (ref.index >= it->second.size() && outOfRangeSamples.size() < 12) {
                            char text[96];
                            std::snprintf(text, sizeof(text), "0x%08X 0x%04X pc %zu: table 0x%04X has %zu values, index %u",
                                          e.key.group, e.key.instance,
                                          static_cast<std::size_t>(&in - b.instructions.data()),
                                          ref.instance, it->second.size(), ref.index);
                            outOfRangeSamples.emplace_back(text);
                        }
                    }
                }
                if (id >= 0x80 && !catalog.constants.contains(
                                      groupInstance(kGlobalGroup, 0x0100u + (id - 0x80)))) {
                    ++missingGlobalIds[id];
                }
                if (id < 0x40) {
                    const bool resolved = catalog.constants.contains(
                        groupInstance(e.key.group, 0x1000u + id));
                    const bool fromSemiGlobal = e.key.instance >= 0x2000;
                    ++privateRefs[fromSemiGlobal][resolved];
                }
                for (std::size_t k = 0; k < std::size(classes); ++k) {
                    RangeClass& rc = classes[k];
                    if (id < rc.first || id > rc.last) {
                        continue;
                    }
                    ++rc.refs;
                    for (Candidate& cand : rc.candidates) {
                        const auto it = catalog.constants.find(
                            groupInstance(groups[k], cand.base + (id - rc.first)));
                        if (it != catalog.constants.end()) {
                            ++cand.found;
                            cand.inRange += slot < it->second.size() ? 1 : 0;
                        }
                    }
                }
            }
        }
    }
    std::printf("Expressions             : %zu\n", expressions);
    std::printf("Operator > 0x%02X        : %zu\n", kMaxOperator, badOperator);
    std::printf("Owner > 0x%02X           : %zu\n", kMaxOwner, badOwner);
    std::printf("Non-zero bytes 8..15    : %zu\n", nonzeroTail);
    std::printf("Byte 4 values           :");
    for (const auto& [v, n] : signedByte) {
        std::printf(" %u=%zu", v, n);
    }
    for (const auto& [type, n] : catalog.failures) {
        std::printf("\nUnparsed %s resources: %zu", ts2::types::typeName(type).c_str(), n);
    }
    std::printf("\n\nPrivate constant refs from object BHAVs     : %zu resolved, %zu not\n",
                privateRefs[0][1], privateRefs[0][0]);
    std::printf("Private constant refs from semi-global BHAVs: %zu resolved, %zu not\n",
                privateRefs[1][1], privateRefs[1][0]);
    std::printf("\nConstant references: %zu\n", constantRefs);
    for (const auto& rc : classes) {
        std::printf("  ids %-30s %6zu refs\n", rc.name, rc.refs);
        for (const auto& cand : rc.candidates) {
            std::printf("      instance 0x%04X + offset: BCON exists %6zu, index in range %6zu\n",
                        cand.base, cand.found, cand.inRange);
        }
    }
    for (int bit = 0; bit < 2; ++bit) {
        std::printf("Refs with bit 15 %s: invalid range %zu, table found %zu, index in range %zu\n",
                    bit ? "set  " : "clear", decoded[bit][0], decoded[bit][1], decoded[bit][2]);
    }
    std::printf("Constants [temp] refs: %zu, table found %zu, bit 15 set %zu; low 7 bits:",
                byTempRefs, byTempTableFound, byTempBit15);
    for (const auto& [v, n] : byTempLow7) {
        std::printf(" %u=%zu", v, n);
    }
    std::printf("\n");
    std::printf("Constants [temp], data >> 3 as BCON instance (own=1 semi=2 global=4):");
    for (const auto& [mask, n] : byTempShift3) {
        std::printf(" %d=%zu", mask, n);
    }
    std::printf("\nConstants [temp], data >> 3 with scaled ranges: object BHAVs found %zu / missing %zu;"
                " semi-global BHAVs found %zu / missing %zu",
                byTempScaled[0][1], byTempScaled[0][0], byTempScaled[1][1], byTempScaled[1][0]);
    std::printf("\nConstants [temp], data & 7:");
    for (const auto& [v, n] : byTempLow3) {
        std::printf(" %d=%zu", v, n);
    }
    std::printf("\n");
    for (const auto& s : byTempSamples) {
        std::printf("  %s\n", s.c_str());
    }
    std::printf("Refs whose index does not fit:\n");
    for (const auto& s : outOfRangeSamples) {
        std::printf("  %s\n", s.c_str());
    }
    std::printf("\nUnresolved global ids:");
    for (const auto& [id, n] : missingGlobalIds) {
        std::printf(" 0x%X(%zu)", id, n);
    }
    std::printf("\n\nOwner usage:\n");
    for (const auto& [owner, n] : owners) {
        std::printf("  0x%02X %8zu   max data %d\n", owner, n, maxData[owner]);
    }
    return 0;
}

int opcodeSurvey(ResourceIndex& index, std::uint16_t opcode, int filterByte, int filterValue) {
    std::size_t uses = 0;
    std::array<std::map<std::uint8_t, std::size_t>, 16> byteValues;
    std::vector<std::string> samples;
    std::map<std::string, std::size_t> exits;

    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        if (e.key.type != ts2::types::kBehavior) {
            continue;
        }
        const Behavior b = ts2::bhav::parse(index.read(r));
        for (std::size_t pc = 0; pc < b.instructions.size(); ++pc) {
            const auto& in = b.instructions[pc];
            if (in.opcode != opcode ||
                (filterByte >= 0 && in.operands[static_cast<std::size_t>(filterByte)] != filterValue)) {
                continue;
            }
            ++uses;
            for (std::size_t k = 0; k < 16; ++k) {
                ++byteValues[k][in.operands[k]];
            }
            const auto describeTarget = [&](std::uint16_t t) -> std::string {
                if (t == pc) return "self";
                if (t == ts2::bhav::kTargetTrue) return "return true";
                if (t == ts2::bhav::kTargetFalse) return "return false";
                if (t == ts2::bhav::kTargetError) return "error";
                return "other instruction";
            };
            ++exits["T: " + describeTarget(in.trueTarget) + ", F: " + describeTarget(in.falseTarget)];
            if (samples.size() < 12 && (uses % (filterByte >= 0 ? 7 : 97)) == 1) {
                char line[128];
                int n = std::snprintf(line, sizeof(line), "%-34.34s ", b.name.c_str());
                for (std::size_t k = 0; k < 16; ++k) {
                    n += std::snprintf(line + n, sizeof(line) - static_cast<std::size_t>(n),
                                       "%02X%s", in.operands[k], k == 7 ? "  " : " ");
                }
                samples.emplace_back(line);
            }
        }
    }

    std::printf("Opcode 0x%04X used %zu times\n\nPer operand byte: distinct values, most common\n", opcode, uses);
    for (std::size_t k = 0; k < 16; ++k) {
        std::vector<std::pair<std::size_t, std::uint8_t>> top;
        for (const auto& [v, n] : byteValues[k]) {
            top.emplace_back(n, v);
        }
        std::sort(top.rbegin(), top.rend());
        std::printf("  byte %2zu: %3zu distinct |", k, top.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(top.size(), 8); ++i) {
            std::printf(" %02X(%zu)", top[i].second, top[i].first);
        }
        std::printf("\n");
    }
    std::printf("\nExits:\n");
    for (const auto& [text, n] : exits) {
        std::printf("  %6zu  %s\n", n, text.c_str());
    }
    std::printf("\nSamples:\n");
    for (const auto& s : samples) {
        std::printf("  %s\n", s.c_str());
    }
    return 0;
}

int namedTreeSurvey(ResourceIndex& index) {
    const Catalog catalog = buildCatalog(index);

    std::unordered_map<std::uint32_t, std::set<std::string>> namesByGroup;
    for (const auto& [key, name] : catalog.bhavNames) {
        namesByGroup[static_cast<std::uint32_t>(key >> 32)].insert(name);
    }
    auto has = [&](std::uint32_t group, const std::string& name) {
        const auto it = namesByGroup.find(group);
        return it != namesByGroup.end() && it->second.contains(name);
    };

    struct Tally {
        std::size_t total = 0, nameMissing = 0, privateHit = 0, semiHit = 0, globalHit = 0, noHit = 0;
    };
    std::map<std::string, Tally> tallies;
    std::size_t stackObjectTable = 0;

    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        if (e.key.type != ts2::types::kBehavior) {
            continue;
        }
        const Behavior b = ts2::bhav::parse(index.read(r));
        for (const auto& in : b.instructions) {
            if (in.opcode != kRunTreeByName) {
                continue;
            }
            const auto& o = in.operands;
            if (o[2] & 0x02) {
                ++stackObjectTable;
                continue;
            }
            const std::uint32_t tableGroup = (o[2] & 0x01) ? kGlobalGroup : e.key.group;
            const std::size_t nameIndex = o[4] | ((o[2] & 0x40) ? 0x100 : 0);
            char key[64];
            std::snprintf(key, sizeof(key), "bit2=%d bit3=%d table=%s", (o[2] >> 2) & 1, (o[2] >> 3) & 1,
                          (o[2] & 0x01) ? "global" : "own");
            Tally& t = tallies[key];
            ++t.total;
            const auto table = catalog.treeNames.find(tableGroup);
            if (table == catalog.treeNames.end() || nameIndex >= table->second.size()) {
                ++t.nameMissing;
                continue;
            }
            const std::string& name = table->second[nameIndex];
            const bool inPrivate = has(e.key.group, name);
            const bool inSemi = has(catalog.semiGlobalGroupOf(e.key.group), name);
            const bool inGlobal = has(kGlobalGroup, name);
            t.privateHit += inPrivate;
            t.semiHit += inSemi;
            t.globalHit += inGlobal;
            t.noHit += !(inPrivate || inSemi || inGlobal);
        }
    }

    std::printf("Run Tree by Name using the stack object's string table: %zu (not resolvable statically)\n\n",
                stackObjectTable);
    std::printf("%-34s %6s %8s %8s %6s %7s %6s\n", "", "uses", "no name", "private", "semi", "global", "none");
    for (const auto& [key, t] : tallies) {
        std::printf("%-34s %6zu %8zu %8zu %6zu %7zu %6zu\n", key.c_str(), t.total, t.nameMissing,
                    t.privateHit, t.semiHit, t.globalHit, t.noHit);
    }
    return 0;
}

int search(ResourceIndex& index, std::string_view text) {
    const Catalog catalog = buildCatalog(index);
    const std::string needle = lower(text);

    std::vector<std::pair<std::uint64_t, std::string>> hits;
    for (const auto& [key, name] : catalog.bhavNames) {
        const auto group = static_cast<std::uint32_t>(key >> 32);
        const auto objIt = catalog.objectNames.find(group);
        const std::string objName = objIt != catalog.objectNames.end() ? objIt->second : "";
        if (lower(name).find(needle) != std::string::npos ||
            lower(objName).find(needle) != std::string::npos) {
            hits.emplace_back(key, name);
        }
    }
    std::sort(hits.begin(), hits.end());

    for (const auto& [key, name] : hits) {
        const auto group = static_cast<std::uint32_t>(key >> 32);
        const auto objIt = catalog.objectNames.find(group);
        std::printf("0x%08X 0x%04X  %-40s [%s]\n", group, static_cast<std::uint32_t>(key),
                    name.c_str(), objIt != catalog.objectNames.end() ? objIt->second.c_str() : "");
    }
    std::printf("%zu matches\n", hits.size());
    return 0;
}

int show(ResourceIndex& index, std::uint32_t group, std::uint32_t instance) {
    const Catalog catalog = buildCatalog(index);
    for (const auto& r : index.resources()) {
        const auto& e = r.entry;
        if (e.key.type != ts2::types::kBehavior || e.key.group != group ||
            e.key.instance != instance) {
            continue;
        }
        const Behavior b = ts2::bhav::parse(index.read(r));
        const auto objIt = catalog.objectNames.find(group);

        std::printf("\"%s\"", b.name.c_str());
        if (objIt != catalog.objectNames.end()) {
            std::printf("  (object: %s)", objIt->second.c_str());
        }
        std::printf("\nformat 0x%04X, %u args, %u locals, tree type %u, version %d\n\n",
                    b.format, b.argCount, b.localCount, b.treeType,
                    static_cast<std::int32_t>(b.treeVersion));

        const std::size_t operandCount = b.format < 0x8003 ? 8 : 16;
        const BehaviorContext ctx{catalog, group, instance >= 0x2000};
        for (std::size_t i = 0; i < b.instructions.size(); ++i) {
            const auto& in = b.instructions[i];
            std::printf("%3zu  %-58s T: %-13s F: %s\n", i,
                        opcodeText(in.opcode, group, catalog).c_str(),
                        targetText(in.trueTarget).c_str(), targetText(in.falseTarget).c_str());
            if (in.opcode == ts2::bhav::kExpressionOpcode) {
                std::printf("     %s\n", expressionText(in, ctx).c_str());
                continue;
            }
            std::printf("     ");
            for (std::size_t k = 0; k < operandCount; ++k) {
                std::printf("%02X%s", in.operands[k], k == 7 ? "  " : " ");
            }
            std::printf("\n");
        }
        return 0;
    }
    std::fprintf(stderr, "no BHAV with group 0x%08X instance 0x%04X\n", group, instance);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "Usage:\n"
                     "  bhavdump <file.package | install-folder> --survey | --expr-survey\n"
                     "  bhavdump <file.package | install-folder> --search <text>\n"
                     "  bhavdump <file.package | install-folder> --show <group> <instance>\n");
        return 1;
    }

    try {
        ResourceIndex index;
        const std::filesystem::path source(argv[1]);
        if (std::filesystem::is_directory(source)) {
            for (const auto& path :
                 ts2::content::objectLoadOrder(ts2::content::findProducts(source))) {
                index.addPackage(path);
            }
        } else {
            index.addPackage(source);
        }

        const std::string_view mode = argv[2];
        if (mode == "--expr-survey") {
            return expressionSurvey(index);
        }
        if (mode == "--named-tree-survey") {
            return namedTreeSurvey(index);
        }
        if (mode == "--opcode-survey" && argc >= 4) {
            return opcodeSurvey(index, static_cast<std::uint16_t>(std::stoul(argv[3], nullptr, 0)),
                                argc >= 6 ? std::stoi(argv[4], nullptr, 0) : -1,
                                argc >= 6 ? std::stoi(argv[5], nullptr, 0) : -1);
        }
        if (mode == "--survey") {
            return survey(index);
        }
        if (mode == "--search" && argc >= 4) {
            return search(index, argv[3]);
        }
        if (mode == "--show" && argc >= 5) {
            return show(index, static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)),
                        static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
        }
        std::fprintf(stderr, "unknown or incomplete command\n");
        return 1;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
