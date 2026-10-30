// Finding the game's installs and save folders, for the launcher.
#include "test_support.hpp"

#include "paths/game_folders.hpp"
#include "paths/names.hpp"
#include "paths/new_save_folder.hpp"
#include "paths/save_folders.hpp"
#include "paths/steam_files.hpp"
#include "settings/launcher_settings.hpp"

#include <algorithm>
#include <fstream>
#include <random>

namespace {

namespace fs = std::filesystem;

// Both forms of Steam's library list: blocks with a "path" (current Steam) and plain numbered
// folders (older Steam). Backslashes in quoted text are escaped.
void steamLibrariesAreRead() {
    const auto current = ts2::paths::steamLibraryFolders(R"("libraryfolders"
{
    "0"
    {
        "path"      "C:\\Program Files (x86)\\Steam"
        "apps" { "3314070" "4100000000" }
    }
    // a comment
    "1" { "label" "" "path" "D:\\SteamLibrary" }
})");
    CHECK(current.size() == 2 && current[0] == "C:\\Program Files (x86)\\Steam" && current[1] == "D:\\SteamLibrary");

    const auto older = ts2::paths::steamLibraryFolders(R"("LibraryFolders" { "TimeNextStatsReport" "1" "1" "E:\\Games" })");
    CHECK(older.size() == 1 && older[0] == "E:\\Games");
}

void steamManifestNamesItsFolder() {
    const auto folder = ts2::paths::steamInstallFolder(
        R"("AppState" { "appid" "3314070" "installdir" "The Sims 2 Legacy Collection" })");
    CHECK(folder && *folder == "The Sims 2 Legacy Collection");
    // A folder name that leaves the library is refused.
    CHECK(!ts2::paths::steamInstallFolder(R"("AppState" { "installdir" "..\\..\\Windows" })"));
    CHECK(!ts2::paths::steamInstallFolder(R"("AppState" { "installdir" ".." })"));
}

// Broken and hostile files give nothing instead of failing.
void brokenSteamFilesGiveNothing() {
    CHECK(ts2::paths::steamLibraryFolders("").empty());
    CHECK(ts2::paths::steamLibraryFolders(R"("libraryfolders" { "0" { "path" "C:\\x")").empty());
    CHECK(ts2::paths::steamLibraryFolders("}}}{{{").empty());
    std::string deep = "\"libraryfolders\" {";
    for (int i = 0; i < 10000; ++i) {
        deep += " \"a\" {";
    }
    CHECK(ts2::paths::steamLibraryFolders(deep).empty());
    CHECK(!ts2::paths::steamInstallFolder(std::string(2 << 20, 'a')));
}

void installCheckReadsNamesOnly() {
    const fs::path root = fs::temp_directory_path() / ("neots2-paths-test-" + std::to_string(std::random_device{}()));
    const auto touch = [](const fs::path& file) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary).put('x');
    };
    fs::create_directories(root / "Seasons" / "TSData");
    auto check = ts2::paths::checkInstall(root);
    CHECK(!check.ready() && check.packFolders == 1 && !check.hasBaseObjects);

    touch(root / "Double Deluxe" / "Base" / "TSData" / "Res" / "Objects" / "objects.package");
    check = ts2::paths::checkInstall(root);
    CHECK(check.ready() && check.packFolders == 2);

    touch(root / "Seasons" / "TSData" / "Res" / "Sound" / "Sounds.package_DiP_Staged");
    check = ts2::paths::checkInstall(root);
    CHECK(!check.ready() && check.downloading);

    std::error_code error;
    fs::remove_all(root, error);
    CHECK(!ts2::paths::checkInstall(root).ready());
}

void installRootHoldsEveryPack() {
    using ts2::paths::commonFolder;
    const fs::path root = fs::temp_directory_path().root_path();
    const fs::path uc = root / "Games" / "UC";
    CHECK(commonFolder({uc / "Double Deluxe" / "Base", uc / "Seasons", root / "games" / "uc" / "University Life" / "EP1"}) == uc);
    CHECK(commonFolder({uc / "Double Deluxe" / "Base"}) == uc / "Double Deluxe" / "Base");
    CHECK(commonFolder({root / "Base", root / "Seasons"}).empty());
    CHECK(commonFolder({}).empty());
#ifdef _WIN32
    CHECK(commonFolder({"C:\\A\\Base", "D:\\B\\Seasons"}).empty());
#endif
}

