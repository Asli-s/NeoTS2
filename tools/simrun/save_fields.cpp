#include "save_fields.hpp"

#include "content/lot.hpp"
#include "formats/cobject_record.hpp"
#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"
#include "formats/xobj.hpp"
#include "scene/animation_library.hpp"
#include "scene/assets.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using ts2::cobject::Word;

// One output line: its kind, then name=value pairs. Text is kept to one line and free of tabs.
class Line {
public:
    explicit Line(const char* kind) : text_(kind) {}

    void text(const std::string& name, std::string value) {
        for (char& c : value) {
            if (c == '\t' || c == '\n' || c == '\r' || c == '=') {
                c = '_';
            }
        }
        text_ += '\t' + name + '=' + value;
    }
    void number(const std::string& name, double value) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", value);
        text(name, buf);
    }
    void word(const std::string& name, Word value) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08X", value);
        text(name, buf);
    }
    void real(const std::string& name, Word bits) { number(name, std::bit_cast<float>(bits)); }
    void print() const { std::printf("%s\n", text_.c_str()); }

private:
    std::string text_;
};

std::string indexed(const char* name, std::size_t i) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%s%02zX", name, i);
    return buf;
}

struct Owner {
    std::uint32_t guid = 0;
    std::string kind;
};

void sceneFields(Line& line, const ts2::cobject::Record& r) {
    const auto& o = r.object;
    line.number("chain", static_cast<int>(r.chain));
    line.text("model", o.model);
    line.number("materials", static_cast<double>(o.materials.size()));
    line.number("joints", static_cast<double>(o.joints.size()));
    line.text("firstJoint", o.joints.empty() ? "" : o.joints.front().name);
    line.number("blends", static_cast<double>(o.blends.size()));
    for (std::size_t i = 0; i < o.place.size(); ++i) {
        line.real(indexed("place", i), o.place[i]);
    }
    const auto& a = r.animatable;
    line.real("scale", a.scale);
    line.number("twoPlaces", a.places ? 1 : 0);
    for (std::size_t i = 0; i < a.settings.size(); ++i) {
        line.word(indexed("setting", i), a.settings[i]);
    }
    for (std::size_t i = 0; i < a.place.size(); ++i) {
        line.real(indexed("animPlace", i), a.place[i]);
    }
    int clips = 0, attached = 0, empty = 0;
    std::uint32_t maxSerial = 0;
    for (const auto& an : a.animations) {
        clips += an.kind == ts2::cobject::Animation::Kind::Clip;
        attached += an.kind == ts2::cobject::Animation::Kind::Attached;
        empty += an.kind == ts2::cobject::Animation::Kind::None;
        if ((an.request.flags & 0xE0000000u) == 0x40000000u) {
            maxSerial = std::max(maxSerial, (an.request.flags >> 21) & 0xFFu);
        }
    }
    line.number("clips", clips);
    line.number("attached", attached);
    line.number("empty", empty);
    line.number("maxSimSerial", maxSerial);
    line.number("setting00Low", a.settings[0] & 0xFFu);
    line.word("locomotionValue", r.locomotable.value);
    line.number("locomotionSets", static_cast<double>(r.locomotable.sets.size()));
}

