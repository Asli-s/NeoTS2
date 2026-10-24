#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ts2::ui {

// One window of a UI script: its attributes as written (name, value) and the windows inside it.
// A script's lines are `<LEGACY name=value ...>` windows, each optionally followed by a
// `<CHILDREN>` ... `</CHILDREN>` block of its own windows. Lines starting with # are comments.
struct Window {
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<Window> children;

    // An attribute's value as written (quoted values without their quotes, entities decoded).
    [[nodiscard]] std::optional<std::string_view> get(std::string_view name) const;
    std::string text(std::string_view name) const;
    bool flag(std::string_view name) const;  // "yes"
    [[nodiscard]] std::optional<std::uint32_t> number(std::string_view name) const;  // decimal or 0x hex
    // A list such as area=(l,t,r,b) or a key such as image={group,instance}, as numbers (braces hex).
    std::vector<std::int64_t> list(std::string_view name) const;
};

// The windows at the top of a script.
std::vector<Window> parse(std::string_view script);

} // namespace ts2::ui
