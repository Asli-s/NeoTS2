#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace ts2::sim {

struct SimObject;

// The model an object made during play shows, as the game picks it: the row of its model names
// (string table 0x85, read through `modelNames`) that its object data 0 gives, or its definition's
// field 0x48 when that is 0. Row 0 means no model. Empty for none. A trailing "_cres" is left off.
[[nodiscard]] std::string createdModelName(const SimObject& object,
                                           const std::function<std::optional<std::string>(std::size_t row)>& modelNames);

} // namespace ts2::sim
