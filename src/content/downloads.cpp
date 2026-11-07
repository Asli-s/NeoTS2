#include "content/downloads.hpp"

#include "content/download_index.hpp"
#include "diag/log.hpp"
#include "formats/hash.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

namespace fs = std::filesystem;

namespace ts2::content {

namespace {

// The game has no limits here. These guard against a folder that links back into itself, and are
// far above real mod collections, which reach about 20,000 packages and 8 folders deep.
constexpr int kMostFolderDepth = 32;
constexpr std::size_t kMostPackages = 500000;
// A saved index holds about 32 bytes per resource. A larger file is not read.
constexpr std::uintmax_t kMostIndexBytes = 1u << 30;
constexpr unsigned kMostReadingThreads = 8;

std::vector<IndexedPackage>& chosen() {
    static std::vector<IndexedPackage> packages;
    return packages;
}

// What the content indexes take of the chosen packages, made when the first one asks.
std::shared_ptr<const ResourceIndex::SharedPackages>& sharedPackages() {
    static std::shared_ptr<const ResourceIndex::SharedPackages> packages;
    return packages;
}

std::mutex& sharedLock() {
    static std::mutex lock;
    return lock;
}

void choose(std::vector<IndexedPackage> packages) {
    const std::lock_guard lock(sharedLock());
    chosen() = std::move(packages);
    sharedPackages().reset();
}

// Only ASCII letters are folded. How the game orders other letters is not known yet.
std::string folded(const fs::path& name) {
    const std::u8string text = name.u8string();
    std::string out(text.begin(), text.end());
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

void walk(const fs::path& folder, int depth, std::vector<IndexedPackage>& out) {
    if (depth > kMostFolderDepth) {
        diag::warnOnce("downloads: folders nested more than 32 deep are left out");
        return;
    }
    struct Item {
        std::string name;
        fs::directory_entry entry;
        bool isFolder;
    };
    std::vector<Item> items;
    std::error_code error;
    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        std::error_code kindError;
        const bool isFolder = it->is_directory(kindError);
        if (!kindError) {
            items.push_back({folded(it->path().filename()), *it, isFolder});
        }
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.name < b.name; });
    for (const Item& item : items) {
        if (item.isFolder) {
            const std::string extension = folded(item.entry.path().extension());
            // The game's scan skips these names at any depth.
            const bool skipped = item.name == "thumbnails" || item.name == "characters" || item.name == "lots" || item.name == "lotcatalog";
            if (!skipped && extension != ".files" && extension != ".sgfiles") {
                walk(item.entry.path(), depth + 1, out);
            }
        } else if (folded(item.entry.path().extension()) == ".package") {
            if (out.size() >= kMostPackages) {
                diag::warnOnce("downloads: packages past the first 500,000 are left out");
                return;
            }
            IndexedPackage found;
            found.path = item.entry.path();
            std::error_code statError;
            found.size = item.entry.file_size(statError);
            found.modified = statError ? 0 : static_cast<std::int64_t>(item.entry.last_write_time(statError).time_since_epoch().count());
            out.push_back(std::move(found));
        }
    }
}

void readOne(IndexedPackage& package, std::string& problem) {
    try {
        const dbpf::Package file(package.path);
        package.usable = !file.appleDouble();
        package.entries = file.entries();
    } catch (const std::exception& ex) {
        package.usable = false;
        problem = ex.what();
    }
}

// Reads the packages' indexes on a few threads, as the first read of each file is slow.
void readAll(std::vector<IndexedPackage*>& packages) {
    if (packages.empty()) {
        return;
    }
    std::vector<std::string> problems(packages.size());
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i = next++; i < packages.size(); i = next++) {
            readOne(*packages[i], problems[i]);
        }
    };
    const unsigned count = std::clamp(std::thread::hardware_concurrency(), 1u, kMostReadingThreads);
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < count && i < packages.size(); ++i) {
        threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t i = 0; i < packages.size(); ++i) {
        if (!problems[i].empty()) {
            diag::warning("downloads: a package that can't be read is left out: " + diag::utf8(packages[i]->path.filename()) + ": " + problems[i]);
        }
    }
}

