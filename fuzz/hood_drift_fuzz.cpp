#include "formats/effects.hpp"
#include "scene/hood_drift.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <span>

// Reads an effects file, then plays its neighborhood drift for a while with a few interrupts. A
// goal or sway that isn't a finite number is a bug: the camera would fly off.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    try {
        const auto file = ts2::effects::parse(std::span<const std::uint8_t>(data, size));
        auto drift = ts2::scene::HoodDrift::load(file, 1);
        if (!drift) {
            return 0;
        }
        for (int i = 0; i < 3000; ++i) {
            if (i % 1000 == 999) {
                drift->interrupt();
            }
            const auto step = drift->step({100, 200, 312.5f}, 1.0f / 120);
            if ((step.sway && !std::isfinite(*step.sway)) ||
                (step.goal && !(std::isfinite((*step.goal)[0]) && std::isfinite((*step.goal)[1]) && std::isfinite((*step.goal)[2])))) {
                std::abort();
            }
        }
    } catch (const std::exception&) {
    }
    return 0;
}
