#include "diag/log.hpp"
#include "sim/vm_execution.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

namespace ts2::sim::vm_detail {

namespace {

constexpr std::uint16_t kDialogTexts = 0x12D;
constexpr std::uint32_t kBehaviorGroup = 0x7FE59FD0, kAspirationGroup = 0x7F96C284, kWordGroup = 0x7FA7DC7C;
constexpr std::uint16_t kDayTexts = 0x99, kSkillTexts = 0xCB, kPointTexts = 0xCC;
// Notification styles (byte 12). 4 and 5 show a Sim's bubble with more to it.
constexpr std::uint8_t kSystemDialogStyle = 2, kExtraInTempStyle = 4, kNeighborInTempStyle = 5;
// A text a broken script makes grow without end stops here.
constexpr std::size_t kMostTextBytes = 4096;
constexpr int kMostTokenHits = 256;

// A whole number as the game's English texts write it, with commas between thousands.
std::string grouped(std::int64_t v) {
    std::string digits = std::to_string(v < 0 ? -v : v);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return v < 0 ? "-" + digits : digits;
}

std::string lowered(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// The tokens in the order the game expands them. Each is looked for through the whole text before
// the next, so a token one leaves in place can still match a shorter one later, as in the game.
// Tokens the engine can't fill yet are kept as they are.
struct TokenRule {
    std::string_view name;
    bool filled;
};
constexpr TokenRule kTokens[] = {
    {"$TokenPersonalCountedLocalName", false}, {"$TokenPersonalSingularLocalName", false},
    {"$TokenFamilyCountedLocalName", false},   {"$TokenFamilySingularLocalName", false},
    {"$TokenPersonalCountedLocalDesc", false}, {"$TokenPersonalSingularLocalDesc", false},
    {"$TokenFamilyCountedLocalDesc", false},   {"$TokenFamilySingularLocalDesc", false},
    {"$FamilyAssets", false},                  {"$Family", false},
    {"$LastName", true},                       {"$NeighborLocal", true},
    {"$NeighborhoodLocal", false},             {"$PrimaryNeighborhood", false},
    {"$Neighbor", true},                       {"$Me", true},
    {"$Object", true},                         {"$Local", true},
    {"$Money", true},                          {"$Cash", true},
    {"$TimeLocal", true},                      {"$Attrib", true},
    {"$NameAttrib", true},                     {"$NameLocal", true},
    {"$NameParam", true},                      {"$FullNameLocal", true},
    {"$FullName", true},                       {"$TokenNameLocal", false},
    {"$LotNameLocalByLotId", false},           {"$LotNameLocal", false},
    {"$CatalogLocal", false},                  {"$DayLocal", true},
    {"$DayFlags", true},                       {"$JobTitle", true},
    {"$PrivateString", true},                  {"$BehaviorString", true},
    {"$AspirationString", true},               {"$SemiGlobalString", true},
    {"$GlobalString", true},                   {"$SkillName", true},
    {"$WordPoints", true},                     {"$WantDescriptionLocal", false},
    {"$FearDescriptionLocal", false},          {"$WantHelpTextLocal", false},
    {"$FearHelpTextLocal", false},             {"$GroupName", false},
    {"$PropertyPrivateString", false},         {"$PropertySemiGlobalString", false},
    {"$PropertyGlobalString", false},          {"$GUIDCatalogNameLocal", true},
};

// Reads ":n" at `at`, moving past it. -1 when there is no number.
int readNumber(const std::string& text, std::size_t& at) {
    if (at + 1 >= text.size() || text[at] != ':' || !std::isdigit(static_cast<unsigned char>(text[at + 1]))) {
        return -1;
    }
    ++at;
    int v = 0;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
        v = std::min(v * 10 + (text[at] - '0'), 99999);
        ++at;
    }
    return v;
}

} // namespace

// An object type's catalog name (its catalog strings' first line), empty when unknown.
std::string Execution::catalogName(std::uint32_t guid) {
    const ObjectType* type = code_.objectType(guid);
    if (type == nullptr) {
        return {};
    }
    const auto set = static_cast<std::uint16_t>(type->definition.field(objd::field::kCatalogStringsId));
    return code_.catalogString(type->group, set, 0).value_or("");
}

// A neighbor's first and last name: its character's catalog strings, lines 0 and 2, or the names
// given while playing. None for an unknown neighbor.
std::optional<std::array<std::string, 2>> Execution::neighborNames(std::int16_t id) {
    const SavedNeighbor* neighbor = world().neighbor(id);
    if (neighbor == nullptr) {
        return std::nullopt;
    }
    std::array<std::string, 2> names;
    if (const ObjectType* type = code_.objectType(neighbor->guid)) {
        const auto set = static_cast<std::uint16_t>(type->definition.field(objd::field::kCatalogStringsId));
        names[0] = code_.catalogString(type->group, set, 0).value_or("");
        names[1] = code_.catalogString(type->group, set, 2).value_or("");
    }
    if (auto given = world().neighborFirstName(id)) {
        names[0] = std::move(*given);
    }
    if (auto taken = world().neighborSurname(id)) {
        names[1] = std::move(*taken);
    }
    return names;
}

// One token's text, or none when it fails (the token then stays in the text). `a` and `b` are its
// ":n" numbers, -1 when missing. Locals and params are the running frame's.
std::optional<std::string> Execution::expandToken(std::string_view token, int a, int b, Frame& f, TokenPicture& picture) {
    const std::int16_t stack = stackObject();
    auto local = [&](int i) -> std::optional<std::int16_t> {
        return i >= 0 && static_cast<std::size_t>(i) < f.locals.size() ? std::optional(f.locals[static_cast<std::size_t>(i)]) : std::nullopt;
    };
    auto param = [&](int i) -> std::optional<std::int16_t> {
        return i >= 0 && static_cast<std::size_t>(i) < f.params.size() ? std::optional(f.params[static_cast<std::size_t>(i)]) : std::nullopt;
    };
    auto objectName = [&](std::optional<std::int16_t> id) -> std::optional<std::string> {
        const SimObject* o = id ? world().find(*id) : nullptr;
        return o != nullptr ? std::optional(catalogName(o->definition.guid())) : std::nullopt;
    };
    auto fullName = [](const std::array<std::string, 2>& n) {
        return n[1].empty() ? n[0] : n[0].empty() ? n[1] : n[0] + " " + n[1];
    };
    auto line = [&](std::uint32_t group, std::optional<std::int16_t> instance, std::optional<std::int16_t> number) -> std::optional<std::string> {
        if (!instance || !number) {
            return std::nullopt;
        }
        // Lines are numbered from 1, and a missing one reads as empty.
        return *number < 1 ? std::string() : code_.string(group, static_cast<std::uint16_t>(*instance), static_cast<std::size_t>(*number - 1)).value_or("");
    };
    const SimObject* stackObj = world().find(stack);

    if (token == "$LastName") {
        const auto n = neighborNames(stack);
        return n ? std::optional((*n)[1]) : std::nullopt;
    }
    if (token == "$NeighborLocal") {
        const auto id = local(a);
        const auto n = id ? neighborNames(*id) : std::nullopt;
        return n ? std::optional((*n)[0]) : std::nullopt;
    }
    if (token == "$Neighbor" || token == "$FullName") {
        const auto n = neighborNames(stack);
        if (!n) {
            return std::nullopt;
        }
        picture = {stack, 0};
        return token == "$Neighbor" ? (*n)[0] : fullName(*n);
    }
    if (token == "$FullNameLocal") {
        const auto id = local(a);
        auto n = id ? neighborNames(*id) : std::nullopt;
        n = n ? n : neighborNames(stack);
        return n ? std::optional(fullName(*n)) : std::nullopt;
    }
    if (token == "$Me") {
        return catalogName(me().definition.guid());
    }
    if (token == "$Object") {
        if (stackObj == nullptr) {
            return std::nullopt;
        }
        picture = pictureOf(*stackObj);
        return catalogName(stackObj->definition.guid());
    }
    if (token == "$Local") {
        const auto v = local(a);
        return v ? std::optional(grouped(*v)) : std::nullopt;
    }
    if (token == "$Money" || token == "$Cash") {
        const auto v = local(a), w = local(a + 1);
        if (!v || !w) {
            return std::nullopt;
        }
        // Money multiplies by the next local when it isn't 0. Cash counts the next local in ten thousands.
        const std::int64_t value = token == "$Money" ? std::int64_t{*v} * (*w != 0 ? *w : 1) : std::int64_t{*v} + std::int64_t{*w} * 10000;
        return grouped(value);
    }
    if (token == "$Attrib" || token == "$NameAttrib") {
        if (stackObj == nullptr || a < 0 || static_cast<std::size_t>(a) >= stackObj->attributes.size()) {
            return std::nullopt;
        }
        const std::int16_t v = stackObj->attributes[static_cast<std::size_t>(a)];
        return token == "$Attrib" ? std::optional(grouped(v)) : objectName(v);
    }
    if (token == "$NameLocal") {
        return objectName(local(a));
    }
    if (token == "$NameParam") {
        return objectName(param(a));
    }
    if (token == "$DayLocal") {
        const auto v = local(a);
        return v ? line(kAspirationGroup, kDayTexts, static_cast<std::int16_t>(*v + 1)) : std::nullopt;
    }
    if (token == "$TimeLocal") {
        // An hour from 0 to 23 on the whole hour, as US English writes it ("9:00 am", "12:00 pm").
        const auto hour = local(a);
        if (!hour || *hour < 0 || *hour > 23) {
            return std::nullopt;
        }
        const int twelve = *hour == 0 ? 12 : *hour > 12 ? *hour - 12 : *hour;
        return std::format("{}:00 {}", twelve, *hour < 12 ? "am" : "pm");
    }
    if (token == "$JobTitle") {
        // Line local a + 1 of the job titles (string table 0x93) of the career object that local b names.
        constexpr std::int16_t kJobTitles = 0x93;
        const auto title = local(a);
        const auto career = local(b);
        const SimObject* o = career ? world().find(*career) : nullptr;
        return o != nullptr && title ? line(o->group, kJobTitles, static_cast<std::int16_t>(*title + 1)) : std::nullopt;
    }
    if (token == "$DayFlags") {
        // Bits 0 to 5 name lines 2 to 7, bit 6 line 1, joined with commas.
        const auto mask = local(a);
        if (!mask) {
            return std::nullopt;
        }
        std::string out;
        for (int bit = 0; bit < 7; ++bit) {
            if (*mask & (1 << bit)) {
                out += (out.empty() ? "" : ", ") + line(kAspirationGroup, kDayTexts, static_cast<std::int16_t>(bit == 6 ? 1 : bit + 2)).value_or("");
            }
        }
        return out;
    }
    if (token == "$PrivateString" || token == "$SemiGlobalString") {
        if (stackObj == nullptr) {
            return std::string();
        }
        const std::uint32_t group = token == "$PrivateString" ? stackObj->group : code_.semiGlobalGroup(stackObj->group);
        return group == 0 ? std::optional(std::string()) : line(group, local(a), local(b));
    }
    if (token == "$BehaviorString") {
        return line(kBehaviorGroup, local(a), local(b));
    }
    if (token == "$AspirationString") {
        return line(kAspirationGroup, local(a), local(b));
    }
    if (token == "$GlobalString") {
        return line(kGlobalGroup, local(a), local(b));
    }
    if (token == "$SkillName") {
        const auto v = local(a);
        return v && *v > 0 ? line(kAspirationGroup, kSkillTexts, *v) : std::nullopt;
    }
    if (token == "$WordPoints") {
        // Line 1 for one point, 3 for five or more, else 2.
        const auto v = local(a);
        return v ? line(kWordGroup, kPointTexts, static_cast<std::int16_t>(*v == 1 ? 1 : *v >= 5 ? 3 : 2)) : std::nullopt;
    }
    if (token == "$GUIDCatalogNameLocal") {
        const auto low = local(a), high = local(b);
        if (!low || !high) {
            return std::nullopt;
        }
        const std::uint32_t guid = static_cast<std::uint16_t>(*low) | static_cast<std::uint32_t>(static_cast<std::uint16_t>(*high)) << 16;
        return code_.objectType(guid) != nullptr ? std::optional(catalogName(guid)) : std::nullopt;
    }
    return std::nullopt;
}

// The picture a notification shows for an object: a Sim's portrait, a multi-tile part's master type,
// or the object's own type.
Execution::TokenPicture Execution::pictureOf(const SimObject& object) {
    if (const std::int16_t neighbor = world().neighborIdOf(object); neighbor != 0) {
        return {neighbor, 0};
    }
    std::uint32_t guid = object.definition.guid();
    if (object.definition.field(objd::field::kMultiTileMasterId) != 0) {
        const ObjectType* type = code_.objectType(guid);
        const ObjectType* master = type != nullptr ? code_.multiTileMaster(*type) : nullptr;
        guid = master != nullptr ? master->definition.guid() : guid;
    }
    return {0, guid};
}

// Puts names, numbers and texts in place of a notification's tokens ($Neighbor, $Local:2 and the
// rest), found without regard to case. Each match resumes the search one letter on, so a token's
// own text is searched again.
std::string Execution::expandTokens(std::string text, Frame& f, TokenPicture& picture) {
    // Tokens the engine can't fill are hidden behind a marker while the others are expanded, so a
    // shorter token can't match inside them. The game would have filled them.
    constexpr char kHidden = '\x01';
    std::vector<std::string> hidden;
    int hits = 0;
    for (const TokenRule& rule : kTokens) {
        const std::string name = lowered(rule.name);
        for (std::size_t at = lowered(text).find(name); at != std::string::npos && hits < kMostTokenHits; at = lowered(text).find(name, at + 1)) {
            ++hits;
            std::size_t end = at + name.size();
            const int a = readNumber(text, end);
            const int b = a >= 0 ? readNumber(text, end) : -1;
            if (!rule.filled) {
                ts2::diag::warnOnce(std::format("text notification: the {} token isn't filled in yet", rule.name));
                hidden.push_back(text.substr(at, end - at));
                text.replace(at, end - at, std::string(1, kHidden));
                continue;
            }
            if (const auto value = expandToken(rule.name, a, b, f, picture)) {
                text.replace(at, end - at, *value);
                if (text.size() > kMostTextBytes) {
                    ts2::diag::warnOnce("text notification: a text grew past 4 KB and was cut there");
                    text.resize(kMostTextBytes);
                }
            }
        }
    }
    std::size_t next = 0;
    for (std::size_t at = text.find(kHidden); at != std::string::npos && next < hidden.size(); at = text.find(kHidden, at + 1)) {
        text.replace(at, 1, hidden[next]);
        at += hidden[next++].size() - 1;
    }
    return text;
}

// Text notification (type 8), and Modify (0x0A) and Append (0x1C) of a shown one by the id in temp
// n. The text is line bytes 13-14 (or temp 0 + 1 with byte 8 bit 1) of string table 0x12D, from 1,
// with its tokens filled in. Byte 7 bits 1-3 pick the picture: 0 the one the text names, or the
// stack object's own type with the stack object as the extra when it names none, 2 the neighbor whose
// id the stack object holds, and any other value none. Byte 9 is the priority, byte 10 the timeout in
// seconds. False for a style the game has no bubble for, or a Modify of one not shown.
bool Execution::textNotification(const bhav::Instruction& in, Frame& f, std::int16_t& id) {
    const auto& o = in.operands;
    const std::uint32_t owner = f.codeOwner;
    const std::uint32_t textGroup = (o[8] & 0x01) ? code_.semiGlobalGroup(owner) : (o[8] & 0x40) ? kGlobalGroup : owner;
    const int number = (o[8] & 0x02) ? thread_.temps[0] + 1 : (o[13] | o[14] << 8);
    std::string text = number < 1 ? std::string() : code_.string(textGroup, kDialogTexts, static_cast<std::size_t>(number - 1)).value_or("");
    TokenPicture picture;
    text = expandTokens(std::move(text), f, picture);
    std::int16_t extra = 0;
    const int pictureKind = (o[7] >> 1) & 0x07;
    if (pictureKind == 0) {
        const SimObject* stack = world().find(stackObject());
        if (picture.neighbor == 0 && picture.objectType == 0 && stack != nullptr) {
            picture = pictureOf(*stack);
            extra = stack->id;
        }
    } else if (pictureKind == 2) {
        if (world().neighbor(stackObject()) == nullptr) {
            throw Stop{Outcome::Error, "neighbor not found", std::to_string(stackObject())};
        }
        picture = {stackObject(), 0};
    } else {
        picture = {};
    }
    std::uint8_t style = o[12];
    if (style == kExtraInTempStyle || style == kNeighborInTempStyle) {
        extra = thread_.temps[1];
        if (style == kNeighborInTempStyle && world().neighbor(thread_.temps[2]) != nullptr) {
            picture = {thread_.temps[2], 0};
        }
        style = 0;
    }
    if (o[5] == kTextNotification) {
        if (style > kSystemDialogStyle) {
            return false;
        }
        id = world().showNotification({0, style, std::move(text), picture.neighbor, picture.objectType, extra, o[9], o[10], 0});
    } else if (!world().modifyNotification(id, std::move(text), picture.neighbor, picture.objectType, extra, o[5] == kAppendNotification)) {
        return false;
    }
    world().setNotificationTiming(id, o[10], o[9]);
    return true;
}

} // namespace ts2::sim::vm_detail
