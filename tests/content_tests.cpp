// Content found in the install: products, neighborhoods, UI data and lot lists.
#include "test_support.hpp"

#include "content/resource_index.hpp"
#include "content/save_folder.hpp"
#include "content/user_props.hpp"
#include "content/ui_queue.hpp"
#include "formats/dbpf_writer.hpp"
#include "parse_float.hpp"
#include "paths/save_folders.hpp"

#include <cstdlib>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

namespace {

// The build catalog scripts: pattern materials keyed in lowercase, wall styles and roof patterns.
void buildScriptsReadPatternsAndWallStyles() {
    ts2::content::BuildScripts b;
    b.addPatterns("# comment\r\nwallPattern \"deckredwood\"\r\nmaterial \"wall_deckRedwoodAlpha_base\"\r\nnotInCatalog\r\n", true);
    b.addPatterns("floorPattern \"sidewalk\"\nmaterial \"floor_sidewalk\"\n", false);
    b.addWallStyles("wall 16\ndefaultPattern \"deckRedwood\"\nwallThickness none\nverticalSpan floorToFloorBasedCeiling\n"
                    "wall 4\nnotRendered\nwall 1\nwallThickness standard\nwall 3\nverticalSpan floorToRoof\n"
                    "wall 301\nstageWall\nwall 23\nverticalSpan floorToFloor\n");
    // Room walls (nWorld.GetNumWallSections): drawn and floor to floor. Fences and unknown styles are not.
    CHECK(b.isRoomWall(1) && b.isRoomWall(23));
    CHECK(!b.isRoomWall(4) && !b.isRoomWall(16) && !b.isRoomWall(3) && !b.isRoomWall(301));
    CHECK(!b.isRoomWall(2) && !b.isRoomWall(0x8D0EB171));
    CHECK(b.wallPatterns.at("deckredwood") == "wall_deckRedwoodAlpha_base" && b.floorPatterns.at("sidewalk") == "floor_sidewalk");
    CHECK(!b.wallStyles.at(16).thick && b.wallStyles.at(16).belowFloor && b.wallStyles.at(16).defaultPattern == "deckredwood");
    CHECK(!b.wallStyles.at(4).rendered && b.wallStyles.at(1).thick && !b.wallStyles.at(1).belowFloor);
    CHECK(b.wallStyles.at(3).toRoof && !b.wallStyles.at(1).toRoof && !b.wallStyles.at(16).toRoof);
    // The base game's default roof, pattern 0.
    b.addRoofPatterns("version 1\nroofPattern 0\nrooftop \"roof-manor\"\nrooftopbump \"roof-manor-bump\"\nmayUseBump true\n"
                      "roofunder \"roof-manor-under\"\nroofedges \"roof-manor-edges\"\nrooftrim \"roof-manor-trim\"\nnotInCatalog\ndefault");
    const auto& manor = b.roofPatterns.at(0);
    CHECK(manor.top == "roof-manor" && manor.under == "roof-manor-under" && manor.edges == "roof-manor-edges" && manor.trim == "roof-manor-trim");
}

// Animate Sim from the adult list: a Sim reads its own age's list at the same entry, the entry's
// kind dropped from the name, and the adult entry when its age's names nothing the game has.
void animateSimUsesItsAgesList() {
    Instruction animate;
    animate.opcode = 0x006A;
    animate.trueTarget = T;
    animate.falseTarget = E;
    animate.operands[3] = 32;
    animate.operands[6] = 0x81;
    MemoryCodeSource code;
    code.addStrings(kObjectGroup, 0x81, {"a-idle-breatheloop(idle)"});
    code.addStrings(kObjectGroup, 0x8A, {"t-idle-breatheloop(idle)"});
    code.addStrings(kObjectGroup, 0x8B, {""});
    code.addStrings(kObjectGroup, 0x82, {"c-missing(idle)"});
    code.add(kObjectGroup, 0x1000, bhav({animate}));
    ts2::sim::World world;
    world.setAnimationLengths([](const std::string& name) -> std::optional<float> {
        if (name == "a-idle-breatheloop" || name == "t-idle-breatheloop") return 1000.0f;
        return std::nullopt;
    });
    Vm vm(code, {}, &world);
    const auto plays = [&](std::int16_t age) {
        auto& sim = world.create(kObjectGroup, typed(0x51515151, ts2::sim::kPersonType));
        sim.personData[0x3A] = age;
        vm.run(sim, kObjectGroup, 0x1000);
        return sim.animation ? sim.animation->name : std::string();
    };
    CHECK(plays(19) == "a-idle-breatheloop");
    CHECK(plays(16) == "t-idle-breatheloop");
    CHECK(plays(51) == "a-idle-breatheloop");
    CHECK(plays(5) == "a-idle-breatheloop");

    using ts2::content::ageAnimationList;
    CHECK(ageAnimationList(0x81, 0) == 0x91 && ageAnimationList(0x81, 2) == 0x89 && ageAnimationList(0x81, 3) == 0x82 &&
          ageAnimationList(0x81, 15) == 0x82 && ageAnimationList(0x81, 18) == 0x8A && ageAnimationList(0x81, 50) == 0x81 &&
          ageAnimationList(0x81, 51) == 0x8B && ageAnimationList(0x86, 16) == 0x86);
    using ts2::content::objectAnimationList;
    CHECK(objectAnimationList(0, 30) == 0x86 && objectAnimationList(0, 51) == 0x190 && objectAnimationList(0, 16) == 0x191 &&
          objectAnimationList(0, 3) == 0x192 && objectAnimationList(0, 2) == 0x193 && objectAnimationList(0, 1) == 0x194);
    CHECK(objectAnimationList(1, 30) == 0x195 && objectAnimationList(1, 10) == 0x197 && objectAnimationList(1, 60) == 0x19A &&
          objectAnimationList(2, 30) == 0x199 && objectAnimationList(2, 10) == 0x197 && objectAnimationList(2, 60) == 0x19B &&
          objectAnimationList(3, 30) == 0x196 && objectAnimationList(3, 10) == 0x198 && objectAnimationList(3, 60) == 0x19C &&
          objectAnimationList(4, 30) == 0x86 && objectAnimationList(-1, 30) == 0x86);
    CHECK(ts2::content::animationName("a-x(idle)") == "a-x" && ts2::content::animationName("a-x") == "a-x" &&
          ts2::content::animationName("(idle)").empty());
    CHECK(ts2::content::animationName("a-x ") == "a-x" && ts2::content::animationName(" ").empty());
}

void objectsStandOnTheirStoreyWhenGridsStartBelowTheGround() {
    // A one-tile lot whose grids start a level below the ground: -3 there, the ground at 0 and a
    // foundation floor at 0.75.
    ts2::content::Lot lot;
    ts2::array3d::Array3D heights;
    heights.width = heights.height = 2;
    heights.levels = 3;
    heights.elementSize = 4;
    for (const float h : {-3.0f, 0.0f, 0.75f}) {
        for (int corner = 0; corner < 4; ++corner) {
            std::uint8_t bytes[4];
            std::memcpy(bytes, &h, 4);
            heights.elements.insert(heights.elements.end(), bytes, bytes + 4);
        }
    }
    lot.grids.emplace(ts2::content::kFloorHeightGridInstance, heights);
    lot.roomGraph.emplace();
    lot.roomGraph->firstLevel = -1;
    const ts2::scene::Heights grid(lot);
    CHECK(ts2::content::gridLevel(lot, 0) == 0 && grid.tile(ts2::content::gridLevel(lot, 0), 0.5f, 0.5f) == -3.0f);
    CHECK(ts2::content::gridLevel(lot, 1) == 1 && grid.tile(ts2::content::gridLevel(lot, 1), 0.5f, 0.5f) == 0.0f);
    CHECK(ts2::content::gridLevel(lot, 2) == 2 && grid.tile(ts2::content::gridLevel(lot, 2), 0.5f, 0.5f) == 0.75f);
    // Walls, fences and roofs save their level counted from the ground storey (0).
    CHECK(ts2::content::gridIndex(lot, 1) == 2 && ts2::content::gridIndex(lot, -1) == 0);
    // Sun shadows count levels from the ground storey too.
    ts2::array3d::Array3D floors;
    floors.width = floors.height = 1;
    floors.levels = 3;
    floors.elementSize = 8;
    floors.elements.assign(3 * 8, 0);
    lot.grids.emplace(ts2::content::kFloorGridInstance, floors);
    CHECK(ts2::content::groundGridLevel(lot) == 1);
    const auto shadowIn = ts2::scene::lotshadow::fromLot(lot, {});
    CHECK(shadowIn.levels == 2 && shadowIn.corner(0, 0, 0) == 0.0f && shadowIn.corner(0, 0, 1) == 0.75f);
    // The lighting's view starts at the ground storey: no grid levels or walls below it.
    lot.roomGraph->points = {{1, 0, 0, -1}, {2, 1, 0, -1}, {3, 0, 0, 0}, {4, 1, 0, 0}};
    lot.roomGraph->walls = {{1, 1, 0, 2, 0}, {2, 3, 0, 4, 0}};
    const auto lit = ts2::content::fromGroundStorey(lot);
    CHECK(lit.grids.at(ts2::content::kFloorGridInstance).levels == 2 && ts2::content::groundGridLevel(lit) == 0);
    CHECK(ts2::scene::Heights(lit).tile(0, 0.5f, 0.5f) == 0.0f && ts2::content::gridLevel(lit, 2) == 1);
    CHECK(lit.roomGraph->firstLevel == 0 && lit.roomGraph->points.size() == 2 && lit.roomGraph->walls.size() == 1 &&
          lit.roomGraph->walls[0].id == 2);
    // Grids starting at the ground storey (most lots), or no room graph at all.
    lot.roomGraph->firstLevel = 0;
    CHECK(ts2::content::gridLevel(lot, 2) == 1);
    lot.roomGraph.reset();
    CHECK(ts2::content::gridLevel(lot, 1) == 0);
    // Broken saved values stay in a safe range.
    lot.roomGraph.emplace();
    lot.roomGraph->firstLevel = std::numeric_limits<std::int32_t>::max();
    CHECK(ts2::content::gridLevel(lot, std::numeric_limits<std::int32_t>::min()) < 0);
    CHECK(grid.tile(ts2::content::gridLevel(lot, std::numeric_limits<std::int32_t>::min()), 0.5f, 0.5f) < -3.0f);
}

void outsideGridCoversUnderFloorsAndRoofs() {
    // Two tiles on three storeys (heights 0, 3, 6), and a deck on storey 1 over the first tile.
    ts2::content::Lot lot;
    ts2::array3d::Array3D heights, surface, floors;
    heights.width = 3;
    heights.height = 2;
    heights.levels = 3;
    heights.elementSize = 4;
    for (const float h : {0.0f, 3.0f, 6.0f}) {
        for (int corner = 0; corner < 6; ++corner) {
            std::uint8_t bytes[4];
            std::memcpy(bytes, &h, 4);
            heights.elements.insert(heights.elements.end(), bytes, bytes + 4);
        }
    }
    surface.width = floors.width = 2;
    surface.height = floors.height = 1;
    surface.levels = floors.levels = 3;
    surface.elementSize = 4;
    floors.elementSize = 8;
    surface.elements.assign(2 * 3 * 4, std::uint8_t{1});
    floors.elements.assign(2 * 3 * 8, std::uint8_t{0});
    std::fill_n(floors.elements.begin() + 2 * 8, 8, std::uint8_t{1});  // storey 1, first tile
    lot.grids.emplace(ts2::content::kFloorHeightGridInstance, heights);
    lot.grids.emplace(ts2::content::kSurfaceGridInstance, surface);
    lot.grids.emplace(ts2::content::kFloorGridInstance, floors);
    const ts2::sim::OutsideGrid open(lot, {});
    CHECK(open.width() == 2 && open.height() == 1 && open.levels() == 3);
    CHECK(open.tile(1, 0, 0) == 0xF && open.tile(0, 0, 0) == 0x10 && open.tile(0, 1, 0) == 0xF && open.tile(1, 1, 0) == 0xF);
    CHECK(open.open(1, 0.5f, 0.5f) == true && open.open(0, 0.5f, 0.5f) == false && !open.open(0, 2.5f, 0.5f) &&
          !open.open(0, std::nanf(""), 0.5f) && !open.open(7, 0.5f, 0.5f));
    // A gable over the first tile, its eaves on storey 2, covers the deck. Its overhang doesn't reach the second tile.
    lot.roofs.push_back({1, 0, 0, 2, 1, 1, 2, 3, 0});
    const ts2::sim::OutsideGrid roofed(lot, {});
    CHECK(roofed.tile(1, 0, 0) == 0x10 && roofed.tile(1, 1, 0) == 0xF);
    // A broken roof is left out.
    lot.roofs.back().x1 = std::nanf("");
    CHECK(ts2::sim::OutsideGrid(lot, {}).tile(1, 0, 0) == 0xF);
}

void lightFilesAreReadAsTheGame() {
    // Macros, overrides by light name in any case, conditions and state modifiers.
    const auto l = ts2::content::readLightingText(
        "seti lightsOff 0\n"
        "define Four(lightName)\n"
        "  lightAttribOverride &lightName diffusionSourceIntensity 1   # four\n"
        "  lightAttribOverride &{lightName} diffusionSourceFloorIntensityScale .65\n"
        "enddef\n"
        "define Amber(lightName)\n"
        "  lightAttribOverride &lightName diffusionSourceColor (1,0.85,0.7)\n"
        "  lightAttribOverride &lightName color(.5,.6,.7)\n"
        "enddef\n"
        "create Four(MyLamp_light)\n"
        "create Amber (MyLamp_light)\n"
        "lightAttribOverride MyLamp_light diffusionSourceMin (0,.2)\n"
        "lightAttribOverride MyLamp_light diffusionSourceIsDirectional 1\n"
        "if ($lightsOff)\n"
        "  create Four(Hidden_light)\n"
        "else\n"
        "  create Four(Shown_light)\n"
        "endif\n"
        "lightingState day\n"
        "  modifiers insideObjects .8 .7 .35 .5\n"
        "  diffParam insideAmbientColor (.85, .75, .75)\n"
        "  skylight (1, 1, 1) 0.5\n"
        "  portalSaturation 0.6\n"
        "  projectiveShadowStrength 0.73\n"
        "end\n"
        "lightingState dusk : day\n"
        "  modifiers insideObjects 2 3\n"
        "end\n"
        "lightingState dark\n"
        "  skylight (0.2, 0.2, 0.4)\n"
        "end\n"
        "lightingState plain\n"
        "end\n"
        "include \"elsewhere.txt\"\n");
    const auto* lamp = l.light("mylamp_LIGHT");
    CHECK(lamp != nullptr);
    if (lamp) {
        CHECK(lamp->diffusionIntensity == 1.0f && lamp->diffusionFloorScale == 0.65f);
        CHECK((lamp->diffusionColor == std::array<float, 3>{1, 0.85f, 0.7f}));
        CHECK((lamp->color == std::array<float, 3>{0.5f, 0.6f, 0.7f}));
        CHECK((lamp->diffusionMin == std::array<float, 2>{0, 0.2f} && lamp->diffusionDirectional == true));
        CHECK(!lamp->diffusionFalloff);
    }
    CHECK(l.light("hidden_light") == nullptr && l.light("shown_light") != nullptr);
    const auto& day = l.states.at("day").modifiers.at("insideObjects");
    CHECK(day.practicalStrength == 0.8f && day.diffusionStrength == 0.7f && day.practicalFalloff == 0.35f && day.diffusionFalloff == 0.5f);
    const auto& dusk = l.states.at("dusk").modifiers.at("insideObjects");
    CHECK(dusk.practicalStrength == 2 && dusk.diffusionStrength == 3 && dusk.diffusionFalloff == 0.5f);
    CHECK((l.states.at("day").diffParams.at("insideAmbientColor") == std::vector<float>{0.85f, 0.75f, 0.75f}));
    // A skylight line marks the state (its strength may be left out), and states inherit it.
    CHECK(l.states.at("day").hasSkylight && l.states.at("dusk").hasSkylight && l.states.at("dusk").portalSaturation == 0.6f);
    CHECK(l.states.at("dark").hasSkylight && (l.states.at("dark").skylight == std::array<float, 3>{0.2f, 0.2f, 0.4f}));
    CHECK(!l.states.at("plain").hasSkylight && l.states.at("plain").portalSaturation == 1);
    CHECK(l.states.at("dusk").projectiveShadowStrength == 0.73f && l.states.at("plain").projectiveShadowStrength == 1);

    // Macros that create themselves or multiply stop at the limits instead of running away.
    const auto bomb = ts2::content::readLightingText(
        "define A(x)\n create A(&x)\n create A(&x)\n lightAttribOverride &x intensity 1\nenddef\n"
        "create A(boom_light)\n");
    CHECK(bomb.light("boom_light") != nullptr);
    // An argument repeated in the macro's own create grows 8 times per level: lines past 64 KB
    // are left out instead of taking all memory.
    const auto growth = ts2::content::readLightingText(
        "define G(x)\n lightAttribOverride &x intensity 1\n create G(&x&x&x&x&x&x&x&x)\nenddef\n"
        "create G(grow_light)\n");
    CHECK(growth.light("grow_light") != nullptr);
    // Long lines that double at every level stop at the byte budget (64 MB), far short of the
    // lines and line length the other limits allow.
    const std::string wide(30000, 'w');
    const auto doubling = ts2::content::readLightingText(
        "define D(x)\n lightAttribOverride &x intensity 1\n create D(&x)\n create D(&x)\nenddef\n"
        "create D(" + wide + ")\n");
    CHECK(doubling.light(wide) != nullptr);
}

void objectDaylightMatchesTheGame() {
    ts2::content::Lighting lighting;
    auto& day = lighting.states["day"];
    day.skylight = {0.5f, 0.5f, 0.5f};
    day.lights["sun"] = {{1.5f, 1.2f, 2}, {0.97f, 0.94f, 0.83f}, 2};
    day.lights["sky"] = {{-1.5f, -1.2f, 2}, {0.8f, 0.8f, 0.73f}, 0.5f};
    day.lights["fill1"] = {{1, 1, -1}, {1, 0.8f, 0.9f}, 0.5f};
    const auto l = ts2::scene::objectDaylight(lighting);
    CHECK(l && l->lights.size() == 3);
    auto near = [](const std::array<float, 3>& a, std::array<float, 3> b) {
        return std::abs(a[0] - b[0]) < 1e-4f && std::abs(a[1] - b[1]) < 1e-4f && std::abs(a[2] - b[2]) < 1e-4f;
    };
    CHECK(near(l->ambient, {0.5f, 0.5f, 0.5f}));
    CHECK(near(l->lights[0].color, {0.94f, 0.88f, 0.66f}));  // sun
    CHECK(near(l->lights[1].color, {0.25f, 0.15f, 0.2f}));   // fill1
    CHECK(near(l->lights[2].color, {0.15f, 0.15f, 0.115f})); // sky
    CHECK(near(l->lights[0].direction, {0.5409f, 0.4327f, 0.7212f}));
    CHECK(!ts2::scene::objectDaylight(ts2::content::Lighting{}));
    // At night the skylight taken off is the night's: the sun the game drew on a winter lot at 7 pm.
    ts2::content::Lighting::State night;
    night.skylight = {0.1875f, 0.1875f, 0.225f};
    night.lights["sun"] = {{-1.5f, -1, 2}, {0.45f, 0.45f, 1}, 0.6f};
    const auto n = ts2::scene::outdoorLighting(night);
    CHECK(n.lights.size() == 1 && near(n.lights[0].color, {0.1575f, 0.1575f, 0.465f}));

    // Sims outdoors also get simFill1 and simFill2, all sorted by brightness as the game drew them on
    // the Smiths' lot: sun, simFill2, simFill1, fill1, sky.
    day.lights["simFill1"] = {{1, -1.2f, 1}, {1, 0.95f, 0.89f}, 0.5f};
    day.lights["simFill2"] = {{-2, 2, 1}, {1, 0.95f, 0.89f}, 1.75f};
    const auto withSims = ts2::scene::objectDaylight(lighting);
    CHECK(withSims && withSims->lights.size() == 3 && withSims->simLights.size() == 2);
    if (!withSims) return;
    const auto sims = ts2::scene::withSimLights(*withSims);
    CHECK(sims.lights.size() == 5 && sims.simLights.empty());
    if (sims.lights.size() == 5) {
        CHECK(near(sims.lights[0].color, {0.94f, 0.88f, 0.66f}) && near(sims.lights[1].color, {0.875f, 0.7875f, 0.6825f}));
        CHECK(near(sims.lights[2].color, {0.25f, 0.225f, 0.195f}) && near(sims.lights[3].color, {0.25f, 0.15f, 0.2f}));
    }
}

void installedPacksAreFound() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "neots2-install-test";
    fs::remove_all(root);
    auto pack = [&](const fs::path& folder) {
        fs::create_directories(root / folder / "TSData" / "Res" / "Objects");
        std::ofstream(root / folder / "TSData" / "Res" / "Objects" / "objects.package").put('\0');
    };
    auto codes = [&] {
        std::string out;
        for (const auto& product : ts2::content::findProducts(root)) {
            out += product.code + " ";
        }
        return out;
    };
    for (const char* folder : {"Legacy/Base", "Legacy/EP4", "Legacy/SP8", "Legacy/EP9"}) {
        pack(folder);
    }
    CHECK(codes() == "base ep4 sp8 sp9 ");
    CHECK((ts2::content::installedPacksMask(ts2::content::findProducts(root)) & (1u << 16)) != 0);  // Mansion & Garden
    fs::remove_all(root);
    for (const char* folder : {"Double Deluxe/Base", "Fun with Pets/SP9", "Seasons"}) {
        pack(folder);
    }
    CHECK(codes() == "base ep5 sp9 ");

