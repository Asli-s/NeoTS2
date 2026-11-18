#include "content/broadcasts.hpp"

#include "diag/log.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <system_error>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

// Far above the game's own files, so a broken or huge file can't hold the start for long.
constexpr std::size_t kMaxIniBytes = 1u << 20;
constexpr std::size_t kMaxStations = 4096;
constexpr std::size_t kMaxMoviesPerFolder = 4096;
constexpr int kMaxFolderDepth = 8;

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

// The last part of a folder setting such as "Broadcast/TvSBN", in lower case.
std::string lastPart(std::string_view folder) {
    while (!folder.empty() && (folder.back() == '/' || folder.back() == '\\')) {
        folder.remove_suffix(1);
    }
    const auto slash = folder.find_last_of("/\\");
    return lower(slash == std::string_view::npos ? folder : folder.substr(slash + 1));
}

std::string readText(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::string text(kMaxIniBytes, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    return text;
}

// Each pack's TSData/Res folder, oldest pack first.
std::vector<fs::path> resFolders(const std::vector<Product>& products) {
    std::vector<const Product*> order;
    for (const auto& p : products) {
        order.push_back(&p);
    }
    std::stable_sort(order.begin(), order.end(), [](const Product* a, const Product* b) { return a->releaseOrder < b->releaseOrder; });
    std::vector<fs::path> out;
    std::set<fs::path> seen;
    for (const Product* product : order) {
        for (const auto& objects : product->objectPackages) {
            fs::path data = objects.parent_path();
            while (data.has_relative_path() && lower(utf8(data.filename())) != "tsdata") {
                data = data.parent_path();
            }
            const fs::path res = data / "Res";
            std::error_code ec;
            if (fs::is_directory(res, ec) && seen.insert(res).second) {
                out.push_back(res);
            }
        }
    }
    return out;
}

// Every .movie under each Movies folder, by the lower-case name of the folder holding it. A name
// found in an older pack's folder stays, so each file is listed once.
std::map<std::string, std::vector<fs::path>> moviesByFolder(const std::vector<fs::path>& res) {
    std::map<std::string, std::vector<fs::path>> out;
    std::map<std::string, std::set<std::string>> names;
    for (const auto& root : res) {
        const fs::path movies = root / "Movies";
        std::error_code ec;
        if (!fs::is_directory(movies, ec)) {
            continue;
        }
        fs::recursive_directory_iterator it(movies, fs::directory_options::skip_permission_denied, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            if (it.depth() >= kMaxFolderDepth) {
                it.disable_recursion_pending();
            }
            if (!it->is_regular_file(ec) || lower(utf8(it->path().extension())) != ".movie") {
                continue;
            }
            const std::string folder = lower(utf8(it->path().parent_path().filename()));
            auto& list = out[folder];
            if (list.size() < kMaxMoviesPerFolder && names[folder].insert(lower(utf8(it->path().filename()))).second) {
                list.push_back(it->path());
            }
        }
    }
    // The order the game plays a folder in is not known yet. File name order stands in.
    for (auto& [folder, list] : out) {
        std::sort(list.begin(), list.end(), [](const fs::path& a, const fs::path& b) { return lower(utf8(a.filename())) < lower(utf8(b.filename())); });
    }
    return out;
}

} // namespace

std::vector<BroadcastEntry> parseBroadcastIni(std::string_view text) {
    text = text.substr(0, kMaxIniBytes);
    std::vector<std::string> listed;
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::string section;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            const auto close = line.find(']');
            section = lower(trim(line.substr(1, close == std::string_view::npos ? std::string_view::npos : close - 1)));
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value(trim(line.substr(eq + 1)));
        if (section == "broadcasts") {
            if (listed.size() < kMaxStations && !value.empty()) {
                listed.push_back(lower(value));
            }
        } else if (sections.size() < kMaxStations || sections.contains(section)) {
            sections[section][key] = value;
        }
    }
    std::vector<BroadcastEntry> out;
    std::set<std::string> seen;
    for (const auto& name : listed) {
        const auto it = sections.find(name);
        if (it == sections.end() || !seen.insert(name).second) {
            continue;
        }
        auto get = [&](const char* key) {
            const auto v = it->second.find(key);
            return v == it->second.end() ? std::string() : v->second;
        };
        BroadcastEntry e;
        e.name = name;
        e.folder = get("folder");
        e.file = get("file");
        e.commercialFolder = get("commercialfolder");
        e.isGames = get("isgames") == "1";
        e.dontRestart = get("dontrestart") == "1";
        out.push_back(std::move(e));
    }
    return out;
}

Broadcasts Broadcasts::make(const std::vector<BroadcastEntry>& entries, const std::map<std::string, std::vector<fs::path>>& movies) {
    Broadcasts b;
    auto folderMovies = [&](const std::string& folder) {
        const auto it = movies.find(lastPart(folder));
        return it == movies.end() ? std::vector<fs::path>{} : it->second;
    };
    for (const auto& e : entries) {
        if (b.stations_.contains(e.name)) {
            continue;  // the first INI to name a station wins
        }
        Station s;
        s.entry = e;
        if (e.isGames) {
            const std::string want = lower(e.file);
            for (const auto& p : folderMovies(e.folder)) {
                if (lower(utf8(p.filename())) == want) {
                    s.shows.push_back(p);
                    break;
                }
            }
        } else {
            s.shows = folderMovies(e.folder);
            if (!e.commercialFolder.empty()) {
                s.commercials = folderMovies(e.commercialFolder);
            }
        }
        b.stations_.emplace(e.name, std::move(s));
    }
    return b;
}

Broadcasts Broadcasts::load(const std::vector<Product>& products, const fs::path& saveFolder) {
    const std::vector<fs::path> res = resFolders(products);
    std::vector<BroadcastEntry> entries;
    auto add = [&](const fs::path& file) {
        for (auto& e : parseBroadcastIni(readText(file))) {
            entries.push_back(std::move(e));
        }
    };
    std::error_code ec;
    const fs::path own = saveFolder / "Config" / "Broadcast.ini";
    if (!saveFolder.empty() && fs::is_regular_file(own, ec)) {
        add(own);
    } else if (const auto shipped = newestDataFile(products, "Res/UserData/Config/Broadcast.ini")) {
        add(*shipped);
    }
    for (const auto& root : res) {
        const fs::path folder = root / "Movies" / "Games" / "INI";
        if (!fs::is_directory(folder, ec)) {
            continue;
        }
        std::vector<fs::path> inis;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && lower(utf8(it->path().extension())) == ".ini") {
                inis.push_back(it->path());
            }
        }
        std::sort(inis.begin(), inis.end());
        for (const auto& ini : inis) {
            add(ini);
        }
    }
    Broadcasts b = make(entries, moviesByFolder(res));
    diag::info("broadcasts: " + std::to_string(b.size()) + " stations");
    return b;
}

const Station* Broadcasts::find(std::string_view name) const {
    const auto it = stations_.find(lower(name));
    return it == stations_.end() ? nullptr : &it->second;
}

} // namespace ts2::content
