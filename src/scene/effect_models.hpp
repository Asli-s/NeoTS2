#pragma once

#include "formats/effects.hpp"
#include "scene/builder.hpp"
#include "scene/effect_player.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::scene {

// The models effects show (model blocks, as the neighborhood's sailboats and buoys), drawn by
// instances kept in the scene for them: as many of each model as the effects can show at once.
class EffectModels {
public:
    // Keeps instances for the models `effect` of `file` can show, hidden until placed, up to a limit
    // for each model in the scene. Returns how many it added. Instances are mirrored in the
    // neighborhood's water, as the game mirrors effects.
    std::size_t reserve(SceneBuilder& builder, const effects::File& file, const std::string& effect);

    // One instance as it should be drawn now.
    struct Placed {
        std::size_t instance = 0;
        bool shown = false;
        Transform transform;
        std::optional<std::array<float, 4>> shapeColor;  // none for a model drawn as it is
    };

    // Where every kept instance is drawn for the models `player` shows now. Instances left over are
    // hidden, all of them without a player. A model shown more often than it has instances is drawn
    // that many times only.
    void place(const EffectPlayer* player, std::vector<Placed>& out) const;

    bool empty() const { return instances_.empty(); }

private:
    // Lower-case model name -> its instances.
    std::map<std::string, std::vector<std::size_t>> instances_;
};

// The turn of `place` as the game gives a model: its rotation made a unit quaternion (x, y, z, w),
// so a scale in it is dropped.
std::array<float, 4> modelTurn(const EmitterPlace& place);

} // namespace ts2::scene
