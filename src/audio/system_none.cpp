// Audio without SDL 3: the SDL 1.2 build has no audio backend yet, so sounds are silent and the
// reason is logged once. The interface is the same as system.cpp's.
#include "audio/system.hpp"

#include "diag/log.hpp"

namespace ts2::audio {

struct System::Impl {};

System::System() : impl_(std::make_unique<Impl>()) {}
System::~System() = default;

void System::addInstall(const std::filesystem::path&) {
    diag::warnOnce("this build has no audio: sounds are silent (audio needs SDL 3)");
}

bool System::ready() const { return false; }
void System::setPerformanceLevel(int) {}
void System::play(const Event&) {}
void System::stopObject(std::int16_t) {}
void System::stopAll() {}
void System::setPaused(bool) {}
void System::setSpeed(int) {}
void System::setListener(const Vec3&, const Vec3&, const Vec3&) {}
void System::updatePositions(const std::function<std::optional<Vec3>(std::int16_t)>&) {}
void System::update() {}
std::uint32_t System::playUi(std::string_view) { return 0; }
std::uint32_t System::playUiId(std::uint32_t) { return 0; }
void System::stop(std::uint32_t) {}

double System::playName(std::string_view) {
    diag::warnOnce("this build has no audio: sounds are silent (audio needs SDL 3)");
    return 0;
}

} // namespace ts2::audio
