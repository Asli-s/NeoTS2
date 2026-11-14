#pragma once

#include "formats/dbpf.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ts2::content {

// A resource a rewrite puts in the package in place of the one with its key, or adds when the
// package has none. It is stored uncompressed (NeoTS2 has no QFS compressor yet).
struct ReplacedResource {
    dbpf::ResourceKey key;
    std::vector<std::uint8_t> bytes;
};

struct RewriteReport {
    std::size_t copied = 0;    // resources kept exactly as they were stored
    std::size_t replaced = 0;  // resources written from `changes`
    std::size_t added = 0;     // keys of `changes` the package didn't have
    std::size_t removed = 0;   // resources left out, as `removed` asked
    std::filesystem::path backup;
    bool madeBackup = false;   // false when the backup from an earlier rewrite was kept
};

// Saving's first step: writes the package at `file` again, every resource as it was stored (still
// compressed where it was) in the same order, with `changes` in place of the resources with their
// keys and without those `removed` names. The header keeps its word at 0x40 and an index with instance-high fields keeps them.
//
// It never leaves `file` half written: the new package goes to "<file>.neots2-new", is read back
// with NeoTS2's own reader and checked resource by resource against the old one, and only then
// replaces it. Before the first rewrite of a file the original is kept as "<file>.neots2bak"; later
// rewrites leave that backup alone, so it stays the file as the game last wrote it.
//
// Throws std::runtime_error (and leaves `file` as it was) when the package can't be read, the new
// one can't be written, or the check finds any difference it shouldn't have.
RewriteReport rewritePackage(const std::filesystem::path& file, std::span<const ReplacedResource> changes = {},
                             std::span<const dbpf::ResourceKey> removed = {});

} // namespace ts2::content