    // Catalog bundles with objects (the Holiday packs' H05/H06) belong to their product.
    // globalcatbin and the localized bins are not object sources.
    auto file = [&](const fs::path& relative) {
        fs::create_directories((root / relative).parent_path());
        std::ofstream(root / relative).put('\0');
    };
    file("Double Deluxe/Base/TSData/Res/Catalog/Bins/H05.bundle.package");
    file("Double Deluxe/Base/TSData/Res/Catalog/Bins/globalcatbin.bundle.package");
    file("Double Deluxe/Base/TSData/Res/Locale/Japanese/Bins/H05.bundle.package");
    std::vector<std::string> bundles;
    for (const auto& product : ts2::content::findProducts(root)) {
        for (const auto& b : product.objectBundles) {
            bundles.push_back(product.code + " " + b.filename().string());
        }
    }
    CHECK((bundles == std::vector<std::string>{"base H05.bundle.package"}));
    fs::remove_all(root);
}

namespace savefolder {

namespace fs = std::filesystem;

void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int count(const fs::path& folder) {
    std::error_code error;
    int n = 0;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        ++n;
    }
    return n;
}

// Three packs, a folder the install holds beyond the known packs, and a file outside the install.
fs::path makeInstall(const fs::path& root) {
    const fs::path install = root / "install";
    for (const char* pack : {"Double Deluxe/Base", "Seasons", "Apartment Life", "Backup"}) {
        write(install / pack / "TSData/Res/Objects/objects.package", "x");
        write(install / pack / "TSData/Res/UserData/Config/broadcast.ini", pack);
        write(install / pack / "TSData/Res/UserData/Storytelling/story.jpg", pack);
    }
    write(install / "Double Deluxe/Base/TSData/Res/UserData/Neighborhoods/N001/N001_Neighborhood.package", "n1");
    write(install / "Seasons/TSData/Res/UserData/Neighborhoods/G001/G001_Neighborhood.package", "g1");
    write(install / "Backup/TSData/Res/UserData/Neighborhoods/X001/X001_Neighborhood.package", "x1");
    write(install / "Seasons/TSData/Res/UserData/Accessory.cache", "acc");
    write(install / "Seasons/TSData/Res/UserData/properties.txt", "not copied");
    write(root / "outside/secret.txt", "secret");
    return install;
}

