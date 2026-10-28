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
    out.windowSize = state.windowSize;
    return out;
}

// True when the player added this folder by hand. Paths are compared as spelled, in any letter case,
// so no disk is read while drawing.
bool addedByHand(const std::vector<fs::path>& added, const fs::path& folder) {
    const auto same = [&](const fs::path& p) { return paths::spelledWithin(p, folder) && paths::spelledWithin(folder, p); };
    return std::any_of(added.begin(), added.end(), same);
}

// The width a remove button takes beside the item before it. The button is a square as tall as a
// radio button.
float removeButtonWidth() {
    return ImGui::GetStyle().ItemSpacing.x + ImGui::GetFrameHeight();
}

// A remove button on the line of a folder the player added by hand: a cross at `right`, the column's
// right edge, so every one lines up. Lit when the pointer is on it. True when it was pressed.
bool removeButton(const std::vector<fs::path>& added, const fs::path& folder, float right) {
    if (!addedByHand(added, folder)) {
        return false;
    }
    ImGui::SameLine();
    const float size = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(std::max(right - size, ImGui::GetCursorScreenPos().x), ImGui::GetCursorScreenPos().y));
    const ImVec2 corner = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("remove", ImVec2(size, size));
    const bool lit = ImGui::IsItemHovered();
    ImGui::SetItemTooltip("Remove from the list");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 far(corner.x + size, corner.y + size);
    if (lit) {
        draw->AddRectFilled(corner, far, ImGui::GetColorU32(ImGuiCol_ButtonHovered), ImGui::GetStyle().FrameRounding);
    }
    const float inset = size * 0.33f;
    const float thickness = std::max(1.0f, ImGui::GetFontSize() / 12);
    const ImU32 color = ImGui::GetColorU32(lit ? ImGuiCol_Text : ImGuiCol_TextDisabled);
    draw->AddLine(ImVec2(corner.x + inset, corner.y + inset), ImVec2(far.x - inset, far.y - inset), color, thickness);
    draw->AddLine(ImVec2(corner.x + inset, far.y - inset), ImVec2(far.x - inset, corner.y + inset), color, thickness);
    return pressed;
}

// `text` cut down to fit `width` by taking characters out of its middle, which keeps a path's drive
// and its last folder's name. Whole when it fits.
std::string fitted(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    // Where each character starts, so a cut never splits one (the text is UTF-8).
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            starts.push_back(i);
        }
    }
    const std::size_t count = starts.size();
    // A third of the kept characters come from the start and the rest from the end.
    const auto keep = [&](std::size_t kept) {
        const std::size_t head = kept / 3;
        const std::size_t tail = kept - head;
        const std::size_t headEnd = head < count ? starts[head] : text.size();
        const std::size_t tailStart = tail > 0 ? starts[count - tail] : text.size();
        return text.substr(0, headEnd) + "..." + text.substr(tailStart);
    };
    // The most characters that still fit, found by halving.
    std::size_t low = 0;
    std::size_t high = count > 0 ? count - 1 : 0;
    while (low < high) {
        const std::size_t middle = (low + high + 1) / 2;
        if (ImGui::CalcTextSize(keep(middle).c_str()).x <= width) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return keep(low);
}

