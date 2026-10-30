#include "sim/object_model.hpp"

#include "formats/objd.hpp"
#include "sim/world.hpp"

#include <algorithm>
#include <cctype>

namespace ts2::sim {

std::string createdModelName(const SimObject& object, const std::function<std::optional<std::string>(std::size_t row)>& modelNames) {
    const auto own = static_cast<std::uint16_t>(object.data[0]);
    const std::uint16_t row = own != 0 ? own : object.definition.field(objd::field::kDefaultModel);
    const auto name = row != 0 ? modelNames(row) : std::nullopt;
    if (!name) {
        return {};
    }
    std::string model = *name;
    std::string low = model;
    std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (low.size() >= 5 && low.ends_with("_cres")) {
        model.resize(model.size() - 5);
        low.resize(low.size() - 5);
    }
    return low.empty() || low == "nullmodel" || low == "nomodel" ? std::string{} : model;
}

} // namespace ts2::sim