// A Windows junction (a folder link anyone can make) at `link`, leading to `target`.
bool junction(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    fs::create_directories(link.parent_path());
    const std::wstring command = L"cmd /c mklink /J \"" + link.native() + L"\" \"" + target.native() + L"\" >nul";
    return _wsystem(command.c_str()) == 0 && fs::exists(link);
#else
    (void)link;
    (void)target;
    return false;
#endif
}

// Fills the way the game does, with the folder that holds the save folder made and its lock held.
ts2::content::SaveFolderFill fillFolder(const fs::path& install, const fs::path& folder) {
    const fs::path named = ts2::paths::saveFolderPath(folder);
    std::error_code error;
    if (!named.empty()) {
        fs::create_directories(named.parent_path(), error);
    }
    const ts2::paths::SaveFolderLock lock(folder);
    return ts2::content::fillNewSaveFolder(install, lock);
}

} // namespace savefolder

// An empty save folder gets every known pack's shipped folders, the newest pack's copy of a shared
// file, and Storytelling from the base game only. Nothing comes from outside the install or from
// folders that aren't packs.
void newSaveFolderIsFilledAsTheGame() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    const fs::path saves = root / "saves";
    fs::create_directories(saves);
    const bool linked = junction(install / "Seasons/TSData/Res/UserData/Neighborhoods/Link", root / "outside");

    const auto fill = fillFolder(install, saves);
    CHECK(fill.filled && fill.problem.empty());
    CHECK(read(saves / "Neighborhoods/N001/N001_Neighborhood.package") == "n1");
    CHECK(read(saves / "Neighborhoods/G001/G001_Neighborhood.package") == "g1");
    CHECK(read(saves / "Config/broadcast.ini") == "Apartment Life");
    CHECK(read(saves / "Storytelling/story.jpg") == "Double Deluxe/Base");
    CHECK(read(saves / "Accessory.cache") == "acc");
    CHECK(fs::is_directory(saves / "Thumbnails") && fs::is_empty(saves / "Thumbnails"));
    CHECK(fs::is_directory(saves / "LotCatalog") && fs::is_empty(saves / "LotCatalog"));
    CHECK(!fs::exists(saves / "Neighborhoods/X001") && !fs::exists(saves / "Neighborhoods/Link"));
    CHECK(!fs::exists(saves / "properties.txt"));
    CHECK(!fs::exists(root / "saves.neots2-setup"));
    CHECK(!ts2::paths::saveFolderBusy(saves));
#ifdef _WIN32
    // Only Windows deletes the lock file when the lock ends. Elsewhere the empty file stays.
    CHECK(!fs::exists(root / "saves.neots2-lock"));
    CHECK(linked);
