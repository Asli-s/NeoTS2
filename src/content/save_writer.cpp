#include "content/save_writer.hpp"

#include "diag/log.hpp"
#include "formats/dbpf_writer.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace ts2::content {

namespace {

constexpr std::string_view kJournalFirstLine = "NeoTS2 save journal 1";
constexpr std::string_view kJournalLastLine = "end";
// Added once every target holds its new package: the save is complete even if the journal stays.
constexpr std::string_view kJournalDoneLine = "done";
// A journal lists the packages of one save. Far more than any save holds, to refuse a broken one.
constexpr std::size_t kMaxJournalBytes = 1 << 20;

fs::path withSuffix(const fs::path& path, std::u8string_view suffix) {
    fs::path out = path;
    out += fs::path(std::u8string(suffix));
    return out;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool present(const fs::path& path) {
    std::error_code error;
    return fs::exists(fs::symlink_status(path, error));
}

bool isPlainFile(const fs::path& path) {
    std::error_code error;
    return fs::is_regular_file(fs::symlink_status(path, error));
}

std::vector<std::uint8_t> readWhole(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw SaveError("could not open " + path.filename().string() + " to read it");
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad()) {
        throw SaveError("could not read " + path.filename().string());
    }
    return bytes;
}

bool startsAsPackage(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[4] = {};
    return in.read(magic, 4) && magic[0] == 'D' && magic[1] == 'B' && magic[2] == 'P' && magic[3] == 'F';
}

// Makes `path`, which must not exist yet, holding `bytes`, and waits until the disk has them. A file
// left half written is removed.
void writeNewFile(const fs::path& path, std::span<const std::uint8_t> bytes) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw SaveError("could not make " + path.filename().string() + " (error " + std::to_string(GetLastError()) + ")");
    }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1u << 30));
        DWORD written = 0;
        ok = WriteFile(file, bytes.data() + done, chunk, &written, nullptr) && written == chunk;
        done += written;
    }
    ok = ok && FlushFileBuffers(file);
    CloseHandle(file);
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (file < 0) {
        throw SaveError("could not make " + path.filename().string() + " (error " + std::to_string(errno) + ")");
    }
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
        const ssize_t written = ::write(file, bytes.data() + done, bytes.size() - done);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        ok = written > 0;
        if (ok) {
            done += static_cast<std::size_t>(written);
        }
    }
    ok = ok && ::fsync(file) == 0;
    ok = ::close(file) == 0 && ok;
#endif
    if (!ok) {
        std::error_code error;
        fs::remove(path, error);
        throw SaveError("could not write " + path.filename().string() + " to the disk");
    }
}

// Adds `text` to the end of an existing file and waits until the disk has it.
bool appendToFile(const fs::path& path, std::string_view text) {
#ifdef _WIN32
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size();
    ok = FlushFileBuffers(file) && ok;
    CloseHandle(file);
    return ok;
#else
    const int file = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0) {
        return false;
    }
    bool ok = ::write(file, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = ::fsync(file) == 0 && ok;
    ok = ::close(file) == 0 && ok;
    return ok;
#endif
}

// Moves a file within its folder. Never replaces a file already at `to`.
bool moveNoReplace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
    // A hard link refuses an existing name in one step. Disks without links (FAT) fall back to a
    // look first, which is safe while NeoTS2 holds the save folder's lock.
    if (::link(from.c_str(), to.c_str()) == 0) {
        return ::unlink(from.c_str()) == 0;
    }
    if (errno == EEXIST || present(to)) {
        return false;
    }
    return std::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// Waits until the disk has a folder's list of names, so moves in it survive a power cut.
