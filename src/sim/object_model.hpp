#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace ts2::sim {

struct SimObject;

// A model name as the scene draws it: a trailing "_cres" left off, and empty for "nullModel" and
// "noModel", which draw nothing.
[[nodiscard]] std::string drawnModelName(std::string model);

// The model an object made during play shows, as the game picks it: the one a script gave it by
// changing its graphic (see SimObject::model), else the row of its model names (string table 0x85,
// read through `modelNames`) that its object data 0 gives, or its definition's field 0x48 when that
// is 0. Row 0 means no model. Empty for none.
[[nodiscard]] std::string createdModelName(const SimObject& object,
                                           const std::function<std::optional<std::string>(std::size_t row)>& modelNames);

// Whether scripts have hidden an object's or a Sim's model: bit 0x01 or 0x20 of object data 0x22, the
// Hidden Flags. Emitters and thought balloons set it as they start, which is why their placeholder
// models never show. A script shows the model again by clearing it. Effects play either way.
[[nodiscard]] bool modelHidden(const SimObject& object);

} // namespace ts2::sim
