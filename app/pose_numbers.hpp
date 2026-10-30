#pragma once

#include <filesystem>

namespace ts2::scene {
struct Scene;
}

namespace app {

class LotLife;

// Runs a lot for `seconds` in steps of a tenth of a second, as a shot does, and writes what it shows
// once a second as plain numbers: each Sim's place, clips and named bones, and each animated or
// moved object's place and joints. Places are in metres to the millimetre. Nothing is drawn, so two
// runs can be compared line by line much faster than pictures. Returns false when the file can't be
// written.
bool writePoseNumbers(LotLife& life, ts2::scene::Scene& scene, double seconds, const std::filesystem::path& file);

} // namespace app
