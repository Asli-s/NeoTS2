#include "launcher/launcher_ui.hpp"

#include "launcher/game_start.hpp"
#include "launcher/running_games.hpp"
#include "paths/names.hpp"
#include "paths/new_save_folder.hpp"
#include "paths/save_folders.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <system_error>

namespace ts2::launcher {

namespace {

namespace fs = std::filesystem;

// `name` when the game has that renderer, else nothing, which is the game's own choice. A setting made
// with a build that had OpenGL 2 must not make the game of one without it stop at every start.
std::string builtRenderer(const std::string& name) {
#ifndef NEOTS2_GL2
    if (name == "gl2") {
        return {};
    }
#endif
    return name;
}

void heading(const LauncherFonts& fonts, const char* text) {
    ImGui::PushFont(fonts.heading);
    ImGui::SeparatorText(text);
    ImGui::PopFont();
}

const char* installStatus(const paths::InstallCheck& check) {
    if (check.tooLarge) {
        return "Not a game folder (too many files)";
    }
    if (check.downloading) {
        return "Still downloading";
    }
    if (!check.hasBaseObjects) {
        return "The base game's objects are missing";
    }
    return "Ready";
}

// What starting with a save folder will do to it. The rules are the ones the game itself follows,
// so the note never promises what the game then refuses.
std::string saveFolderNote(const fs::path& folder, const fs::path& install) {
    if (const char* refused = paths::saveFolderRefusal(folder, install)) {
        return refused;
    }
    std::error_code error;
    switch (paths::inspectSaveFolder(folder)) {
    case paths::SaveFolderState::New:
        if (!paths::isOriginalGamesFolder(folder)) {
            return "Empty. NeoTS2 adds what your install ships for a new save on its first start.";
        }
        return fs::exists(folder, error) ? "Empty. Start the original game once to set it up, or choose another folder."
                                         : "Doesn't exist yet. Start the original game once to make it, or choose another folder.";
    case paths::SaveFolderState::Unreadable:
        return "Not a folder NeoTS2 can use.";
    case paths::SaveFolderState::Linked:
        return "Leads to another folder. NeoTS2 plays with it as it is and adds nothing to it.";
    case paths::SaveFolderState::InUse:
        break;
    }
    if (paths::lacksNeighborhoods(folder)) {
        return "Has no neighborhoods. NeoTS2 asks before adding the ones your install ships.";
    }
    return {};
}

// True when two paths name the same folder, in any letter case or spelling.
bool samePath(const fs::path& a, const fs::path& b) {
    std::error_code error;
    return a == b || fs::equivalent(a, b, error);
}

// Adds an install the player picked, unless it is listed already. Returns its index.
int addInstall(LauncherState& state, const fs::path& root) {
    for (std::size_t i = 0; i < state.installs.size(); ++i) {
        if (samePath(state.installs[i].root, root)) {
            return static_cast<int>(i);
        }
    }
    paths::GameInstall install;
    install.root = root;
    install.check = paths::checkInstall(root);
    // Any save folder works with any install, so a picked install offers the ones already listed.
    if (!state.installs.empty()) {
        install.saveFolders = state.installs.front().saveFolders;
    }
    state.installs.push_back(std::move(install));
    state.addedInstalls.push_back(root);
    return static_cast<int>(state.installs.size()) - 1;
}

// Lists a save folder under every install, once. Returns its index in the chosen install's list.
int listSaveFolder(LauncherState& state, const fs::path& folder) {
    for (paths::GameInstall& install : state.installs) {
        if (std::none_of(install.saveFolders.begin(), install.saveFolders.end(),
                         [&](const fs::path& listed) { return samePath(listed, folder); })) {
            install.saveFolders.push_back(folder);
        }
    }
    if (state.install < 0) {
        return -1;
    }
    const auto& folders = state.installs[static_cast<std::size_t>(state.install)].saveFolders;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        if (samePath(folders[i], folder)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

settings::LauncherSettings chosenSettings(const LauncherState& state) {
    settings::LauncherSettings out;
    if (state.install >= 0 && static_cast<std::size_t>(state.install) < state.installs.size()) {
        const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
        out.install = install.root;
        if (state.saveFolder >= 0 && static_cast<std::size_t>(state.saveFolder) < install.saveFolders.size()) {
            out.saveFolder = install.saveFolders[static_cast<std::size_t>(state.saveFolder)];
        }
    }
    out.addedInstalls = state.addedInstalls;
    out.addedSaveFolders = state.addedSaveFolders;
    out.backupReminderSeen = state.backupReminderSeen;
    out.renderer = state.renderer;
    out.smoothFollow = state.smoothFollow;
    return out;
}

// A Remove button beside a folder the player added by hand. True when it was pressed. Paths are
// compared as spelled, in any letter case, so no disk is read while drawing.
bool removeButton(const std::vector<fs::path>& added, const fs::path& folder) {
    const auto same = [&](const fs::path& p) { return paths::spelledWithin(p, folder) && paths::spelledWithin(folder, p); };
    if (std::none_of(added.begin(), added.end(), same)) {
        return false;
    }
    ImGui::SameLine();
    const bool pressed = ImGui::SmallButton("Remove");
    ImGui::SetItemTooltip("Takes it off this list. The folder itself stays as it is.");
    return pressed;
}

void removeInstall(LauncherState& state, int index) {
    const fs::path root = state.installs[static_cast<std::size_t>(index)].root;
    std::erase_if(state.addedInstalls, [&](const fs::path& path) { return samePath(path, root); });
    state.installs.erase(state.installs.begin() + index);
    if (state.install == index) {
        state.install = -1;
        for (std::size_t i = 0; i < state.installs.size() && state.install < 0; ++i) {
            if (state.installs[i].check.ready()) {
                state.install = static_cast<int>(i);
            }
        }
        state.saveFolder = state.install >= 0 && !state.installs[static_cast<std::size_t>(state.install)].saveFolders.empty() ? 0 : -1;
    } else if (state.install > index) {
        --state.install;
    }
    state.folderNotes.clear();
}

void removeSaveFolder(LauncherState& state, const fs::path& folder) {
    std::optional<fs::path> chosenFolder;
    if (state.install >= 0 && state.saveFolder >= 0) {
        chosenFolder = state.installs[static_cast<std::size_t>(state.install)].saveFolders[static_cast<std::size_t>(state.saveFolder)];
    }
    std::erase_if(state.addedSaveFolders, [&](const fs::path& path) { return samePath(path, folder); });
    for (paths::GameInstall& install : state.installs) {
        std::erase_if(install.saveFolders, [&](const fs::path& path) { return samePath(path, folder); });
    }
    if (state.install < 0) {
        return;
    }
    const auto& folders = state.installs[static_cast<std::size_t>(state.install)].saveFolders;
    state.saveFolder = folders.empty() ? -1 : 0;
    for (std::size_t i = 0; chosenFolder && i < folders.size(); ++i) {
        if (folders[i] == *chosenFolder) {
            state.saveFolder = static_cast<int>(i);
        }
    }
}

// Saves the player's choices when they changed since the last save.
void rememberChoices(LauncherState& state) {
    settings::LauncherSettings now = chosenSettings(state);
    if (now == state.remembered || state.settingsFile.empty()) {
        return;
    }
    if (!settings::saveLauncherSettings(state.settingsFile, now) && state.message.empty()) {
        state.message = "The launcher couldn't save your choices, so it won't remember them next time.";
    }
    state.remembered = std::move(now);
}

// Called by SDL when the folder dialog closes, possibly on another thread.
void SDLCALL folderPicked(void* userdata, const char* const* files, int) {
    auto* picked = static_cast<PickedFolder*>(userdata);
    std::lock_guard<std::mutex> guard(picked->lock);
    if (files && files[0]) {
        try {
            picked->folder = files[0];
        } catch (const std::exception&) {
            picked->what = Picking::Nothing;  // out of memory: taken as no answer
        }
    } else {
        picked->what = Picking::Nothing;
    }
}

// True while a folder dialog is open. Some systems don't block the window meanwhile, and a second
// dialog would lose the first one's answer.
bool picking(LauncherState& state) {
    std::lock_guard<std::mutex> guard(state.picked.lock);
    return state.picked.what != Picking::Nothing;
}

void pickFolder(LauncherState& state, SDL_Window* window, Picking what) {
    {
        std::lock_guard<std::mutex> guard(state.picked.lock);
        state.picked.what = what;
        state.picked.folder.reset();
    }
    SDL_ShowOpenFolderDialog(folderPicked, &state.picked, window, nullptr, false);
}

// Takes a folder the player picked since the last frame.
void takePickedFolder(LauncherState& state) {
    Picking what = Picking::Nothing;
    fs::path folder;
    {
        std::lock_guard<std::mutex> guard(state.picked.lock);
        if (!state.picked.folder) {
            return;
        }
        what = state.picked.what;
        const auto path = paths::pathFromUtf8(*state.picked.folder);
        state.picked.folder.reset();
        state.picked.what = Picking::Nothing;
        if (!path) {
            state.message = "That folder's name can't be read.";
            return;
        }
        folder = *path;
    }
    state.message.clear();
    if (what == Picking::Install) {
        state.install = addInstall(state, folder);
        const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
        if (!install.check.ready()) {
            state.message = paths::utf8Text(folder) + ": " + installStatus(install.check) + ".";
        }
        state.saveFolder = install.saveFolders.empty() ? -1 : 0;
        state.folderNotes.clear();
    } else if (what == Picking::SaveFolder && state.install >= 0) {
        const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
        if (const char* refused = paths::saveFolderRefusal(folder, install.root)) {
            state.message = paths::utf8Text(folder) + ": " + refused;
            return;
        }
        state.saveFolder = listSaveFolder(state, folder);
        if (std::none_of(state.addedSaveFolders.begin(), state.addedSaveFolders.end(),
                         [&](const fs::path& added) { return samePath(added, folder); })) {
            state.addedSaveFolders.push_back(folder);
        }
    }
}

void drawInstalls(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts) {
    heading(fonts, "Game");
    if (state.installs.empty()) {
        ImGui::TextUnformatted("No Sims 2 install was found. Choose the folder it is installed in.");
    }
    int removed = -1;
    for (std::size_t i = 0; i < state.installs.size(); ++i) {
        const paths::GameInstall& install = state.installs[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string name(paths::editionName(install.edition));
        if (ImGui::RadioButton(name.c_str(), state.install == static_cast<int>(i))) {
            state.install = static_cast<int>(i);
            state.saveFolder = install.saveFolders.empty() ? -1 : 0;
            state.folderNotes.clear();
            state.message.clear();
        }
        if (removeButton(state.addedInstalls, install.root)) {
            removed = static_cast<int>(i);
        }
        ImGui::Indent();
        ImGui::TextDisabled("%s", paths::utf8Text(install.root).c_str());
        ImGui::TextDisabled("%s, %d packs", installStatus(install.check), install.check.packFolders);
        ImGui::Unindent();
        ImGui::PopID();
    }
    if (removed >= 0) {
        removeInstall(state, removed);
    }
    ImGui::BeginDisabled(picking(state));
    if (ImGui::Button("Add another install...")) {
        pickFolder(state, window, Picking::Install);
    }
    ImGui::EndDisabled();
}

// Asks for a new save folder's name, makes the folder in "EA Games" and chooses it.
void drawNewSaveFolder(LauncherState& state) {
    const float size = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(size * 26, 0));
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("New save folder", nullptr, ImGuiWindowFlags_NoResize)) {
        return;
    }
    const fs::path& parent = state.eaGames;
    const std::u8string prefix(paths::kNeoSaveFolderPrefix);
    ImGui::TextWrapped("%s", paths::utf8Text(parent / fs::path(prefix)).c_str());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(-1);
    const bool entered = ImGui::InputTextWithHint("##name", "Leave empty for the next number", state.newFolderName.data(),
                                                  state.newFolderName.size(), ImGuiInputTextFlags_EnterReturnsTrue);
    if (!state.newFolderProblem.empty()) {
        ImGui::TextWrapped("%s", state.newFolderProblem.c_str());
    }
    ImGui::Spacing();
    if (ImGui::Button("Create") || entered) {
        const auto made = paths::makeNewSaveFolder(parent, state.newFolderName.data());
        state.newFolderProblem = made.problem;
        if (!made.folder.empty()) {
            for (paths::GameInstall& install : state.installs) {
                install.saveFolders.push_back(made.folder);
            }
            if (state.install >= 0) {
                state.saveFolder = static_cast<int>(state.installs[static_cast<std::size_t>(state.install)].saveFolders.size()) - 1;
            }
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void drawSaveFolders(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts) {
    heading(fonts, "Save folder");
    // Amber, so it reads as a warning without looking like an error.
    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "Back up your saves first: NeoTS2 is early, unfinished software.");
    ImGui::Spacing();
    if (state.install < 0) {
        ImGui::TextDisabled("Choose a game first.");
        return;
    }
    const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
    const auto& folders = install.saveFolders;
    if (folders.empty()) {
        ImGui::TextUnformatted("No save folder was found for this game. Choose one.");
    }
    std::optional<fs::path> removed;
    for (std::size_t i = 0; i < folders.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        // The path is text beside the button, as ImGui reads "##" in a label as the end of it.
        bool pick = ImGui::RadioButton("##pick", state.saveFolder == static_cast<int>(i));
        ImGui::SameLine();
        ImGui::TextUnformatted(paths::utf8Text(folders[i]).c_str());
        pick = pick || ImGui::IsItemClicked();
        if (pick && state.saveFolder != static_cast<int>(i)) {
            state.saveFolder = static_cast<int>(i);
            state.message.clear();
        }
        if (removeButton(state.addedSaveFolders, folders[i])) {
            removed = folders[i];
        }
        auto note = state.folderNotes.find(folders[i]);
        if (note == state.folderNotes.end()) {
            note = state.folderNotes.emplace(folders[i], saveFolderNote(folders[i], install.root)).first;
        }
        if (!note->second.empty()) {
            ImGui::Indent();
            ImGui::TextDisabled("%s", note->second.c_str());
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (removed) {
        removeSaveFolder(state, *removed);
    }
    if (ImGui::Button("New save folder...")) {
        state.newFolderName.fill('\0');
        state.newFolderProblem.clear();
        ImGui::OpenPopup("New save folder");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(picking(state));
    if (ImGui::Button("Choose another folder...")) {
        pickFolder(state, window, Picking::SaveFolder);
    }
    ImGui::EndDisabled();
    drawNewSaveFolder(state);
}

// True when a game and one of its save folders are chosen.
bool chosen(const LauncherState& state) {
    return state.install >= 0 && static_cast<std::size_t>(state.install) < state.installs.size() && state.saveFolder >= 0 &&
           static_cast<std::size_t>(state.saveFolder) < state.installs[static_cast<std::size_t>(state.install)].saveFolders.size();
}

void startPlaying(LauncherState& state, bool addNeighborhoods) {
    if (!chosen(state)) {
        return;
    }
    const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
    const fs::path& folder = install.saveFolders[static_cast<std::size_t>(state.saveFolder)];
    state.message.clear();
    if (const char* refused = paths::saveFolderRefusal(folder, install.root)) {
        state.message = refused;
        return;
    }
    if (paths::saveFolderBusy(folder)) {
        state.message = "NeoTS2 is already playing with this save folder.";
        return;
    }
    state.game = startGame(install.root, folder, addNeighborhoods, builtRenderer(state.renderer), state.smoothFollow, state.message);
    state.folderNotes.clear();
}

// Starts the game, or first asks about a save folder that has no neighborhoods.
void playOrAsk(LauncherState& state) {
    if (!chosen(state)) {
        return;
    }
    const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
    const fs::path& folder = install.saveFolders[static_cast<std::size_t>(state.saveFolder)];
    const bool refused = paths::saveFolderRefusal(folder, install.root) != nullptr;
    // The first time a save folder that already holds saves is played, the player is reminded once
    // to back it up.
    const paths::SaveFolderState saves = paths::inspectSaveFolder(folder);
    const bool holdsSaves = saves == paths::SaveFolderState::InUse || saves == paths::SaveFolderState::Linked;
    if (!refused && !state.backupReminderSeen && holdsSaves) {
        state.askBackup = true;
    } else if (!refused && paths::lacksNeighborhoods(folder)) {
        state.askNeighborhoods = true;
    } else {
        startPlaying(state, false);
    }
}

void drawPlay(LauncherState& state, const LauncherFonts& fonts) {
    ImGui::Separator();
    const bool hasInstall = state.install >= 0 && state.installs[static_cast<std::size_t>(state.install)].check.ready();
    const bool hasSaves = hasInstall && state.saveFolder >= 0;
    ImGui::PushFont(fonts.heading);
    const float size = ImGui::GetFontSize();
    const ImVec2 button(size * 6, size * 2);
    ImGui::PopFont();
    // "NeoTS2 is running." on the left, centred on the button's height, and Play in the right corner.
    // The row needs its own width, as Play always reaches the window's edge.
    const float rowTop = ImGui::GetCursorPosY();
    const float rowLeft = ImGui::GetCursorPosX();
    // Measured before the text, which would leave less room after it and push Play to the left.
    const float rowRight = rowLeft + ImGui::GetContentRegionAvail().x;
    state.playRowWidth = button.x;
    if (state.game) {
        ImGui::SetCursorPosY(rowTop + (button.y - ImGui::GetTextLineHeight()) / 2);
        ImGui::TextUnformatted("NeoTS2 is running.");
        state.playRowWidth += ImGui::GetItemRectSize().x + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SameLine();
    }
    ImGui::SetCursorPos(ImVec2(std::max(rowRight - button.x, rowLeft), rowTop));
    ImGui::BeginDisabled(!hasSaves || state.game != nullptr);
    ImGui::PushFont(fonts.heading);
    const bool play = ImGui::Button("Play", button);
    ImGui::PopFont();
    ImGui::EndDisabled();
    if (play) {
        if (originalGameRunning()) {
            ImGui::OpenPopup("The Sims 2 is open");
        } else {
            playOrAsk(state);
        }
    }
    if (state.askBackup) {
        ImGui::OpenPopup("Back up your saves");
        state.askBackup = false;
    }
    if (state.askNeighborhoods) {
        ImGui::OpenPopup("No neighborhoods");
        state.askNeighborhoods = false;
    }
    const auto centered = [&] {
        ImGui::SetNextWindowSize(ImVec2(size * 22, 0));
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    };
    centered();
    if (ImGui::BeginPopupModal("The Sims 2 is open", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("The original game is open. If it uses the same save folder, close it first. Two games saving to "
                           "one folder can damage your saves.");
        ImGui::Spacing();
        if (ImGui::Button("Play anyway")) {
            playOrAsk(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    centered();
    if (ImGui::BeginPopupModal("Back up your saves", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("NeoTS2 is early, unfinished software. Before you play with a save folder that holds your saves, "
                           "copy it somewhere safe, or make a new save folder for NeoTS2 with New save folder.");
        ImGui::Spacing();
        ImGui::TextDisabled("This reminder shows once.");
        ImGui::Spacing();
        if (ImGui::Button("Play")) {
            state.backupReminderSeen = true;
            playOrAsk(state);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    centered();
    if (ImGui::BeginPopupModal("No neighborhoods", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::TextWrapped("This save folder has no neighborhoods. Add the ones your install ships? The other files in the folder "
                           "stay as they are.");
        ImGui::Spacing();
        if (ImGui::Button("Add neighborhoods")) {
            startPlaying(state, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Play without")) {
            startPlaying(state, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!state.message.empty()) {
        ImGui::TextWrapped("%s", state.message.c_str());
    }
}

} // namespace

void findInstalls(LauncherState& state, const settings::LauncherSettings& remembered) {
    state.installs = paths::findInstalls();
    for (const fs::path& root : remembered.addedInstalls) {
        addInstall(state, root);
    }
    state.eaGames = paths::eaGamesFolder();
    // A NeoTS2 save folder isn't tied to one edition, so every install lists them.
    for (const fs::path& folder : paths::neoSaveFolders(state.eaGames)) {
        listSaveFolder(state, folder);
    }
    for (const fs::path& folder : remembered.addedSaveFolders) {
        listSaveFolder(state, folder);
        state.addedSaveFolders.push_back(folder);
    }

    state.install = -1;
    state.saveFolder = -1;
    for (std::size_t i = 0; i < state.installs.size(); ++i) {
        if (!remembered.install.empty() && samePath(state.installs[i].root, remembered.install)) {
            state.install = static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < state.installs.size() && state.install < 0; ++i) {
        if (state.installs[i].check.ready()) {
            state.install = static_cast<int>(i);
        }
    }
    if (state.install >= 0) {
        const auto& folders = state.installs[static_cast<std::size_t>(state.install)].saveFolders;
        state.saveFolder = folders.empty() ? -1 : 0;
        for (std::size_t i = 0; i < folders.size(); ++i) {
            if (!remembered.saveFolder.empty() && samePath(folders[i], remembered.saveFolder)) {
                state.saveFolder = static_cast<int>(i);
            }
        }
    }
    state.backupReminderSeen = remembered.backupReminderSeen;
    state.renderer = builtRenderer(remembered.renderer);
    state.smoothFollow = remembered.smoothFollow;
    state.remembered = chosenSettings(state);
}

// What draws the game: its own choice (Direct3D 12, Metal or Vulkan), or OpenGL 2 for systems with no more
// (offered only when the game is built with it).
void drawRenderer(LauncherState& state, const LauncherFonts& fonts) {
    heading(fonts, "Graphics");
    struct Choice {
        const char* value;
        const char* label;
    };
    static constexpr Choice kChoices[] = {
#ifdef __APPLE__
        {"", "Automatic (Metal)"},
#elif defined(_WIN32)
        {"", "Automatic (Direct3D 12)"},
#else
        {"", "Automatic (Vulkan)"},
#endif
#ifdef NEOTS2_GL2
        {"gl2", "OpenGL 2 (for older systems)"},
#endif
    };
    const Choice* current = &kChoices[0];
    for (const auto& choice : kChoices) {
        if (state.renderer == choice.value) {
            current = &choice;
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##renderer", current->label)) {
        for (const auto& choice : kChoices) {
            if (ImGui::Selectable(choice.label, &choice == current)) {
                state.renderer = choice.value;
            }
        }
        ImGui::EndCombo();
    }
}

// Choices that change the game from the original, all off unless the player turns them on.
void drawChanges(LauncherState& state, const LauncherFonts& fonts) {
    heading(fonts, "Changes from the original");
    ImGui::Checkbox("Smooth camera when following a Sim", &state.smoothFollow);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The game's camera follows in small steps. This makes it glide instead.");
    }
}

void drawLauncher(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts) {
    takePickedFolder(state);
    drawInstalls(state, window, fonts);
    ImGui::Spacing();
    drawSaveFolders(state, window, fonts);
    ImGui::Spacing();
    drawRenderer(state, fonts);
    ImGui::Spacing();
    drawChanges(state, fonts);
    ImGui::Spacing();
    // Measured before the Play row and the message, which both stretch to the window's edge.
    const ImGuiWindow* frame = ImGui::GetCurrentWindow();
    state.contentWidth = frame->DC.IdealMaxPos.x - frame->Pos.x + ImGui::GetStyle().WindowPadding.x;
    drawPlay(state, fonts);
    rememberChoices(state);
}

void watchGame(LauncherState& state) {
    int code = 0;
    if (!state.game || !SDL_WaitProcess(state.game, false, &code)) {
        return;
    }
    SDL_DestroyProcess(state.game);
    state.game = nullptr;
    state.folderNotes.clear();
    if (code != 0) {
        state.message = "NeoTS2 stopped with exit code " + std::to_string(code) + ".";
    }
}

} // namespace ts2::launcher
