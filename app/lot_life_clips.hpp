// How a Sim's or an object's animations become its actor's clips, with their fades.
#pragma once

#include "scene/animation_library.hpp"
#include "scene/scene.hpp"
#include "sim/lot_simulation.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace app {

// Whether `previous` is an Animate Sim clip still playing as `latest` starts. One that has played
// out is gone, as the game drops each Animate Sim clip at its end.
bool stillPlaying(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest);

// Whether `latest` crossfades `previous`, still fading, as the game does between two Animate Sim
// clips that move the body (both have a body target): the latest fades in, and the previous one may
// fade out and has a fade's length or less left. The previous one then keeps the weight it has as
// the latest starts and falls from there to none over a fade, whatever its own fade had left.
bool crossfades(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest, ts2::scene::AnimationLibrary& animations);

// When the animation before `latest` is gone: one still fading at its end, or a fade after the
// latest starts when the latest crossfades it. Others are gone as the latest starts, as the game
// fades an older clip only by its own fades or that crossfade.
double previousGoneMs(const ts2::sim::PlayingAnimation& previous, const ts2::sim::PlayingAnimation& latest,
                      ts2::scene::AnimationLibrary& animations);

// Where the Sim stood as a clip started, by the clip's name and start, if known.
using StartedAt = std::function<std::optional<ts2::scene::Transform>(const std::string& name, double startMs)>;

// Sets the actor's clips to what an object plays, over `previous`: its animation and overlays, or while
// a Sim walks, the walk's clips at the moments and weights its distance gives (`walk`). `walkFade` rises
// from 0 to 1 from the walk's start. A locomotion clip turns from `frameHeading` to its base heading.
// Any other clip with a root joint plays where the Sim stood as it started (`startedAt`), as the game
// places clips. A walk that has handed back (`endedWalk`) plays out under what follows at `endedPlace`.
void playClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, const std::optional<ts2::sim::PlayingAnimation>& own,
               const std::optional<ts2::sim::PlayingAnimation>& previous, float frameHeading, ts2::scene::AnimationLibrary& animations,
               const std::vector<ts2::sim::WalkPose>* walk = nullptr, float walkFade = 1, float walkStartHeading = 0,
               const std::vector<ts2::sim::WalkPose>* endedWalk = nullptr, float endedHeading = 0, const StartedAt& startedAt = {},
               const std::optional<ts2::scene::Transform>& endedPlace = std::nullopt);

// A stopped walk that settled at `settledMs` (see SimObject::walkSettledMs): the pose the Sim
// was in fades out, held where it stood then (`at`), while the walk pose fades in and out.
void addSettleClips(ts2::scene::Actor& actor, const ts2::sim::SimObject& object, double settledMs, const std::optional<ts2::scene::Transform>& at,
                    ts2::scene::AnimationLibrary& animations, const ts2::sim::LocoClipLookup& clips);

} // namespace app
