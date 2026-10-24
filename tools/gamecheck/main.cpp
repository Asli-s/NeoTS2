// Reads every resource of an installation (and, optionally, a user folder) with the engine's own
// readers and reports, per resource type, how many were read, how many failed and why, and which
// types the engine doesn't read yet. Whole lots and neighborhoods are loaded as the engine loads
// them, too. Nothing is written to the game's folders.
//   gamecheck <install folder> [--user <user folder>] [--report <report.md>]
#include "content/lot.hpp"
#include "content/neighborhood.hpp"
#include "formats/anim.hpp"
#include "formats/array3d.hpp"
#include "formats/bcon.hpp"
#include "formats/bhav.hpp"
#include "formats/camera_config.hpp"
#include "formats/datatables.hpp"
#include "formats/dbpf.hpp"
#include "formats/effects.hpp"
#include "formats/fami.hpp"
#include "formats/famt.hpp"
#include "formats/fencepostlayer.hpp"
#include "formats/glob.hpp"
#include "formats/image.hpp"
#include "formats/lot_description.hpp"
#include "formats/luascript.hpp"
#include "formats/namemap.hpp"
#include "formats/ngbh.hpp"
#include "formats/nhood_objects.hpp"
#include "formats/nhood_paint_rules.hpp"
#include "formats/nhood_terrain.hpp"
#include "formats/objd.hpp"
#include "formats/objf.hpp"
#include "formats/objm.hpp"
#include "formats/objt.hpp"
#include "formats/propset.hpp"
#include "formats/reference_list.hpp"
#include "formats/resource_types.hpp"
#include "formats/roofs.hpp"
#include "formats/scenegraph.hpp"
#include "formats/sdsc.hpp"
#include "formats/simi.hpp"
#include "formats/slot.hpp"
#include "formats/srel.hpp"
#include "formats/str.hpp"
#include "formats/stringmap.hpp"
#include "formats/swaf.hpp"
#include "formats/terrain.hpp"
#include "formats/ttab.hpp"
#include "formats/ui_script.hpp"
#include "formats/wallgraph.hpp"
#include "formats/walllayer.hpp"

#include "lauxlib.h"
#include "lua.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::span<const std::uint8_t>;