// The launcher's notes and the game's fill name and judge save folders the same way.
void saveFoldersAreJudgedOneWay() {
    using namespace ts2::paths;
    const fs::path root = fs::temp_directory_path() / ("neots2-paths-test-" + std::to_string(std::random_device{}()));
    CHECK(saveFolderPath(root / "saves" / "") == root / "saves");
    CHECK(saveFolderPath(root / "a" / ".." / "saves") == root / "saves");
    CHECK(saveFolderPath(root.root_path()).empty());

    CHECK(inspectSaveFolder(root / "saves") == SaveFolderState::New);
    fs::create_directories(root / "saves");
    CHECK(inspectSaveFolder(root / "saves") == SaveFolderState::New);
    std::ofstream(root / "saves" / "note.txt") << "x";
    CHECK(inspectSaveFolder(root / "saves") == SaveFolderState::InUse);
    CHECK(inspectSaveFolder(root / "saves" / "note.txt") == SaveFolderState::Unreadable);

    CHECK(isOriginalGamesFolder(root / "EA Games" / "The Sims 2"));
    CHECK(isOriginalGamesFolder(root / "ea games." / "The Sims 2" / ""));
    CHECK(isOriginalGamesFolder(root / "EA Games" / "the sims 2 legacy"));
    CHECK(isOriginalGamesFolder(root / "EA Games" / "The Sims 2 Double Deluxe"));
    CHECK(!isOriginalGamesFolder(root / "EA Games" / "The Sims 2 Neo - Medieval"));
    CHECK(!isOriginalGamesFolder(root / "saves") && !isOriginalGamesFolder(root / "EA Games" / "My Saves"));

    CHECK(isWithin(root / "Install" / "Base", root / "install" / ""));
    CHECK(isWithin(root / "install", root / "install"));
    CHECK(spelledWithin(root / "Install", root / "install" / "") && !spelledWithin(root, root / "install" / ""));
    CHECK(!isWithin(root / "install2", root / "install") && !isWithin(root, root / "install"));
    CHECK(!isSystemFolder(root / "saves"));
#ifdef _WIN32
    CHECK(isSystemFolder("C:/Windows/System32/saves") && isSystemFolder("c:/program files (x86)/Steam/saves"));
    CHECK(isSystemFolder("C:/ProgramData") && !isSystemFolder("C:/Users/Public/Documents/EA Games/The Sims 2"));
    const SaveFolderLock locked("C:/Windows/saves");
    CHECK(!locked.held() && !locked.busy() && !locked.problem().empty());
#endif
    std::error_code error;
    fs::remove_all(root, error);
}

// New NeoTS2 save folders are named "The Sims 2 Neo - <name>". An empty name takes the lowest free
// number, and a name that is taken in any letter case or that Windows can't use makes nothing.
void newSaveFoldersAreNamed() {
    using namespace ts2::paths;
    const fs::path root = fs::temp_directory_path() / ("neots2-paths-test-" + std::to_string(std::random_device{}()));
    const fs::path games = root / "EA Games";
    fs::create_directories(games / "The Sims 2 Neo - 2");
    const auto first = makeNewSaveFolder(games, "");
    CHECK(first.problem.empty() && first.folder == games / "The Sims 2 Neo - 1" && fs::is_directory(first.folder));
    const auto third = makeNewSaveFolder(games, "  ");
    CHECK(third.problem.empty() && third.folder == games / "The Sims 2 Neo - 3");

    const auto named = makeNewSaveFolder(games, " Medieval ");
    CHECK(named.problem.empty() && named.folder == games / "The Sims 2 Neo - Medieval");
    const auto again = makeNewSaveFolder(games, "medieval");
    CHECK(again.folder.empty() && !again.problem.empty());
    for (const char* bad : {"a/b", "a\\b", "what?", "dots.", "tab\there", "0123456789012345678901234567890123456789012345678901234567890123456789"}) {
        const auto refused = makeNewSaveFolder(games, bad);
        CHECK(refused.folder.empty() && !refused.problem.empty());
    }
    const auto unicode = makeNewSaveFolder(games, "\xC3\x87\xC3\xA9");
    CHECK(unicode.problem.empty() && fs::is_directory(unicode.folder));

    fs::create_directories(games / "The Sims 2");
    std::ofstream(games / "The Sims 2 Neo - file") << "x";
    const auto found = neoSaveFolders(games);
    CHECK(found.size() == 5 && found.front() == games / "The Sims 2 Neo - 1");
    CHECK(std::find(found.begin(), found.end(), games / "The Sims 2") == found.end());
    CHECK(makeNewSaveFolder({}, "x").folder.empty());
    std::error_code error;
    fs::remove_all(root, error);
}