#endif

    // A second start changes nothing, even where a player changed a file.
    write(saves / "Config/broadcast.ini", "mine");
    const int before = count(saves);
    CHECK(!fillFolder(install, saves).filled);
    CHECK(read(saves / "Config/broadcast.ini") == "mine" && count(saves) == before);
    std::error_code error;
    fs::remove_all(root, error);
}

// Only a missing or empty folder is filled. A folder with anything in it (here a save folder whose
// neighborhoods were moved away) and the original games' own folders are left exactly as they are.
void saveFoldersInUseAreLeftAlone() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);

    const fs::path used = root / "used";
    write(used / "Downloads/chair.package", "cc");
    auto fill = fillFolder(install, used);
    CHECK(!fill.filled && fill.problem.empty() && count(used) == 2);

    const fs::path games = root / "EA Games" / "The Sims 2";
    fill = fillFolder(install, games);
    CHECK(!fill.filled && !fill.problem.empty() && !fs::exists(games));
    fs::create_directories(games);
    fill = fillFolder(install, games / "");
    CHECK(!fill.filled && !fill.problem.empty() && count(games) == 0);

    // A path that only leads into "EA Games" through a junction or from the working folder.
    if (junction(root / "shortcut", root / "EA Games")) {
        fill = fillFolder(install, root / "shortcut" / "The Sims 2");
        CHECK(!fill.filled && !fill.problem.empty() && count(games) == 0);
    }
    const fs::path working = fs::current_path();
    fs::current_path(root / "EA Games");
    fill = fillFolder(install, "the sims 2");
    fs::current_path(working);
    CHECK(!fill.filled && !fill.problem.empty() && count(games) == 0);

    // A save folder moved elsewhere and linked back is played as it is, with nothing put through
    // the link, even when the folder it leads to is empty.
    fs::create_directories(root / "moved");
    if (junction(root / "linked", root / "moved")) {
        CHECK(ts2::paths::inspectSaveFolder(root / "linked") == ts2::paths::SaveFolderState::Linked);
        CHECK(!ts2::paths::lacksNeighborhoods(root / "linked"));
        fill = fillFolder(install, root / "linked");
        CHECK(!fill.filled && fill.problem.empty() && count(root / "moved") == 0);
    }

    // A folder players make in "EA Games" themselves is theirs, and filled as any other.
    CHECK(fillFolder(install, root / "EA Games" / "My Saves").filled);
    CHECK(read(root / "EA Games" / "My Saves" / "Neighborhoods/N001/N001_Neighborhood.package") == "n1");

    // Saves are kept apart from the install.
    fill = fillFolder(install, install / "saves");
    CHECK(!fill.filled && !fill.problem.empty() && !fs::exists(install / "saves"));

    const fs::path missing = root / "new" / "saves";
    CHECK(fillFolder(install, missing).filled);
    std::error_code error;
    fs::remove_all(root, error);
}

// Leftovers of a fill that stopped are cleared and the fill done again. A UserData folder that
// leads out of the install stops the fill, and the save folder stays empty.
void stoppedOrBrokenFillsLeaveTheFolderAlone() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    const fs::path saves = root / "saves";
    fs::create_directories(saves);
    write(root / "saves.neots2-setup/Collections/half.package", "half");
    write(root / "saves.neots2-setup.neots2-mark", "");
    CHECK(fillFolder(install, saves).filled);
    CHECK(!fs::exists(saves / "Collections/half.package") && !fs::exists(root / "saves.neots2-setup"));
    CHECK(!fs::exists(root / "saves.neots2-setup.neots2-mark") && !fs::exists(saves / ".neots2-staging"));

    // A player's own folder named like a copy folder, without NeoTS2's mark, is never cleared.
    write(root / "kept.neots2-setup/Neighborhoods/N001/N001_Neighborhood.package", "mine");
    const auto kept = fillFolder(install, root / "kept");
    CHECK(!kept.filled && !kept.problem.empty());
    CHECK(read(root / "kept.neots2-setup/Neighborhoods/N001/N001_Neighborhood.package") == "mine");
    CHECK(!fillFolder(install, root / "kept.neots2-setup").filled);

    const fs::path userData = install / "Apartment Life/TSData/Res/UserData";
    std::error_code error;
    fs::remove_all(userData, error);
    write(root / "elsewhere/Neighborhoods/Z001/Z001_Neighborhood.package", "z1");
    if (junction(userData, root / "elsewhere")) {
        const fs::path other = root / "other";
        fs::create_directories(other);
        const auto fill = fillFolder(install, other);
        CHECK(!fill.filled && !fill.problem.empty() && count(other) == 0);
        CHECK(!fs::exists(root / "other.neots2-setup"));
    }
    fs::remove_all(root, error);
}

// A player who removed the base game's neighborhoods from the install and changed a pack's gets
// exactly what the install holds. With no neighborhoods shipped at all, the rest is still copied.
void moddedInstallsAreCopiedAsTheyAre() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    std::error_code error;
    fs::remove_all(install / "Double Deluxe/Base/TSData/Res/UserData/Neighborhoods", error);
    write(install / "Seasons/TSData/Res/UserData/Neighborhoods/G001/G001_Neighborhood.package", "my own");
    const fs::path saves = root / "saves";
    CHECK(fillFolder(install, saves).filled);
    CHECK(!fs::exists(saves / "Neighborhoods/N001"));
    CHECK(read(saves / "Neighborhoods/G001/G001_Neighborhood.package") == "my own");

    for (const char* pack : {"Seasons", "Backup"}) {
        fs::remove_all(install / pack / "TSData/Res/UserData/Neighborhoods", error);
    }
    const fs::path bare = root / "bare";
    CHECK(fillFolder(install, bare).filled);
    CHECK(!fs::exists(bare / "Neighborhoods") && read(bare / "Config/broadcast.ini") == "Apartment Life");
    fs::remove_all(root, error);
}

// The packs' thumbnail packages become one package per file name, in the game's pack order. Every
// thumbnail is filed under group 0xFFFFFFFF. A later pack's thumbnail replaces an earlier one with
// the same type and instance when their high instances match or one is 0, and the stored bytes and
// compression stay as shipped.
void thumbnailsAreMergedAsTheGame() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    using Key = ts2::dbpf::ResourceKey;
    constexpr std::uint32_t kThumb = 0xAC2950C1;
    struct Shipped {
        Key key;
        std::string bytes;
        bool compressed = false;
    };
    const auto ship = [&](const char* pack, const char* file, const std::vector<Shipped>& resources) {
        const fs::path path = install / pack / "TSData/Res/UserData/Thumbnails" / file;
        fs::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        ts2::dbpf::PackageWriter writer(out);
        for (const Shipped& r : resources) {
            writer.add(r.key, std::span(reinterpret_cast<const std::uint8_t*>(r.bytes.data()), r.bytes.size()), r.compressed, 50);
        }
        writer.finish(0);
    };
    write(install / "University Life/EP1/TSData/Res/Objects/objects.package", "x");
    ship("Double Deluxe/Base", "ObjectThumbnails.package", {{{kThumb, 0x6C2A22C3, 1, 0}, "base1"}, {{kThumb, 0x6C2A22C3, 2, 0}, "base2", true}});
    ship("University Life/EP1", "ObjectThumbnails.package", {{{kThumb, 0xFFFFFFFF, 3, 0}, "ep1-3"}});
    ship("Seasons", "objectthumbnails.package",
         {{{kThumb, 0xFFFFFFFF, 3, 0x7F}, "seasons3"}, {{kThumb, 0xFFFFFFFF, 1, 0x55}, "seasons1"}, {{kThumb, 0xFFFFFFFF, 4, 1}, "seasons4"}});
    ship("Apartment Life", "ObjectThumbnails.package", {{{kThumb, 0xFFFFFFFF, 4, 2}, "al4"}});
    ship("Backup", "ObjectThumbnails.package", {{{kThumb, 0xFFFFFFFF, 9, 0}, "not a pack"}});

    const fs::path saves = root / "saves";
    CHECK(fillFolder(install, saves).filled);
    CHECK(count(saves / "Thumbnails") == 1);
    ts2::dbpf::Package merged(saves / "Thumbnails/ObjectThumbnails.package");
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> found;
    for (const auto& entry : merged.entries()) {
        if (entry.key.type == kThumb) {
            const auto bytes = merged.readRaw(entry);
            found[{entry.key.instance, entry.key.instanceHigh}] = std::string(bytes.begin(), bytes.end());
            CHECK(entry.key.group == 0xFFFFFFFF);
            CHECK(entry.compressed == (entry.key.instance == 2));
        }
    }
    const std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> expected = {
        {{1, 0x55}, "seasons1"}, {{2, 0}, "base2"}, {{3, 0x7F}, "seasons3"}, {{4, 1}, "seasons4"}, {{4, 2}, "al4"}};
    CHECK(found == expected);
    merged.close();

    // A broken thumbnail package stops the fill, and the save folder stays as it was.
    write(install / "Seasons/TSData/Res/UserData/Thumbnails/CASThumbnails.package", "not a package");
    const fs::path other = root / "other";
    fs::create_directories(other);
    const auto broken = fillFolder(install, other);
    CHECK(!broken.filled && !broken.problem.empty() && count(other) == 0);
    std::error_code error;
    fs::remove_all(root, error);
}

