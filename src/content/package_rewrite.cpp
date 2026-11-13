#include "content/package_rewrite.hpp"

#include "formats/dbpf_writer.hpp"
#include "formats/resource_types.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

namespace ts2::content {

namespace fs = std::filesystem;

namespace {

// The header's words a rewrite keeps, as stored. The reader's Header can't give the index minor
// version as stored: it reads a version 1.0 file's as 1, as the game does.
struct KeptWords {
    std::uint32_t minorVersion = 0;       // at 0x08
    std::uint32_t indexMinorVersion = 0;  // at 0x3C
    std::uint32_t word40 = 0;             // at 0x40

    bool operator==(const KeptWords&) const = default;
};

KeptWords keptWords(const fs::path& file) {
    std::array<std::uint8_t, 0x44> bytes{};
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("can't read the package's header");
    }
    const auto at = [&](std::size_t o) {
        return static_cast<std::uint32_t>(bytes[o]) | static_cast<std::uint32_t>(bytes[o + 1]) << 8 |
               static_cast<std::uint32_t>(bytes[o + 2]) << 16 | static_cast<std::uint32_t>(bytes[o + 3]) << 24;
    };
    return {at(0x08), at(0x3C), at(0x40)};
}

bool isDirectory(const dbpf::IndexEntry& entry) { return entry.key.type == types::kDirectory; }

const ReplacedResource* changeFor(std::span<const ReplacedResource> changes, const dbpf::ResourceKey& key) {
    const auto found = std::ranges::find_if(changes, [&](const ReplacedResource& c) { return c.key == key; });
    return found != changes.end() ? &*found : nullptr;
}

// What one resource of the new package must be: the old one's stored bytes, or a change.
struct Expected {
    dbpf::ResourceKey key;
    const ReplacedResource* change = nullptr;
    std::vector<std::uint8_t> stored;  // when kept
    bool compressed = false;
    std::uint32_t uncompressedSize = 0;
};

[[noreturn]] void mismatch(const fs::path& written, const std::string& why) {
    std::error_code error;
    fs::remove(written, error);
    throw std::runtime_error("the rewritten package doesn't match the old one (" + why + "), so it was not saved");
}

} // namespace

RewriteReport rewritePackage(const fs::path& file, std::span<const ReplacedResource> changes) {
    for (std::size_t i = 0; i < changes.size(); ++i) {
        if (isDirectory({changes[i].key}) || changeFor(changes.subspan(0, i), changes[i].key) != nullptr) {
            throw std::invalid_argument("a change names the compression directory, or a key twice");
        }
    }
    RewriteReport report;
    const fs::path written = fs::path(file) += ".neots2-new";
    std::vector<Expected> expected;
    dbpf::Header oldHeader;
    KeptWords kept;
    {
        // The old package is closed before anything replaces it (Windows won't replace an open file).
        dbpf::Package old(file);
        oldHeader = old.header();
        if (oldHeader.majorVersion != 1 || oldHeader.minorVersion > 2 || oldHeader.indexMajorVersion != 7 ||
            oldHeader.indexMinorVersion > 2) {
            throw std::runtime_error("the package's version isn't one NeoTS2 writes, so it was left alone");
        }
        kept = keptWords(file);
        if (kept.minorVersion != oldHeader.minorVersion || kept.indexMinorVersion > 2) {
            throw std::runtime_error("the package's header isn't one NeoTS2 writes, so it was left alone");
        }
        std::vector<bool> used(changes.size(), false);
        std::ofstream out(written, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("can't write " + written.string());
        }
        try {
            dbpf::PackageWriter writer(out);
            writer.keepVersions(kept.minorVersion, kept.indexMinorVersion);
            for (const dbpf::IndexEntry& entry : old.entries()) {
                // The writer makes the compression directory again from the resources.
                if (isDirectory(entry)) {
                    continue;
                }
                Expected e;
                e.key = entry.key;
                e.change = changeFor(changes, entry.key);
                if (e.change != nullptr) {
                    used[static_cast<std::size_t>(e.change - changes.data())] = true;
                    writer.add(entry.key, e.change->bytes, false, 0);
                    ++report.replaced;
                } else {
                    e.stored = old.readRaw(entry);
                    e.compressed = entry.compressed;
                    e.uncompressedSize = entry.uncompressedSize;
                    writer.add(entry.key, e.stored, e.compressed, e.uncompressedSize);
                    ++report.copied;
                }
                expected.push_back(std::move(e));
            }
            for (std::size_t i = 0; i < changes.size(); ++i) {
                if (!used[i]) {
                    writer.add(changes[i].key, changes[i].bytes, false, 0);
                    expected.push_back({changes[i].key, &changes[i], {}, false, 0});
                    ++report.added;
                }
            }
            writer.finish(kept.word40);
        } catch (...) {
            out.close();
            std::error_code error;
            fs::remove(written, error);
            throw;
        }
        out.close();
        if (!out) {
            mismatch(written, "the file could not be finished");
        }
    }

    // Read the new package back as the game and NeoTS2 will, and check every resource.
    {
        std::optional<dbpf::Package> check;
        try {
            check.emplace(written);
        } catch (const std::exception& e) {
            mismatch(written, std::string("it can't be read: ") + e.what());
        }
        const dbpf::Header& gotHeader = check->header();
        if (gotHeader.majorVersion != oldHeader.majorVersion || gotHeader.minorVersion != oldHeader.minorVersion ||
            gotHeader.indexMajorVersion != oldHeader.indexMajorVersion || gotHeader.indexMinorVersion != oldHeader.indexMinorVersion ||
            keptWords(written) != kept) {
            mismatch(written, "the header's versions changed");
        }
        std::vector<dbpf::IndexEntry> entries;
        std::ranges::copy_if(check->entries(), std::back_inserter(entries), [](const dbpf::IndexEntry& e) { return !isDirectory(e); });
        if (entries.size() != expected.size()) {
            mismatch(written, "a different number of resources");
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const dbpf::IndexEntry& got = entries[i];
            const Expected& want = expected[i];
            if (!(got.key == want.key)) {
                mismatch(written, "a resource key changed");
            }
            if (want.change != nullptr) {
                if (got.compressed || check->read(got) != want.change->bytes) {
                    mismatch(written, "a changed resource reads back differently");
                }
            } else if (got.compressed != want.compressed || got.uncompressedSize != want.uncompressedSize ||
                       check->readRaw(got) != want.stored) {
                mismatch(written, "a kept resource reads back differently");
            }
        }
        check->close();
    }

    // Keep the file as the game last wrote it, once, then swap the new one in.
    report.backup = fs::path(file) += ".neots2bak";
    std::error_code error;
    if (!fs::exists(report.backup, error)) {
        if (!fs::copy_file(file, report.backup, error) || error) {
            fs::remove(written, error);
            throw std::runtime_error("can't make the backup " + report.backup.string() + ", so nothing was saved");
        }
        report.madeBackup = true;
    }
    fs::rename(written, file, error);
    if (error) {
        std::error_code ignored;
        fs::remove(written, ignored);
        throw std::runtime_error("can't replace " + file.string() + ": " + error.message());
    }
    return report;
}

} // namespace ts2::content