void animationFields(Line& line, const ts2::cobject::Animation& an, std::size_t index, ts2::scene::AnimationLibrary& clips) {
    const auto& q = an.request;
    const auto& in = an.instance;
    line.number("index", static_cast<double>(index));
    line.number("kind", static_cast<double>(an.kind));
    line.word("handle", q.flags);
    line.word("handleHigh", q.flags & 0xE0000000u);
    line.number("handleSerial", (q.flags >> 21) & 0xFFu);
    line.word("handleLow", q.flags & 0x001FFFFFu);
    line.text("name", q.name);
    line.text("type", q.type);
    line.number("value1", q.value1);
    line.number("category", q.category);
    line.number("eventObject", q.eventObject);
    line.word("eventTree", q.eventTree);
    line.number("value2", q.value2);
    line.real("rate", q.rate);
    for (std::size_t i = 0; i < q.words1.size(); ++i) {
        line.word(indexed("words1_", i), q.words1[i]);
    }
    line.word("eventObjectGuid", q.eventObjectGuid);
    line.word("words2_0", q.words2[0]);
    line.word("words2_1", q.words2[1]);
    line.text("requestText", q.text);
    line.number("hasPart", q.part ? 1 : 0);
    if (q.part) {
        line.number("partValue", q.part->first);
        line.word("partWord", q.part->second);
    }
    line.word("anchorObject", q.words3[0]);
    line.word("anchorName", q.words3[1]);
    line.number("value3", q.value3);
    line.number("state", in.state);
    line.word("word1", in.word1);
    line.number("startTick", in.startTick);
    line.word("stopTick", in.stopTick);
    for (std::size_t i = 0; i < in.words2.size(); ++i) {
        line.word(indexed("words2i_", i), in.words2[i]);
    }
    line.number("layer", in.layer);
    line.real("weight", in.weight);
    line.number("hasCurve", in.curve ? 1 : 0);
    double curveTotal = 0;
    if (in.curve) {
        line.number("curveValue", in.curve->value);
        line.word("curveWord", in.curve->word);
        line.number("segments", static_cast<double>(in.curve->segments.size()));
        for (std::size_t s = 0; s < in.curve->segments.size() && s < 8; ++s) {
            const auto& seg = in.curve->segments[s];
            const std::string at = "seg" + std::to_string(s) + "_";
            line.number(at + "kind", seg.kind);
            line.real(at + "from", seg.values[0]);
            line.real(at + "to", seg.values[1]);
            line.real(at + "length", seg.length);
            curveTotal += std::bit_cast<float>(seg.length);
        }
    }
    line.number("curveTotal", curveTotal);
    line.word("word3", in.word3);
    line.number("plays", in.plays);
    for (std::size_t i = 0; i < in.words4.size(); ++i) {
        line.word(indexed("words4_", i), in.words4[i]);
    }
    line.number("savedTick", in.savedTick);
    line.number("elapsedTicks", static_cast<double>(in.savedTick) - in.startTick);
    for (std::size_t i = 0; i < in.words5.size(); ++i) {
        line.word(indexed("words5_", i), in.words5[i]);
    }
    line.real("time", in.time);
    line.word("word6", in.word6);
    line.text("resource", an.resource);
    line.word("word", an.word);
    for (std::size_t i = 0; i < an.attached.size(); ++i) {
        line.word(indexed("attached", i), an.attached[i]);
    }
    // The clip's own length, and that of all its plays at its rate, for the rules between the two.
    double length = -1;
    std::string types;
    if (an.kind == ts2::cobject::Animation::Kind::Clip && !q.name.empty()) {
        if (const auto clip = clips.find(q.name)) {
            length = clip->duration;
            for (const auto t : clip->targetTypes) {
                types += (types.empty() ? "" : "+") + std::to_string(t);
            }
        }
    }
    line.text("targetTypes", types);
    line.number("clipLength", length);
    const float rate = std::bit_cast<float>(q.rate);
    const double played = length >= 0 && rate != 0 && in.plays > 0 ? length * in.plays / std::abs(rate) : -1;
    line.number("playsLength", played);
}

void interactionFields(const std::string& where, const char* role, std::size_t index, const ts2::xobj::SavedInteraction& s) {
    Line line("interaction");
    line.text("@at", where);
    line.text("role", role);
    line.number("index", static_cast<double>(index));
    line.number("blank", ts2::xobj::isBlank(s) ? 1 : 0);
    line.number("value1", s.value1);
    line.number("number", s.number);
    line.number("actor", s.actor);
    line.number("target", s.target);
    line.number("iconObject", s.iconObject);
    line.number("value2", s.value2);
    line.number("lineIndex", s.index);
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        line.number(indexed("values", i), s.values[i]);
    }
    line.number("priority", s.priority);
    line.word("tree", s.tree);
    line.number("value5", s.value5);
    line.word("flags", static_cast<Word>(s.flags));
    line.word("lineFlag1", s.lineFlag1);
    line.word("lineFlag2", s.lineFlag2);
    line.word("lineFlag3", s.lineFlag3);
    line.number("id", s.id);
    line.number("linkId", s.linkId);
    line.text("name", s.name);
    line.word("iconGuid", static_cast<Word>(s.iconGuid));
    line.number("iconIndex", s.iconIndex);
    line.number("value6", s.value6);
    line.print();
}