// The launcher's settings come back as saved. A file of another version, or a broken one, gives
// the defaults with a note. Lines it can't use are skipped.
void launcherSettingsRoundTrip() {
    using namespace ts2::settings;
    const fs::path root = fs::temp_directory_path() / ("neots2-paths-test-" + std::to_string(std::random_device{}()));
    LauncherSettings settings;
    settings.install = root / "Games" / "UC";
    settings.saveFolder = root / "EA Games" / u8"The Sims\u2122 2 Ultimate Collection";
    settings.addedInstalls = {root / "Other"};
    settings.addedSaveFolders = {root / "Saves A", root / "Saves B"};
    settings.backupReminderSeen = true;
    settings.renderer = "gl2";
    const fs::path file = root / "NeoTS2" / "launcher-settings.txt";
    CHECK(saveLauncherSettings(file, settings));
    const auto loaded = loadLauncherSettings(file);
    CHECK(loaded.problem.empty() && loaded.settings == settings);
    CHECK(!fs::exists(root / "NeoTS2" / "launcher-settings.txt.new"));

    const auto missing = loadLauncherSettings(root / "none.txt");
    CHECK(missing.problem.empty() && missing.settings == LauncherSettings{});
    std::ofstream(root / "broken.txt") << "something else\ninstall=C:\\x\n";
    const auto broken = loadLauncherSettings(root / "broken.txt");
    CHECK(!broken.problem.empty() && broken.settings == LauncherSettings{});

    // Settings hold absolute paths, and what is absolute depends on the system.
#ifdef _WIN32
    const std::string absolute = "C:\\a";
#else
    const std::string absolute = "/a";
#endif
    const auto parsed = parseLauncherSettings("NeoTS2 launcher settings 1\r\ninstall=relative\\path\r\nsave-folder=\xFF\xFE\r\n"
                                              "unknown=C:\\x\r\nno equals sign\r\nadded-save-folder=" + absolute + "\r\n"
                                              "added-save-folder=" + absolute + "\r\n");
    CHECK(parsed && parsed->install.empty() && parsed->saveFolder.empty() && parsed->addedSaveFolders.size() == 1);
    CHECK(!parseLauncherSettings("") && !parseLauncherSettings("NeoTS2 launcher settings 2\n"));
    // Only the renderers there are are taken.
    const auto drawn = parseLauncherSettings("NeoTS2 launcher settings 1\nrenderer=sdlgpu\n");
    CHECK(drawn && drawn->renderer == "sdlgpu");
    const auto unknown = parseLauncherSettings("NeoTS2 launcher settings 1\nrenderer=metal\n");
    CHECK(unknown && unknown->renderer.empty());
    LauncherSettings invalid;
    invalid.renderer = "metal";
    CHECK(formatLauncherSettings(invalid).find("renderer") == std::string::npos);
    // Smooth follow is off unless the file turns it on, and is kept when saved.
    CHECK(drawn && !drawn->smoothFollow);
    LauncherSettings smooth;
    smooth.smoothFollow = true;
    const auto kept = parseLauncherSettings(formatLauncherSettings(smooth));
    CHECK(kept && kept->smoothFollow);
    const auto other = parseLauncherSettings("NeoTS2 launcher settings 1\nsmooth-follow=yes\n");
    CHECK(other && !other->smoothFollow);
    // The window size is one the launcher offers, or the game's default.
    CHECK(drawn && drawn->windowSize.empty());
    LauncherSettings sized;
    sized.windowSize = "1920x1080";
    const auto sizeKept = parseLauncherSettings(formatLauncherSettings(sized));
    CHECK(sizeKept && sizeKept->windowSize == "1920x1080");
    const auto oddSize = parseLauncherSettings("NeoTS2 launcher settings 1\nwindow-size=99999x99999\n");
    CHECK(oddSize && oddSize->windowSize.empty());
    sized.windowSize = "fit-screen";
    const auto fitKept = parseLauncherSettings(formatLauncherSettings(sized));
    CHECK(fitKept && fitKept->windowSize == "fit-screen" && findWindowSize("fit-screen")->fitScreen);
    sized.windowSize = "800x600";
    CHECK(formatLauncherSettings(sized).find("window-size") == std::string::npos);
    // The UI's size is one the launcher offers, or the game's own 100%.
    CHECK(drawn && drawn->uiScale == 100);
    LauncherSettings scaled;
    scaled.uiScale = 175;
    const auto scaleKept = parseLauncherSettings(formatLauncherSettings(scaled));
    CHECK(scaleKept && scaleKept->uiScale == 175);
    const auto oddScale = parseLauncherSettings("NeoTS2 launcher settings 1\nui-scale=1000\n");
    CHECK(oddScale && oddScale->uiScale == 100);
    scaled.uiScale = 300;
    CHECK(formatLauncherSettings(scaled).find("ui-scale") == std::string::npos);
    CHECK(!parseLauncherSettings("NeoTS2 launcher settings 1\n" + std::string(70 * 1024, 'x')));
    std::error_code error;
    fs::remove_all(root, error);
}

