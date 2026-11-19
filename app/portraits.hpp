#pragma once

#include "formats/tga.hpp"
#include "render/irenderer.hpp"
#include "scene/animation_library.hpp"
#include "scene/cas_picture.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace app {

// Draws a Sim on the lot as the game draws its portrait: alone, in one of its age's UI poses picked
// at random, from in front of its head, over a transparent background (256 pixels square). Nullopt
// when the Sim has no actor on the lot or can't be drawn. The renderer's shown instances and the
// Sim's joints are changed, and the next frame sets them again.
[[nodiscard]] std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, std::uint32_t sim, std::mt19937& random);
// The same for an actor kept out of the lot's actors, such as a Sim's copy in its everyday outfit.
[[nodiscard]] std::optional<ts2::tga::Pixels> drawPortrait(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                             ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor,
                                             std::mt19937& random);

// Draws a part picture's Sim (see LotLife::addHairPictureSim) alone in its picture pose with the
// picture's camera and lights, over a transparent background (PictureView::kSize pixels square).
// Nullopt when it can't be drawn. The renderer's shown instances are changed, and the caller shows
// the lot's again.
[[nodiscard]] std::optional<ts2::tga::Pixels> drawPartPicture(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                                ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor,
                                                const std::string& pose, const ts2::scene::PictureView& view);

// Draws the appearance editor's Sim view: the Sim view's Sim (see LotLife::addSimViewSim) alone in
// `pose`, turned `turn` radians, with the Sim view camera and the portrait's lights, over a
// transparent background, `width` x `height` pixels. Nullopt when it can't be drawn. The renderer's shown instances are
// changed, and the caller shows the lot's again.
[[nodiscard]] std::optional<ts2::tga::Pixels> drawSimView(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                            ts2::scene::AnimationLibrary& animations, const ts2::scene::Actor& actor,
                                            const std::string& pose, unsigned width, unsigned height, float turn);

// A Sim in the family picture, built at its slot (see LotLife::addFamilyPictureSim).
struct FamilyPictureActor {
    ts2::scene::Actor actor;
    std::string pose;  // the animation's name without "_anim"
};

// Draws a household's family picture: each actor in its pose, with the stage's camera and lights
// and their soft shadow on the floor, over a transparent background (FamilyPictureStage::kSize
// pixels square). Its colour is premultiplied by its alpha. Nullopt when nothing can be drawn.
// The renderer's shown instances are changed, and the caller shows the lot's again.
[[nodiscard]] std::optional<ts2::tga::Pixels> drawFamilyPicture(const ts2::scene::Scene& lot, ts2::render::IRenderer& renderer,
                                                  ts2::scene::AnimationLibrary& animations, const std::vector<FamilyPictureActor>& actors,
                                                  const ts2::scene::FamilyPictureStage& stage);

} // namespace app