// The packs' shipped lots are numbered from 1 in the game's pack order and each pack's file name
// order. Each lot is kept as it is up to its last used part, with version 1.2 and the pack's game
// number plus 1 in the header at 0x40. Character files follow their lot to its new number.
void lotCatalogIsImportedAsTheGame() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    const auto lot = [&](const char* pack, const char* file, const std::string& bytes) {
        const fs::path path = install / pack / "TSData/Res/UserData/LotCatalog" / file;
        fs::create_directories(path.parent_path());
        {
            std::ofstream out(path, std::ios::binary);
            ts2::dbpf::PackageWriter writer(out);
            writer.add({0x0C560F39, 1, 2, 0}, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), false, 0);
            writer.finish(0);
        }
        std::ofstream(path, std::ios::binary | std::ios::app) << "spare";  // bytes past the package's last part
        return path;
    };
    lot("Seasons", "cx_00000002.package", "seasons2");
    lot("Seasons", "cx_00000001.package", "seasons1");
    lot("Double Deluxe/Base", "cx_00000001.package", "base1");
    write(install / "Seasons/TSData/Res/UserData/LotCatalog/cx_Character_cx_00000002_1234abcd.package", "sim");
    write(install / "Seasons/TSData/Res/UserData/LotCatalog/cx_Character_cx_00000009_1234abcd.package", "no lot");
    write(install / "Seasons/TSData/Res/UserData/LotCatalog/notes.txt", "not a lot");
    lot("Backup", "cx_00000001.package", "not a pack");

    const fs::path saves = root / "saves";
    CHECK(fillFolder(install, saves).filled);
    const fs::path catalog = saves / "LotCatalog";
    CHECK(count(catalog) == 4);
    const auto check = [&](const char* file, const std::string& bytes, std::uint8_t stamp) {
        const std::string data = read(catalog / file);
        CHECK(data.size() > 0x44 && data.find("spare") == std::string::npos);
        CHECK(data.size() > 8 && data[8] == 2 && data[0x40] == static_cast<char>(stamp) && data[0x42] == 1);
        ts2::dbpf::Package package(catalog / file);
        CHECK(package.entries().size() == 1);
        const auto raw = package.readRaw(package.entries()[0]);
        CHECK(std::string(raw.begin(), raw.end()) == bytes);
    };
    check("cx_00000001.package", "base1", 1);
    check("cx_00000002.package", "seasons1", 8);
    check("cx_00000003.package", "seasons2", 8);
    CHECK(read(catalog / "cx_Character_cx_00000003_1234abcd.package") == "sim");

    // A broken lot stops the fill, and the save folder stays as it was.
    write(install / "Seasons/TSData/Res/UserData/LotCatalog/cx_00000005.package", "not a package");
    const fs::path other = root / "other";
    fs::create_directories(other);
    const auto broken = fillFolder(install, other);
    CHECK(!broken.filled && !broken.problem.empty() && count(other) == 0);
    std::error_code error;
    fs::remove_all(root, error);
}

// When the player asks, a save folder with files but no neighborhoods gets the install's shipped ones
// and nothing else changes. A folder with neighborhoods is left alone, and an install that ships
// none leaves the folder as it was.
void neighborhoodsAreAddedOnlyWhenMissing() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    const auto add = [&](const fs::path& folder) {
        const ts2::paths::SaveFolderLock lock(folder);
        return ts2::content::addShippedNeighborhoods(install, lock);
    };

    const fs::path missing = root / "missing";
    write(missing / "Downloads/chair.package", "cc");
    CHECK(ts2::paths::lacksNeighborhoods(missing));
    CHECK(add(missing).filled);
    CHECK(read(missing / "Neighborhoods/N001/N001_Neighborhood.package") == "n1");
    CHECK(read(missing / "Neighborhoods/G001/G001_Neighborhood.package") == "g1");
    CHECK(!fs::exists(missing / "Neighborhoods/X001") && !fs::exists(missing / "Config"));
    CHECK(read(missing / "Downloads/chair.package") == "cc" && count(missing) == 7);
    CHECK(!fs::exists(root / "missing.neots2-neighborhoods") && !ts2::paths::lacksNeighborhoods(missing));

    const fs::path empty = root / "empty";
    write(empty / "Config/userProps.xml", "mine");
    fs::create_directories(empty / "Neighborhoods");
    CHECK(add(empty).filled && read(empty / "Neighborhoods/N001/N001_Neighborhood.package") == "n1");
    CHECK(read(empty / "Config/userProps.xml") == "mine");

    const fs::path played = root / "played";
    write(played / "Neighborhoods/N009/N009_Neighborhood.package", "mine");
    CHECK(!ts2::paths::lacksNeighborhoods(played));
    const auto left = add(played);
    CHECK(!left.filled && left.problem.empty() && count(played) == 3);

    std::error_code error;
    for (const char* pack : {"Double Deluxe/Base", "Seasons", "Backup"}) {
        fs::remove_all(install / pack / "TSData/Res/UserData/Neighborhoods", error);
    }
    const fs::path bare = root / "bare";
    write(bare / "Downloads/chair.package", "cc");
    const auto none = add(bare);
    CHECK(!none.filled && !none.problem.empty() && !fs::exists(bare / "Neighborhoods") && count(bare) == 2);
    fs::remove_all(root, error);
}

// One NeoTS2 at a time plays with a save folder, however its path is spelled, and a fill never
// starts on a folder another NeoTS2 holds.
void oneNeoTS2PerSaveFolder() {
    using namespace savefolder;
    const fs::path root = fs::temp_directory_path() / ("neots2-save-test-" + std::to_string(std::random_device{}()));
    const fs::path install = makeInstall(root);
    const fs::path saves = root / "saves";
    fs::create_directories(saves);
    {
        const ts2::paths::SaveFolderLock first(saves);
        CHECK(first.held() && !first.busy() && first.folder() == saves);
        const ts2::paths::SaveFolderLock again(saves / "");
        CHECK(!again.held() && again.busy());
        CHECK(!ts2::content::fillNewSaveFolder(install, again).filled && fs::is_empty(saves));
#ifdef _WIN32
        CHECK(ts2::paths::SaveFolderLock(root / "SAVES").busy());
        if (junction(root / "link", saves)) {
            CHECK(ts2::paths::SaveFolderLock(root / "link").busy());
        }
#endif
    }
    CHECK(fillFolder(install, saves).filled);
#ifdef _WIN32
    // Held the way another program would hold it: open, shared with no one.
    int held = -1;
    _wsopen_s(&held, (root / "other.neots2-lock").c_str(), _O_CREAT | _O_WRONLY, _SH_DENYRW, _S_IREAD | _S_IWRITE);
    CHECK(held != -1);
    const auto blocked = fillFolder(install, root / "other");
    CHECK(!blocked.filled && !blocked.problem.empty() && !fs::exists(root / "other"));
    _close(held);
#endif
    // Anything but a plain file where the lock goes is never used as one.
    fs::create_directories(root / "odd.neots2-lock");
    const ts2::paths::SaveFolderLock odd(root / "odd");
    CHECK(!odd.held() && !odd.busy());
    std::error_code error;
    fs::remove_all(root, error);
}

void lotNumberComesFromThePackageName() {
    CHECK(ts2::content::lotNumber("Neighborhoods/N001/Lots/N001_Lot18.package") == 18);
    CHECK(ts2::content::lotNumber("N002_Lot7.package") == 7);
    CHECK(!ts2::content::lotNumber("N001_Lot.package"));
    CHECK(!ts2::content::lotNumber("N001_Lot1x.package"));
    CHECK(!ts2::content::lotNumber("N001_Neighborhood.package"));
    CHECK(!ts2::content::lotNumber("N001_Lot12345678901.package"));  // too long to be a lot number
}