void syncFolder([[maybe_unused]] const fs::path& folder) {
#ifndef _WIN32
    const int handle = ::open(folder.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (handle >= 0) {
        ::fsync(handle);
        ::close(handle);
    }
#endif
}

bool removeFile(const fs::path& path) {
    std::error_code error;
    fs::remove(path, error);
    return !error && !present(path);
}

struct JournalEntry {
    SaveTarget kind = SaveTarget::Replace;
    fs::path target;
};

std::string journalText(const fs::path& neighborhoods, const std::vector<JournalEntry>& entries) {
    std::string text(kJournalFirstLine);
    text += '\n';
    for (const JournalEntry& entry : entries) {
        const std::u8string relative = entry.target.lexically_relative(neighborhoods).generic_u8string();
        text += entry.kind == SaveTarget::Replace ? "replace " : "create ";
        text.append(reinterpret_cast<const char*>(relative.data()), relative.size());
        text += '\n';
    }
    text += kJournalLastLine;
    text += '\n';
    return text;
}

bool nameIsPackage(const fs::path& path) {
    const std::string name = lower(path.filename().string());
    return name.size() > 8 && name.ends_with(".package") && name[0] != '.';
}

enum class JournalState {
    Broken,    // not a journal NeoTS2 wrote, or naming files a save could not have written
    CutShort,  // its "end" line is missing, so no move had started
    Listed,    // whole: the moves may have started and the save did not finish
    Done,      // the save finished
};

// Reads a journal. The moves start only once the whole of it, "end" line and all, is on the disk.
// A journal cut short by a crash therefore means nothing was moved yet.
JournalState readJournal(const fs::path& journal, const fs::path& neighborhoods, std::vector<JournalEntry>& entries) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(journal, error);
    if (error || size > kMaxJournalBytes) {
        return JournalState::Broken;
    }
    std::ifstream in(journal, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string firstLine = std::string(kJournalFirstLine) + "\n";
    if (!text.starts_with(firstLine)) {
        // Only a start of the first line, from a crash while it was written, is still ours.
        return firstLine.starts_with(text) ? JournalState::CutShort : JournalState::Broken;
    }
    std::vector<std::string> lines;
    std::size_t at = firstLine.size();
    while (at < text.size()) {
        const std::size_t next = text.find('\n', at);
        if (next == std::string::npos) {
            break;  // a last line without its end of line was cut short
        }
        lines.push_back(text.substr(at, next - at));
        at = next + 1;
    }
    const auto endLine = std::find(lines.begin(), lines.end(), std::string(kJournalLastLine));
    if (endLine == lines.end()) {
        return JournalState::CutShort;
    }
    for (auto line = lines.begin(); line != endLine; ++line) {
        JournalEntry entry;
        std::string rest;
        if (line->starts_with("replace ")) {
            rest = line->substr(8);
        } else if (line->starts_with("create ")) {
            entry.kind = SaveTarget::Create;
            rest = line->substr(7);
        } else {
            return JournalState::Broken;
        }
        const fs::path relative(std::u8string(reinterpret_cast<const char8_t*>(rest.data()), rest.size()));
        if (rest.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) {
            return JournalState::Broken;
        }
        for (const fs::path& part : relative) {
            if (part == ".." || part == ".") {
                return JournalState::Broken;
            }
        }
        entry.target = neighborhoods / relative;
        if (!nameIsPackage(entry.target) || !paths::isWithin(entry.target.parent_path(), neighborhoods)) {
            return JournalState::Broken;
        }
        entries.push_back(std::move(entry));
    }
    const std::size_t after = static_cast<std::size_t>(endLine - lines.begin()) + 1;
    if (after == lines.size() && at == text.size()) {
        return JournalState::Listed;
    }
    if (after + 1 == lines.size() && lines[after] == kJournalDoneLine && at == text.size()) {
        return JournalState::Done;
    }
    // Text after the "end" line that is not a whole "done" line. A "done" cut short means the save
    // was not marked finished.
    if (after == lines.size() && std::string(kJournalDoneLine).starts_with(text.substr(at))) {
        return JournalState::Listed;
    }
    return JournalState::Broken;
}

// Puts one target back as it was before its save: the old package back in its place, the new one
// gone. False when a file could not be moved or removed.
bool restoreTarget(const JournalEntry& entry, int& restored, int& cleaned) {
    const fs::path fresh = newPackagePath(entry.target);
    const fs::path old = oldPackagePath(entry.target);
    bool ok = true;
    if (entry.kind == SaveTarget::Replace) {
        if (present(old)) {
            // The old package was moved aside, so the file at the target, if any, is the new one.
            if (present(entry.target) && !removeFile(entry.target)) {
                return false;
            }
            if (!moveNoReplace(old, entry.target)) {
                return false;
            }
            ++restored;
        }
    } else if (present(entry.target) && !present(fresh)) {
        // A new package was moved into place. Before the save nothing was there.
        ok = removeFile(entry.target);
        restored += ok ? 1 : 0;
    }
    if (present(fresh)) {
        ok = removeFile(fresh) && ok;
        cleaned += ok ? 1 : 0;
    }
    syncFolder(entry.target.parent_path());
    return ok;
}

// Removes work files a finished save left beside its targets. The targets hold their new,
// checked packages, so the old ones are no longer needed.
int removeLeftovers(const fs::path& neighborhoods) {
    int removed = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator it(neighborhoods, fs::directory_options::skip_permission_denied, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_symlink(error)) {
            if (it->is_directory(error)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(error)) {
            continue;
        }
        const std::string name = lower(it->path().filename().string());
        const bool work = name.ends_with(lower(std::string(reinterpret_cast<const char*>(paths::kSaveNewSuffix.data()), paths::kSaveNewSuffix.size()))) ||
                          name.ends_with(lower(std::string(reinterpret_cast<const char*>(paths::kSaveOldSuffix.data()), paths::kSaveOldSuffix.size())));
        if (work && removeFile(it->path())) {
            ++removed;
        }
    }
    return removed;
}

} // namespace

