#include "paths/game_folders.hpp"

#include "paths/names.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <system_error>

namespace ts2::paths {

namespace {

namespace fs = std::filesystem;

// A complete install has about 6,000 files in folders a few levels deep. The limits stop the walk
// early when the player picks a whole drive or a folder that only links back to itself.
constexpr int kMaxEntries = 200000;
constexpr int kMaxDepth = 10;

// True for <...>/Base/TSData/Res/Objects/objects.package.
bool isBaseObjects(const fs::path& file) {
    const fs::path objects = file.parent_path();
    const fs::path res = objects.parent_path();
    const fs::path tsdata = res.parent_path();
    return foldedName(file.filename()) == "objects.package" && foldedName(objects.filename()) == "objects" &&
           foldedName(res.filename()) == "res" && foldedName(tsdata.filename()) == "tsdata" &&
           foldedName(tsdata.parent_path().filename()) == "base";
}

// A package's header as the game reads it: the DBPF mark, version 1, then the index's entry count,
// offset and size at 0x24, 0x28 and 0x2C. Each index entry takes at least 20 bytes.
bool packageHeaderWhole(const fs::path& file, std::uintmax_t size) {
    constexpr std::size_t kHeaderSize = 96;
    std::array<unsigned char, kHeaderSize> h{};
    std::ifstream in(file, std::ios::binary);
    if (size < kHeaderSize || !in.read(reinterpret_cast<char*>(h.data()), h.size())) {
        return false;
    }
    const auto u32 = [&](std::size_t at) {
        return std::uint64_t{h[at]} | std::uint64_t{h[at + 1]} << 8 | std::uint64_t{h[at + 2]} << 16 | std::uint64_t{h[at + 3]} << 24;
    };
    const std::uint64_t count = u32(0x24), offset = u32(0x28), indexSize = u32(0x2C);
    return h[0] == 'D' && h[1] == 'B' && h[2] == 'P' && h[3] == 'F' && u32(4) == 1 &&
           (count == 0 || (offset + indexSize <= size && count * 20 <= indexSize));
}

} // namespace

std::string_view editionName(Edition edition) {
    switch (edition) {
    case Edition::UltimateCollection: return "The Sims 2 Ultimate Collection";
    case Edition::LegacyCollection: return "The Sims 2 Legacy Collection";
    case Edition::Other: break;
    }
    return "The Sims 2";
}

InstallCheck checkInstall(const fs::path& root) {
    InstallCheck check;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    int entries = 0;
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (++entries > kMaxEntries) {
            check.tooLarge = true;
            break;
        }
        const fs::directory_entry& entry = *it;
        std::error_code typeError;
        if (entry.is_symlink(typeError)) {
            it.disable_recursion_pending();
            continue;
        }
        if (entry.is_directory(typeError)) {
            if (it.depth() >= kMaxDepth) {
                it.disable_recursion_pending();
            } else if (foldedName(entry.path().filename()) == "tsdata") {
                ++check.packFolders;
            }
            continue;
        }
        const std::string name = foldedName(entry.path().filename());
        if (name.ends_with("_dip_staged")) {
            check.downloading = true;
        } else if (isBaseObjects(entry.path())) {
            check.hasBaseObjects = true;
        }
    }
    return check;
}

DamagedPackages findDamagedPackages(const fs::path& root) {
    constexpr std::size_t kExamples = 5;
    DamagedPackages damaged;
    std::error_code error;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error);
    int entries = 0;
    for (; !error && it != fs::recursive_directory_iterator() && ++entries <= kMaxEntries; it.increment(error)) {
        const fs::directory_entry& entry = *it;
        std::error_code typeError;
        if (entry.is_symlink(typeError) || (entry.is_directory(typeError) && it.depth() >= kMaxDepth)) {
            it.disable_recursion_pending();
            continue;
        }
        if (!entry.is_regular_file(typeError) || !foldedName(entry.path().filename()).ends_with(".package")) {
            continue;
        }
        const std::uintmax_t size = entry.file_size(typeError);
        if (typeError || !packageHeaderWhole(entry.path(), size)) {
            ++damaged.count;
            if (damaged.examples.size() < kExamples) {
                damaged.examples.push_back(entry.path());
            }
        }
    }
    return damaged;
}

fs::path commonFolder(const std::vector<fs::path>& folders) {
    if (folders.empty()) {
        return {};
    }
    fs::path common = folders.front().lexically_normal();
    for (const fs::path& folder : folders) {
        const fs::path normal = folder.lexically_normal();
        fs::path shared;
        auto a = common.begin();
        auto b = normal.begin();
        for (; a != common.end() && b != normal.end() && foldedName(*a) == foldedName(*b); ++a, ++b) {
            shared /= *a;
        }
        common = shared;
    }
    // A shared drive alone isn't an install folder.
    if (common == common.root_path() || common == common.root_name()) {
        return {};
    }
    return common;
}

#ifndef _WIN32
std::vector<GameInstall> findInstalls() {
    return {};
}

std::vector<std::filesystem::path> originalSaveFolderNames() {
    return {u8"The Sims 2", u8"The Sims 2 Legacy"};
}

std::filesystem::path eaGamesFolder() {
    return {};
}
#endif

} // namespace ts2::paths
