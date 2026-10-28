#include "paths/new_save_folder.hpp"

#include "paths/names.hpp"
#include "paths/save_folders.hpp"

#include <algorithm>
#include <system_error>

namespace ts2::paths {

namespace {

namespace fs = std::filesystem;

// Long enough for any name a player picks, and well inside Windows' path limits.
constexpr std::size_t kMaxNameBytes = 64;
// Keeps the search for a free number short in an "EA Games" folder full of numbered folders.
constexpr int kMaxNumber = 9999;
// Keeps the startup listing short in an "EA Games" folder with huge numbers of entries.
constexpr int kMaxEntries = 10000;

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    return text;
}

// A name Windows can use as the end of a folder name.
bool usableName(std::string_view name) {
    if (name.size() > kMaxNameBytes || name.back() == '.') {
        return false;
    }
    return std::none_of(name.begin(), name.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
    });
}

fs::path folderFor(const fs::path& parent, std::string_view name) {
    std::u8string text(kNeoSaveFolderPrefix);
    text.append(name.begin(), name.end());
    return parent / fs::path(text);
}

// Whether `parent` holds an entry named as `folder` apart from letter case. Windows sees those as one
// name, so a save folder stays usable when it is copied there from a system that doesn't.
bool takenIgnoringCase(const fs::path& parent, const fs::path& folder) {
    const std::string wanted = foldedName(folder.filename());
    std::error_code error;
    int seen = 0;
    for (fs::directory_iterator it(parent, error), end; !error && it != end && ++seen <= kMaxEntries; it.increment(error)) {
        if (foldedName(it->path().filename()) == wanted) {
            return true;
        }
    }
    return false;
}

NewSaveFolder makeFolder(const fs::path& parent, std::string_view name) {
    NewSaveFolder result;
    std::error_code error;
    if (parent.empty()) {
        result.problem = "There is no Documents folder to put it in.";
        return result;
    }
    fs::create_directories(parent, error);
    if (error) {
        result.problem = "The EA Games folder can't be made: " + error.message();
        return result;
    }
    name = trimmed(name);
    if (name.empty()) {
        for (int number = 1; number <= kMaxNumber; ++number) {
            const fs::path folder = folderFor(parent, std::to_string(number));
            if (fs::exists(fs::symlink_status(folder, error))) {
                continue;
            }
            if (fs::create_directory(folder, error)) {
                result.folder = folder;
                return result;
            }
            if (error) {
                result.problem = "The save folder can't be made: " + error.message();
                return result;
            }
        }
        result.problem = "There are already too many numbered save folders.";
        return result;
    }
    if (!usableName(name)) {
        result.problem = "A folder name can't use < > : \" / \\ | ? *, can't end with a dot, and has to be short.";
        return result;
    }
    const fs::path folder = folderFor(parent, name);
    if (isWorkName(folder.filename())) {
        result.problem = "That name ends like the folders NeoTS2 makes for itself while it works.";
        return result;
    }
    if (fs::exists(fs::symlink_status(folder, error)) || takenIgnoringCase(parent, folder)) {
        result.problem = "A save folder with that name already exists.";
        return result;
    }
    // Fails also when a folder of that name was made a moment ago.
    if (!fs::create_directory(folder, error)) {
        result.problem = error ? "The save folder can't be made: " + error.message() : "A save folder with that name already exists.";
        return result;
    }
    result.folder = folder;
    return result;
}

} // namespace

NewSaveFolder makeNewSaveFolder(const fs::path& parent, std::string_view name) {
    try {
        return makeFolder(parent, name);
    } catch (const std::exception&) {
        return {{}, "That name can't be used for a folder."};  // text that isn't valid UTF-8, for one
    }
}

std::vector<fs::path> neoSaveFolders(const fs::path& parent) {
    std::vector<fs::path> folders;
    const std::string prefix = foldedName(fs::path(std::u8string(kNeoSaveFolderPrefix)));
    std::error_code error;
    int seen = 0;
    for (fs::directory_iterator it(parent, error), end; !error && it != end && ++seen <= kMaxEntries; it.increment(error)) {
        std::error_code typeError;
        const fs::path name = it->path().filename();
        if (it->symlink_status(typeError).type() == fs::file_type::directory && foldedName(name).starts_with(prefix) &&
            !isWorkName(name)) {
            folders.push_back(it->path());
        }
    }
    std::sort(folders.begin(), folders.end());
    return folders;
}

} // namespace ts2::paths
