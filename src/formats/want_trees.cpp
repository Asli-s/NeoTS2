#include "formats/want_trees.hpp"

#include <charconv>
#include <optional>

namespace ts2::want_trees {

namespace {

constexpr std::string_view kAdNames[kAdCount] = {"age", "aspiration", "mood", "personality", "relationship", "aspirationScore", "personData"};

// The value of `name="..."` in a tag, empty when missing.
std::string_view attribute(std::string_view tag, std::string_view name) {
    for (std::size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
        // The name must stand on its own: "min" is not the end of "superZero" or "minScore".
        const bool starts = at > 0 && (tag[at - 1] == ' ' || tag[at - 1] == '\t' || tag[at - 1] == '\n' || tag[at - 1] == '\r');
        const auto quote = at + name.size();
        if (!starts || tag.substr(quote, 2) != "=\"") {
            continue;
        }
        const auto end = tag.find('"', quote + 2);
        return end == std::string_view::npos ? std::string_view{} : tag.substr(quote + 2, end - quote - 2);
    }
    return {};
}

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r\n") - first + 1);
}

// A decimal or 0x hex number, as the files write them. Nothing for anything else.
std::optional<std::int64_t> number(std::string_view s) {
    s = trim(s);
    bool negative = false;
    if (!s.empty() && s.front() == '-') {
        negative = true;
        s.remove_prefix(1);
    }
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s.remove_prefix(2);
    }
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (error != std::errc{} || end != s.data() + s.size() || value > 0xFFFFFFFFull) {
        return std::nullopt;
    }
    return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}

std::int32_t signedNumber(std::string_view s) { return static_cast<std::int32_t>(number(s).value_or(0)); }

std::uint32_t unsignedNumber(std::string_view s) { return static_cast<std::uint32_t>(number(s).value_or(0)); }

bool truth(std::string_view s) { return trim(s) == "True" || trim(s) == "true"; }

// The tag's name, with the "/" of a closing tag.
std::string_view tagName(std::string_view tag) {
    const auto end = tag.find_first_of(" \t\r\n/", 1);
    return tag.substr(0, end);
}

void setProperty(Node& node, std::string_view key, std::string_view value) {
    if (key == "wantId") {
        node.wantId = unsignedNumber(value);
    } else if (key == "id") {
        node.id = unsignedNumber(value);
    } else if (key == "visible") {
        node.visible = truth(value);
    } else if (key == "lifetime") {
        node.lifetime = signedNumber(value);
    } else if (key == "regeneration") {
        node.regeneration = truth(value);
    } else if (key == "objectField") {
        node.objectField = unsignedNumber(value);
    } else if (key == "integerField") {
        node.integerField = unsignedNumber(value);
    } else if (key == "inheritSubject") {
        node.inheritSubject = truth(value);
    } else if (key == "minScore") {
        node.minScore = signedNumber(value);
    } else if (key == "maxScore") {
        node.maxScore = signedNumber(value);
    } else if (key == "scoreOverride") {
        node.scoreOverride = signedNumber(value);
    }
}

} // namespace

Tree parse(std::string_view text) {
    Tree tree;
    std::vector<std::int32_t> open;  // the nodes being read, innermost last
    std::size_t skipped = 0;         // nodes past the limits being skipped, with all they hold
    Ad* ad = nullptr;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto start = text.find('<', at);
        if (start == std::string_view::npos) {
            break;
        }
        if (text.substr(start, 4) == "<!--") {
            const auto end = text.find("-->", start + 4);
            at = end == std::string_view::npos ? text.size() : end + 3;
            continue;
        }
        const auto end = text.find('>', start);
        if (end == std::string_view::npos) {
            break;
        }
        const auto tag = text.substr(start + 1, end - start - 1);
        at = end + 1;
        const auto name = tagName(tag);
        if (name == "wantNode") {
            // A tree has one root. The game drops a later top-level node (some files hold such
            // "orphan" nodes) with all it holds.
            if (skipped > 0 || open.size() >= kMaxDepth || tree.nodes.size() >= kMaxNodes || (open.empty() && !tree.nodes.empty())) {
                ++skipped;
                continue;
            }
            const auto index = static_cast<std::int32_t>(tree.nodes.size());
            Node& node = tree.nodes.emplace_back();
            if (!open.empty()) {
                node.parent = open.back();
                tree.nodes[static_cast<std::size_t>(open.back())].children.push_back(index);
            }
            open.push_back(index);
            ad = nullptr;
        } else if (name == "/wantNode") {
            if (skipped > 0) {
                --skipped;
            } else if (!open.empty()) {
                open.pop_back();
            }
            ad = nullptr;
        } else if (skipped > 0) {
            continue;
        } else if (name.starts_with("Any")) {
            // A property: its value is the text up to the closing tag.
            const auto close = text.find('<', at);
            if (close == std::string_view::npos) {
                break;
            }
            const auto key = attribute(tag, "key");
            const auto value = text.substr(at, close - at);
            at = close;
            if (!open.empty()) {
                setProperty(tree.nodes[static_cast<std::size_t>(open.back())], key, value);
            } else if (key == "id") {
                tree.id = unsignedNumber(value);
            } else if (key == "folder") {
                tree.folder = std::string(trim(value));
            }
        } else if (name == "ads" && !open.empty()) {
            ad = nullptr;
            const auto key = attribute(tag, "key");
            for (std::size_t i = 0; i < kAdCount; ++i) {
                if (key == kAdNames[i]) {
                    ad = &tree.nodes[static_cast<std::size_t>(open.back())].ads[i];
                    ad->weight = signedNumber(attribute(tag, "weight"));
                    ad->superZero = truth(attribute(tag, "superZero"));
                }
            }
        } else if (name == "/ads") {
            ad = nullptr;
        } else if (name == "bool" && ad != nullptr) {
            // Each factor names only its handful of choices.
            constexpr std::size_t kMaxTicked = 32;
            if (truth(attribute(tag, "value")) && ad->ticked.size() < kMaxTicked) {
                ad->ticked.emplace_back(attribute(tag, "key"));
            }
        } else if (name == "range" && ad != nullptr) {
            ad->min = signedNumber(attribute(tag, "min"));
            ad->max = signedNumber(attribute(tag, "max"));
            ad->index = signedNumber(attribute(tag, "index"));
        }
    }
    return tree;
}

} // namespace ts2::want_trees
