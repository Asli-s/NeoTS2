#include "content/scripts.hpp"

#include "formats/dbpf.hpp"
#include "formats/resource_types.hpp"

#include <cctype>
#include <exception>
#include <system_error>

namespace ts2::content {

namespace fs = std::filesystem;

std::optional<fs::path> objectScriptsPackage(const fs::path& installRoot) {
    std::optional<fs::path> best;
    std::uintmax_t bestSize = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(installRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        std::string name = it->path().filename().string();
        for (char& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (!it->is_regular_file() || name != "objectscripts.package") {
            continue;
        }
        const auto size = it->file_size(ec);
        if (!ec && size > bestSize) {
            bestSize = size;
            best = it->path();
        }
    }
    return best;
}

ObjectScripts loadObjectScripts(const fs::path& path) {
    dbpf::Package package{path};
    ObjectScripts result;
    for (const auto& e : package.entries()) {
        if (e.key.type != types::kLuaModule && e.key.type != types::kLuaScript) {
            continue;
        }
        try {
            auto script = luascript::parse(package.read(e));
            (e.key.type == types::kLuaModule ? result.modules : result.scripts).push_back(std::move(script));
        } catch (const std::exception& ex) {
            result.problems.push_back("script " + std::to_string(e.key.instance) + ": " + ex.what());
        }
    }
    return result;
}

} // namespace ts2::content