fs::path newPackagePath(const fs::path& target) {
    return withSuffix(target, paths::kSaveNewSuffix);
}

fs::path oldPackagePath(const fs::path& target) {
    return withSuffix(target, paths::kSaveOldSuffix);
}

fs::path saveJournalPath(const fs::path& saveFolder) {
    return withSuffix(paths::saveFolderPath(saveFolder), paths::kSaveJournalSuffix);
}

SaveRecovery recoverInterruptedSave(const paths::SaveFolderLock& lock) {
    SaveRecovery out;
    if (!lock.held()) {
        out.problem = "NeoTS2 does not hold the save folder.";
        return out;
    }
    const fs::path neighborhoods = lock.folder() / "Neighborhoods";
    std::error_code error;
    if (!fs::is_directory(neighborhoods, error)) {
        return out;
    }
    const fs::path journal = saveJournalPath(lock.folder());
    if (present(journal)) {
        std::vector<JournalEntry> entries;
        const JournalState state = isPlainFile(journal) ? readJournal(journal, neighborhoods, entries) : JournalState::Broken;
        if (state == JournalState::Broken) {
            out.problem = "A save that stopped part way left a journal NeoTS2 can't read, so nothing was changed.";
            diag::error("save recovery: the journal " + journal.filename().string() + " can't be read, nothing changed");
            return out;
        }
        if (state == JournalState::Listed) {
            bool ok = true;
            for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
                if (!restoreTarget(*it, out.restored, out.cleaned)) {
                    ok = false;
                    diag::error("save recovery: could not put back " + it->target.filename().string());
                }
            }
            if (!ok) {
                out.problem = "A save that stopped part way could not be fully undone. Its files were left as they are.";
                return out;
            }
            diag::warning("save recovery: a save that stopped part way was undone, " + std::to_string(out.restored) + " packages put back");
        }
        // Cut short, nothing had moved. Done, every target holds its new package. Either way only
        // work files are left.
        if (!removeFile(journal)) {
            out.problem = "The journal of the last save could not be removed.";
            return out;
        }
        syncFolder(journal.parent_path());
    }
    out.cleaned += removeLeftovers(neighborhoods);
    if (out.cleaned > 0) {
        diag::info("save recovery: removed " + std::to_string(out.cleaned) + " leftover save work files");
    }
    return out;
}

SaveWriter::SaveWriter(const paths::SaveFolderLock& lock) : lock_(lock), neighborhoods_(lock.folder() / "Neighborhoods") {}

void SaveWriter::add(const fs::path& target, std::vector<std::uint8_t> bytes, SaveTarget kind) {
    if (!lock_.held()) {
        throw SaveError("NeoTS2 does not hold the save folder");
    }
    const fs::path named = paths::saveFolderPath(target);
    const std::string shown = named.filename().string();
    std::error_code error;
    if (named.empty() || !nameIsPackage(named) || paths::isWorkName(named.filename())) {
        throw SaveError("not a package a save can write: " + shown);
    }
    if (!fs::is_directory(neighborhoods_, error) || !fs::is_directory(named.parent_path(), error) ||
        !paths::isWithin(named.parent_path(), neighborhoods_)) {
        throw SaveError(shown + " is not inside the save folder's Neighborhoods");
    }
    if (kind == SaveTarget::Replace) {
        if (!isPlainFile(named) || !startsAsPackage(named)) {
            throw SaveError(shown + " is not an existing package to replace");
        }
    } else if (present(named)) {
        throw SaveError(shown + " already exists");
    }
    for (const Item& item : items_) {
        if (lower(item.target.filename().string()) == lower(shown) && paths::isWithin(item.target.parent_path(), named.parent_path()) &&
            paths::isWithin(named.parent_path(), item.target.parent_path())) {
            throw SaveError(shown + " is in this save twice");
        }
    }
    try {
        dbpf::verifyImage(bytes, dbpf::readImage(bytes));
    } catch (const std::exception& e) {
        throw SaveError(shown + " failed its check: " + e.what());
    }
    items_.push_back({named, std::move(bytes), kind});
}

