#include "content/save_folder.hpp"

#include "content/install.hpp"
#include "content/lot_import.hpp"
#include "content/thumbnail_merge.hpp"
#include "paths/names.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace ts2::content {

namespace {

namespace fs = std::filesystem;

// The packs ship about 3,000 files and 700 MB here. These limits stop a broken or tampered install
// (a folder nested without end, a huge file) from filling the disk.
constexpr int kMaxFiles = 50000;
constexpr std::uintmax_t kMaxBytes = std::uintmax_t{8} << 30;
constexpr int kMaxDepth = 16;

struct Step {
    std::string_view name;
    bool baseGameOnly = false;
};

// The folders in the game's order. Each step fills only its own folder.
constexpr std::array<Step, 10> kFolders = {{
    {"Neighborhoods"},
    {"Storytelling", true},
    {"Collections"},
    {"SC4Terrains"},
    {"Cameras"},
    {"PetBreeds"},
    {"PetCoats"},
    {"LockedBins"},
    {"Paintings"},
    {"Config"},
}};

fs::file_type typeOf(const fs::path& path, std::error_code& error) {
    return fs::symlink_status(path, error).type();
}

// The file beside a copy folder that marks it as NeoTS2's own, so only such a folder is ever
// cleared. It sits beside the folder, not in it, so it can never end up in a save.
fs::path markFor(const fs::path& staging) {
    return staging.parent_path() / (staging.filename().native() + fs::path(std::u8string(paths::kMarkSuffix)).native());
}

// A pack's TSData/Res/UserData folder, found above its objects package.
std::optional<fs::path> userDataFolder(const Product& product) {
    if (product.objectPackages.empty()) {
        return std::nullopt;
    }
    for (fs::path p = product.objectPackages.front().parent_path(); p.has_relative_path(); p = p.parent_path()) {
        if (paths::foldedName(p.filename()) == "res") {
            return p / "UserData";
        }
    }
    return std::nullopt;
}

// Lot catalog numbers have eight digits.
constexpr int kMaxCatalogNumber = 99999999;

bool digits(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// A shipped lot, cx_<8 digits>.package, given as a lowercase name.
bool isCatalogLot(std::string_view name) {
    return name.size() == 19 && name.starts_with("cx_") && digits(name.substr(3, 8)) && name.ends_with(".package");
}

// A lot's character file, cx_character_cx_<the lot's 8 digits>_<id>.package, given as a lowercase
// name. The lot's number sits at 16 and the id part starts at 24, after the fixed prefix.
bool isCatalogCharacter(std::string_view name) {
    return name.size() > 33 && name.starts_with("cx_character_cx_") && digits(name.substr(16, 8)) && name[24] == '_' &&
           name.ends_with(".package");
}

struct Copier {
    SaveFolderFill& result;
    fs::path installRoot;  // resolved, so links can't lead out of it

    // Notes why the step failed. The first reason is kept, so a failed clean-up after it can't
    // hide it.
    bool fail(std::string problem) {
        if (result.problem.empty()) {
            result.problem = std::move(problem);
        }
        return false;
    }

    std::uintmax_t remaining() const { return result.bytes >= kMaxBytes ? 0 : kMaxBytes - result.bytes; }

    // Counts bytes written and stops the fill when they pass the limit.
    bool count(std::uintmax_t bytes) {
        result.bytes += bytes;
        return result.bytes <= kMaxBytes || fail("the install's shipped content is larger than any real install");
    }

    bool copyFile(const fs::path& from, const fs::path& to) {
        std::error_code error;
        const std::uintmax_t size = fs::file_size(from, error);
        if (error) {
            return fail("can't read " + paths::utf8Text(from));
        }
        if (++result.files > kMaxFiles || !count(size)) {
            return fail("the install's shipped content is larger than any real install");
        }
        fs::create_directories(to.parent_path(), error);
        if (error || !fs::copy_file(from, to, fs::copy_options::overwrite_existing, error) || error) {
            return fail("can't copy " + paths::utf8Text(from) + ": " + error.message());
        }
        return true;
    }

    // A pack folder to copy, when it is a real folder that really lies inside the install. A
    // missing folder is fine (not every pack ships every folder). Anything else stops the fill.
    std::optional<bool> usable(const fs::path& folder) {
        std::error_code error;
        const fs::file_type type = typeOf(folder, error);
        if (type == fs::file_type::not_found) {
            return false;
        }
        const fs::path real = fs::canonical(folder, error);
        if (error || type != fs::file_type::directory || !paths::spelledWithin(real, installRoot)) {
            fail(paths::utf8Text(folder) + " isn't a plain folder inside the install");
            return std::nullopt;
        }
        return true;
    }

    // Copies every file under `from` into `to`, replacing files already there. Only real files and
    // folders are taken. Links and junctions are skipped, so the copy never leaves the install.
    bool copyTree(const fs::path& from, const fs::path& to) {
        const auto use = usable(from);
        if (!use || !*use) {
            return use.has_value();
        }
        std::error_code error;
        fs::recursive_directory_iterator it(from, fs::directory_options::none, error);
        for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            std::error_code typeError;
            const fs::file_type type = it->symlink_status(typeError).type();
            if (typeError) {
                return fail("can't read " + paths::utf8Text(it->path()));
            }
            if (type == fs::file_type::directory) {
                if (it.depth() >= kMaxDepth) {
                    return fail("folders nest too deep in " + paths::utf8Text(from));
                }
            } else if (type == fs::file_type::regular) {
                if (!copyFile(it->path(), to / it->path().lexically_relative(from))) {
                    return false;
                }
            } else {
                it.disable_recursion_pending();
            }
        }
        return !error || fail("can't read " + paths::utf8Text(from) + ": " + error.message());
    }

    // Merges the packs' shipped Thumbnails packages into one package per file name. The folder is
    // made even when no pack ships one, as the game makes it.
    bool mergeThumbnails(const std::vector<Product>& products, const fs::path& to) {
        std::vector<std::pair<std::string, std::vector<fs::path>>> byName;
        int listed = 0;
        for (const Product& product : products) {
            const auto source = userDataFolder(product);
            const auto use = source ? usable(*source / "Thumbnails") : std::optional<bool>(false);
            if (!use) {
                return false;
            }
            if (!*use) {
                continue;
            }
            std::error_code error;
            for (fs::directory_iterator it(*source / "Thumbnails", error), end; !error && it != end; it.increment(error)) {
                std::error_code typeError;
                if (++listed > kMaxFiles) {
                    return fail("the install ships more thumbnail files than any real install");
                }
                if (it->symlink_status(typeError).type() != fs::file_type::regular || paths::foldedName(it->path().extension()) != ".package") {
                    continue;
                }
                const std::string name = paths::foldedName(it->path().filename());
                auto same = std::find_if(byName.begin(), byName.end(), [&](const auto& n) { return n.first == name; });
                if (same == byName.end()) {
                    same = byName.insert(byName.end(), {name, {}});
                }
                same->second.push_back(it->path());
            }
            if (error) {
                return fail("can't read " + paths::utf8Text(*source / "Thumbnails") + ": " + error.message());
            }
        }
        std::error_code error;
        fs::create_directories(to, error);
        if (error) {
            return fail("can't make " + paths::utf8Text(to) + ": " + error.message());
        }
        for (const auto& [name, sources] : byName) {
            if (++result.files > kMaxFiles) {
                return fail("the install's shipped content is larger than any real install");
            }
            try {
                if (!count(mergeThumbnailPackages(sources, to / sources.front().filename(), remaining()))) {
                    return false;
                }
            } catch (const std::exception& ex) {
                return fail("can't merge the thumbnails in " + paths::utf8Text(sources.front().filename()) + ": " + ex.what());
            }
        }
        return true;
    }

    // Imports the packs' shipped lots into the lot catalog, numbered from 1 in the game's pack order
    // and each pack's file name order. A lot's character files follow it to its new number. The
    // folder is made even when no pack ships a lot, as the game makes it.
    bool importLotCatalog(const std::vector<Product>& products, const fs::path& to) {
        std::error_code error;
        fs::create_directories(to, error);
        if (error) {
            return fail("can't make " + paths::utf8Text(to) + ": " + error.message());
        }
        int next = 1;
        int listed = 0;
        for (const Product& product : products) {
            const auto source = userDataFolder(product);
            const auto use = source ? usable(*source / "LotCatalog") : std::optional<bool>(false);
            if (!use) {
                return false;
            }
            if (!*use) {
                continue;
            }
            std::vector<std::pair<std::string, fs::path>> lots;
            std::vector<std::pair<std::string, fs::path>> characters;
            for (fs::directory_iterator it(*source / "LotCatalog", error), end; !error && it != end; it.increment(error)) {
                if (++listed > kMaxFiles) {
                    return fail("the install ships more lots than any real install");
                }
                std::error_code typeError;
                if (it->symlink_status(typeError).type() != fs::file_type::regular) {
                    continue;
                }
                const std::string name = paths::foldedName(it->path().filename());
                if (isCatalogLot(name)) {
                    lots.emplace_back(name, it->path());
                } else if (isCatalogCharacter(name)) {
                    characters.emplace_back(name, it->path());
                }
            }
            if (error) {
                return fail("can't read " + paths::utf8Text(*source / "LotCatalog") + ": " + error.message());
            }
            std::sort(lots.begin(), lots.end());
            std::map<std::string, int> renumbered;
            for (const auto& [name, path] : lots) {
                if (next > kMaxCatalogNumber || ++result.files > kMaxFiles) {
                    return fail("the install ships more lots than any real install");
                }
                try {
                    if (!count(importCatalogLot(path, to / std::format("cx_{:08}.package", next), gameNumber(product),
                                                remaining()))) {
                        return false;
                    }
                } catch (const std::exception& ex) {
                    return fail("can't import the lot " + paths::utf8Text(path) + ": " + ex.what());
                }
                renumbered[name.substr(3, 8)] = next++;
            }
            // What the game does with a character file whose lot the pack doesn't ship is not known
            // yet, so such a file is left out.
            for (const auto& [name, path] : characters) {
                const auto lot = renumbered.find(name.substr(16, 8));
                if (lot != renumbered.end()) {
                    const fs::path target = to / (fs::path(std::format("cx_Character_cx_{:08}", lot->second)).native() +
                                                  path.filename().native().substr(24));
                    if (!copyFile(path, target)) {
                        return false;
                    }
                }
            }
        }
        return true;
    }

    // Deletes a copy folder left by a fill that stopped, and its mark. Only an empty folder or a
    // marked one is deleted, so a player's own folder that happens to have such a name is never
    // touched.
    bool clear(const fs::path& staging) {
        std::error_code error;
        const fs::path mark = markFor(staging);
        std::error_code markError;
        const bool marked = typeOf(mark, markError) == fs::file_type::regular;
        const fs::file_type type = typeOf(staging, error);
        if (type != fs::file_type::not_found) {
            if (error || type != fs::file_type::directory) {
                return fail(paths::utf8Text(staging) + " isn't a plain folder, so it was left alone");
            }
            if (!marked && !(fs::is_empty(staging, error) && !error)) {
                return fail(paths::utf8Text(staging) + " isn't a copy NeoTS2 made, so it was left alone");
            }
            fs::remove_all(staging, error);
            if (error) {
                return fail("can't clear " + paths::utf8Text(staging) + ": " + error.message());
            }
        }
        if (marked) {
            fs::remove(mark, markError);
        }
        return true;
    }

    // Makes the empty, marked folder a copy is built in, clearing one left by a fill that stopped.
    // The mark is made first, so a folder of NeoTS2's is never left without it.
    bool prepare(const fs::path& staging) {
        if (!clear(staging)) {
            return false;
        }
        // Anything still where the mark goes (a link, a folder) isn't NeoTS2's, and writing
        // through a link would change the file it leads to.
        const fs::path mark = markFor(staging);
        std::error_code markError;
        if (typeOf(mark, markError) != fs::file_type::not_found) {
            return fail(paths::utf8Text(mark) + " isn't a mark NeoTS2 made, so it was left alone");
        }
        if (!std::ofstream(mark, std::ios::binary)) {
            return fail("can't mark " + paths::utf8Text(staging));
        }
        std::error_code error;
        if (!fs::create_directory(staging, error)) {
            return fail("can't make " + paths::utf8Text(staging) + ": " + error.message());
        }
        return true;
    }

    // Puts a finished copy in place of `target`, which is missing or an empty folder. Removing an
    // empty folder fails when anything was put in it meanwhile, so nothing of a player's is
    // replaced. When the move fails, an empty folder that was there is made again.
    bool place(const fs::path& staging, const fs::path& target) {
        std::error_code error;
        std::error_code missing;  // set when the target doesn't exist, which is fine here
        const bool existed = typeOf(target, missing) == fs::file_type::directory;
        if (existed) {
            fs::remove(target, error);
        }
        if (!error) {
            fs::rename(staging, target, error);
            if (error && existed) {
                std::error_code again;
                fs::create_directory(target, again);
            }
        }
        if (error) {
            return fail("can't put the copy in place: " + error.message());
        }
        fs::remove(markFor(staging), error);  // a mark left by a power cut is cleared on the next fill
        return true;
    }
};

// Clears a copy folder when a fill ends with an exception. A fill that ends normally clears it
// itself, so its result has any problem with that.
class StagingCleanup {
public:
    StagingCleanup(Copier& copier, fs::path staging) : copier_(copier), staging_(std::move(staging)) {}
    StagingCleanup(const StagingCleanup&) = delete;
    StagingCleanup& operator=(const StagingCleanup&) = delete;
    ~StagingCleanup() {
        try {
            copier_.clear(staging_);
        } catch (const std::exception&) {
            // The folder and its mark stay, and the next fill clears them.
        }
    }

private:
    Copier& copier_;
    fs::path staging_;
};

SaveFolderFill fill(const fs::path& installRoot, const paths::SaveFolderLock& lock) {
    SaveFolderFill result;
    const fs::path& folder = lock.folder();
    if (folder.empty()) {
        result.problem = "the save folder isn't a usable folder";
        return result;
    }
    if (!lock.held()) {
        result.problem = lock.busy() ? "another NeoTS2 is using " + paths::utf8Text(folder) : lock.problem();
        return result;
    }
    if (const char* refused = paths::saveFolderRefusal(folder, installRoot)) {
        result.problem = refused;
        return result;
    }
    const paths::SaveFolderState state = paths::inspectSaveFolder(folder);
    if (state == paths::SaveFolderState::InUse || state == paths::SaveFolderState::Linked) {
        return result;
    }
    if (state == paths::SaveFolderState::Unreadable) {
        result.problem = "the save folder " + paths::utf8Text(folder) + " can't be read";
        return result;
    }
    if (paths::isOriginalGamesFolder(folder)) {
        result.problem = "the save folder " + paths::utf8Text(folder) +
                         " belongs to the original game. Start the original game once to make it, or choose another folder.";
        return result;
    }

    std::error_code error;
    const fs::path staging =
        folder.parent_path() / (folder.filename().native() + fs::path(std::u8string(paths::kStagingSuffix)).native());
    Copier copier{result, fs::canonical(installRoot, error)};
    if (error) {
        result.problem = "the install " + paths::utf8Text(installRoot) + " can't be read";
        return result;
    }
    // Made even when the install ships nothing, so the save folder always ends up as it should.
    bool ok = copier.prepare(staging);
    const StagingCleanup cleanup(copier, staging);

    // Packs come in release order, so the newest pack's copy of a shared file is written last.
    // Folders the install holds beyond the known packs are left out.
    const std::vector<Product> products = findProducts(installRoot);
    for (const Step& step : kFolders) {
        for (const Product& product : products) {
            if (!ok) {
                break;
            }
            if (product.code == "?" || (step.baseGameOnly && product.code != "base")) {
                continue;
            }
            if (const auto source = userDataFolder(product)) {
                ok = copier.copyTree(*source / step.name, staging / step.name);
            }
        }
    }
    const std::vector<Product> numbered = inGameOrder(products);
    ok = ok && copier.mergeThumbnails(numbered, staging / "Thumbnails");
    ok = ok && copier.importLotCatalog(numbered, staging / "LotCatalog");
    // Only Pets ships Accessory.cache, so which pack the game would prefer among several doesn't
    // matter for the known editions.
    for (const Product& product : products) {
        const auto source = userDataFolder(product);
        if (!ok || product.code == "?" || !source) {
            continue;
        }
        const fs::path file = *source / "Accessory.cache";
        std::error_code typeError;
        if (typeOf(file, typeError) != fs::file_type::regular) {
            continue;
        }
        const auto use = copier.usable(*source);
        ok = use.has_value() && (!*use || copier.copyFile(file, staging / "Accessory.cache"));
        break;
    }

    // The one step that changes the save folder.
    if (ok && paths::inspectSaveFolder(folder) == paths::SaveFolderState::New) {
        ok = copier.place(staging, folder);
    } else if (ok) {
        ok = copier.fail("something was put in " + paths::utf8Text(folder) + " while it was being set up");
    }
    copier.clear(staging);
    result.filled = ok;
    if (!ok) {
        result.files = 0;
        result.bytes = 0;
    }
    return result;
}

SaveFolderFill addNeighborhoods(const fs::path& installRoot, const paths::SaveFolderLock& lock) {
    SaveFolderFill result;
    const fs::path& folder = lock.folder();
    if (folder.empty() || !lock.held()) {
        result.problem = "the save folder isn't held for this game";
        return result;
    }
    if (!paths::lacksNeighborhoods(folder)) {
        return result;
    }
    if (const char* refused = paths::saveFolderRefusal(folder, installRoot)) {
        result.problem = refused;
        return result;
    }

    std::error_code error;
    const fs::path target = folder / "Neighborhoods";
    const fs::path staging =
        folder.parent_path() / (folder.filename().native() + fs::path(std::u8string(paths::kNeighborhoodsStagingSuffix)).native());
    Copier copier{result, fs::canonical(installRoot, error)};
    if (error) {
        result.problem = "the install " + paths::utf8Text(installRoot) + " can't be read";
        return result;
    }
    bool ok = copier.prepare(staging);
    const StagingCleanup cleanup(copier, staging);
    // Copied as on a new save folder's first start, the newest pack's copy of a shared file kept.
    for (const Product& product : findProducts(installRoot)) {
        if (!ok) {
            break;
        }
        if (product.code == "?") {
            continue;
        }
        if (const auto source = userDataFolder(product)) {
            ok = copier.copyTree(*source / "Neighborhoods", staging);
        }
    }
    if (ok && result.files == 0) {
        ok = copier.fail("your install ships no neighborhoods");
    }

    // The one step that changes the save folder.
    if (ok && paths::lacksNeighborhoods(folder)) {
        ok = copier.place(staging, target);
    } else if (ok) {
        ok = copier.fail("neighborhoods were put in " + paths::utf8Text(folder) + " while they were being added");
    }
    copier.clear(staging);
    result.filled = ok;
    if (!ok) {
        result.files = 0;
        result.bytes = 0;
    }
    return result;
}

} // namespace

SaveFolderFill addShippedNeighborhoods(const fs::path& installRoot, const paths::SaveFolderLock& lock) {
    try {
        return addNeighborhoods(installRoot, lock);
    } catch (const std::exception& ex) {
        SaveFolderFill result;
        result.problem = std::string("adding the neighborhoods failed: ") + ex.what();
        return result;
    }
}

SaveFolderFill fillNewSaveFolder(const fs::path& installRoot, const paths::SaveFolderLock& lock) {
    try {
        return fill(installRoot, lock);
    } catch (const std::exception& ex) {
        SaveFolderFill result;
        result.problem = std::string("setting up the save folder failed: ") + ex.what();
        return result;
    }
}

} // namespace ts2::content