void personFields(Line& line, const std::string& where, const ts2::xobj::PersonPart& p) {
    line.number("personVersion", p.version);
    for (std::size_t i = 0; i < p.motives.size(); ++i) {
        line.number(indexed("motive", i), p.motives[i]);
        line.number(indexed("motiveB", i), p.motives2[i]);
    }
    for (std::size_t i = 0; i < p.personData.size(); ++i) {
        line.number(indexed("p", i), p.personData[i]);
    }
    const auto& s = p.state;
    line.number("stateUnread", p.stateUnread ? 1 : 0);
    line.number("queue", static_cast<double>(s.queue.size()));
    line.number("runs", static_cast<double>(s.runs.size()));
    line.number("motiveChanges", static_cast<double>(s.entries.size()));
    line.number("subStartCount", s.subStartCount);
    line.number("stateValue1", s.value1);
    line.number("hasRunningSub", s.runningSub ? 1 : 0);
    line.number("subQueue", static_cast<double>(s.subQueue.size()));
    line.text("stateText", s.text);
    line.number("pickedIconIndex", s.pickedIconIndex);
    line.word("pickedIconGuid", static_cast<Word>(s.pickedIconGuid));
    line.number("pickedIconObject", s.pickedIconObject);
    for (std::size_t i = 0; i < s.values.size(); ++i) {
        line.number(indexed("stateValues", i), s.values[i]);
    }
    line.number("stateValue2", s.value2);
    line.number("list", static_cast<double>(s.list.size()));
    line.word("stateValue3", s.collectiblesLow);
    line.word("stateValue4", s.collectiblesHigh);
    if (p.stateUnread) {
        return;
    }
    interactionFields(where, "running", 0, s.running);
    interactionFields(where, "interrupted", 0, s.interrupted);
    for (std::size_t i = 0; i < s.queue.size(); ++i) {
        interactionFields(where, "queue", i, s.queue[i]);
    }
    if (s.runningSub) {
        interactionFields(where, "runningSub", 0, *s.runningSub);
    }
    for (std::size_t i = 0; i < s.subQueue.size(); ++i) {
        interactionFields(where, "subQueue", i, s.subQueue[i]);
    }
    for (std::size_t i = 0; i < s.list.size(); ++i) {
        interactionFields(where, "list", i, s.list[i]);
    }
    for (std::size_t i = 0; i < s.entries.size(); ++i) {
        Line change("motiveChange");
        change.text("@at", where);
        change.number("index", static_cast<double>(i));
        change.number("key", s.entries[i].key);
        change.number("value1", s.entries[i].value1);
        change.number("value2", s.entries[i].value2);
        change.print();
    }
}