void characterFilesIgnoreAppleDoubleSidecars() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("neots2-character-files-" + std::to_string(std::random_device{}()));
    const std::filesystem::path neighborhood = root / "Neighborhoods/N001";
    const std::filesystem::path characters = neighborhood / "Characters";
    const auto write = [](const std::filesystem::path& file, const std::string& text) {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    };
    write(characters / "N001_User00000.package", "a real package would be here");
    write(characters / "._N001_User00000.package", std::string{"\x00\x05\x16\x07", 4});
    write(characters / "._N001_User00001.package", "not an AppleDouble file");
    write(characters / "note.txt", "not a package");
    // Names the system's code page can't hold are read too, a sidecar among them.
    const std::filesystem::path wide = characters / std::filesystem::path(u8"N001_User\u4E2D.package");
    write(wide, "a real package would be here");
    write(characters / std::filesystem::path(u8"._\u4E2D.package"), std::string{"\x00\x05\x16\x07", 4});
    const auto files = ts2::content::characterFiles(neighborhood);
    CHECK(files.size() == 3);
    CHECK(std::find(files.begin(), files.end(), characters / "N001_User00000.package") != files.end());
    CHECK(std::find(files.begin(), files.end(), characters / "._N001_User00001.package") != files.end());
    CHECK(std::find(files.begin(), files.end(), wide) != files.end());
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void resourceIndexLeavesOutAppleDoubleSidecars() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("neots2-sidecar-index-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root);
    const ts2::dbpf::ResourceKey key{0xAC2950C1, 0x6C2A22C3, 1, 0};
    {
        std::ofstream out(root / "objects.package", std::ios::binary);
        ts2::dbpf::PackageWriter writer(out);
        const std::string bytes = "resource";
        writer.add(key, std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), false, 0);
        writer.finish(0);
        std::ofstream sidecar(root / "._objects.package", std::ios::binary);
        sidecar.write("\x00\x05\x16\x07", 4);
    }
    ts2::content::ResourceIndex index;
    CHECK(!index.addPackage(root / "._objects.package"));
    CHECK(index.addPackage(root / "objects.package"));
    CHECK(index.packageCount() == 1 && index.packagePath(0) == root / "objects.package");
    const auto* found = index.find(key);
    CHECK(found && found->package == 0);
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void portraitImageIsTheLifeStageFlag() {
    CHECK(ts2::content::portraitImage(1) == 0x20u);   // baby
    CHECK(ts2::content::portraitImage(2) == 0x01u);   // toddler
    CHECK(ts2::content::portraitImage(3) == 0x02u);   // child
    CHECK(ts2::content::portraitImage(16) == 0x04u);  // teen
    CHECK(ts2::content::portraitImage(19) == 0x08u);  // adult
    CHECK(ts2::content::portraitImage(51) == 0x10u);  // elder
    CHECK(!ts2::content::portraitImage(0));
    CHECK(!ts2::content::portraitImage(-1));
}

// Rows the game showed for Dina's needs (whole motives) and the two Calientes' moods.
void needsBarRowsMatchTheGame() {
    const std::pair<int, int> seen[] = {{59, 49}, {58, 48}, {88, 58}, {49, 46}, {39, 43}, {78, 55}, {56, 48}};
    for (const auto& [value, row] : seen) {
        CHECK(ts2::content::meterRow(value, -100, 100, 62) == row);
    }
    CHECK(ts2::content::meterRow(100, -100, 100, 62) == 61);
    CHECK(ts2::content::meterRow(-100, -100, 100, 62) == 0);
    CHECK(ts2::content::meterRow(500, -100, 100, 62) == 61);  // kept within the range
    CHECK(ts2::content::meterRow(0, 0, 0, 62) == 0);          // no range
    CHECK(ts2::content::meterRow(0, -100, 100, 0) == 0);      // no picture
}

void moodRowsMatchTheGame() {
    CHECK(ts2::content::moodRow(45, 200) == 55);
    CHECK(ts2::content::moodRow(59, 200) == 41);
    CHECK(ts2::content::moodRow(100, 200) == 0);
    CHECK(ts2::content::moodRow(-100, 200) == 199);
    CHECK(ts2::content::moodRow(-300, 200) == 199);
    CHECK(ts2::content::moodRow(0, 0) == 0);
}

// The mood meter the game showed for Nina (held value 50: gradient column 98 of 131).
// What can't be had is grey: 0.3 red, 0.6 green, 0.1 blue, alpha kept.
void greyedWeighsTheColours() {
    ts2::tga::Pixels p{2, 1, {200, 100, 50, 255, 0, 0, 0, 7}};
    const auto g = ts2::content::greyed(p);
    CHECK(g.rgba[0] == 125 && g.rgba[1] == 125 && g.rgba[2] == 125 && g.rgba[3] == 255);  // 60 + 60 + 5
    CHECK(g.rgba[4] == 0 && g.rgba[7] == 7);
}

// The cache of drawn object pictures: a missing or broken file has none.
void objectThumbnailsWithoutACache() {
    ts2::content::ObjectThumbnails none({});
    CHECK(!none.picture(0x7F765BD9, "moneyTree", 256));
    const std::filesystem::path broken = std::filesystem::temp_directory_path() / "neots2-broken-thumbnails.package";
    {
        std::ofstream out(broken, std::ios::binary);
        out << "DBPF but not really";
    }
    ts2::content::ObjectThumbnails bad(broken);
    CHECK(!bad.picture(0x7F765BD9, "moneyTree", 256));
    std::filesystem::remove(broken);
}

// A drawn picture's key, as the game filed the racecar toy box's (group 0x7FE0E7BF, model
// "toyBoxRacecar"): the CRC of "2145445823toyboxracecar". Models without a name or named
// "embedded" are never cached.
void objectThumbnailKeys() {
    CHECK(ts2::content::ObjectThumbnails::key(0x7FE0E7BF, "toyBoxRacecar") == 0xAC4E8AB2u);
    CHECK(!ts2::content::ObjectThumbnails::key(0x7FE0E7BF, ""));
    CHECK(!ts2::content::ObjectThumbnails::key(0x7FE0E7BF, "Embedded"));
}

// The needs arrows: the newest average of the last 10 values minus the oldest.
void needTrendCountsArrows() {
    ts2::content::NeedTrend trend;
    CHECK(trend.arrows() == 0);
    trend.add(50);
    trend.add(50);
    CHECK(trend.arrows() == 0);   // averages 50, 50
    trend.add(53);
    CHECK(trend.arrows() == 1);   // average 51 (51.0): 1 up
    trend.add(41);
    CHECK(trend.arrows() == -1);  // average 48.5 rounds away from zero to 49: 1 down
    trend.add(20);
    CHECK(trend.arrows() == -3);  // average 42.8 -> 43: 7 down
    trend.clear();
    for (int i = 0; i < 10; ++i) {
        trend.add(-10 - i);
    }
    CHECK(trend.arrows() == -3);  // averages from -10 to -14.5, rounded to -15: 5 down
    for (int i = 0; i < 10; ++i) {
        trend.add(0);
    }
    CHECK(trend.arrows() == 3);   // only the last 10 averages count: from -13.5 (-14) up to 0
}

void moodMeterPicksAColumn() {
    CHECK(ts2::content::meterRow(50, -100, 100, 131) == 98);
    CHECK(ts2::content::moodMeterValue(50, 19) == 50);
    CHECK(ts2::content::moodMeterValue(-20, 75) == 100);  // platinum: full
    CHECK(ts2::content::moodMeterValue(-20, 74) == -20);
    CHECK(ts2::content::isPlatinum(75) && !ts2::content::isPlatinum(74));
    ts2::tga::Pixels fill{2, 2, {9, 9, 9, 255, 9, 9, 9, 0, 9, 9, 9, 128, 9, 9, 9, 255}};
    ts2::tga::Pixels gradient{2, 2, {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}};
    const auto painted = ts2::content::paintColumns(fill, gradient, 1);
    CHECK(painted.rgba == (std::vector<std::uint8_t>{4, 5, 6, 255, 0, 0, 0, 0, 5, 5, 6, 128, 10, 11, 12, 255}));
    // A fill taller than the gradient keeps its own last row.
    ts2::tga::Pixels tall{1, 3, {9, 9, 9, 255, 9, 9, 9, 255, 9, 9, 9, 255}};
    ts2::tga::Pixels flat{1, 2, {1, 2, 3, 255, 4, 5, 6, 255}};
    CHECK(ts2::content::paintColumns(tall, flat, 0).rgba == (std::vector<std::uint8_t>{1, 2, 3, 255, 4, 5, 6, 255, 9, 9, 9, 255}));
}

// The age bar counts the stage as its duration plus one day and the days left plus one, at most
// the whole stage. The picture follows the life stage.
void ageBarFollowsTheStage() {
    using ts2::content::ageProgress;
    CHECK(ageProgress(3, 3).lived == 0 && ageProgress(3, 3).length == 4);
    CHECK(ageProgress(3, 0).lived == 3);
    CHECK(ageProgress(3, 9).lived == 0);                                     // more days left than the stage has
    CHECK(ageProgress(-1, 0).length == 65536 && ageProgress(-1, 0).lived == 65535);  // read unsigned
    CHECK(ts2::content::barFill(1, 4, 48) == 12);
    CHECK(ts2::content::barFill(0, 4, 48) == 0 && ts2::content::barFill(5, 4, 48) == 48 && ts2::content::barFill(1, 0, 48) == 0);
    CHECK(ts2::content::ageStage(1) == -1 && ts2::content::ageStage(2) == 0 && ts2::content::ageStage(3) == 1);
    CHECK(ts2::content::ageStage(16) == 2 && ts2::content::ageStage(19) == 3 && ts2::content::ageStage(51) == 4);
}