std::vector<IndexedPackage> readSavedIndex(const fs::path& file) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(file, error);
    if (error || size > kMostIndexBytes) {
        return {};
    }
    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        std::ifstream in(file, std::ios::binary);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            return {};
        }
        return readDownloadIndex(bytes);
    } catch (const std::exception& ex) {
        diag::warnOnce(std::string("downloads: the saved index can't be read: ") + ex.what());
        return {};
    }
}

// Written beside its place and moved there, so a closed game never leaves half a file.
void saveIndex(const fs::path& file, const std::vector<IndexedPackage>& packages) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    fs::path part = file;
    part += ".part";
    {
        const std::vector<std::uint8_t> bytes = writeDownloadIndex(packages);
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
            diag::warnOnce("downloads: the index can't be saved, so the packages will be read again next time");
            return;
        }
    }
    fs::rename(part, file, error);
}

} // namespace

std::vector<fs::path> downloadPackages(const fs::path& saveFolder) {
    std::vector<IndexedPackage> found;
    walk(saveFolder, 0, found);
    std::vector<fs::path> paths;
    paths.reserve(found.size());
    for (IndexedPackage& p : found) {
        paths.push_back(std::move(p.path));
    }
    return paths;
}

std::uint32_t packageNameGroup(const fs::path& package) {
    const std::string name = folded(package.filename());
    return hash::groupId(name.substr(0, name.find('.')));
}

void useDownloads(const std::vector<fs::path>& packages) {
    std::vector<IndexedPackage> list(packages.size());
    std::vector<IndexedPackage*> toRead;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        list[i].path = packages[i];
        std::error_code error;
        list[i].size = fs::file_size(packages[i], error);
        toRead.push_back(&list[i]);
    }
    readAll(toRead);
    choose(std::move(list));
}

void loadDownloads(const fs::path& saveFolder, const fs::path& indexFile) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<IndexedPackage> list;
    walk(saveFolder, 0, list);

    std::vector<IndexedPackage> saved;
    if (!list.empty() && !indexFile.empty()) {
        saved = readSavedIndex(indexFile);
    }
    std::unordered_map<std::string, IndexedPackage*> savedByPath;
    savedByPath.reserve(saved.size());
    for (IndexedPackage& p : saved) {
        savedByPath.emplace(folded(p.path), &p);
    }
    std::vector<IndexedPackage*> toRead;
    for (IndexedPackage& p : list) {
        const auto it = savedByPath.find(folded(p.path));
        if (it != savedByPath.end() && it->second->size == p.size && it->second->modified == p.modified) {
            p.usable = it->second->usable;
            p.entries = std::move(it->second->entries);
        } else {
            toRead.push_back(&p);
        }
    }
    readAll(toRead);
    if (!indexFile.empty() && (!toRead.empty() || saved.size() != list.size())) {
        saveIndex(indexFile, list);
    }
    std::size_t leftOut = 0, resources = 0;
    for (const IndexedPackage& p : list) {
        leftOut += p.usable ? 0 : 1;
        resources += p.entries.size();
    }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!list.empty()) {
        diag::info("downloads: " + std::to_string(list.size()) + " packages, " + std::to_string(leftOut) + " left out, " + std::to_string(resources) + " resources, " +
                   std::to_string(toRead.size()) + " read anew, in " + std::to_string(took) + " ms");
    }
    choose(std::move(list));
}

std::size_t addDownloads(ResourceIndex& index) {
    std::shared_ptr<const ResourceIndex::SharedPackages> packages;
    {
        const std::lock_guard lock(sharedLock());
        if (!sharedPackages()) {
            auto made = std::make_shared<ResourceIndex::SharedPackages>();
            for (IndexedPackage& package : chosen()) {
                if (package.usable) {
                    made->add(package.path, package.size, package.entries, packageNameGroup(package.path));
                }
                std::vector<dbpf::IndexEntry>{}.swap(package.entries);
            }
            sharedPackages() = std::move(made);
        }
        packages = sharedPackages();
    }
    const std::size_t added = packages->packageCount();
    if (added > 0) {
        index.addShared(std::move(packages));
    }
    return added;
}

} // namespace ts2::content
