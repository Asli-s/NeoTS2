#pragma once

#include "scene/lot_shadow.hpp"
#include "scene/scene.hpp"

#include <array>
#include <memory>
#include <vector>

namespace ts2::scene::objectsun {

// How much of the sun reaches a point under the buildings' shadow: all of it off the shadow's grid
// or above the shadow, fading out over `zone` below it.
float at(const lotshadow::Grid& shadow, float zone, const std::array<float, 3>& point);

// The points an instance is tested at, one per primitive of its mesh, by its SunTest.
std::vector<std::array<float, 3>> testPoints(const Scene& scene, const Instance& instance);

// Sets every lot object's Instance::sun from its test points.
void shade(Scene& scene, const lotshadow::Grid& shadow, float zone);

// The same for one object wherever it stands (see Scene::thingSun), for objects created or moved
// after the lot was lit.
ThingSun thingSun(std::shared_ptr<const lotshadow::Grid> shadow, float zone);

} // namespace ts2::scene::objectsun