// Names that end like NeoTS2's own work folders are never save folders, so a fill can't take a
// player's folder for its leftovers. Checking whether a folder is busy never makes a lock file.
void workNamesStayNeoTS2s() {
    using namespace ts2::paths;
    const fs::path root = fs::temp_directory_path() / ("neots2-paths-test-" + std::to_string(std::random_device{}()));
    const fs::path games = root / "EA Games";
    fs::create_directories(games / "The Sims 2 Neo - A.neots2-setup");
    fs::create_directories(games / "The Sims 2 Neo - B");
    CHECK(isWorkName("Saves.NEOTS2-SETUP") && isWorkName("x.neots2-neighborhoods") && isWorkName("x.neots2-lock"));
    CHECK(!isWorkName("Saves") && !isWorkName("neots2-setup notes"));
    CHECK(saveFolderRefusal(root / "Saves.neots2-setup", root / "install") != nullptr);
    CHECK(saveFolderRefusal(root / "Saves", root / "install") == nullptr);
    CHECK(makeNewSaveFolder(games, "C.neots2-lock").folder.empty());
    const auto listed = neoSaveFolders(games);
    CHECK(listed.size() == 1 && listed.front() == games / "The Sims 2 Neo - B");

    CHECK(!saveFolderBusy(games / "The Sims 2 Neo - B") && !fs::exists(games / "The Sims 2 Neo - B.neots2-lock"));
    {
        const SaveFolderLock lock(games / "The Sims 2 Neo - B");
        CHECK(lock.held() && saveFolderBusy(games / "The Sims 2 Neo - B"));
    }
    CHECK(!saveFolderBusy(games / "The Sims 2 Neo - B"));

    // A work folder's name is caught however the path ends.
    CHECK(saveFolderRefusal(root / "Saves.neots2-setup" / "", root / "install") != nullptr);
    CHECK(saveFolderRefusal(root / "Saves.neots2-setup" / ".", root / "install") != nullptr);

#ifdef _WIN32
    // A player's own file with the lock's name is never used, so it is never deleted. An empty one
    // left by a power cut is.
    std::ofstream(root / "Mine.neots2-lock") << "a player's file";
    {
        const SaveFolderLock lock(root / "Mine");
        CHECK(!lock.held() && !lock.busy() && !lock.problem().empty());
    }
    CHECK(fs::file_size(root / "Mine.neots2-lock") == 15);
    std::ofstream(root / "Stale.neots2-lock").close();
    {
        const SaveFolderLock lock(root / "Stale");
        CHECK(lock.held());
    }
    CHECK(!fs::exists(root / "Stale.neots2-lock"));
#endif
    std::error_code error;
    fs::remove_all(root, error);
}

