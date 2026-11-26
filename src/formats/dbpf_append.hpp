#pragma once

#include "formats/dbpf_writer.hpp"

#include <filesystem>
#include <span>

namespace ts2::dbpf {

// Adds resources to a package file in place, as the game adds the pictures it draws to the user
// folder's Thumbnails caches. Their data, a new index and hole list go at the end of the file and the
// header is written last, so the file reads as before until then. An added key the package already
// has replaces that resource, and the space the old one and the old lists took becomes holes.
// Throws when the file is not a DBPF 1 package with index 7.1 or 7.2 or looks broken, another program
// is writing it, a key is added twice or needs an instance-high field the index lacks, or the file
// can't be written.
void appendToPackage(const std::filesystem::path& file, std::span<const StoredResource> added);

} // namespace ts2::dbpf
