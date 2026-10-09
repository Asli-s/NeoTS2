#include "content/install.hpp"
#include "content/lot.hpp"
#include "content/resource_index.hpp"
#include "sim/index_code_source.hpp"
#include "sim/rooms.hpp"

#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

constexpr double kScale = 14.0;
constexpr std::size_t kRoomField = 0x1D;

std::string escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c; break;
        }
    }
    return out;
}

std::string roomColor(std::int32_t room, const ts2::sim::RoomMap& rooms) {
    if (room == ts2::sim::RoomMap::kNoRoom) {
        return "#ffffff";
    }
    const auto* info = rooms.room(room);
    if (info && info->touchesLotEdge) {
        return "#d9ecd0";
    }
    const double hue = std::fmod(static_cast<double>(room) * 137.508, 360.0);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "hsl(%.0f,55%%,72%%)", hue);
    return buffer;
}

std::string point(double x, double y) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%.1f,%.1f ", x * kScale, y * kScale);
    return buffer;
}

std::string patternColor(std::int32_t pattern) {
    if (pattern == 0) {
        return "#ffffff";
    }
    const double hue = std::fmod(static_cast<double>(pattern) * 97.3 + 20.0, 360.0);
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "hsl(%.0f,35%%,62%%)", hue);
    return buffer;
}