void objectFields(Line& line, const ts2::xobj::SavedObject& o) {
    line.number("version", o.version);
    const auto& h = o.header;
    for (std::size_t i = 0; i < h.reserved.size(); ++i) {
        line.number(indexed("reserved", i), h.reserved[i]);
    }
    line.number("location0", h.location[0]);
    line.number("location1", h.location[1]);
    line.number("header1", h.value1);
    line.number("header2", h.value2);
    line.number("header3", h.value3);
    line.number("header4", h.value4);
    line.number("header5", h.value5);
    line.number("header6", h.value6);
    line.number("attributeCount", static_cast<double>(o.attributes.size()));
    line.number("semiAttributeCount", static_cast<double>(o.semiAttributes.size()));
    const auto joined = [](const auto& values) {
        std::string text;
        for (const auto v : values) {
            text += (text.empty() ? "" : ",") + std::to_string(v);
        }
        return text;
    };
    line.text("attributes", joined(o.attributes));
    line.text("semiAttributes", joined(o.semiAttributes));
    for (std::size_t i = 0; i < o.temps.size(); ++i) {
        line.number(indexed("temp", i), o.temps[i]);
    }
    for (std::size_t i = 0; i < o.data.size(); ++i) {
        line.number(indexed("d", i), o.data[i]);
    }
    for (std::size_t i = 0; i < o.tokenSource.size(); ++i) {
        line.number(indexed("tokenSource", i), o.tokenSource[i]);
    }
    line.number("restBytes", static_cast<double>(o.rest.size()));
    line.number("hasTail", o.tail ? 1 : 0);
    if (!o.tail) {
        return;
    }
    const auto& t = *o.tail;
    line.word("tokenGuid", t.token.guid);
    line.word("tokenFlags", t.token.flags);
    line.number("tokenValue1", t.token.value1);
    line.number("tokenValue2", t.token.value2);
    line.number("tokenValue3", t.token.value3);
    line.number("tokenProperties", static_cast<double>(t.token.properties.size()));
    line.number("arrays", static_cast<double>(t.arrays.size()));
    line.number("shorts", static_cast<double>(t.shorts.size()));
    line.number("tailValue1", t.value1);
    line.number("strings", static_cast<double>(t.strings.size()));
    line.number("tailValue2", t.value2);
    line.number("stackFrames", static_cast<double>(t.stack.size()));
    line.number("stackValue", t.stackValue);
    line.number("relationshipVersion", t.relationshipVersion);
    line.number("relationships", static_cast<double>(t.relationshipIds.size()));
    std::string partners;
    for (const auto id : t.relationshipIds) {
        partners += (partners.empty() ? "" : ",") + std::to_string(id);
    }
    line.text("relationshipList", partners);
    line.number("slotMarker", t.slots.marker);
    line.number("slotExtra", t.slots.extra);
    line.number("slotValues", static_cast<double>(t.slots.values.size()));
    std::string slots;
    for (const auto v : t.slots.values) {
        slots += (slots.empty() ? "" : ",") + std::to_string(v);
    }
    line.text("slotList", slots);
    line.number("effectFlag", t.effectFlag);
    line.number("effectVersion", t.effectVersion);
    line.number("effects", static_cast<double>(t.effects.size()));
    line.number("materialOverrides", static_cast<double>(t.materials.size()));
    line.number("timers", static_cast<double>(t.timers.size()));
    line.number("materialBlockBytes", static_cast<double>(t.materialBlock.size()));
}

