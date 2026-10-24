#include "sim/walk_plan.hpp"

#include <algorithm>
#include <cmath>

namespace ts2::sim {

namespace {

constexpr float kPi = 3.14159265358979f;
// Parts fade into each other over this distance, or 0.95 of a start's or stop's distance when shorter.
constexpr float kBlend = 0.2f;
constexpr float kBlendShare = 0.95f;
// A walk is long (it gets a walk loop) when the long start and stop clips cover less than 0.9 of it.
constexpr float kShortWalkShare = 0.9f;
// Walks shorter than this take a slower speed, towards the start and stop clips' own.
constexpr float kFullSpeedLength = 4.0f;
// The loop starts this share of a straight start's distance early (and ends as far into a straight stop).
constexpr float kStraightOverlap = 0.09f;
// The speed the walk ramps from and to: this share of its own, the rest of the straight starts' speed.
constexpr float kRampShare = 0.7f;
// Ramps cover at most this many strides, and a joint without a ramp is smoothed over at most 50 ms.
constexpr float kMaxRampStrides = 2.0f;
constexpr float kMaxJoinMs = 50.0f;
// The speed curve runs this far past the walk's end.
constexpr float kEndSlack = 0.01f;
// Leftovers shorter than this are none. Clip weights below this are off.
constexpr float kLeastLeftover = 0.001f;
constexpr float kLeastWeight = 0.001f;
// Guards against broken clip data: a walk loop counted in more half cycles than this is cut short,
// and travels, lengths, speeds, walks and Sim sizes beyond these are taken as these (the game's are
// far below).
constexpr float kMaxHalfCycles = 100000.0f;
constexpr float kMaxTravel = 1000.0f;
constexpr float kMaxSpeed = 1.0f;
constexpr float kMaxLength = 100000.0f;
constexpr float kMaxClipMs = 3600000.0f;
constexpr float kMaxScale = 10.0f;
constexpr std::size_t kMaxLoopStyles = 20;

float finiteOr(float v, float fallback) {
    return std::isfinite(v) ? v : fallback;
}

// A value from a clip file within [0, most], 0 when it isn't a number.
float within(float v, float most) {
    return std::clamp(finiteOr(v, 0), 0.0f, most);
}

LocoClip sane(LocoClip c) {
    c.lengthMs = within(c.lengthMs, kMaxClipMs);
    c.travel = within(c.travel, kMaxTravel);
    c.loopPhase = within(c.loopPhase, 1);
    c.match = std::clamp(finiteOr(c.match, 0), -kMaxTravel, kMaxTravel);
    c.speed = within(c.speed, kMaxSpeed);
    c.rootTurn = std::clamp(finiteOr(c.rootTurn, 0), -2 * kPi, 2 * kPi);
    return c;
}

std::optional<LocoPair> sane(const std::optional<LocoPair>& pair) {
    if (!pair) {
        return std::nullopt;
    }
    LocoPair p = *pair;
    p.clip = sane(p.clip);
    if (p.alt) {
        p.alt = sane(*p.alt);
    }
    return p;
}

// A pair's clips with their travel shortened by the Sim's size.
std::optional<LocoPair> sized(std::optional<LocoPair> pair, float scale) {
    if (pair) {
        pair->clip.travel *= scale;
        if (pair->alt) {
            pair->alt->travel *= scale;
        }
    }
    return pair;
}

WalkPlanInput sane(const WalkPlanInput& raw) {
    WalkPlanInput in = raw;
    in.length = within(raw.length, kMaxLength);
    in.scale = within(raw.scale, kMaxScale);
    in.start = sane(raw.start);
    in.stop = sane(raw.stop);
    in.stopOtherFoot = sane(raw.stopOtherFoot);
    // At most as many loop styles as a walk set holds, their shares scaled to add up to 1. When none
    // has a share, the first plays alone.
    in.loops.clear();
    float sum = 0;
    for (const auto& style : raw.loops) {
        if (in.loops.size() < kMaxLoopStyles) {
            in.loops.push_back({*sized(sane(std::optional<LocoPair>(style.clips)), in.scale), within(style.share, 1)});
            sum += in.loops.back().share;
        }
    }
    if (in.loops.empty()) {
        in.loops.push_back({});
    }
    for (auto& style : in.loops) {
        style.share = sum > 0 ? style.share / sum : 0.0f;
    }
    if (!(sum > 0)) {
        in.loops.front().share = 1;
    }
    in.speed = within(raw.speed, kMaxSpeed);
    in.topLoopSpeed = within(raw.topLoopSpeed, kMaxSpeed);
    in.topStraightSpeed = within(raw.topStraightSpeed, kMaxSpeed);
    return in;
}

float travelOf(const std::optional<LocoPair>& pair) {
    return pair ? pair->clip.travel : 0.0f;
}

// The loop's styles together: their short and long clips' travels and match values added up by
// share (a style without a long clip counts its short one for both).
LocoPair stylesTogether(const std::vector<LoopStyle>& loops) {
    LocoPair out;
    out.clip.travel = out.clip.match = 0;
    LocoClip longOne;
    bool anyLong = false;
    for (const auto& style : loops) {
        const LocoClip& a = style.clips.clip;
        const LocoClip& b = style.clips.alt ? *style.clips.alt : a;
        out.clip.travel += style.share * a.travel;
        out.clip.match += style.share * a.match;
        longOne.travel += style.share * b.travel;
        longOne.match += style.share * b.match;
        anyLong = anyLong || style.clips.alt.has_value();
    }
    if (anyLong) {
        out.alt = longOne;
    }
    return out;
}
float altTravelOf(const std::optional<LocoPair>& pair) {
    return pair ? (pair->alt ? pair->alt->travel : pair->clip.travel) : 0.0f;
}
float altSpeedOf(const LocoPair& pair) {
    return pair.alt ? pair.alt->speed : pair.clip.speed;
}

// The travel of the short and long clips' mix whose match value is `f` (the long clip's must be
// the higher), nullopt when they can't be mixed that way.
std::optional<float> travelAtMatch(const LocoPair& pair, float f) {
    if (!pair.alt || !(pair.clip.match < pair.alt->match)) {
        return std::nullopt;
    }
    const float t = std::clamp((f - pair.clip.match) / (pair.alt->match - pair.clip.match), 0.0f, 1.0f);
    return pair.clip.travel + t * (pair.alt->travel - pair.clip.travel);
}

// For a short walk: where between `lo` and `hi` the match value of the mix that travels `distance`
// lies, 1 at `lo`, nullopt when it doesn't lie between them.
std::optional<float> matchShare(const LocoPair& pair, float distance, float lo, float hi) {
    if (!pair.alt || !(lo < hi) || !(pair.clip.travel < pair.alt->travel)) {
        return std::nullopt;
    }
    const float u = (distance - pair.clip.travel) / (pair.alt->travel - pair.clip.travel);
    if (!(u >= 0 && u <= 1)) {
        return std::nullopt;
    }
    const float p = u * pair.clip.match + (1 - u) * pair.alt->match;
    if (!(p >= lo && p <= hi)) {
        return std::nullopt;
    }
    return (hi - p) / (hi - lo);
}

float fraction(float v) {
    return v - std::floor(v);
}

float eased(float w) {
    return 1 - (std::cos(w * kPi) + 1) / 2;
}

struct Distances {
    float start = 0, stop = 0;
    float halfCycles = 0;  // the loop's length in cycles (n)
    float leftover = 0;
    float strideStart = 0, strideEnd = 0;
    bool loop = false;
    bool otherFoot = false;  // the stop for the other foot ends it (an odd count of half cycles)
};

// Where the start ends and the stop begins, and the loop between them (the game's planner).
Distances layOut(const WalkPlanInput& in, float d) {
    Distances out;
    float startA = travelOf(in.start), startB = altTravelOf(in.start);
    float stopA = travelOf(in.stop), stopB = altTravelOf(in.stop);
    const LocoPair loop = stylesTogether(in.loops);
    const float s1 = loop.clip.travel;
    const float s2 = loop.alt ? loop.alt->travel : s1;
    const float p1 = loop.clip.match;
    const float p2 = loop.alt ? loop.alt->match : p1;
    const float sumA = startA + stopA, sumB = startB + stopB;
    if (sumB * kShortWalkShare >= d) {
        // Short: the start and stop alone, mixed to cover the walk exactly.
        const float w = sumA >= sumB ? 1.0f : std::min(1.0f, 1 - (d - sumA) / (sumB - sumA));
        out.start = w * startA + (1 - w) * startB;
        out.stop = w * stopA + (1 - w) * stopB;
        if (d < sumA) {
            const float both = out.start + out.stop;
            out.start = both > 0 ? out.start * d / both : 0.0f;
            out.stop = d - out.start;
            return out;
        }
        // A loop of no length between them when both mixes' match values lie within the loop's.
        const auto a = in.start ? matchShare(*in.start, out.start, p1, p2) : std::nullopt;
        const auto b = in.stop ? matchShare(*in.stop, out.stop, p1, p2) : std::nullopt;
        if (a && b) {
            const float wl = (*a + *b) / 2;
            out.loop = true;
            out.strideStart = out.strideEnd = wl * s1 + (1 - wl) * s2;
        }
        return out;
    }
    // Long: the start's and stop's clips are mixed to match the loop's short and long clips.
    const auto sa = in.start ? travelAtMatch(*in.start, p1) : std::nullopt, sb = in.start ? travelAtMatch(*in.start, p2) : std::nullopt;
    const auto matched = [&](const std::optional<LocoPair>& stop) -> std::optional<std::pair<float, float>> {
        const auto ea = stop ? travelAtMatch(*stop, p1) : std::nullopt, eb = stop ? travelAtMatch(*stop, p2) : std::nullopt;
        return sa && sb && ea && eb ? std::optional(std::pair{*ea, *eb}) : std::nullopt;
    };
    const auto otherStop = in.stopOtherFoot ? in.stopOtherFoot : in.stop;
    const auto [stopA0, stopB0] = matched(in.stop).value_or(std::pair{stopA, stopB});
    const auto [stopA1, stopB1] = matched(otherStop).value_or(std::pair{travelOf(otherStop), altTravelOf(otherStop)});
    if (matched(in.stop)) {
        startA = *sa;
        startB = *sb;
    }
    stopA = stopA0;
    stopB = stopB0;
    // The loop in half cycles of the long clip, then the mix of short and long that fits the rest.
    // An odd count of half cycles ends on the other foot, so the stop for that foot is taken (and
    // the count grows by half a cycle, at most twice, while the walk doesn't fit).
    float n = 0;
    if (s2 > 0) {
        n = 0.5f * std::min(std::ceil(std::max(0.0f, (d - startB - stopB) / (0.5f * s2))), kMaxHalfCycles);
        const auto pick = [&] {
            out.otherFoot = std::fmod(n * 2, 2.0f) == 1.0f;
            stopA = out.otherFoot ? stopA1 : stopA0;
            stopB = out.otherFoot ? stopB1 : stopB0;
        };
        pick();
        for (int pass = 0; pass < 2 && n * s2 + stopB + startB < d; ++pass) {
            n += 0.5f;
            pick();
        }
    }
    const float sA = n * s1 + stopA + startA, sB = n * s2 + stopB + startB;
    float w = 1, ws = 1, counted = n;
    if (sA < sB) {
        w = (sB - d) / (sB - sA);
        if (w > 0.3f && n >= 0.5f) {
            // The last half cycle changes stride: solve for w with the stop's mix stepped down until w fits.
            counted = n - 0.5f;
            for (int step = 10; step >= 0; --step) {
                ws = static_cast<float>(step) / 10;
                const float rest = 0.25f * (ws * s1 + (1 - ws) * s2) + ws * stopA + (1 - ws) * stopB;
                const float slope = (startA - startB) + (n - 0.25f) * (s1 - s2);
                w = slope != 0 ? (d - startB - (n - 0.25f) * s2 - rest) / slope : 1.0f;
                if (w >= 0) {
                    break;
                }
            }
        } else {
            ws = w;
        }
        w = std::clamp(w, 0.0f, 1.0f);
    }
    out.loop = d >= sumA;
    out.halfCycles = n;
    out.strideStart = w * s1 + (1 - w) * s2;
    out.strideEnd = ws * s1 + (1 - ws) * s2;
    out.start = w * startA + (1 - w) * startB;
    out.stop = ws * stopA + (1 - ws) * stopB;
    out.leftover = d - out.start - counted * out.strideStart - out.stop;
    if (out.leftover < kLeastLeftover) {
        out.leftover = 0;
    }
    return out;
}

struct Speeds {
    float start = 0, cruise = 0, stop = 0;
    float startMs = 0, stopMs = 0;
    float rampFrom = 0, rampUpMs = 0, rampTo = 0, rampDownMs = 0;
    float holdMs = 0;
};

void fitSpeeds(Speeds& s, float d);

// The speed curve's values (the game's planner, then its curve fit).
Speeds speedsFor(const WalkPlanInput& in, const Distances& at, float d) {
    Speeds s;
    float v = in.speed;
    float r = v > 0 && in.topLoopSpeed > 0 ? v / in.topLoopSpeed : 1.0f;
    const float sumB = altTravelOf(in.start) + altTravelOf(in.stop);
    const float span = kFullSpeedLength - kShortWalkShare * sumB;
    const float raw = span > 0 ? (d - kShortWalkShare * sumB) / span : 1.0f;
    float wv = std::min(raw, 1.0f);
    if (raw <= 0) {
        wv = 0;
        r = r / (1 - raw);
    }
    v = wv * v + (1 - wv) * in.topStraightSpeed;
    s.cruise = v;
    s.start = in.start ? altSpeedOf(*in.start) * r : 0.0f;
    s.stop = in.stop ? altSpeedOf(*in.stop) * r : 0.0f;
    s.startMs = s.start > 0 ? at.start / s.start : 0.0f;
    s.stopMs = s.stop > 0 ? at.stop / s.stop : 0.0f;
    const float ramp = kRampShare * v + (1 - kRampShare) * in.topStraightSpeed;
    const float strides = std::min(at.halfCycles / 2, kMaxRampStrides);
    s.rampFrom = s.rampTo = v;
    if (at.halfCycles > 1 && ramp + v > 0) {
        s.rampFrom = ramp;
        s.rampUpMs = 2 * strides * at.strideStart / (ramp + v);
    }
    if (in.stop && (at.leftover > 0 || at.halfCycles > 1) && ramp + v > 0) {
        s.rampTo = ramp;
        const float x = at.leftover > 0 ? at.leftover : strides * at.strideStart;
        s.rampDownMs = 2 * x / (ramp + v);
    }
    fitSpeeds(s, d);
    return s;
}

// Fits the speed curve to a walk of `d` tiles: when its parts would cover more, all are shortened
// alike and the cruise a little more. The hold at the cruise covers the rest.
void fitSpeeds(Speeds& s, float d) {
    const auto areas = [&] {
        return s.start * s.startMs + s.stop * s.stopMs + (s.rampFrom + s.cruise) * s.rampUpMs / 2 + (s.rampTo + s.cruise) * s.rampDownMs / 2;
    };
    if (const float sum = areas(); sum > d + 1e-6f && sum > 0) {
        const float k = std::sqrt(d / sum);
        for (float* t : {&s.startMs, &s.stopMs, &s.rampUpMs, &s.rampDownMs, &s.start, &s.stop}) {
            *t *= k;
        }
        for (float* t : {&s.cruise, &s.rampFrom, &s.rampTo}) {
            *t *= 0.98f * k;
        }
    }
    s.holdMs = s.cruise > 0 ? std::abs((d - areas() + kEndSlack) / s.cruise) : 0.0f;
}

std::vector<SpeedPart> speedCurve(const Speeds& s) {
    const float joinIn = s.rampUpMs == 0 ? std::min({kMaxJoinMs, s.startMs, s.holdMs}) : 0.0f;
    const float joinOut = s.rampDownMs == 0 ? std::min({kMaxJoinMs, s.stopMs, s.holdMs}) : 0.0f;
    std::vector<SpeedPart> parts;
    const auto add = [&](float from, float to, float ms) {
        if (ms > 0 && std::isfinite(ms) && std::isfinite(from) && std::isfinite(to)) {
            parts.push_back({std::max(from, 0.0f), std::max(to, 0.0f), ms});
        }
    };
    add(s.start, s.start, s.startMs - joinIn / 2);
    if (s.rampUpMs > 0) {
        add(s.rampFrom, s.cruise, s.rampUpMs);
    } else {
        add(s.start, s.cruise, joinIn);
    }
    add(s.cruise, s.cruise, s.holdMs - joinIn / 2 - joinOut / 2);
    if (s.rampDownMs > 0) {
        add(s.cruise, s.rampTo, s.rampDownMs);
    } else {
        add(s.cruise, s.stop, joinOut);
    }
    add(s.stop, s.stop, s.stopMs - joinOut / 2);
    return parts;
}

} // namespace

float walkSpeed(float loopSpeed, float pathLength) {
    // Under a quarter tile half speed, then up to 0.7 at half a tile and full speed from a tile.
    float f = 1;
    if (pathLength < 0.25f) {
        f = 0.5f;
    } else if (pathLength < 0.5f) {
        f = 0.5f + (pathLength - 0.25f) / 0.25f * 0.2f;
    } else if (pathLength < 1.0f) {
        f = 0.7f + (pathLength - 0.5f) / 0.5f * 0.3f;
    }
    return std::max(0.0f, finiteOr(loopSpeed, 0)) * f;
}

WalkPlan planWalk(const WalkPlanInput& raw) {
    WalkPlanInput in = sane(raw);
    WalkPlan plan;
    const float d = in.length;
    plan.length = d;
    const Distances at = layOut(in, d);
    if (at.otherFoot && in.stopOtherFoot) {
        in.stop = in.stopOtherFoot;
    }
    const Speeds speeds = speedsFor(in, at, d);
    plan.speed = speedCurve(speeds);
    float blend = kBlend;
    if (in.start) {
        blend = std::min(blend, kBlendShare * at.start);
    }
    if (in.stop) {
        blend = std::min(blend, kBlendShare * at.stop);
    }
    blend = std::max(0.0f, blend);
    if (in.start) {
        WalkSegment s;
        s.kind = WalkSegment::Kind::Start;
        s.clips = *in.start;
        s.start = 0;
        s.end = at.start;
        s.blend = blend;
        s.strideStart = s.strideEnd = at.start;
        plan.segments.push_back(s);
    }
    if (at.loop && at.strideStart > 0 && at.strideEnd > 0) {
        WalkSegment s;
        s.kind = WalkSegment::Kind::Loop;
        s.blend = blend;
        if (in.loops.size() > 1) {
            s.styles = stylesTogether(in.loops);
        }
        s.strideStart = at.strideStart;
        s.strideEnd = at.strideEnd;
        // The loop's phase as the start ends: the start clip's (taken the other foot when mirrored).
        float phase = 0.5f;
        if (in.start) {
            phase = in.start->mirrored ? fraction(in.start->clip.loopPhase + 0.5f) : in.start->clip.loopPhase;
        }
        float endPhase = phase + at.halfCycles;
        endPhase -= std::max(0.0f, std::ceil(endPhase - 1));
        const float loopStart = at.start, loopEnd = d - at.stop;
        s.originStart = loopStart - phase * at.strideStart;
        s.originEnd = loopEnd - endPhase * at.strideEnd;
        if (at.leftover > 0) {
            s.changeAt = loopEnd - at.leftover;
            s.changeLength = at.leftover;
            s.changeSpeedFrom = speeds.cruise;
            s.changeSpeedTo = speeds.rampTo;
            s.changeMs = speeds.rampDownMs;
        }
        // The loop reaches into a straight start and stop (none into a half turn).
        const auto overlap = [&](const std::optional<LocoPair>& pair, float distance) {
            if (!pair) {
                return 0.0f;
            }
            const float straight = std::max(1 - std::abs(pair->clip.rootTurn) / kPi, 0.0f);
            return kStraightOverlap * straight * (distance - blend / 2);
        };
        s.start = loopStart - overlap(in.start, at.start);
        s.end = loopEnd + overlap(in.stop, at.stop);
        // A part for each style, showing by its share.
        for (const auto& style : in.loops) {
            if (style.share > 0) {
                s.clips = style.clips;
                s.weight = style.share;
                plan.segments.push_back(s);
            }
        }
    }
    if (in.stop) {
        WalkSegment s;
        s.kind = WalkSegment::Kind::Stop;
        s.clips = *in.stop;
        s.start = d - at.stop;
        s.end = d;
        s.blend = blend;
        s.strideStart = s.strideEnd = at.stop;
        plan.segments.push_back(s);
    }
    return plan;
}

namespace {

// The game's limits for a step: a path under a tile, straight to this, without this much turn.
constexpr float kStepLongest = 1.0f;
constexpr float kStepStraightness = 0.02f;
constexpr float kStepTurn = 0.01f;
// A direction this close to a step clip's takes that clip alone.
constexpr float kStepExact = 0.01f;
// Steps play this much faster than authored (the walk set's speed tag, 1 for the plain walk, would
// multiply it. Other walk sets aren't ported yet).
constexpr float kStepSpeed = 1.4f;

float wrapped(float angle) {
    while (angle > kPi) angle -= 2 * kPi;
    while (angle < -kPi) angle += 2 * kPi;
    return angle;
}

struct StepPart {
    LocoPair clips;
    float angle = 0;
    float weight = 0;
};

} // namespace

bool takesStep(float length, float straightLength, float turn) {
    return length < kStepLongest && std::abs(straightLength - length) <= kStepStraightness && std::abs(wrapped(turn)) < kStepTurn;
}

std::optional<WalkPlan> planStep(float rawLength, float rawAngle, float rawScale, const StepClipAt& clipAt, const std::optional<LocoClip>& inPlace) {
    const float d = within(rawLength, kMaxLength);
    const float angle = wrapped(std::clamp(finiteOr(rawAngle, 0), -kPi, kPi));
    const float scale = std::clamp(finiteOr(rawScale, 1), 0.01f, 100.0f);
    if (!clipAt || !(d > 0)) {
        return std::nullopt;
    }
    // A pick counts when its full clip reaches the whole move. The first decides whether a second
    // one, 45 degrees on, is mixed in. When the first is there but too short there is no step.
    StepPart parts[2];
    int counted = 0;
    float residual = 0;
    float at = angle;
    for (int i = 0; i < 2; ++i) {
        const auto pick = clipAt(at);
        bool fits = false;
        if (pick) {
            LocoPair clips = *sane(std::optional<LocoPair>(pick->clips));
            clips.clip.travel *= scale;
            if (clips.alt) {
                clips.alt->travel *= scale;
            }
            const float full = clips.alt ? clips.alt->travel : clips.clip.travel;
            fits = d <= full;
            if (fits) {
                if (d < clips.clip.travel && inPlace) {
                    // Shorter than the short step: mixed with the step on the spot instead.
                    LocoClip still = sane(*inPlace);
                    still.travel = 0;
                    clips.alt = clips.clip;
                    clips.clip = still;
                }
                parts[i] = {clips, pick->angle, 1};
                ++counted;
            }
        }
        if (i == 0 && (!pick || fits)) {
            const float picked = pick ? pick->angle : at;
            residual = wrapped(angle - picked);
            if (std::abs(residual) <= kStepExact) {
                break;
            }
            at = picked + (residual > 0 ? kPi / 4 : -kPi / 4);
        }
    }
    if (counted == 0) {
        return std::nullopt;
    }
    if (counted == 2) {
        const float share = std::abs(residual) * 4 / kPi;
        parts[0].weight = 1 - share;
        parts[1].weight = share;
    }
    // Either pick played mirrored mirrors both.
    const bool mirrored = parts[0].clips.mirrored || parts[1].clips.mirrored;
    WalkPlan plan;
    plan.length = d;
    plan.keepsFacing = true;
    float speed = 0, shares = 0;
    for (const auto& p : parts) {
        if (!(p.weight > 0)) {
            continue;
        }
        const LocoPair& c = p.clips;
        const float u = c.alt && c.alt->travel != c.clip.travel ? std::clamp((d - c.clip.travel) / (c.alt->travel - c.clip.travel), 0.0f, 1.0f) : 0.0f;
        speed += (1 - u) * c.clip.speed + (c.alt ? u * c.alt->speed : 0.0f);
        shares += (1 - u) + (c.alt ? u : 0.0f);
        WalkSegment s;
        s.kind = WalkSegment::Kind::Step;
        s.clips = c;
        s.clips.mirrored = mirrored;
        s.start = 0;
        s.end = d;
        s.strideStart = s.strideEnd = d;
        s.weight = p.weight;
        plan.segments.push_back(s);
    }
    speed = shares > 0 ? speed / shares * kStepSpeed : 0.0f;
    if (!(speed > 0)) {
        return std::nullopt;
    }
    plan.speed.push_back({speed, speed, d / speed});
    return plan;
}

float walkMs(const WalkPlan& plan) {
    float ms = 0;
    for (const auto& p : plan.speed) {
        ms += p.ms;
    }
    return ms;
}

float walkedAt(const WalkPlan& plan, float ms) {
    float walked = 0;
    float left = std::max(0.0f, finiteOr(ms, 0));
    for (const auto& p : plan.speed) {
        const float t = std::min(left, p.ms);
        walked += t * (p.from + (p.to - p.from) * t / (2 * p.ms));
        left -= t;
        if (left <= 0) {
            break;
        }
    }
    return std::isfinite(walked) ? std::clamp(walked, 0.0f, plan.length) : 0.0f;
}

namespace {

// How far through a changing stride the loop is at `distance`: 0 before the change, 1 after it.
float changeShare(const WalkSegment& s, float distance) {
    if (!s.changeAt || distance <= *s.changeAt) {
        return 0;
    }
    const float x = distance - *s.changeAt;
    const float v1 = s.changeSpeedFrom, v2 = s.changeSpeedTo, t = s.changeMs;
    if (!(t > 0)) {
        return 1;
    }
    float tau = 0;
    if (std::abs(v1 - v2) < 1e-5f) {
        tau = v1 + v2 > 0 ? 2 * x / (v1 + v2) : t;
    } else {
        // x = v1 tau + (v2 - v1) / (2 t) tau^2
        const float a = (v2 - v1) / (2 * t);
        const float disc = v1 * v1 + 4 * a * x;
        tau = disc >= 0 ? (-v1 + std::sqrt(disc)) / (2 * a) : t;
    }
    return std::clamp(tau / t, 0.0f, 1.0f);
}

// A loop's stride at `distance`, and how far through its cycle it is then (0-1).
struct LoopAt {
    float stride = 0, phase = 0;
};
LoopAt loopAt(const WalkSegment& s, float d) {
    const float u = changeShare(s, d);
    float cycles = (d - s.originStart) / s.strideStart;
    if (u >= 1) {
        cycles = (d - s.originEnd) / s.strideEnd;
    } else if (u > 0) {
        cycles = (d - *s.changeAt) / s.changeLength * 0.5f + (*s.changeAt - s.originStart) / s.strideStart;
    }
    return {(1 - u) * s.strideStart + u * s.strideEnd, fraction(finiteOr(cycles, 0))};
}

// The short clip's share of a part's mix that travels `stride` per play.
float shortShareOf(const LocoPair& clips, float stride) {
    if (!clips.alt || clips.alt->travel == clips.clip.travel) {
        return 1;
    }
    return std::clamp((clips.alt->travel - stride) / (clips.alt->travel - clips.clip.travel), 0.0f, 1.0f);
}

// The loop runs on this share of a cycle past the stop's start, and the speed the walk had is
// taken over its time up to this many milliseconds ago.
constexpr float kStopSoonOverlap = 0.05f;
constexpr float kLookBackMs = 25.0f;
// Clip foot values, and turns in radians, within these of each other are the same.
constexpr float kSameFoot = 0.01f;
constexpr float kSameTurn = 0.01f;

} // namespace

std::optional<LocoPair> turnClipsForFoot(std::vector<LocoClip> clips, float angle, float foot) {
    for (auto& c : clips) {
        c = sane(c);
    }
    std::stable_sort(clips.begin(), clips.end(), [](const LocoClip& a, const LocoClip& b) { return a.travel < b.travel; });
    const float turn = finiteOr(angle, 0), f = finiteOr(foot, 0);
    std::optional<LocoPair> out;
    for (const auto& c : clips) {
        const float apart = std::abs(f - c.loopPhase);
        const bool same = apart <= kSameFoot && std::abs(turn - c.rootTurn) <= kSameTurn;
        const bool other = std::abs(apart - 0.5f) <= kSameFoot && std::abs(turn + c.rootTurn) <= kSameTurn;
        if (!same && !other) {
            continue;
        }
        if (!out) {
            out = LocoPair{c, std::nullopt, !same};
        } else {
            out->alt = c;
            break;
        }
    }
    return out;
}

float speedAt(const WalkPlan& plan, float ms) {
    float left = std::max(0.0f, finiteOr(ms, 0));
    for (const auto& p : plan.speed) {
        if (left < p.ms) {
            return p.from + (p.to - p.from) * left / p.ms;
        }
        left -= p.ms;
    }
    return 0;
}

StopSoon stopSoon(WalkPlan& plan, float ms, const std::optional<LocoPair>& straightStop, float scale, float topLoopSpeed) {
    using Kind = WalkSegment::Kind;
    const float t = std::max(0.0f, finiteOr(ms, 0));
    const float speed = speedAt(plan, t);
    if (!(speed > 0)) {
        return StopSoon::Now;
    }
    const float x0 = walkedAt(plan, t);
    // The part playing: of those begun, the one that ends last.
    std::size_t at = plan.segments.size();
    for (std::size_t i = 0; i < plan.segments.size(); ++i) {
        const WalkSegment& s = plan.segments[i];
        if (s.start <= x0 && (at == plan.segments.size() || s.end > plan.segments[at].end)) {
            at = i;
        }
    }
    if (at == plan.segments.size() || plan.segments[at].kind == Kind::Stop || plan.segments[at].kind == Kind::Step) {
        return StopSoon::AsPlanned;
    }
    // The parts after it go: the planned stop, and the loop when a start is playing (a loop of
    // several styles keeps every style's part).
    const bool loop = plan.segments[at].kind == Kind::Loop;
    std::erase_if(plan.segments, [&](const WalkSegment& s) { return s.kind == Kind::Stop || (!loop && s.kind == Kind::Loop); });
    at = 0;
    while (at < plan.segments.size() && plan.segments[at].kind != (loop ? Kind::Loop : Kind::Start)) {
        ++at;
    }
    if (at == plan.segments.size()) {
        return StopSoon::AsPlanned;
    }
    WalkSegment& part = plan.segments[at];
    const LocoPair& mix = part.styles ? *part.styles : part.clips;
    // How far through the walk cycle the part is: a start's own foot value.
    float stride = part.strideStart, phase = part.clips.clip.loopPhase;
    if (loop) {
        const LoopAt l = loopAt(part, x0);
        stride = l.stride;
        phase = l.phase;
    }
    const bool laterFoot = phase == 0 || phase > 0.5f;
    const float foot = part.clips.mirrored == laterFoot ? 0.5f : 0.0f;
    const auto clips = sized(sane(straightStop), within(scale, kMaxScale));
    std::vector<LocoClip> straight;
    if (clips) {
        straight.push_back(clips->clip);
        if (clips->alt) {
            straight.push_back(*clips->alt);
        }
    }
    const auto stop = turnClipsForFoot(straight, 0, foot);
    if (!stop) {
        return StopSoon::Soon;
    }
    // A loop is cut at the next half of its cycle. A start plays to its end.
    float from = part.end;
    float share = shortShareOf(mix, stride);
    if (loop) {
        const float left = phase > 0 ? (phase < 0.5f ? 0.5f : 1.0f) - phase : 0.0f;
        from = x0 + left * stride;
        for (auto& s : plan.segments) {
            if (s.kind == Kind::Loop) {
                s.end = from + kStopSoonOverlap * stride;
            }
        }
        // The stop's short and long clips are mixed so its match value equals the loop's, as at a
        // planned walk's end. Without a usable pair it keeps the loop's short share.
        const float match = mix.alt ? share * mix.clip.match + (1 - share) * mix.alt->match : mix.clip.match;
        if (stop->alt && stop->clip.match > 0 && stop->clip.match < stop->alt->match) {
            share = std::clamp((stop->alt->match - match) / (stop->alt->match - stop->clip.match), 0.0f, 1.0f);
        }
    }
    const float length = share * stop->clip.travel + (1 - share) * altTravelOf(stop);
    WalkSegment s;
    s.kind = Kind::Stop;
    s.clips = *stop;
    s.start = from;
    s.end = from + length;
    s.strideStart = s.strideEnd = length;
    plan.segments.push_back(s);
    plan.length = s.end;
    // The speed: what the walk has covered at its average speed, the speed it has now up to the
    // stop, and the stop's own speed over it.
    float stopSpeed = altSpeedOf(*stop);
    if (loop && std::isfinite(topLoopSpeed) && topLoopSpeed > 0) {
        stopSpeed *= speed / topLoopSpeed;
    }
    float elapsed = std::floor(t), covered = x0;
    if (elapsed >= kLookBackMs && x0 - kLookBackMs * speed >= 0) {
        elapsed -= kLookBackMs;
        covered = x0 - kLookBackMs * speed;
    }
    Speeds v;
    v.start = elapsed > 0 ? covered / elapsed : 0.0f;
    v.startMs = elapsed > 0 ? elapsed : 0.0f;
    v.cruise = v.rampFrom = v.rampTo = speed;
    v.stop = stopSpeed;
    v.stopMs = stopSpeed > 0 ? length / stopSpeed : 0.0f;
    fitSpeeds(v, plan.length);
    plan.speed = speedCurve(v);
    return StopSoon::Soon;
}

void walkPoses(const WalkPlan& plan, float distance, std::vector<WalkPose>& out) {
    out.clear();
    const float d = finiteOr(distance, 0);
    for (const auto& s : plan.segments) {
        if (d - s.start < -s.blend / 2) {
            continue;
        }
        float w = d >= s.start && d <= s.end ? 1.0f : 0.0f;
        if (s.blend > 0) {
            w = eased(std::clamp(std::min((d - s.start + s.blend / 2) / s.blend, (s.end - d + s.blend / 2) / s.blend), 0.0f, 1.0f));
        }
        if (w <= 0) {
            continue;
        }
        float stride = s.strideStart, phase = 0;
        if (s.kind == WalkSegment::Kind::Loop) {
            const LoopAt l = loopAt(s, d);
            stride = l.stride;
            phase = l.phase;
        } else {
            phase = s.end > s.start ? std::clamp((d - s.start) / (s.end - s.start), 0.0f, 1.0f) : 1.0f;
        }
        // The short and long clips mixed so that their travel is the stride.
        const float shortShare = shortShareOf(s.styles ? *s.styles : s.clips, stride);
        w *= s.weight;
        const auto turnOf = [&](const LocoClip& c) {
            return s.kind == WalkSegment::Kind::Step ? (s.clips.mirrored ? -c.rootTurn : c.rootTurn) : 0.0f;
        };
        if (w * shortShare > kLeastWeight) {
            out.push_back({&s.clips.clip, w * shortShare, phase, s.clips.mirrored, turnOf(s.clips.clip)});
        }
        if (s.clips.alt && w * (1 - shortShare) > kLeastWeight) {
            out.push_back({&*s.clips.alt, w * (1 - shortShare), phase, s.clips.mirrored, turnOf(*s.clips.alt)});
        }
    }
    float sum = 0;
    for (const auto& p : out) {
        sum += p.weight;
    }
    for (auto& p : out) {
        p.weight /= sum;
    }
}

} // namespace ts2::sim
