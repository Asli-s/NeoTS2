#pragma once

#include "formats/simi.hpp"

#include <cstdint>
#include <span>

namespace ts2::sim {

class LotSimulation;

// The lot's simulator record (SIMI) as the game saves it from a running lot: the simulator's 60
// globals and the state its ticks draw random numbers from, as they are now. The rest of the record
// (values whose meaning is not known yet and the random state used outside the ticks) is kept from
// `loaded`, the record the lot was loaded with.
simi::Record simulatorRecord(const simi::Record& loaded, std::span<const std::int16_t> globals, std::uint32_t tickRandomState);
simi::Record simulatorRecord(const simi::Record& loaded, LotSimulation& simulation);

} // namespace ts2::sim