// A path in at most `width`, shortened in the middle when it is longer, with the whole path shown
// when the pointer rests on it.
void pathText(const fs::path& path, float width, bool dimmed) {
    const std::string whole = paths::utf8Text(path);
    const std::string shown = fitted(whole, width);
    if (dimmed) {
        ImGui::TextDisabled("%s", shown.c_str());
    } else {
        ImGui::TextUnformatted(shown.c_str());
    }
    if (shown != whole) {
        ImGui::SetItemTooltip("%s", whole.c_str());
    }
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
        ImGui::TextWrapped("No Sims 2 install was found. Choose the folder it is installed in.");
    }
    int removed = -1;
    ImGui::PushID("installs");  // the save folders' rows are numbered from 0 too
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
        // The status goes beside the name, or under the path when it is too long for that line.
        const std::string status = std::string(installStatus(install.check)) + ", " + std::to_string(install.check.packFolders) + " packs";
        const float removeRoom = addedByHand(state.addedInstalls, install.root) ? removeButtonWidth() : 0;
        ImGui::SameLine();
        const bool besideName =
            ImGui::GetCursorScreenPos().x + ImGui::CalcTextSize(status.c_str()).x + removeRoom <= state.columnRight;
        if (besideName) {
            ImGui::TextDisabled("%s", status.c_str());
        } else {
            ImGui::Dummy(ImVec2(0, 0));
        }
        if (removeButton(state.addedInstalls, install.root, state.columnRight)) {
            removed = static_cast<int>(i);
        }
        ImGui::Indent();
        pathText(install.root, state.columnRight - ImGui::GetCursorScreenPos().x, true);
        if (!besideName) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", status.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Unindent();
        ImGui::PopID();
    }
    ImGui::PopID();
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
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
    ImGui::TextWrapped("Back up your saves first: NeoTS2 is early, unfinished software.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (state.install < 0) {
        ImGui::TextDisabled("Choose a game first.");
        return;
    }
    const paths::GameInstall& install = state.installs[static_cast<std::size_t>(state.install)];
    const auto& folders = install.saveFolders;
    if (folders.empty()) {
        ImGui::TextWrapped("No save folder was found for this game. Choose one.");
    }
    std::optional<fs::path> removed;
    ImGui::PushID("saveFolders");
    for (std::size_t i = 0; i < folders.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        // The path is text beside the button, as ImGui reads "##" in a label as the end of it.
        bool pick = ImGui::RadioButton("##pick", state.saveFolder == static_cast<int>(i));
        ImGui::SameLine();
        // Room is kept for the remove button, so a long path never pushes it out of sight.
        const float removeRoom = addedByHand(state.addedSaveFolders, folders[i]) ? removeButtonWidth() : 0;
        pathText(folders[i], state.columnRight - ImGui::GetCursorScreenPos().x - removeRoom, false);
        pick = pick || ImGui::IsItemClicked();
        if (pick && state.saveFolder != static_cast<int>(i)) {
            state.saveFolder = static_cast<int>(i);
            state.message.clear();
        }
        if (removeButton(state.addedSaveFolders, folders[i], state.columnRight)) {
            removed = folders[i];
        }
        auto note = state.folderNotes.find(folders[i]);
        if (note == state.folderNotes.end()) {
            note = state.folderNotes.emplace(folders[i], saveFolderNote(folders[i], install.root)).first;
        }
        if (!note->second.empty()) {
            ImGui::Indent();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", note->second.c_str());
            ImGui::PopStyleColor();
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    ImGui::PopID();
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
    const settings::WindowSize* size = settings::findWindowSize(state.windowSize);
    state.game = startGame(install.root, folder, addNeighborhoods, builtRenderer(state.renderer), state.smoothFollow, size ? size->width : 0,
                           size ? size->height : 0, size && size->fitScreen, state.message);
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
    state.windowSize = remembered.windowSize;
    state.remembered = chosenSettings(state);
}

// What draws the game: its own choice (Direct3D 12, Metal or Vulkan), or OpenGL 2 for systems with no more
// (offered only when the game is built with it). And the size of the game's window.
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
    // Each choice has its name in front, and the choices line up after the longer name.
    const float labelWidth = std::max(ImGui::CalcTextSize("Renderer").x, ImGui::CalcTextSize("Window size").x) +
                             ImGui::GetStyle().ItemSpacing.x * 2;
    const auto label = [labelWidth](const char* text) {
        const float start = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + labelWidth);
    };
    label("Renderer");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##renderer", current->label)) {
        for (const auto& choice : kChoices) {
            if (ImGui::Selectable(choice.label, &choice == current)) {
                state.renderer = choice.value;
            }
        }
        ImGui::EndCombo();
    }
    // The window sizes are offered here until the game has its own display settings.
    static constexpr const char* kDefaultSize = "Window 1280 x 800 (default)";
    static constexpr const char* kSizeLabels[] = {"Window 1280 x 720 (720p)", "Window 1920 x 1080 (1080p)", "Window 2560 x 1440 (1440p)",
                                                  "Fit the screen (windowed)"};
    static_assert(std::size(kSizeLabels) == std::size(settings::kWindowSizes));
#ifdef _WIN32
    constexpr bool kFitScreenWorks = true;
#else
    constexpr bool kFitScreenWorks = false;  // the game can't measure its title bar here yet
