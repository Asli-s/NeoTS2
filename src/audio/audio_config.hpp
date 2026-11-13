#pragma once

// The sound packages a pack's TSAudioPacked*.ini (TSData/Sys) names, in the order it names them.
// The game keeps the first package's copy of a sound that several packages hold, so this order
// decides which wins.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ts2::audio {

// The [PackedDBSegments] entries, each a segment id, a class and a package's path after
// %DataDirectory%: each package's path below the pack's TSData folder, lowercase with forward
// slashes, in file order. Comment lines are skipped, as are paths that name the %Language% folder.
// At most kMaxSegments are read.
constexpr std::size_t kMaxSegments = 1024;
std::vector<std::string> packedSegments(std::string_view ini);

} // namespace ts2::audio
