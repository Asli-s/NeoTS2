#include "scene/terrain_water.hpp"

#include "parse_float.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

namespace ts2::scene::terrainwater {

namespace {

constexpr std::size_t kMostCurvePoints = 64;
constexpr std::uint32_t kMostCorners = 1025;  // a side of a lot of 1024 tiles

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

} // namespace

float AlphaCurve::alpha(float depth) const {
    if (points.empty()) {
        return 0;
    }
    if (depth <= points.front().first) {
        return points.front().second;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        const auto [d0, a0] = points[i - 1];
        const auto [d1, a1] = points[i];
        if (depth <= d1) {
            return d1 > d0 ? a0 + (a1 - a0) * (depth - d0) / (d1 - d0) : a1;
        }
    }
    return points.back().second;
}

AlphaCurve readAlphaCurve(std::string_view ini) {
    constexpr std::string_view kKey = "AlphaVsDepthCurve";
    while (!ini.empty()) {
        const std::size_t end = ini.find('\n');
        const std::string_view line = trim(ini.substr(0, end));
        ini = end == std::string_view::npos ? std::string_view{} : ini.substr(end + 1);
        if (!line.starts_with(kKey)) {
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos || trim(line.substr(kKey.size(), equals - kKey.size())).size() != 0) {
            continue;
        }
        // Numbers split by commas and spaces, read in pairs.
        std::vector<float> numbers;
        std::string_view rest = line.substr(equals + 1);
        while (!rest.empty()) {
            const std::size_t skip = rest.find_first_not_of(" \t,");
            if (skip == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(skip);
            const std::size_t length = std::min(rest.find_first_of(" \t,"), rest.size());
            const auto value = parseFloat(rest.substr(0, length));
            if (!value || !std::isfinite(*value)) {
                return {};
            }
            numbers.push_back(*value);
            rest.remove_prefix(length);
        }
        if (numbers.empty() || numbers.size() % 2 != 0 || numbers.size() / 2 > kMostCurvePoints) {
            return {};
        }
        AlphaCurve curve;
        for (std::size_t i = 0; i < numbers.size(); i += 2) {
            if (!curve.points.empty() && numbers[i] < curve.points.back().first) {
                return {};
            }
            curve.points.emplace_back(numbers[i], numbers[i + 1]);
        }
        return curve;
    }
    return {};
}

Mesh build(const terrain::Array2D& water, const std::function<float(int, int)>& terrain, const AlphaCurve& curve) {
    Mesh out;
    if (water.width < 2 || water.height < 2 || water.width > kMostCorners || water.height > kMostCorners || curve.points.empty()) {
        return out;
    }
    auto alphaOf = [&](float depth) { return std::floor(std::clamp(curve.alpha(depth), 0.0f, 255.0f)) / 255.0f; };
    auto add = [&](float x, float y, float z, float alpha) {
        const auto at = static_cast<std::uint32_t>(out.alphas.size());
        out.positions.insert(out.positions.end(), {x, y, z});
        out.uvs.insert(out.uvs.end(), {x, y});
        out.alphas.push_back(alpha);
        return at;
    };
    std::map<std::pair<int, int>, std::uint32_t> corners;
    for (int y = 0; y + 1 < static_cast<int>(water.height); ++y) {
        for (int x = 0; x + 1 < static_cast<int>(water.width); ++x) {
            static constexpr std::array<std::pair<int, int>, 4> kRound{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
            std::array<float, 4> surface{}, ground{};
            bool complete = true, wet = false;
            for (std::size_t k = 0; k < kRound.size(); ++k) {
                const auto w = cornerHeight(water, x + kRound[k].first, y + kRound[k].second);
                const float t = terrain(x + kRound[k].first, y + kRound[k].second);
                complete = complete && w && std::isfinite(t);
                if (!complete) {
                    break;
                }
                surface[k] = *w;
                ground[k] = t;
                wet = wet || t < *w;
            }
            if (!complete || !wet) {
                continue;
            }
            std::array<std::uint32_t, 4> at{};
            for (std::size_t k = 0; k < kRound.size(); ++k) {
                const int cx = x + kRound[k].first, cy = y + kRound[k].second;
                const auto [it, added] = corners.emplace(std::pair{cx, cy}, 0);
                if (added) {
                    it->second = add(static_cast<float>(cx), static_cast<float>(cy), surface[k], alphaOf(surface[k] - ground[k]));
                }
                at[k] = it->second;
            }
            const float w = (surface[0] + surface[1] + surface[2] + surface[3]) / 4;
            const float t = (ground[0] + ground[1] + ground[2] + ground[3]) / 4;
            const std::uint32_t centre = add(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, w, alphaOf(w - t));
            for (std::size_t k = 0; k < at.size(); ++k) {
                out.indices.insert(out.indices.end(), {centre, at[k], at[(k + 1) % at.size()]});
            }
        }
    }
    return out;
}

std::optional<float> cornerHeight(const terrain::Array2D& water, int x, int y) {
    const auto w = water.f32(x, y);
    return w && std::isfinite(*w) ? w : std::nullopt;
}

std::optional<float> heightAt(const terrain::Array2D& water, float x, float y) {
    if (water.width < 2 || water.height < 2 || !(x >= 0 && y >= 0)) {
        return std::nullopt;
    }
    const float maxX = static_cast<float>(water.width - 1), maxY = static_cast<float>(water.height - 1);
    if (x > maxX || y > maxY) {
        return std::nullopt;
    }
    // A point on the far edge reads the last tile.
    const int ix = static_cast<int>(std::min(std::floor(x), maxX - 1)), iy = static_cast<int>(std::min(std::floor(y), maxY - 1));
    const float fx = x - static_cast<float>(ix), fy = y - static_cast<float>(iy);
    const auto h00 = cornerHeight(water, ix, iy), h10 = cornerHeight(water, ix + 1, iy);
    const auto h01 = cornerHeight(water, ix, iy + 1), h11 = cornerHeight(water, ix + 1, iy + 1);
    if (!h00 || !h10 || !h01 || !h11) {
        return std::nullopt;
    }
    return fx <= fy ? *h00 + fx * (*h11 - *h01) + fy * (*h01 - *h00) : *h00 + fx * (*h10 - *h00) + fy * (*h11 - *h10);
}

} // namespace ts2::scene::terrainwater