int view(const std::filesystem::path& lotPath, std::int32_t level, const std::filesystem::path& out,
         bool floors, ts2::sim::IndexCodeSource* code) {
    const auto lot = ts2::content::loadLot(lotPath);
    if (!lot.roomGraph) {
        std::fprintf(stderr, "the lot has no room wall graph\n");
        return 1;
    }
    const auto savedRooms = lot.grids.find(ts2::content::kRoomGridInstance);
    const ts2::sim::RoomMap rooms = savedRooms != lot.grids.end() ? ts2::sim::RoomMap(savedRooms->second, lot.roomGraph->firstLevel)
                                                                  : ts2::sim::RoomMap(*lot.roomGraph);
    const std::int32_t width = rooms.width();
    const std::int32_t height = rooms.height();
    const auto floorGrid = lot.grids.find(ts2::content::kFloorGridInstance);
    if (floors && (floorGrid == lot.grids.end() || floorGrid->second.elementSize != 8)) {
        std::fprintf(stderr, "the lot has no floor grid\n");
        return 1;
    }
    // Floor patterns per quarter, stored in the same order as rooms (left, bottom, right, top).
    auto value = [&](std::int32_t x, std::int32_t y, int quarter) {
        if (!floors) {
            return rooms.quarterRoom(level, x, y, quarter);
        }
        return static_cast<std::int32_t>(floorGrid->second.i16(level - lot.roomGraph->firstLevel, x, y,
                                                                 static_cast<std::size_t>(3 - quarter)));
    };
    auto color = [&](std::int32_t v) { return floors ? patternColor(v) : roomColor(v, rooms); };

    std::ofstream svg(out);
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width * kScale << "\" height=\""
        << height * kScale + 24 << "\" font-family=\"sans-serif\" font-size=\"12\">\n"
        << "<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>\n"
        << "<text x=\"4\" y=\"" << height * kScale + 16 << "\">" << escape(lotPath.filename().string())
        << ", level " << level + 1 << (floors ? ", floors" : ", rooms") << " (version 0x" << std::hex << lot.objectMap.version << std::dec
        << ")</text>\n<g stroke=\"none\">\n";

    for (std::int32_t y = 0; y < height; ++y) {
        for (std::int32_t x = 0; x < width; ++x) {
            const std::int32_t q[4] = {value(x, y, 0), value(x, y, 1), value(x, y, 2), value(x, y, 3)};
            if (q[0] == q[1] && q[1] == q[2] && q[2] == q[3]) {
                if (q[0] == ts2::sim::RoomMap::kNoRoom) {
                    continue;
                }
                svg << "<rect x=\"" << x * kScale << "\" y=\"" << y * kScale << "\" width=\"" << kScale
                    << "\" height=\"" << kScale << "\" fill=\"" << color(q[0]) << "\"/>\n";
                continue;
            }
            const double cx = x + 0.5;
            const double cy = y + 0.5;
            const double corners[5][2] = {{x + 0.0, y + 0.0}, {x + 1.0, y + 0.0}, {x + 1.0, y + 1.0},
                                          {x + 0.0, y + 1.0}, {x + 0.0, y + 0.0}};
            for (int i = 0; i < 4; ++i) {
                svg << "<polygon points=\"" << point(corners[i][0], corners[i][1])
                    << point(corners[i + 1][0], corners[i + 1][1]) << point(cx, cy) << "\" fill=\""
                    << color(q[i]) << "\"/>\n";
            }
        }
    }
    svg << "</g>\n<g stroke=\"#e4e4e4\" stroke-width=\"0.5\">\n";
    for (std::int32_t i = 0; i <= width; ++i) {
        svg << "<line x1=\"" << i * kScale << "\" y1=\"0\" x2=\"" << i * kScale << "\" y2=\""
            << height * kScale << "\"/>\n";
    }
    for (std::int32_t i = 0; i <= height; ++i) {
        svg << "<line x1=\"0\" y1=\"" << i * kScale << "\" x2=\"" << width * kScale << "\" y2=\""
            << i * kScale << "\"/>\n";
    }

    svg << "</g>\n<g stroke=\"#333333\" stroke-width=\"2.5\" stroke-linecap=\"round\">\n";
    std::unordered_map<std::int32_t, const ts2::wallgraph::Point*> points;
    for (const auto& p : lot.roomGraph->points) {
        points[p.id] = &p;
    }
    for (const auto& w : lot.roomGraph->walls) {
        const auto a = points.find(w.from);
        const auto b = points.find(w.to);
        if (a == points.end() || b == points.end() || a->second->level != level) {
            continue;
        }
        svg << "<line x1=\"" << a->second->x * kScale << "\" y1=\"" << a->second->y * kScale
            << "\" x2=\"" << b->second->x * kScale << "\" y2=\"" << b->second->y * kScale << "\"/>\n";
    }

    svg << "</g>\n<g fill=\"#2a4d8f\" stroke=\"#ffffff\" stroke-width=\"0.8\">\n";
    std::size_t drawn = 0;
    for (const auto& o : lot.objects) {
        const auto& h = o.state.header;
        if (h.location[0] < 0 || h.location[1] < 0 || h.value1 - 1 != level) {
            continue;
        }
        std::string name = o.type ? o.type->name : "";
        if (code && o.type) {
            if (const auto* type = code->objectType(o.type->guid)) {
                name = type->definition.name;
            }
        }
        svg << "<circle cx=\"" << h.location[1] * kScale << "\" cy=\"" << h.location[0] * kScale
            << "\" r=\"3.2\"><title>" << escape(name) << " (object " << o.id << ", room "
            << o.state.data[kRoomField] << ")</title></circle>\n";
        ++drawn;
    }
    svg << "</g>\n</svg>\n";
    std::printf("%s: level %d, %d x %d tiles, %zu rooms, %zu objects drawn\n", out.string().c_str(),
                level + 1, width, height, rooms.rooms().size(), drawn);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "Usage: lotview <lot.package> [--level <n, 1 = ground>] [--floors] [--install <folder>]\n"
                     "               [-o <file.svg>]\n");
        return 1;
    }
    try {
        std::int32_t level = 0;
        std::optional<std::filesystem::path> install;
        std::filesystem::path out = "lot.svg";
        bool floors = false;
        for (int i = 2; i < argc; ++i) {
            const std::string_view arg = argv[i];
            if (arg == "--level" && i + 1 < argc) {
                level = std::stoi(argv[++i]) - 1;
            } else if (arg == "--install" && i + 1 < argc) {
                install = std::filesystem::path(argv[++i]);
            } else if (arg == "--floors") {
                floors = true;
            } else if (arg == "-o" && i + 1 < argc) {
                out = std::filesystem::path(argv[++i]);
            } else {
                std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
                return 1;
            }
        }
        ts2::content::ResourceIndex index;
        std::unique_ptr<ts2::sim::IndexCodeSource> code;
        if (install) {
            for (const auto& path : ts2::content::objectLoadOrder(ts2::content::findProducts(*install))) {
                index.addPackage(path);
            }
            code = std::make_unique<ts2::sim::IndexCodeSource>(index);
        }
        return view(std::filesystem::path(argv[1]), level, out, floors, code.get());
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "error: %s\n", ex.what());
        return 1;
    }
}
