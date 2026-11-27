#pragma once

#include "content/lot.hpp"
#include "formats/dbpf_writer.hpp"

namespace ts2::content {

// Puts the lot's build data back into its package as a save writes it: every wall graph, the wall
// coverings (wall layer 4), the wall and floor pattern maps, the fence posts and the cells of the
// grids. Only resources the package holds already are written, and a grid only when its size is
// unchanged, but a wall graph the lot holds and the package doesn't (a layer a shipped lot lacks) is
// added. The coverings name walls by their ids in the wall graphs, so when a record can't be
// made, or a wall graph the wall manager keeps wasn't read, none is written. Returns how many were.
int putBuildRecords(dbpf::PackageImage& package, const Lot& lot);

} // namespace ts2::content