// A settings list longer than the reader takes is written short, so the file always reads back.
void longSettingsListsStillLoad() {
    using namespace ts2::settings;
    LauncherSettings settings;
    for (int i = 0; i < 100; ++i) {
        settings.addedSaveFolders.push_back(fs::temp_directory_path() / "Saves" / std::to_string(i));
    }
    const auto parsed = parseLauncherSettings(formatLauncherSettings(settings));
    CHECK(parsed && parsed->addedSaveFolders.size() == 64);
}

// A package of `size` bytes whose header says it is DBPF 1.1 with `count` index entries at `offset`.
void writePackage(const fs::path& file, std::size_t size, std::uint32_t count, std::uint32_t offset, const char* mark = "DBPF") {
    std::vector<char> bytes(size, 0);
    const auto put = [&](std::size_t at, std::uint32_t v) {
        for (std::size_t i = 0; i < 4 && at + i < bytes.size(); ++i) {
            bytes[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        }
    };
    for (std::size_t i = 0; i < 4 && i < bytes.size(); ++i) {
        bytes[i] = mark[i];
    }
    put(4, 1);
    put(8, 1);
    put(0x20, 7);
    put(0x24, count);
    put(0x28, offset);
    put(0x2C, count * 20);
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Packages cut short, never written or not packages at all are found. Whole ones, files that
// aren't packages, downloads still under way and Mac sidecars aren't.
void damagedPackagesAreFound() {
    const fs::path root = fs::temp_directory_path() / ("neots2-damage-test-" + std::to_string(std::random_device{}()));
    writePackage(root / "Base/TSData/Res/Objects/objects.package", 116, 1, 96);
    writePackage(root / "Base/TSData/Res/UI/empty.package", 96, 0, 0);
    writePackage(root / "EP1/TSData/Res/Objects/objects.package", 100, 1, 96);   // index past the end
    writePackage(root / "EP1/TSData/Res/UI/ui.package", 50, 0, 0);                // shorter than its header
    writePackage(root / "EP2/TSData/Res/UI/zeros.package", 4096, 0, 0, "\0\0\0\0");  // never written
    writePackage(root / "EP2/TSData/Res/UI/ui.package_DiP_Staged", 10, 0, 0);
    writePackage(root / "EP2/TSData/Res/UI/._ui.package", 4096, 0, 0, "\x00\x05\x16\x07");  // a Mac's sidecar
    std::ofstream(root / "EP2/readme.txt") << "not a package";

    const auto damaged = ts2::paths::findDamagedPackages(root);
    CHECK(damaged.count == 3 && damaged.examples.size() == 3);
    std::error_code error;
    fs::remove_all(root, error);
}

} // namespace

std::vector<Test> pathsTests() {
    return {
        {"steam libraries are read", steamLibrariesAreRead},
        {"steam manifest names its folder", steamManifestNamesItsFolder},
        {"broken steam files give nothing", brokenSteamFilesGiveNothing},
        {"install check reads names only", installCheckReadsNamesOnly},
        {"install root holds every pack", installRootHoldsEveryPack},
        {"save folders are judged one way", saveFoldersAreJudgedOneWay},
        {"new save folders are named", newSaveFoldersAreNamed},
        {"launcher settings round trip", launcherSettingsRoundTrip},
        {"work names stay NeoTS2's", workNamesStayNeoTS2s},
        {"long settings lists still load", longSettingsListsStillLoad},
        {"damaged packages are found", damagedPackagesAreFound},
    };
}