void dumpLot(const std::filesystem::path& path, ts2::scene::AnimationLibrary& clips) {
    const ts2::content::Lot lot = ts2::content::loadLot(path);
    const auto name = path.u8string();
    const std::string file(name.begin(), name.end());
    for (const auto& entry : lot.types.entries) {
        Line type("typeEntry");
        type.text("@lot", file);
        type.number("index", entry.index);
        type.word("guid", entry.guid);
        type.text("name", entry.name);
        for (std::size_t i = 0; i < entry.unknown.size(); ++i) {
            type.number(indexed("value", i), entry.unknown[i]);
        }
        type.number("kind", entry.kind);
        type.word("flag", entry.flag);
        type.word("secondGuid", entry.secondGuid);
        type.print();
    }
    std::map<std::uint32_t, Owner> owners;
    for (const auto& object : lot.objects) {
        const char* kind = object.resourceType == ts2::types::kSavedPerson ? "person"
                           : object.resourceType == ts2::types::kSavedObjectWithMaterial ? "objectWithMaterial" : "object";
        owners[object.id] = {object.type ? object.type->guid : 0u, kind};
        const std::string where = file + "#" + std::to_string(object.id);
        Line line("savedObject");
        line.text("@at", where);
        line.text("@lot", file);
        line.number("@lotVersion", lot.objectMap.version);
        line.word("guid", owners[object.id].guid);
        line.text("objectKind", kind);
        // The ticks left of a Sleep it was in, as the object map keeps them by id.
        const auto& waits = lot.objectMap.waitTicks;
        line.number("waitTicks", object.id >= 1 && object.id <= waits.size() ? waits[object.id - 1] : -1);
        objectFields(line, object.state);
        if (object.state.person) {
            personFields(line, where, *object.state.person);
        }
        line.print();
        for (std::size_t i = 0; object.state.tail && i < object.state.tail->stack.size(); ++i) {
            const auto& s = object.state.tail->stack[i];
            Line frame("stackFrame");
            frame.text("@at", where);
            frame.word("guid", owners[object.id].guid);
            frame.number("index", static_cast<double>(i));
            frame.number("objectId", s.objectId);
            frame.word("tree", static_cast<Word>(static_cast<std::uint16_t>(s.treeId)));
            frame.number("node", s.node);
            frame.number("value1", s.value1);
            frame.number("value2", s.value2);
            std::string params, locals;
            for (const auto v : s.params) {
                params += (params.empty() ? "" : ",") + std::to_string(v);
            }
            for (const auto v : s.locals) {
                locals += (locals.empty() ? "" : ",") + std::to_string(v);
            }
            frame.text("params", params);
            frame.text("locals", locals);
            frame.number("value3", s.value3);
            frame.number("codeOwner", s.codeOwner);
            frame.print();
        }
        for (std::size_t i = 0; object.state.tail && i < object.state.tail->effects.size(); ++i) {
            const auto& e = object.state.tail->effects[i];
            Line effect("effect");
            effect.text("@at", where);
            effect.word("guid", owners[object.id].guid);
            effect.number("index", static_cast<double>(i));
            effect.text("name", e.name);
            effect.text("bone", e.slot);
            effect.number("number", e.value1);
            effect.number("kind", e.value2);
            effect.number("keep", e.value3);
            effect.print();
        }
    }
    ts2::dbpf::Package package{path};
    for (const auto& e : package.entries()) {
        if (e.key.type != ts2::types::kSceneObject) {
            continue;
        }
        const auto record = ts2::cobject::readRecord(package.read(e));
        const std::string where = file + "#" + std::to_string(e.key.instance);
        const auto owner = owners.find(e.key.instance);
        const Owner who = owner == owners.end() ? Owner{0, "none"} : owner->second;
        Line line("sceneRecord");
        line.text("@at", where);
        line.text("@lot", file);
        line.number("@lotVersion", lot.objectMap.version);
        line.word("guid", who.guid);
        line.text("objectKind", who.kind);
        line.number("reads", record ? 1 : 0);
        if (record) {
            sceneFields(line, *record);
        }
        line.print();
        if (!record) {
            continue;
        }
        for (std::size_t i = 0; i < record->animatable.animations.size(); ++i) {
            Line clip("animation");
            clip.text("@at", where);
            clip.text("@lot", file);
            clip.number("@lotVersion", lot.objectMap.version);
            clip.word("guid", who.guid);
            clip.text("objectKind", who.kind);
            clip.number("chain", static_cast<int>(record->chain));
            animationFields(clip, record->animatable.animations[i], i, clips);
            clip.print();
        }
    }
}

} // namespace

int dumpSaveFields(const char* install, int count, char** lots) {
    ts2::scene::Assets assets(install);
    ts2::scene::AnimationLibrary clips(assets);
    // "@list" names a UTF-8 file of lot paths, one per line, for folder names the command line can't carry.
    std::vector<std::filesystem::path> paths;
    for (int i = 0; i < count; ++i) {
        if (lots[i][0] != '@') {
            paths.emplace_back(lots[i]);
            continue;
        }
        std::ifstream list(lots[i] + 1, std::ios::binary);
        for (std::string text; std::getline(list, text);) {
            while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) {
                text.pop_back();
            }
            if (!text.empty()) {
                paths.emplace_back(std::u8string(text.begin(), text.end()));
            }
        }
    }
    int failed = 0;
    for (const auto& path : paths) {
        try {
            dumpLot(path, clips);
        } catch (const std::exception& e) {
            ++failed;
            const auto name = path.filename().u8string();
            std::fprintf(stderr, "%s: %s\n", std::string(name.begin(), name.end()).c_str(), e.what());
        }
    }
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}