// The age tooltip names the days left of the stage, the last day with its own text, and an
// elder's age in days. Babies have none.
void ageTipFollowsTheStage() {
    using ts2::content::ageTip;
    const auto is = [](ts2::content::AgeTip t, std::uint16_t entry, int days) { return t.entry == entry && t.days == days; };
    CHECK(is(ageTip(19, false, 28, 4, 0), 0x2C, 5));   // adult, 5 days left
    CHECK(is(ageTip(19, false, 28, 0, 0), 0x30, 1));   // adult, last day
    CHECK(is(ageTip(16, false, 14, 2, 0), 0x2D, 3));   // teen
    CHECK(is(ageTip(3, false, 7, 6, 0), 0x2E, 7));     // child
    CHECK(is(ageTip(2, false, 3, 0, 0), 0x33, 1));     // toddler, last day
    CHECK(is(ageTip(2, true, 3, 2, 0), 0x3E, 3));      // PlantSim toddler
    CHECK(is(ageTip(2, true, 3, 0, 0), 0x3F, 1));
    CHECK(is(ageTip(51, false, 9, 6, 70), 0x34, 73));  // elder: 70 days before, 3 as an elder
    CHECK(is(ageTip(1, false, 3, 0, 0), 0, 0));        // baby
    CHECK(is(ageTip(19, false, -1, -1, 0), 0x2C, 0));  // 65536 days left wraps to 0 in 16 bits
}

// Stepped bars: personality pips are 8 pixels each of 80 (a full bar draws 79), the fitness bar
// moves a pixel at a time and stops one short of its 79.
void steppedBarsFillAsTheGame() {
    using ts2::content::barPixels;
    CHECK(barPixels(0, 0, 1000, 10, 80) == 0 && barPixels(99, 0, 1000, 10, 80) == 0);
    CHECK(barPixels(500, 0, 1000, 10, 80) == 40 && barPixels(799, 0, 1000, 10, 80) == 56);
    CHECK(barPixels(1000, 0, 1000, 10, 80) == 79 && barPixels(5000, 0, 1000, 10, 80) == 79);
    CHECK(barPixels(-30, 0, 1000, 10, 80) == 0);
    CHECK(barPixels(500, 0, 1000, 1000, 79) == 39 && barPixels(13, 0, 1000, 1000, 79) == 1);
    CHECK(barPixels(1000, 0, 1000, 1000, 79) == 78);
    CHECK(barPixels(5, 5, 5, 10, 80) == 0);
    CHECK(ts2::content::nearestZodiacSign(800, 300, 300, 500, 600) == 1);     // Aries exactly
    CHECK(ts2::content::nearestZodiacSign(300, 700, 300, 500, 700) == 12);
    CHECK(ts2::content::nearestZodiacSign(0, 0, 0, 0, 0) == 4);      // ties with sign 11: the first wins
}

// Live panel texts: money with the simoleon sign and commas, hours on a 12-hour clock.
void livePanelTextsAsTheGame() {
    using namespace ts2::content;
    CHECK(moneyText(0) == "\xC2\xA7" "0" && moneyText(154) == "\xC2\xA7" "154");
    CHECK(moneyText(1234) == "\xC2\xA7" "1,234" && moneyText(1234567) == "\xC2\xA7" "1,234,567");
    CHECK(moneyText(-5000) == "-\xC2\xA7" "5,000");
    CHECK(groupedNumber(1000) == "1,000" && groupedNumber(-250) == "-250" && groupedNumber(0) == "0");
    CHECK(hourText(0) == "12 am" && hourText(9) == "9 am" && hourText(12) == "12 pm" && hourText(13) == "1 pm");
    CHECK(hourText(25) == "1 am" && hourText(-1) == "11 pm");
    CHECK(formatted("%s to %s", {"9 am", "3 pm"}) == "9 am to 3 pm");
    CHECK(formatted("GPA %d.%d", {"3"}) == "GPA 3.");
    CHECK(formatted("100%", {}) == "100%");
}

// WantTuning's object categories: id and pictures from each entry. Broken entries are skipped.
void wantCategoriesAreRead() {
    const auto c = ts2::content::parseWantCategories(
        "<category id=\"10076\" name=\"Lemonade Stand\" primaryIcon=\"0xc0001065\" secondaryIcon=\"0x00000000\"/>\n"
        "<category name=\"no id\" primaryIcon=\"0x1\"/><category id=\"12\" primaryIcon=\"oops\"/><category id=\"7");
    CHECK(c.size() == 2 && c.at(10076).primaryIcon == 0xC0001065 && c.at(10076).secondaryIcon == 0);
    CHECK(c.at(12).primaryIcon == 0);
}

// WantTuning's influence curve, read and inverted as the game keeps it (levels by points),
// and the influence bar's value from it: a level 0..5 times 0.2, as 0..100.
void influenceCurveIsReadAndInverted() {
    const auto curve = ts2::content::parseInfluenceCurve(
        "<influence>\n<influenceTuning curve = \"(0;0) (1;2000) (2;4000) (3;6000) (4;8000) (5;10000)\"/>");
    CHECK(curve && curve->points.size() == 6 && curve->points[5].second == 10000.0f);
    const auto levels = curve->inverse();
    CHECK(levels.points[1].first == 2000.0f && levels.points[1].second == 1.0f);
    CHECK(levels.at(-5) == 0.0f && levels.at(3000) == 1.5f && levels.at(20000) == 5.0f && levels.at(2000) == 1.0f);
    CHECK(ts2::content::influenceMeterValue(&levels, 0) == 0);
    CHECK(ts2::content::influenceMeterValue(&levels, 3000) == 30);
    CHECK(ts2::content::influenceMeterValue(&levels, 10000) == 100 && ts2::content::influenceMeterValue(&levels, 12000) == 100);
    CHECK(ts2::content::influenceMeterValue(&levels, -100) == 0);
    CHECK(ts2::content::influenceMeterValue(nullptr, 30) == 40);
    CHECK(!ts2::content::parseInfluenceCurve("<influenceTuning curve = \"(0;x)\"/>"));
    CHECK(!ts2::content::parseInfluenceCurve("no curve"));
}

// A want's icon is placed 2 pixels in on a transparent 44 x 44 picture (as the game showed it).
void wantIconIsPlacedOnItsPicture() {
    ts2::tga::Pixels icon{2, 1, {1, 2, 3, 255, 4, 5, 6, 128}};
    const auto out = ts2::content::placed(icon, 4, 2, 2, 0);
    CHECK(out.width == 4 && out.height == 2 && out.rgba.size() == 32);
    CHECK(out.rgba[8] == 1 && out.rgba[11] == 255 && out.rgba[12] == 4 && out.rgba[15] == 128);
    CHECK(out.rgba[3] == 0 && out.rgba[19] == 0);
    const auto cut = ts2::content::placed(icon, 4, 2, 3, -1);  // partly outside: cut
    CHECK(cut.rgba.size() == 32 && cut.rgba[31] == 0);
    CHECK(ts2::content::placed(ts2::tga::Pixels{9, 9, {1}}, 2, 2, 0, 0).rgba == std::vector<std::uint8_t>(16, 0));
}

// Thumbnails show the middle of a portrait: 180 square of a 256 picture, 38 off each side (90 of 128).
// A portrait's shadow: black, its outline blurred, at most 210 of 255 opaque (matched against the
// silhouette a character file keeps beside its portrait).
void portraitShadowIsItsBlurredOutline() {
    ts2::tga::Pixels portrait{256, 256, std::vector<std::uint8_t>(256 * 256 * 4, 255)};
    const auto full = ts2::content::portraitSilhouette(portrait);
    CHECK(full.width == 256 && full.rgba[0] == 0 && full.rgba[3] == 210 && full.rgba[(128 * 256 + 128) * 4 + 3] == 210);
    // Half opaque (the left half): sharp in the portrait, soft in its shadow.
    for (std::uint32_t y = 0; y < 256; ++y) {
        for (std::uint32_t x = 128; x < 256; ++x) {
            portrait.rgba[(y * 256 + x) * 4 + 3] = 0;
        }
    }
    const auto half = ts2::content::portraitSilhouette(portrait);
    const auto alpha = [&](std::uint32_t x) { return half.rgba[(100 * 256 + x) * 4 + 3]; };
    CHECK(alpha(0) == 210 && alpha(255) == 0 && alpha(120) > alpha(128) && alpha(128) > alpha(136) && alpha(136) > 0);
    CHECK(ts2::content::portraitSilhouette(ts2::tga::Pixels{4, 4, {1}}).rgba.empty());  // broken: nothing
    // Larger than any portrait: refused rather than blurred.
    const ts2::tga::Pixels huge{2048, 2048, std::vector<std::uint8_t>(2048u * 2048u * 4u, 255)};
    CHECK(ts2::content::portraitSilhouette(huge).rgba.empty() && ts2::content::portraitCrop(huge).rgba.empty());
}

