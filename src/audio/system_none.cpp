// Audio without SDL 3: the SDL 1.2 build has no audio backend yet, so sounds are silent and the
// reason is logged once. The interface is the same as system.cpp's.
#include "audio/system.hpp"

#include "diag/log.hpp"

namespace ts2::audio {

struct System::Impl {};

System::System() : impl_(std::make_unique<Impl>()) {}
System::~System() = default;

void System::addInstall(const std::filesystem::path&) {
    ts2::diag::warnOnce("this build has no audio: sounds are silent (audio needs SDL 3)");
}

void System::play(std::string_view, std::int16_t, bool, const Context&, std::optional<std::array<float, 3>>) {}
void System::stopAll() {}
void System::stopSource(std::int16_t) {}
void System::setPaused(bool) {}
void System::pruneSources(const std::function<bool(std::int16_t)>&) {}
void System::updatePositions(const std::function<std::optional<std::array<float, 3>>(std::int16_t)>&) {}
void System::setListener(std::array<float, 3>, std::array<float, 3>) {}

double System::playName(std::string_view) {
    ts2::diag::warnOnce("this build has no audio: sounds are silent (audio needs SDL 3)");
    return 0;
}

bool System::playUi(std::string_view) { return false; }

std::string System::resolve(std::string_view, const Context&) {
    return {};
}

} // namespace ts2::audio