void SaveWriter::move(const fs::path& from, const fs::path& to) {
    if (++moves_ == failAtMove_ || !moveNoReplace(from, to)) {
        throw SaveError("could not move " + from.filename().string() + " to " + to.filename().string());
    }
}

// Undoes the swaps of the first `count` items, last first, and removes the new packages written
// beside the targets.
void SaveWriter::undo(std::size_t count) {
    int restored = 0;
    int cleaned = 0;
    bool ok = true;
    for (std::size_t i = items_.size(); i-- > 0;) {
        const JournalEntry entry{items_[i].kind, items_[i].target};
        if (i < count) {
            ok = restoreTarget(entry, restored, cleaned) && ok;
        } else if (present(newPackagePath(entry.target))) {
            ok = removeFile(newPackagePath(entry.target)) && ok;
        }
    }
    const fs::path journal = saveJournalPath(lock_.folder());
    if (ok && present(journal)) {
        ok = removeFile(journal);
    }
    if (!ok) {
        // The journal stays, so the next start finishes the undo.
        diag::error("save: undoing a failed save left files behind, the next start puts them right");
    }
}

void SaveWriter::commit() {
    if (items_.empty()) {
        return;
    }
    const SaveRecovery recovery = recoverInterruptedSave(lock_);
    if (!recovery.problem.empty()) {
        throw SaveError(recovery.problem);
    }
    const fs::path journal = saveJournalPath(lock_.folder());
    for (const Item& item : items_) {
        const bool there = present(item.target);
        if ((item.kind == SaveTarget::Replace) != there || present(newPackagePath(item.target)) || present(oldPackagePath(item.target))) {
            throw SaveError(item.target.filename().string() + " changed since the save began");
        }
    }

    // The new packages beside their targets, each read back from the disk.
    std::size_t written = 0;
    try {
        for (const Item& item : items_) {
            const fs::path fresh = newPackagePath(item.target);
            writeNewFile(fresh, item.bytes);
            ++written;
            if (readWhole(fresh) != item.bytes) {
                throw SaveError(fresh.filename().string() + " reads back differently from what was written");
            }
        }
        std::vector<JournalEntry> entries;
        for (const Item& item : items_) {
            entries.push_back({item.kind, item.target});
        }
        const std::string text = journalText(neighborhoods_, entries);
        writeNewFile(journal, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        syncFolder(journal.parent_path());
    } catch (const SaveError&) {
        for (std::size_t i = 0; i < written; ++i) {
            removeFile(newPackagePath(items_[i].target));
        }
        throw;
    }

    // The swaps. From the first move on, a failure undoes them all.
    std::size_t swapped = 0;
    try {
        for (const Item& item : items_) {
            ++swapped;
            if (item.kind == SaveTarget::Replace) {
                move(item.target, oldPackagePath(item.target));
            }
            move(newPackagePath(item.target), item.target);
            syncFolder(item.target.parent_path());
        }
        for (const Item& item : items_) {
            if (readWhole(item.target) != item.bytes) {
                throw SaveError(item.target.filename().string() + " reads back differently after the swap");
            }
        }
    } catch (const std::exception& e) {
        diag::error(std::string("save failed, undoing it: ") + e.what());
        undo(swapped);
        throw SaveError(e.what());
    }

    // The save is complete once the journal says so or is gone. The old packages can go after that.
    // When neither can be done, the save is undone now, as the next start would undo it.
    const std::string doneLine = std::string(kJournalDoneLine) + "\n";
    const bool marked = appendToFile(journal, doneLine);
    if (!removeFile(journal) && !marked) {
        diag::error("save: the journal could not be marked done or removed, undoing the save");
        undo(items_.size());
        throw SaveError("the save could not be finished, so it was undone");
    }
    syncFolder(journal.parent_path());
    for (const Item& item : items_) {
        if (item.kind == SaveTarget::Replace && !removeFile(oldPackagePath(item.target))) {
            diag::warning("save: an old package could not be removed: " + oldPackagePath(item.target).filename().string());
        }
    }
    diag::info("saved " + std::to_string(items_.size()) + " packages");
    items_.clear();
}

} // namespace ts2::content