// Thumbnail pictures are scaled by area (colour weighted by alpha) and cut to the thumbnail's
// rounded shape (alpha only). Drawn portraits are made straight alpha.
// The queue's icons: a row from the row's place, one icon width and margin apart. Nested ones are in a
// column under the first, which then shows its nest cutout, each nested one but the last joined.
void queueIconsAreLaidOutAsTheGame() {
    using ts2::content::queueSlots;
    ts2::content::QueueLayout layout{50, 10, 4, 51, 52, -4};
    const auto row = queueSlots({false, false, false}, layout);
    CHECK(row.size() == 3 && row[0].x == 50 && row[1].x == 105 && row[2].x == 160 && row[2].y == 10);
    CHECK(!row[0].joined && !row[0].nested);
    const auto mixed = queueSlots({false, true, true, false}, layout);
    CHECK(mixed[0].joined && mixed[0].x == 50);
    CHECK(mixed[1].nested && mixed[1].x == 50 && mixed[1].y == 58 && mixed[1].joined);
    CHECK(mixed[2].nested && mixed[2].y == 110 && !mixed[2].joined);
    CHECK(!mixed[3].nested && mixed[3].x == 105 && !mixed[3].joined);

    // Sliding: a share of the distance each step, at least a pixel, landing within a pixel.
    using ts2::content::slideStep;
    CHECK(slideStep(105, 50, 16, 4, 1) == 105 - 16 * 55 * 0.02f / 4);  // 4.4 pixels left
    CHECK(slideStep(50.5f, 50, 16, 4, 1) == 50);                       // within a pixel
    CHECK(slideStep(60, 50, 1, 4, 1) == 59);                           // too small a step: one pixel
    CHECK(slideStep(60, 50, 500, 4, 1) == 50);                         // would pass it
    CHECK(slideStep(60, 50, 16, 0, 1) == slideStep(60, 50, 16, 1, 1));  // no steps counts as one

    using ts2::content::uiConstant;
    const std::vector<std::string> lines{"kLocationX=50", "kLocationY=10", "kIconMarginX=4x", "kIcon=7"};
    CHECK(uiConstant(lines, "kLocationX") == 50 && uiConstant(lines, "kLocationY") == 10);
    CHECK(!uiConstant(lines, "kIconMarginX") && !uiConstant(lines, "kLocation") && uiConstant(lines, "kIcon") == 7);
}

void thumbnailPicturesAreScaledAndMasked() {
    ts2::tga::Pixels two{2, 1, {0, 0, 0, 0, 200, 100, 50, 255}};
    const auto one = ts2::content::scaled(two, 1, 1);
    CHECK(one.width == 1 && one.rgba == (std::vector<std::uint8_t>{200, 100, 50, 128}));  // the clear pixel adds no colour
    const auto up = ts2::content::scaled(ts2::tga::Pixels{1, 1, {9, 9, 9, 9}}, 3, 2);
    CHECK(up.rgba.size() == 24 && up.rgba[20] == 9);
    CHECK(ts2::content::scaled(two, 0, 5).rgba.empty() && ts2::content::scaled(two, 100000, 1).rgba.empty());
    ts2::tga::Pixels shape{2, 1, {0, 0, 0, 255, 0, 0, 0, 0}};
    const auto cut = ts2::content::masked(ts2::tga::Pixels{2, 1, {10, 20, 30, 255, 10, 20, 30, 255}}, shape);
    CHECK(cut.rgba == (std::vector<std::uint8_t>{10, 20, 30, 255, 10, 20, 30, 0}));
    const auto straight = ts2::content::unpremultiplied(ts2::tga::Pixels{2, 1, {50, 25, 0, 128, 7, 7, 7, 0}});
    CHECK(straight.rgba == (std::vector<std::uint8_t>{100, 50, 0, 128, 7, 7, 7, 0}));
    CHECK(ts2::content::masked(two, ts2::tga::Pixels{1, 1, {0, 0, 0, 0}}).rgba == two.rgba);  // sizes differ: as it is
}

void numbersAreReadWholeOrFromTheStart() {
    CHECK(ts2::parseFloat("1.25") == 1.25f && ts2::parseFloat("-0.5") == -0.5f);
    CHECK(!ts2::parseFloat("1.25x") && !ts2::parseFloat("") && !ts2::parseFloat(" 1") && !ts2::parseFloat("x"));
    std::size_t length = 0;
    CHECK(ts2::parseLeadingFloat("1.25x", &length) == 1.25f && length == 4);
    CHECK(ts2::parseLeadingFloat("2", &length) == 2.0f && length == 1);
    CHECK(!ts2::parseLeadingFloat("x1") && !ts2::parseLeadingFloat(""));
}

void userOptionsAreReadAsTheGameSavesThem() {
    // Laid out as the game writes Config/userProps.xml.
    const auto props = ts2::content::parseUserProps(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<cGZPropertySetString>\n"
        "  <AnyBoolean key=\"edgeScrolling\" type=\"0xcba908e1\">true</AnyBoolean>\n"
        "  <AnyFloat32 key=\"horzedgescrollrate\" type=\"0xabc78708\">0.5</AnyFloat32>\n"
        "  <AnyBoolean key=\"rotateCameraSims1Mode\" type=\"0xcba908e1\">false</AnyBoolean>\n"
        "  <AnyStringSet key=\"visitedNeighborhoods\" type=\"0xad9a861e\">\n    <Element>N001_Neighborhood</Element>\n  </AnyStringSet>\n"
        "  <AnyStringSet key=\"visitedNeighborhoodsNoCollege\" type=\"0xad9a861e\"/>\n"
        "  <AnyBoolean key=\"quickTips\" type=\"0xcba908e1\">yes</AnyBoolean>\n</cGZPropertySetString>\n");
    CHECK(props.flag("edgeScrolling") == true && props.flag("rotateCameraSims1Mode") == false);
    CHECK(props.values.at("horzedgescrollrate") == "0.5" && !props.values.contains("visitedNeighborhoods"));
    CHECK(!props.flag("quickTips") && !props.flag("missing") && !props.values.contains("visitedNeighborhoodsNoCollege"));
    // A file cut short keeps what came before the break.
    const auto cut = ts2::content::parseUserProps("<AnyBoolean key=\"a\" type=\"0x1\">true</AnyBoolean><AnyBoolean key=\"b\" type=\"0x1\">tr");
    CHECK(cut.flag("a") == true && !cut.values.contains("b"));
    CHECK(ts2::content::parseUserProps("<Any").values.empty() && ts2::content::parseUserProps("<Any key=\"x\">").values.empty());
    CHECK(ts2::content::readUserProps("no such folder").values.empty());
}

} // namespace

std::vector<Test> contentTests() {
    return {
        {"Animate Sim uses its age's list", animateSimUsesItsAgesList},
        {"Installed packs are found", installedPacksAreFound},
        {"Objects' daylight matches the game", objectDaylightMatchesTheGame},
        {"Light files are read as the game reads them", lightFilesAreReadAsTheGame},
        {"objects stand on their storey when grids start below the ground", objectsStandOnTheirStoreyWhenGridsStartBelowTheGround},
        {"build scripts read patterns and wall styles", buildScriptsReadPatternsAndWallStyles},
        {"lot number comes from the package name", lotNumberComesFromThePackageName},
        {"character files leave out AppleDouble sidecars", characterFilesIgnoreAppleDoubleSidecars},
        {"a resource index leaves out AppleDouble sidecars", resourceIndexLeavesOutAppleDoubleSidecars},
        {"portrait image is the life stage flag", portraitImageIsTheLifeStageFlag},
        {"needs bar rows match the game", needsBarRowsMatchTheGame},
        {"mood rows match the game", moodRowsMatchTheGame},
        {"greyed weighs the colours", greyedWeighsTheColours},
        {"object thumbnails without a cache", objectThumbnailsWithoutACache},
        {"object thumbnail keys", objectThumbnailKeys},
        {"need trend counts arrows", needTrendCountsArrows},
        {"mood meter picks a column", moodMeterPicksAColumn},
        {"age bar follows the stage", ageBarFollowsTheStage},
        {"age tip follows the stage", ageTipFollowsTheStage},
        {"stepped bars fill as the game", steppedBarsFillAsTheGame},
        {"live panel texts as the game", livePanelTextsAsTheGame},
        {"want categories are read", wantCategoriesAreRead},
        {"influence curve is read and inverted", influenceCurveIsReadAndInverted},
        {"want icon is placed on its picture", wantIconIsPlacedOnItsPicture},
        {"thumbnail pictures are scaled and masked", thumbnailPicturesAreScaledAndMasked},
        {"queue icons are laid out as the game", queueIconsAreLaidOutAsTheGame},
        {"portrait shadow is its blurred outline", portraitShadowIsItsBlurredOutline},
        {"outside grid covers under floors and roofs", outsideGridCoversUnderFloorsAndRoofs},
        {"new save folder is filled as the game", newSaveFolderIsFilledAsTheGame},
        {"save folders in use are left alone", saveFoldersInUseAreLeftAlone},
        {"stopped or broken fills leave the folder alone", stoppedOrBrokenFillsLeaveTheFolderAlone},
        {"modded installs are copied as they are", moddedInstallsAreCopiedAsTheyAre},
        {"one NeoTS2 per save folder", oneNeoTS2PerSaveFolder},
        {"thumbnails are merged as the game", thumbnailsAreMergedAsTheGame},
        {"lot catalog is imported as the game", lotCatalogIsImportedAsTheGame},
        {"neighborhoods are added only when missing", neighborhoodsAreAddedOnlyWhenMissing},
        {"user options are read as the game saves them", userOptionsAreReadAsTheGameSavesThem},
        {"numbers are read whole or from the start", numbersAreReadWholeOrFromTheStart},
    };
}