#endif
    const char* currentSize = kDefaultSize;
    for (std::size_t i = 0; i < std::size(kSizeLabels); ++i) {
        if (state.windowSize == settings::kWindowSizes[i].key) {
            currentSize = kSizeLabels[i];
        }
    }
    label("Window size");
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18);
    if (ImGui::BeginCombo("##windowSize", currentSize)) {
        if (ImGui::Selectable(kDefaultSize, state.windowSize.empty())) {
            state.windowSize.clear();
        }
        for (std::size_t i = 0; i < std::size(kSizeLabels); ++i) {
            const auto& size = settings::kWindowSizes[i];
            if (size.fitScreen && !kFitScreenWorks) {
                continue;
            }
            if (ImGui::Selectable(kSizeLabels[i], state.windowSize == size.key)) {
                state.windowSize = size.key;
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

// The space between the two columns.
float columnGap() {
    return ImGui::GetFontSize() * 2;
}

// The game and its save folder in one column, the options in the other. True when they are side by side.
bool drawSections(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts) {
    const ImGuiStyle& style = ImGui::GetStyle();
    // The game's column has one width, so a long folder name never widens the window. Its paths are
    // shortened to fit and its notes wrap. A screen too narrow for it gets what it has.
    const float gameWidth = std::max(
        std::min(ImGui::GetFontSize() * 30, state.maxWidth - style.WindowPadding.x * 2 - style.ScrollbarSize), ImGui::GetFontSize() * 12);
    const auto game = [&] {
        state.columnRight = ImGui::GetCursorScreenPos().x + gameWidth;
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + gameWidth);
        drawInstalls(state, window, fonts);
        ImGui::Spacing();
        drawSaveFolders(state, window, fonts);
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(gameWidth, 0));
        ImGui::EndGroup();
        state.leftWidth = ImGui::GetItemRectSize().x;
    };
    const auto options = [&] {
        ImGui::BeginGroup();
        drawRenderer(state, fonts);
        ImGui::Spacing();
        drawChanges(state, fonts);
        ImGui::EndGroup();
        state.rightWidth = ImGui::GetItemRectSize().x;
    };
    // Side by side when the screen is wide enough for both, one above the other when not.
    const float sideBySide = state.leftWidth + columnGap() + state.rightWidth + style.WindowPadding.x * 2 + style.ScrollbarSize;
    // Each cell is padded on its inner side only, as the table has no outer borders.
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(columnGap() / 2, 0));
    const bool columns = state.leftWidth > 0 && sideBySide <= state.maxWidth &&
                         ImGui::BeginTable("columns", 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX);
    ImGui::PopStyleVar();
    if (columns) {
        ImGui::TableNextColumn();
        game();
        ImGui::TableNextColumn();
        options();
        ImGui::EndTable();
    } else {
        game();
        ImGui::Spacing();
        options();
    }
    return columns;
}

void drawLauncher(LauncherState& state, SDL_Window* window, const LauncherFonts& fonts) {
    takePickedFolder(state);
    const ImGuiStyle& style = ImGui::GetStyle();
    // The sections scroll when the window is too short for them, so the Play row below them always
    // shows. Their height is the one measured in the last frame, and the whole window's space in the
    // first.
    const float room = ImGui::GetContentRegionAvail().y - state.footerHeight - style.ItemSpacing.y;
    const float height = state.sectionsHeight > 0 ? std::min(state.sectionsHeight, std::max(room, ImGui::GetFontSize() * 4)) : 0;
    ImGui::BeginChild("sections", ImVec2(0, height));
    const bool columns = drawSections(state, window, fonts);
    state.sectionsHeight = ImGui::GetCursorPosY() - style.ItemSpacing.y;
    // Measured before the Play row and the message, which both stretch to the window's edge. Side by
    // side, the columns' contents give the width, as a table only spans the width it is given.
    const ImGuiWindow* sections = ImGui::GetCurrentWindow();
    const float scrollbar = sections->ScrollbarY ? style.ScrollbarSize : 0;
    state.contentWidth = columns ? style.WindowPadding.x + state.leftWidth + columnGap() + state.rightWidth + scrollbar
                                 : sections->DC.IdealMaxPos.x - sections->ParentWindow->Pos.x + style.WindowPadding.x + scrollbar;
    ImGui::EndChild();
    const float footerTop = ImGui::GetCursorPosY();
    drawPlay(state, fonts);
    state.footerHeight = ImGui::GetCursorPosY() - footerTop - style.ItemSpacing.y;
    state.contentHeight = state.sectionsHeight + style.ItemSpacing.y + state.footerHeight;
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
