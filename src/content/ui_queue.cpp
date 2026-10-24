#include "content/ui_queue.hpp"

#include <algorithm>
#include <charconv>

namespace ts2::content {

std::vector<QueueSlot> queueSlots(const std::vector<bool>& nested, const QueueLayout& layout) {
    std::vector<QueueSlot> slots(nested.size());
    const std::size_t nestedCount = static_cast<std::size_t>(std::count(nested.begin(), nested.end(), true));
    int row = 0, column = 0;
    for (std::size_t i = 0; i < nested.size(); ++i) {
        QueueSlot& slot = slots[i];
        slot.nested = nested[i];
        if (nested[i]) {
            slot.x = layout.x;
            slot.y = layout.y + layout.height + layout.nestedOffset + column * layout.height;
            ++column;
            slot.joined = static_cast<std::size_t>(column) < nestedCount;
        } else {
            slot.x = layout.x + row * (layout.width + layout.margin);
            slot.y = layout.y;
            slot.joined = row == 0 && nestedCount > 0;
            ++row;
        }
    }
    return slots;
}

float slideStep(float place, float target, double ms, int steps, float delta) {
    const float distance = target - place;
    const float step = static_cast<float>(ms) * distance * 0.02f / static_cast<float>(std::max(steps, 1));
    if (step * step < delta * delta) {
        if (distance * distance <= delta * delta) {
            return target;
        }
        return place + (distance > 0 ? delta : -delta);
    }
    if (distance * distance < step * step) {
        return target;
    }
    return place + step;
}

std::optional<int> uiConstant(const std::vector<std::string>& lines, std::string_view name) {
    for (const std::string& line : lines) {
        const std::string_view text(line);
        if (text.size() <= name.size() || text.substr(0, name.size()) != name || text[name.size()] != '=') {
            continue;
        }
        const std::string_view value = text.substr(name.size() + 1);
        int number = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
        if (error == std::errc() && end == value.data() + value.size()) {
            return number;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

} // namespace ts2::content
