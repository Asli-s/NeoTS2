// The family and Sim records NeoTS2 writes (for Create-a-Sim's Accept Family), and the pieces of a
// Sim's character file: each written as the game writes it, read back the same, and records read
// from bytes written back byte for byte.
#include "test_support.hpp"

#include "formats/binary_writer.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/ngbh.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/scenegraph.hpp"
#include "formats/record_roundtrip.hpp"
#include "formats/resource_types.hpp"
#include "formats/sdsc.hpp"
#include "formats/srel.hpp"
#include "formats/str.hpp"
#include "formats/swaf.hpp"
#include "scene/age_data.hpp"

namespace {

std::vector<std::uint8_t> bytesOf(std::string_view text) { return {text.begin(), text.end()}; }

void familyIsWrittenBack() {
    ts2::fami::Family f;
    f.magic = 0x46414D49;
    f.version = 0x56;
    f.lot = 12;
    f.currentLot = 12;
    f.vacationLot = 0;
    f.funds = 20000;
    f.funds2 = 20000;
    f.flags = 0x10;
    f.members = {0x11223344, 0x55667788};
    f.albumId = 7;
    const auto bytes = ts2::fami::encode(f);
    const auto again = ts2::fami::parse(bytes);
    CHECK(again.version == 0x56 && again.lot == 12 && again.funds == 20000 && again.members == f.members && again.albumId == 7);
    CHECK(again.funds2 == 20000 && again.extraBytes == 0);
    CHECK(ts2::fami::encode(again) == bytes);
    // An old record keeps the value after its funds and the bytes after its last value.
    ts2::BinaryWriter w;
    for (const std::uint32_t v : {0x46414D49u, 0x4Cu, 0u, 3u, 0u, 500u, 9u, 0u, 0u, 1u, 0xABCDu, 7u}) {
        w.u32(v);
    }
    w.u8(0xEE);
    const auto old = w.take();
    const auto read = ts2::fami::parse(old);
    CHECK(read.oldValue == 9 && read.members.size() == 1 && read.albumId == 7 && read.tail.size() == 1);
    CHECK(ts2::fami::encode(read) == old);
}

void simDescriptionIsWrittenBack() {
    ts2::sdsc::SimDescription s;
    s.version = 0x2E;
    s.personDataVersion = 0x20;
    s.personDataFlag = 1;
    s.personData = {100, 200, 300};  // short: the rest are written as 0
    s.neighborId = 42;
    s.guid = 0x8E000123;
    s.rest = {1, 2, 3, 4, 5};
    const auto bytes = ts2::sdsc::encode(s);
    CHECK(bytes.size() == 4 + 4 + 4 + ts2::sdsc::personDataCount(0x20) * 2 + 2 + 4 + 5);
    const auto again = ts2::sdsc::parse(bytes);
    CHECK(again.personData.size() == ts2::sdsc::personDataCount(0x20) && again.personData[2] == 300 && again.personData[3] == 0);
    CHECK(again.neighborId == 42 && again.guid == 0x8E000123 && again.rest == s.rest);
    CHECK(ts2::sdsc::encode(again) == bytes);
}

void relationshipsAndTiesAreWrittenBack() {
    ts2::srel::Relationship rel;
    rel.version = 2;
    rel.values = {10, -20, 0, 3};
    rel.tail = {9, 9};
    const auto relBytes = ts2::srel::encode(rel);
    const auto relAgain = ts2::srel::parse(relBytes);
    CHECK(relAgain.values == rel.values && relAgain.extraBytes == 2 && ts2::srel::encode(relAgain) == relBytes);

    ts2::famt::FamilyTies ties;
    ties.version = 1;
    ties.sims.push_back({5, 0, {{static_cast<std::uint32_t>(ts2::famt::TieType::Spouse), 6}}});
    ties.sims.push_back({6, 0, {{static_cast<std::uint32_t>(ts2::famt::TieType::Spouse), 5}, {4, 7}}});
    const auto tieBytes = ts2::famt::encode(ties);
    const auto tiesAgain = ts2::famt::parse(tieBytes);
    CHECK(tiesAgain.sims.size() == 2 && tiesAgain.sims[1].ties.size() == 2 && tiesAgain.sims[1].ties[1].other == 7);
    CHECK(ts2::famt::encode(tiesAgain) == tieBytes);
}

void stringTablesAreWrittenBack() {
    // A new table: its name padded to 64 bytes, each text in its language.
    ts2::str::StringTable t;
    t.name = "Family name";
    t.format = 0xFFFD;
    t.entries = {{1, "Pleasant", ""}, {2, "Pleasant", "fr"}};
    const auto bytes = ts2::str::encode(t);
    CHECK(bytes.size() == 64 + 2 + 2 + (1 + 9 + 1) + (1 + 9 + 3));
    const auto again = ts2::str::parse(bytes);
    CHECK(again.name == "Family name" && again.entries.size() == 2 && again.entries[1].description == "fr");
    CHECK(ts2::str::encode(again) == bytes);
    // A name field with bytes after the name's end, the old format, and a format not supported.
    std::vector<std::uint8_t> old(64, 0);
    old[0] = 'A';
    old[10] = 0x7F;
    old.insert(old.end(), {0x00, 0x02, 2, 'h', 'i', 0, 0xAB});
    const auto oldRead = ts2::str::parse(old);
    CHECK(oldRead.format == 0 && oldRead.entries.size() == 2 && oldRead.entries[0].value == "hi" && oldRead.tail.size() == 1);
    CHECK(ts2::str::encode(oldRead) == old);
    std::vector<std::uint8_t> odd(64, 0);
    odd.insert(odd.end(), {0x01, 0x02, 3, 4});
    const auto oddRead = ts2::str::parse(odd);
    CHECK(!oddRead.supported && ts2::str::encode(oddRead) == odd);
}

void wantsAreWrittenBack() {
    ts2::swaf::WantsAndFears s;
    s.version = 6;
    ts2::swaf::Want extra;
    extra.version = 10;
    extra.id = 5;
    extra.guid = 0xAAAA;
    extra.type = 6;
    extra.argument = 0x12345678;
    extra.value = 3;
    extra.tail = {1, 2, 3};
    extra.flags = 1;
    s.extra = extra;
    s.extraFlag = 2;
    s.maxWants = 4;
    s.maxFears = 3;
    // A want of each argument type.
    for (std::uint8_t type = 0; type <= 6; ++type) {
        ts2::swaf::Want w;
        w.version = 9;
        w.id = 5;
        w.guid = 0x1000u + type;
        w.type = type;
        w.argument = type == 1 || type == 4 || type == 6 ? 77 : 0x10000 + type;
        w.value = static_cast<std::int16_t>(type);
        w.tail = {10, -20, 30};
        w.flags = 0;
        (type % 2 == 0 ? s.wants : s.fears).push_back(w);
    }
    s.lastValue = 1;
    s.historyVersion = 2;
    s.historyValue = 99;
    // The same GUID twice in the history: kept apart, in order.
    s.historyGroups = {{0xBEEF, {s.wants[0]}}, {0xCAFE, {}}, {0xBEEF, {s.wants[1]}}};
    const auto bytes = ts2::swaf::encode(s);
    const auto again = ts2::swaf::parse(bytes);
    CHECK(again.extra && again.extra->argument == 0x12345678 && again.extraFlag == 2);
    CHECK(again.wants.size() == 4 && again.fears.size() == 3 && again.fears[0].argument == 77);
    CHECK(again.historyGroups.size() == 3 && again.history.at(0xBEEF).size() == 2);
    CHECK(ts2::swaf::encode(again) == bytes);
}

void neighborhoodRecordIsWrittenBack() {
    ts2::ngbh::Raw raw;
    raw.type = 0x4E474248;
    raw.version = 0xCB;
    raw.value0 = 1;
    raw.value1 = 2;
    raw.value2 = 3;
    raw.terrain = "Temperate";
    raw.values.assign(16, 0);
    raw.values[3] = 7;
    ts2::xobj::Token counted;
    counted.guid = 0x1234;
    counted.properties = {};  // the game gives it a count of 1 and a 0 when read
    raw.global.version = 4;
    raw.global.counted = {counted};
    ts2::ngbh::Inventory first, second;
    first.version = 4;
    second.version = 4;
    second.singular = {counted};
    // The same family twice: kept apart when written, the later one used when read.
    raw.families = {{3, first}, {3, second}};
    raw.neighbors = {{9, first}};
    raw.lastByte = 1;
    raw.lastValue = 5;
    raw.tail = {0, 0};
    const auto bytes = ts2::ngbh::encode(raw);
    const auto rawAgain = ts2::ngbh::readRaw(bytes);
    CHECK(rawAgain.families.size() == 2 && rawAgain.global.counted[0].properties.empty() && rawAgain.terrain == "Temperate");
    CHECK(ts2::ngbh::encode(rawAgain) == bytes);
    const auto used = ts2::ngbh::parse(bytes);
    CHECK(used.global.counted[0].properties == (std::vector<std::int16_t>{1, 0}));
    CHECK(used.families.size() == 1 && used.families.at(3).singular.size() == 1 && used.neighbors.count(9) == 1);
    CHECK(used.values[3] == 7 && used.extraBytes == 2);
}

void simDnaIsWrittenBack() {
    // As the neighborhood keeps it: LF line ends, two spaces in, nothing after the last tag.
    const std::string dna = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString>\n"
                            "  <AnyString key=\"1\" type=\"0x0b8bea18\">00000002-0000-0000-0000-000000000000</AnyString>\n"
                            "  <AnyString key=\"268435461\" type=\"0x0b8bea18\"></AnyString>\n"
                            "  <AnyString key=\"4\" type=\"0x0b8bea18\">-858851246:2133684651:1;</AnyString>\n"
                            "  <AnyString key=\"5\" type=\"0x0b8bea18\">nose,brow,cheek,jaw,</AnyString>\n"
                            "  <AnyString key=\"9\" type=\"0x0b8bea18\">a &amp; b &lt;c&gt;</AnyString>\n"
                            "  <AnyUint32 key=\"x\" type=\"0xeb61e4f7\"/>\n"
                            "</cGZPropertySetString>";
    const auto properties = ts2::propset::readText(bytesOf(dna));
    CHECK(properties.size() == 6 && properties[1].value.empty() && properties[2].value == "-858851246:2133684651:1;");
    CHECK(properties[4].value == "a & b <c>" && properties[5].empty && properties[5].element == "AnyUint32");
    const auto written = ts2::propset::encodeText(properties);
    CHECK(std::string(written.begin(), written.end()) == dna);
    // The same as the reader of property sets reads.
    const auto map = ts2::propset::parse(written);
    CHECK(map.at("5") == "nose,brow,cheek,jaw,");
}

void roundTripCheckRewritesTheRecordTypes() {
    CHECK(ts2::roundtrip::kinds().size() == 9);
    ts2::srel::Relationship rel;
    rel.version = 3;
    rel.values = {1};
    const auto bytes = ts2::srel::encode(rel);
    const auto again = ts2::roundtrip::rewrite(ts2::types::kSimRelationship, bytes);
    CHECK(again && *again == bytes);
    CHECK(!ts2::roundtrip::rewrite(0x12345678, bytes));
}

void referenceListsAreWrittenBack() {
    const std::vector<ts2::scenegraph::Link> links = {{0xE519C933, 0xFFFFFFFF, 0xFFDF89DF, 0xFCB631C9}, {}, {0xEBCF3E27, 0x2C17B74A, 0x2C204CEF, 0}};
    const auto bytes = ts2::reflist::encode(links);
    CHECK(bytes.size() == 12 + 3 * 16);
    const auto again = ts2::reflist::parse(bytes);
    CHECK(again.size() == 3 && again[0].instanceHigh == 0xFCB631C9 && again[2].instance == 0x2C204CEF && again[1].type == 0);
}

void ageDataIsWrittenAsTheGameWritesIt() {
    using ts2::scenegraph::Link;
    ts2::scene::AgeData data;
    data.properties = {{"age", "8"}, {"gender", "2"}, {"haircolor", "00000002-0000-0000-0000-000000000000"}, {"stretch", "1"}};
    const Link hair{0xEBCF3E27, 0x2C17B74A, 0x11, 0}, face{0xEBCF3E27, 0xFFFFFFFF, 1, 0}, top{0xEBCF3E27, 0x2C17B74A, 0x22, 0};
    data.lists[1u << 16 | 1] = {hair};
    data.lists[1u << 16 | 2] = {face};
    data.lists[8u << 16 | 1] = {hair};
    data.lists[8u << 16 | 4] = {top};
    data.skeleton = Link{0xE519C933, 0x1C0532FA, 0xFF6B8A91, 0x3CC1ADAA};
    const Link ownNode{0xE519C933, 0xFFFFFFFF, 0xFFDF89DF, 0xFCB631C9};
    const auto written = ts2::scene::writeAgeData(data, {ownNode, Link{}});
    const auto refs = ts2::reflist::parse(written.references);
    // The leading entries, the skeleton, then each part once, in the order first named.
    CHECK(refs.size() == 6 && refs[0].instance == 0xFFDF89DF && refs[2].instance == 0xFF6B8A91 && refs[3].instance == 0x11 &&
          refs[4].instance == 1 && refs[5].instance == 0x22);
    const std::string text(written.properties.begin(), written.properties.end());
    CHECK(text.find("<AnyUint32 key=\"le0_0\" type=\"0xeb61e4f7\">3</AnyUint32>") != std::string::npos);
    CHECK(text.find("<AnyString key=\"lk1\" type=\"0x0b8bea18\">65538</AnyString>") != std::string::npos);
    CHECK(text.find("<AnyFloat32 key=\"stretch\" type=\"0xabc78708\">1</AnyFloat32>") != std::string::npos);
    CHECK(text.find("<AnyString key=\"creatorid\" type=\"0x0b8bea18\">00000000-0000-0000-0000-000000000000</AnyString>") != std::string::npos);
    CHECK(text.find("key=\"skeletonkeyidx\" type=\"0xeb61e4f7\">2<") != std::string::npos);
    const auto again = ts2::scene::readAgeData(ts2::propset::parse(written.properties), refs);
    CHECK(again.lists.size() == 4 && again.lists.at(8u << 16 | 4).front().instance == 0x22 && again.skeleton &&
          again.skeleton->instance == 0xFF6B8A91);
    CHECK(again.properties.at("age") == "8" && again.properties.at("validparts") == "65535");
}

void copiedResourcesAreRenamedAndRelinked() {
    namespace sg = ts2::scenegraph;
    sg::Material m;
    m.name = "amface_s2_txmt";
    m.description = "amface_s2";
    m.type = "SimStandardMaterial";
    m.properties = {{"stdMatBaseTextureName", "amface-s2"}, {"reflectivity", "0"}};
    m.textures = {"amface-s2"};
    auto bytes = sg::encodeMaterial(m);
    const auto read = sg::parseMaterial(bytes);
    CHECK(read.version == 11 && read.name == "amface_s2_txmt" && read.order.front() == "reflectivity" && read.textures.size() == 1);
    CHECK(sg::encodeMaterial(read) == bytes);
    sg::renameResource(bytes, "#0x7fdda187!ss_customFace_0xe0a714f_3_0_cmpm_txmt");
    CHECK(sg::replaceName(bytes, "amface-s2", "uuface-eye-gray") == 2);
    const auto renamed = sg::parseMaterial(bytes);
    CHECK(renamed.name == "#0x7fdda187!ss_customFace_0xe0a714f_3_0_cmpm_txmt" && renamed.properties.at("stdMatBaseTextureName") == "uuface-eye-gray");
    CHECK(renamed.textures.front() == "uuface-eye-gray" && renamed.description == "amface_s2");
    // A link of a container with high instances made another.
    ts2::BinaryWriter w;
    for (const std::uint32_t v : {0xFFFF0001u, 1u, 0x1C0532FAu, 0xFF5451A3u, 0xA9573D6Au, 0xFC6EB1F7u, 0u}) {
        w.u32(v);
    }
    auto node = w.take();
    sg::setContainerLink(node, 0, {0xFC6EB1F7, 0xFFFFFFFF, 0xFFF14C45, 0xD6E9E5B3});
    const auto links = sg::parseContainer(node).links;
    CHECK(links.size() == 1 && links[0].group == 0xFFFFFFFF && links[0].instance == 0xFFF14C45 && links[0].instanceHigh == 0xD6E9E5B3);
    bool threw = false;
    try {
        sg::setContainerLink(node, 1, {});
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void oldMaterialsAreWrittenBackAsTheyWere() {
    namespace sg = ts2::scenegraph;
    // An older material: a links header without high instances, version 10, keys out of order.
    ts2::BinaryWriter w;
    for (const std::uint32_t v : {0u, 1u, 0x49596978u}) {
        w.u32(v);
    }
    w.varString("cMaterialDefinition");
    w.u32(0x49596978);
    w.u32(10);
    w.varString("cSGResource");
    w.u32(0);
    w.u32(2);
    w.varString("old_txmt");
    w.varString("old");
    w.varString("StandardMaterial");
    w.u32(2);
    for (const char* s : {"zKey", "1", "aKey", "2"}) {
        w.varString(s);
    }
    w.u32(1);
    w.varString("oldtexture");
    const auto bytes = w.take();
    const auto m = sg::parseMaterial(bytes);
    CHECK(m.version == 10 && m.order.size() == 2 && m.order[0] == "zKey");
    CHECK(sg::encodeMaterial(m) == bytes);
}

} // namespace

std::vector<Test> recordWriterTests() {
    return {
        {"a family record is written back as the game writes it", familyIsWrittenBack},
        {"a Sim description is written back as the game writes it", simDescriptionIsWrittenBack},
        {"relationships and family ties are written back", relationshipsAndTiesAreWrittenBack},
        {"string tables are written back byte for byte", stringTablesAreWrittenBack},
        {"wants and fears are written back in their saved order", wantsAreWrittenBack},
        {"the neighborhood record is written back with every inventory", neighborhoodRecordIsWrittenBack},
        {"Sim DNA is written back as the neighborhood keeps it", simDnaIsWrittenBack},
        {"the round-trip check rewrites the family record types", roundTripCheckRewritesTheRecordTypes},
        {"reference lists are written back", referenceListsAreWrittenBack},
        {"age data is written as the game writes a Sim's", ageDataIsWrittenAsTheGameWritesIt},
        {"copied scenegraph resources are renamed and relinked", copiedResourcesAreRenamedAndRelinked},
        {"older materials are written back as they were", oldMaterialsAreWrittenBackAsTheyWere},
    };
}