namespace {

// A reader for one resource type: throws (or returns false) when the resource can't be read.
struct Reader {
    std::uint32_t type;
    const char* name;
    std::function<bool(Bytes)> read;
    // Used instead of read for types whose layout depends on the instance.
    std::function<bool(Bytes, std::uint32_t)> readInstance = {};
};

template <typename F>
std::function<bool(Bytes)> throwing(F f) {
    return [f](Bytes data) {
        f(data);
        return true;
    };
}

// The game's compiled Lua: converted for this build and then loaded (compiled checks included).
bool loadsAsLua(Bytes data) {
    const auto script = ts2::luascript::parse(data);
    const auto chunk = ts2::luascript::nativeChunk(script.chunk);
    lua_State* L = lua_open();
    const bool ok = luaL_loadbuffer(L, reinterpret_cast<const char*>(chunk.data()), chunk.size(), script.name.c_str()) == 0;
    std::string error = ok ? "" : (lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown Lua error");
    lua_close(L);
    if (!ok) {
        throw std::runtime_error("Lua: " + error);
    }
    return true;
}

// A texture: its header, then the largest picture kept in the resource itself decoded to pixels.
bool decodesAsTexture(Bytes data) {
    const auto image = ts2::scenegraph::parseImage(data);
    for (const auto& face : image.faces) {
        if (!face.empty() && face.back().largeMipName.empty()) {
            ts2::scenegraph::decodeRgba(face.back().data, image.width, image.height, image.format);
        }
    }
    return true;
}

// Type 0 holds other text and data too (want tuning, music titles, movies). The game loads UI
// scripts from group 0xA99D8A11 or a screen size group, its width and height as decimal digits
// ("0x%.4u%.4u", e.g. 0x08000600 for 800 x 600).
bool isUiScriptGroup(std::uint32_t group) {
    if (group == 0xA99D8A11) {
        return true;
    }
    for (int shift = 0; shift < 32; shift += 4) {
        if (((group >> shift) & 0xF) > 9) {
            return false;
        }
    }
    return true;
}

std::vector<Reader> readers() {
    using namespace ts2;
    namespace t = ts2::types;
    return {
        {t::kBehavior, "BHAV (behavior script)", throwing([](Bytes d) { bhav::parse(d); })},
        {t::kStringTable, "STR# (string table)", throwing([](Bytes d) { str::parse(d); })},
        {0x43545353, "CTSS (catalog description)", throwing([](Bytes d) { str::parse(d); })},
        {0x54544173, "TTAs (pie menu strings)", throwing([](Bytes d) { str::parse(d); })},
        {t::kObjectDefinition, "OBJD (object definition)", throwing([](Bytes d) { objd::parse(d); })},
        {objf::kType, "OBJf (object functions)", throwing([](Bytes d) { objf::parse(d); })},
        {t::kInteractionTable, "TTAB (interactions)", throwing([](Bytes d) { ttab::parse(d); })},
        {0x474C4F42, "GLOB (semi-global link)", throwing([](Bytes d) { glob::parse(d); })},
        {0x42434F4E, "BCON (behavior constants)", throwing([](Bytes d) { bcon::parse(d); })},
        {t::kSlots, "SLOT (routing slots)", throwing([](Bytes d) { slot::parse(d); })},
        {t::kLuaModule, "Lua module", loadsAsLua},
        {t::kLuaScript, "Lua script", loadsAsLua},
        {t::kUiScript, "UI script", throwing([](Bytes d) { ui::parse(std::string_view(reinterpret_cast<const char*>(d.data()), d.size())); })},
        {t::kImage, "IMG (picture)", [](Bytes d) {
             if (!image::decode(d)) {
                 throw std::runtime_error("not a picture the engine can decode");
             }
             return true;
         }},
        {0xE519C933, "CRES (model node tree)", throwing([](Bytes d) {
             scenegraph::parseResourceNode(d);
             scenegraph::parseDataLists(d);
         })},
        {0xFC6EB1F7, "SHPE (shape)", throwing([](Bytes d) { scenegraph::parseShape(d); })},
        {0x7BA3838C, "GMND (geometry node)", throwing([](Bytes d) { scenegraph::parseContainer(d); })},
        {0xAC4F8687, "GMDC (mesh)", throwing([](Bytes d) { scenegraph::parseMesh(d); })},
        {0x49596978, "TXMT (material)", throwing([](Bytes d) { scenegraph::parseMaterial(d); })},
        {0xC9C81BA9, "LGHT (point light)", throwing([](Bytes d) { scenegraph::parseLight(d); })},
        {0xC9C81BAD, "LGHT (spot light)", throwing([](Bytes d) { scenegraph::parseLight(d); })},
        {0x1C4A276C, "TXTR (texture)", decodesAsTexture},
        {0xED534136, "LIFO (large texture level)", throwing([](Bytes d) { scenegraph::parseLevelInfo(d); })},
        {anim::kType, "ANIM (animation)", throwing([](Bytes d) { anim::parse(d); })},
        {0xEBCF3E27, "GZPS (property set)", throwing([](Bytes d) { propset::parse(d); })},
        {0x0C560F39, "BINX (catalog entry)", throwing([](Bytes d) { propset::parse(d); })},
        {0x4C697E5A, "MMAT (material override)", throwing([](Bytes d) { propset::parse(d); })},
        {t::kWantDefinition, "XWNT (want definition)", throwing([](Bytes d) { propset::parse(d); })},
        {reflist::kType, "3IDR (reference list)", throwing([](Bytes d) { reflist::parse(d); })},
        {t::kObjectTypeTable, "OBJT (lot object types)", throwing([](Bytes d) { objt::parse(d); })},
        {t::kObjectMap, "ObjM (lot object map)", {},
         [](Bytes d, std::uint32_t instance) {
             instance == 2 ? (void)objm::parseGroups(d) : (void)objm::parse(d);
             return true;
         }},
        {t::kNameMap, "NMAP (name map)", throwing([](Bytes d) { namemap::parse(d); })},
        {t::kWallGraph, "wall graph", throwing([](Bytes d) { wallgraph::parse(d); })},
        {t::kWallLayer, "wall layer", throwing([](Bytes d) { walllayer::parse(d); })},
        {t::kFencePostLayer, "fence post layer", throwing([](Bytes d) { fencepostlayer::parse(d); })},
        {t::kRoofs, "roofs", throwing([](Bytes d) { roofs::parse(d); })},
        {t::kPatternMap, "pattern map", throwing([](Bytes d) { stringmap::parse(d); })},
        {t::kArray3D, "3D array (lot grid)", {},
         [](Bytes d, std::uint32_t instance) {
             instance == ts2::content::kListGridInstance ? (void)array3d::parseLists(d) : (void)array3d::parse(d);
             return true;
         }},
        {t::kArray2D, "2D array (lot grid)", {},
         [](Bytes d, std::uint32_t instance) {
             instance == ts2::content::kByteListGridInstance ? (void)terrain::parseByteLists(d) : (void)terrain::parseArray2D(d);
             return true;
         }},
        {t::kLotTexture, "lot terrain textures", throwing([](Bytes d) { terrain::parseLotTexture(d); })},
        {t::kSimDescription, "SDSC (Sim description)", throwing([](Bytes d) { sdsc::parse(d); })},
        {t::kSimRelationship, "SREL (relationship)", throwing([](Bytes d) { srel::parse(d); })},
        {t::kFamily, "FAMI (family)", throwing([](Bytes d) { fami::parse(d); })},
        {t::kFamilyTies, "FAMT (family ties)", throwing([](Bytes d) { famt::parse(d); })},
        {t::kWantsAndFears, "SWAF (wants and fears)", throwing([](Bytes d) { swaf::parse(d); })},
        {t::kNeighborhoodRecord, "NGBH (neighborhood record)", throwing([](Bytes d) { ngbh::parse(d); })},
        {t::kDataTables, "data tables", throwing([](Bytes d) { datatables::parse(d); })},
        {simi::kType, "SIMI (simulator state)", throwing([](Bytes d) { simi::parse(d); })},
        {nhood::kLotDescriptionType, "lot description", throwing([](Bytes d) { nhood::parseLotDescription(d); })},
        {nhood::kTerrainType, "neighborhood terrain", throwing([](Bytes d) { nhood::parseTerrain(d); })},
        {nhood::kSavedCameraType, "neighborhood camera", throwing([](Bytes d) { (void)nhood::parseSavedCamera(d); })},
        {nhood::kObjectsType, "neighborhood objects", throwing([](Bytes d) { nhood::parseObjects(d); })},
        {effects::kEffectsType, "effects", throwing([](Bytes d) { (void)effects::parse(d); })},
    };
}

struct Failure {
    std::string where, why;
};

struct Count {
    std::size_t read = 0, failed = 0;
    std::vector<Failure> examples;                       // the first few failures
    std::map<std::uint32_t, std::size_t> failedInstances;  // failures by resource instance
};

struct Results {
    std::mutex lock;
    std::map<std::string, Count> byKind;               // read with a reader, by its name
    std::map<std::uint32_t, std::size_t> unread;       // types the engine has no reader for, by type
    std::size_t packages = 0, resources = 0;
    std::vector<Failure> packageFailures;

    void add(Count& c, bool ok, const std::string& where, const std::string& why) {
        if (ok) {
            ++c.read;
            return;
        }
        ++c.failed;
        if (c.examples.size() < 5) {
            c.examples.push_back({where, why});
        }
    }
};

std::string utf8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string hex(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

// A message with its numbers taken out ("bad count 12" -> "bad count #"), to group alike ones.
std::string withoutNumbers(const std::string& message) {
    std::string out;
    for (const char c : message) {
        if (c >= '0' && c <= '9') {
            if (out.empty() || out.back() != '#') {
                out += '#';
            }
        } else {
            out += c;
        }
    }
    return out;
}

bool isLot(const fs::path& p) {
    return utf8(p.filename()).find("_Lot") != std::string::npos && utf8(p.parent_path().filename()) == "Lots";
}

bool isNeighborhood(const fs::path& p) {
    const auto name = utf8(p.filename());
    return name.size() > 21 && name.ends_with("_Neighborhood.package");
}

void checkPackage(const fs::path& path, const fs::path& root, const std::vector<Reader>& all, Results& out) {
    const std::string where = utf8(fs::relative(path, root));
    std::map<std::string, Count> local;
    std::map<std::uint32_t, std::size_t> unread;
    std::size_t resources = 0;
    try {
        ts2::dbpf::Package package{path};
        for (const auto& e : package.entries()) {
            if (e.key.type == ts2::types::kDirectory) {
                continue;
            }
            ++resources;
            const auto reader = std::find_if(all.begin(), all.end(), [&](const Reader& r) { return r.type == e.key.type; });
            if (reader == all.end() || (e.key.type == ts2::types::kUiScript && !isUiScriptGroup(e.key.group))) {
                ++unread[e.key.type];
                continue;
            }
            Count& c = local[reader->name];
            try {
                const auto data = package.read(e);
                out.add(c, reader->readInstance ? reader->readInstance(data, e.key.instance) : reader->read(data), "", "");
            } catch (const std::exception& ex) {
                out.add(c, false, where + " instance " + hex(e.key.instance), ex.what());
                ++c.failedInstances[e.key.instance];
            }
        }
        // Whole lots and neighborhoods, loaded as the engine loads them.
        // A lot or neighborhood counts as read only when none of its parts was left out with a
        // problem noted (the engine notes those and shows the rest).
        if (isLot(path)) {
            Count& c = local["whole lot (as loaded)"];
            try {
                const auto lot = ts2::content::loadLot(path);
                out.add(c, lot.problems.empty(), where, lot.problems.empty() ? "" : lot.problems.front().message);
                // Each part left out, by what it is ("roofs", "grid"...) and the reason.
                for (const auto& p : lot.problems) {
                    out.add(local["lot part left out: " + withoutNumbers(p.message)], false, where + " instance " + hex(p.id),
                            p.message);
                }
            } catch (const std::exception& ex) {
                out.add(c, false, where, ex.what());
            }
        } else if (isNeighborhood(path)) {
            Count& c = local["whole neighborhood (as loaded)"];
            try {
                const auto hood = ts2::content::loadNeighborhood(path);
                out.add(c, hood.problems.empty(), where, hood.problems.empty() ? "" : hood.problems.front());
            } catch (const std::exception& ex) {
                out.add(c, false, where, ex.what());
            }
        }
    } catch (const std::exception& ex) {
        std::lock_guard g(out.lock);
        ++out.packages;
        out.packageFailures.push_back({where, ex.what()});
        return;
    }
    std::lock_guard g(out.lock);
    ++out.packages;
    out.resources += resources;
    for (auto& [name, c] : local) {
        Count& total = out.byKind[name];
        total.read += c.read;
        total.failed += c.failed;
        for (auto& f : c.examples) {
            if (total.examples.size() < 5) {
                total.examples.push_back(std::move(f));
            }
        }
        for (const auto& [instance, n] : c.failedInstances) {
            total.failedInstances[instance] += n;
        }
    }
    for (const auto& [type, n] : unread) {
        out.unread[type] += n;
    }
}

// Text files the engine reads from the installation: terrain paint rules and camera settings.
void checkTextFiles(const fs::path& root, Results& out) {
    for (const auto& item : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (!item.is_regular_file()) {
            continue;
        }
        const auto name = utf8(item.path().filename());
        const bool rules = name == "NeighborhoodTerrain.ini";
        const bool camera = item.path().extension() == ".txt" && utf8(item.path().parent_path().filename()) == "Cameras";
        if (!rules && !camera) {
            continue;
        }
        std::ifstream in(item.path(), std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        Count& c = out.byKind[rules ? "terrain paint rules (text)" : "camera settings (text)"];
        try {
            if (rules) {
                ts2::nhood::parsePaintRules(text.str());
            } else {
                ts2::camera::parse(text.str());
            }
            out.add(c, true, "", "");
        } catch (const std::exception& ex) {
            out.add(c, false, utf8(fs::relative(item.path(), root)), ex.what());
        }
    }
}

// Appends printf-style text.
template <typename... Args>
void appendf(std::string& out, const char* format, Args... args) {
    if constexpr (sizeof...(args) == 0) {
        out += format;
    } else {
        char buffer[2048];
        std::snprintf(buffer, sizeof buffer, format, args...);
        out += buffer;
    }
}

std::string report(const Results& r, const std::vector<std::string>& roots, double seconds) {
    std::string f;
    std::size_t read = 0, failed = 0, unread = 0;
    for (const auto& [name, c] : r.byKind) {
        read += c.read;
        failed += c.failed;
    }
    for (const auto& [type, n] : r.unread) {
        unread += n;
    }
    appendf(f, "# Game file check\n\n");
    for (const auto& root : roots) {
        appendf(f, "- `%s`\n", root.c_str());
    }
    appendf(f, "\n%zu packages, %zu resources, checked in %.0f s.\n\n", r.packages, r.resources, seconds);
    appendf(f, "**%zu read, %zu failed** with the engine's readers; %zu resources of types it doesn't read yet; "
                    "%zu packages that couldn't be opened.\n\n",
                 read, failed, unread, r.packageFailures.size());
    appendf(f, "| Kind | Read | Failed |\n| --- | ---: | ---: |\n");
    for (const auto& [name, c] : r.byKind) {
        appendf(f, "| %s | %zu | %zu |\n", name.c_str(), c.read, c.failed);
    }
    bool any = false;
    for (const auto& [name, c] : r.byKind) {
        for (const auto& e : c.examples) {
            if (!any) {
                appendf(f, "\n## Failures (first few of each kind)\n\n");
                any = true;
            }
            appendf(f, "- %s: `%s`: %s\n", name.c_str(), e.where.c_str(), e.why.c_str());
        }
    }
    // Failures that gather on a few instances usually mean one kind of resource shares its type
    // with another the reader doesn't know.
    for (const auto& [name, c] : r.byKind) {
        if (c.failedInstances.empty()) {
            continue;
        }
        std::vector<std::pair<std::size_t, std::uint32_t>> top;
        for (const auto& [instance, n] : c.failedInstances) {
            top.emplace_back(n, instance);
        }
        std::sort(top.rbegin(), top.rend());
        appendf(f, "- %s fails most at instances:", name.c_str());
        for (std::size_t i = 0; i < top.size() && i < 6; ++i) {
            appendf(f, " %s (%zu)", hex(top[i].second).c_str(), top[i].first);
        }
        appendf(f, "%s\n", top.size() > 6 ? " ..." : "");
    }
    for (const auto& e : r.packageFailures) {
        appendf(f, "- package `%s`: %s\n", e.where.c_str(), e.why.c_str());
    }
    std::vector<std::pair<std::size_t, std::uint32_t>> types;
    for (const auto& [type, n] : r.unread) {
        types.emplace_back(n, type);
    }
    std::sort(types.rbegin(), types.rend());
    appendf(f, "\n## Types not read yet\n\n| Type | Name | Resources |\n| --- | --- | ---: |\n");
    for (const auto& [n, type] : types) {
        appendf(f, "| %s | %s | %zu |\n", hex(type).c_str(), ts2::types::typeName(type).c_str(), n);
    }
    return f;
}

int run(const std::vector<fs::path>& args);

} // namespace

// Paths arrive as the system gives them: wide on Windows, so folder names like "The Sims™ 2" work.
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    return run(std::vector<fs::path>(argv + 1, argv + argc));
}
#else
int main(int argc, char** argv) {
    return run(std::vector<fs::path>(argv + 1, argv + argc));
}
#endif

namespace {

int run(const std::vector<fs::path>& args) {
    if (args.empty()) {
        std::fprintf(stderr, "Usage: gamecheck <install folder> [--user <user folder>] [--report <report.md>]\n");
        return 2;
    }
    std::vector<fs::path> roots{args[0]};
    fs::path reportPath;
    for (std::size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--user") {
            roots.push_back(args[++i]);
        } else if (args[i] == "--report") {
            reportPath = args[++i];
        }
    }
    const auto start = std::chrono::steady_clock::now();
    const auto all = readers();
    Results results;
    std::vector<std::pair<fs::path, fs::path>> packages;  // package, the root it was found under
    for (const auto& root : roots) {
        for (const auto& item : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
            if (item.is_regular_file() && item.path().extension() == ".package") {
                packages.emplace_back(item.path(), root);
            }
        }
        checkTextFiles(root, results);
    }
    std::printf("checking %zu packages...\n", packages.size());
    std::atomic<std::size_t> next = 0;
    std::vector<std::thread> workers;
    const unsigned count = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned w = 0; w < count; ++w) {
        workers.emplace_back([&] {
            for (std::size_t i = next++; i < packages.size(); i = next++) {
                checkPackage(packages[i].first, packages[i].second, all, results);
            }
        });
    }
    for (auto& t : workers) {
        t.join();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::vector<std::string> names;
    for (const auto& r : roots) {
        names.push_back(utf8(r));
    }
    const std::string text = report(results, names, seconds);
    std::fwrite(text.data(), 1, text.size(), stdout);
    if (!reportPath.empty()) {
        std::ofstream(reportPath, std::ios::binary) << text;
    }
    std::size_t failed = results.packageFailures.size();
    for (const auto& [name, c] : results.byKind) {
        failed += c.failed;
    }
    return failed == 0 ? 0 : 1;
}

} // namespace
