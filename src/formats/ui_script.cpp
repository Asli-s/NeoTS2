#include "formats/ui_script.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>

namespace ts2::ui {

namespace {

// How deep windows may nest. The game's own scripts go 6 deep at most.
constexpr std::size_t kMaxDepth = 64;

bool space(char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

std::string decodeEntities(std::string_view s) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}};
    std::string out;
    for (std::size_t i = 0; i < s.size();) {
        bool replaced = false;
        if (s[i] == '&') {
            for (const auto& [entity, c] : kEntities) {
                if (s.substr(i, entity.size()) == entity) {
                    out += c;
                    i += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) {
            out += s[i++];
        }
    }
    return out;
}

std::optional<std::int64_t> toNumber(std::string_view s, int base) {
    while (!s.empty() && space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && space(s.back())) {
        s.remove_suffix(1);
    }
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    }
    std::int64_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

} // namespace

std::optional<std::string_view> Window::get(std::string_view name) const {
    for (const auto& [key, value] : attributes) {
        if (key == name) {
            return std::string_view(value);
        }
    }
    return std::nullopt;
}

std::string Window::text(std::string_view name) const {
    const auto v = get(name);
    return v ? std::string(*v) : std::string();
}

bool Window::flag(std::string_view name) const {
    return get(name) == std::optional<std::string_view>("yes");
}

std::optional<std::uint32_t> Window::number(std::string_view name) const {
    const auto v = get(name);
    if (!v) {
        return std::nullopt;
    }
    const auto n = toNumber(*v, 10);
    return n ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*n)) : std::nullopt;
}

std::vector<std::int64_t> Window::list(std::string_view name) const {
    const auto v = get(name);
    std::vector<std::int64_t> out;
    if (!v || v->size() < 2) {
        return out;
    }
    const int base = v->front() == '{' ? 16 : 10;
    std::string_view rest = v->substr(1, v->size() - 2);
    while (!rest.empty()) {
        const std::size_t comma = rest.find(',');
        const auto n = toNumber(rest.substr(0, comma), base);
        if (!n) {
            return {};
        }
        out.push_back(*n);
        if (comma == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(comma + 1);
    }
    return out;
}

std::vector<Window> parse(std::string_view script) {
    std::vector<Window> top;
    std::vector<std::vector<Window>*> open{&top};
    std::size_t i = 0;
    while (i < script.size()) {
        const char c = script[i];
        if (space(c)) {
            ++i;
            continue;
        }
        if (c == '#') {
            const std::size_t eol = script.find('\n', i);
            i = eol == std::string_view::npos ? script.size() : eol + 1;
            continue;
        }
        if (c != '<') {
            throw std::runtime_error("ui script: unexpected text at " + std::to_string(i));
        }
        std::size_t j = i + 1;
        while (j < script.size() && !space(script[j]) && script[j] != '>') {
            ++j;
        }
        const std::string_view tag = script.substr(i + 1, j - i - 1);
        if (tag == "CHILDREN") {
            if (open.back()->empty()) {
                throw std::runtime_error("ui script: children without a window");
            }
            // Windows are handled recursively everywhere, so a broken script nested thousands
            // deep would run out of stack.
            if (open.size() > kMaxDepth) {
                throw std::runtime_error("ui script: windows nested too deeply");
            }
            open.push_back(&open.back()->back().children);
        } else if (tag == "/CHILDREN") {
            if (open.size() < 2) {
                throw std::runtime_error("ui script: unbalanced children");
            }
            open.pop_back();
        } else if (tag == "LEGACY") {
            Window w;
            while (j < script.size() && script[j] != '>') {
                if (space(script[j])) {
                    ++j;
                    continue;
                }
                const std::size_t eq = script.find('=', j);
                if (eq == std::string_view::npos) {
                    throw std::runtime_error("ui script: attribute without a value");
                }
                std::string name(script.substr(j, eq - j));
                std::size_t k = eq + 1;
                std::string value;
                if (k < script.size() && script[k] == '"') {
                    const std::size_t close = script.find('"', k + 1);
                    if (close == std::string_view::npos) {
                        throw std::runtime_error("ui script: unterminated string");
                    }
                    value = decodeEntities(script.substr(k + 1, close - k - 1));
                    k = close + 1;
                } else {
                    const char opener = k < script.size() ? script[k] : '\0';
                    const char closer = opener == '(' ? ')' : opener == '{' ? '}' : '\0';
                    std::size_t end = k;
                    if (closer) {
                        end = script.find(closer, k);
                        if (end == std::string_view::npos) {
                            throw std::runtime_error("ui script: unterminated list");
                        }
                        ++end;
                    } else {
                        while (end < script.size() && !space(script[end]) && script[end] != '>') {
                            ++end;
                        }
                    }
                    value = std::string(script.substr(k, end - k));
                    k = end;
                }
                w.attributes.emplace_back(std::move(name), std::move(value));
                j = k;
            }
            open.back()->push_back(std::move(w));
        } else {
            throw std::runtime_error("ui script: unknown tag " + std::string(tag));
        }
        const std::size_t close = script.find('>', j);
        if (close == std::string_view::npos) {
            throw std::runtime_error("ui script: unterminated tag");
        }
        i = close + 1;
    }
    if (open.size() != 1) {
        throw std::runtime_error("ui script: unbalanced children");
    }
    return top;
}

} // namespace ts2::ui
