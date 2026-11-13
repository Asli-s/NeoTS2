#include "sim/lot_save.hpp"

#include "sim/lot_simulation.hpp"

#include <algorithm>

namespace ts2::sim {

simi::Record simulatorRecord(const simi::Record& loaded, std::span<const std::int16_t> globals, std::uint32_t tickRandomState) {
    simi::Record record = loaded;
    // The record saves the first 60 globals.
    std::copy_n(globals.begin(), std::min(globals.size(), record.globals.size()), record.globals.begin());
    record.tickRandomState = tickRandomState;
    return record;
}

simi::Record simulatorRecord(const simi::Record& loaded, LotSimulation& simulation) {
    return simulatorRecord(loaded, simulation.globals(), simulation.vm().rng.state());
}

} // namespace ts2::sim
