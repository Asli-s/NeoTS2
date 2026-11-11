#include "render/frame_plan.hpp"

#include "scene/sim_shadows.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>

namespace ts2::render {

namespace {

// A Sim's lights outdoors: the lighting with its Sim-only lights merged in. A Sim's parts and the
// Sims beside it are drawn with the same lighting, so the last merge is kept and reused while its
// source is the same. Each drawing thread keeps its own.
const scene::ObjectLighting& simLightingFor(const scene::ObjectLighting& lighting) {
    thread_local scene::ObjectLighting source, merged;
    thread_local bool made = false;
    if (!made || !(lighting == source)) {
        source = lighting;
        merged = scene::withSimLights(lighting);
        made = true;
    }
    return merged;
}

} // namespace

namespace {

// A box in clip space: its low corner, and its three edges from there, each either not walked or
// walked. Every object takes this each frame, so it skips the arrays' bounds checks.
struct ClipBox {
    float base[4];
    float edge[3][2][4] = {};

    ClipBox(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high) {
        const float* const rows = clip.m.data();
        const float lx = low.data()[0], ly = low.data()[1], lz = low.data()[2];
        const float sizes[3] = {high.data()[0] - lx, high.data()[1] - ly, high.data()[2] - lz};
        for (int r = 0; r < 4; ++r) {
            const float* row = rows + r * 4;
            base[r] = row[0] * lx + row[1] * ly + row[2] * lz + row[3];
            for (int k = 0; k < 3; ++k) {
                edge[k][1][r] = row[k] * sizes[k];
            }
        }
    }

    // One bit for each side of the clip volume corner `c` is outside of.
    unsigned sidesOutside(int c) const {
        const float(&x)[4] = edge[0][c & 1];
        const float(&y)[4] = edge[1][(c >> 1) & 1];
        const float(&z)[4] = edge[2][(c >> 2) & 1];
        float p[4];
        for (int r = 0; r < 4; ++r) {
            p[r] = base[r] + x[r] + y[r] + z[r];
        }
        return unsigned{p[0] < -p[3]} | unsigned{p[0] > p[3]} << 1 | unsigned{p[1] < -p[3]} << 2 | unsigned{p[1] > p[3]} << 3 |
               unsigned{p[2] < 0} << 4 | unsigned{p[2] > p[3]} << 5;
    }
};

} // namespace

InView boxInView(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high) {
    const ClipBox box(clip, low, high);
    unsigned onAll = 63, onAny = 0;
    for (int c = 0; c < 8; ++c) {
        const unsigned sides = box.sidesOutside(c);
        onAll &= sides;
        onAny |= sides;
        if (onAll == 0 && onAny != 0) {
            return InView::Partly;
        }
    }
    return onAll != 0 ? InView::Outside : InView::Inside;
}

bool boxOutside(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high) {
    const ClipBox box(clip, low, high);
    unsigned onAll = 63;
    for (int c = 0; c < 8 && onAll != 0; ++c) {
        onAll &= box.sidesOutside(c);
    }
    return onAll != 0;
}

float nearestDepth(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high, float nearZ) {
    float nearest = std::numeric_limits<float>::max();
    for (int c = 0; c < 8; ++c) {
        const float x = c & 1 ? high[0] : low[0], y = c & 2 ? high[1] : low[1], z = c & 4 ? high[2] : low[2];
        const float w = clip.m[12] * x + clip.m[13] * y + clip.m[14] * z + clip.m[15];
        nearest = std::min(nearest, w > 1e-5f ? w : nearZ);
    }
    return nearest;
}

FrameView FrameView::make(const Camera& camera, unsigned width, unsigned height) {
    FrameView f;
    f.camera = &camera;
    f.aspect = height ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    f.fovY = camera.fovX > 0 ? 2 * std::atan(std::tan(camera.fovX / 2) / f.aspect) : camera.fovY;
    f.nearZ = camera.nearPlane > 0 ? camera.nearPlane : camera.farPlane / 10000;
    const auto& box = camera.orthographic;
    f.viewProjection = (box[1] > box[0] ? Mat4::orthographic(box[0], box[1], box[2], box[3], f.nearZ, camera.farPlane)
                                        : Mat4::perspective(f.fovY, f.aspect, f.nearZ, camera.farPlane)) *
                       Mat4::lookAt(camera.eye, camera.target, camera.up);
    f.sun = normalize({0.4f, -0.5f, 0.75f});
    return f;
}

Mat4 poolMirror(const Mat4& viewProjection, float plane, float keepBelow) {
    Mat4 mirror;
    mirror.m[10] = -1;
    mirror.m[11] = 2 * plane;
    Mat4 r = viewProjection * mirror;
    for (int c = 0; c < 4; ++c) {
        r.m[c] = -r.m[c];
    }
    const std::array<float, 4> above{0, 0, 1, -(plane - keepBelow)};
    for (int c = 0; c < 4; ++c) {
        r.m[8 + c] = r.m[12 + c] - above[c];
    }
    return r;
}

namespace {

// The poolWater-1 material's waves: 5 and 6 repeats a unit along x and y, each 0.01 high, at speed 3.
constexpr float kPoolRepeatX = 5, kPoolRepeatY = 6, kPoolWaveHeight = 0.01f, kPoolWaveSpeed = 3;
// The most the ripples move where a pool surface reads its mirror image, in texture units times w
// (see pool.frag.hlsl).
constexpr float kPoolRippleShift = 0.1f;
// The most murky water's blur reaches, in texture units, at full murk.
constexpr float kPoolMurkReach = 0.016f;

} // namespace

std::array<float, 4> poolMirrorArea(const FrameView& f, std::span<const Draw> blended, unsigned width, unsigned height) {
    constexpr std::array<float, 4> kWhole{-1, 1, -1, 1};
    float x0 = 1, x1 = -1, y0 = 1, y1 = -1, nearest = std::numeric_limits<float>::max(), murk = 0;
    bool any = false;
    for (const Draw& d : blended) {
        if (!d.material || !d.material->poolSurface) {
            continue;
        }
        const GpuPrimitive& p = *d.primitive;
        if (!p.hasBox || width == 0 || height == 0) {
            return kWhole;
        }
        Mat4 model;
        std::copy_n(d.instance->transform.m.begin(), 12, model.m.begin());
        const Mat4 clip = f.viewProjection * model;
        const float lift = 2 * kPoolWaveHeight;
        for (int c = 0; c < 8; ++c) {
            const float x = c & 1 ? p.high[0] : p.low[0], y = c & 2 ? p.high[1] : p.low[1], z = c & 4 ? p.high[2] + lift : p.low[2] - lift;
            const float* m = clip.m.data();
            const float w = m[12] * x + m[13] * y + m[14] * z + m[15];
            // A corner near or behind the camera can land anywhere on the screen.
            if (!(w >= f.nearZ)) {
                return kWhole;
            }
            const float sx = (m[0] * x + m[1] * y + m[2] * z + m[3]) / w, sy = (m[4] * x + m[5] * y + m[6] * z + m[7]) / w;
            x0 = std::min(x0, sx);
            x1 = std::max(x1, sx);
            y0 = std::min(y0, sy);
            y1 = std::max(y1, sy);
            nearest = std::min(nearest, w);
        }
        murk = std::max(murk, std::clamp(d.material->color[3], 0.0f, 1.0f));
        any = true;
    }
    if (!any) {
        return kWhole;
    }
    // Clip space is twice texture units. Two pixels more cover filtering and rounding.
    const float margin = 2 * kPoolRippleShift / nearest + 2 * kPoolMurkReach * murk + 4.0f / static_cast<float>(std::min(width, height));
    // The mirror is flipped left to right.
    return {std::max(-1.0f, -x1 - margin), std::min(1.0f, -x0 + margin), std::max(-1.0f, y0 - margin), std::min(1.0f, y1 + margin)};
}

Mat4 narrowedTo(const Mat4& viewProjection, const std::array<float, 4>& area) {
    Mat4 r = viewProjection;
    const float* m = viewProjection.m.data();
    for (int axis = 0; axis < 2; ++axis) {
        const float low = area[static_cast<std::size_t>(axis * 2)], high = area[static_cast<std::size_t>(axis * 2 + 1)];
        if (!(high - low > 1e-6f)) {
            continue;
        }
        const float scale = 2 / (high - low), shift = (low + high) / (high - low);
        for (int c = 0; c < 4; ++c) {
            r.m[static_cast<std::size_t>(axis * 4 + c)] = scale * m[axis * 4 + c] - shift * m[12 + c];
        }
    }
    return r;
}

std::array<float, 12> skyCamera(const FrameView& f, float up) {
    const Camera& camera = *f.camera;
    const Vec3 forward = normalize(sub(camera.target, camera.eye));
    // A camera looking straight up or down takes its own up for the screen's sides.
    Vec3 side = cross(forward, {0, 0, 1});
    if (dot(side, side) < 1e-8f) {
        side = cross(forward, camera.up);
    }
    const Vec3 s = normalize(side);
    const Vec3 u = cross(s, forward);
    // A square-on view's rays all run the same way.
    const bool square = camera.orthographic[1] != camera.orthographic[0];
    const float ty = square ? 0 : std::tan(f.fovY / 2), tx = ty * f.aspect;
    return {s[0] * tx, s[1] * tx, s[2] * tx, 0, u[0] * ty, u[1] * ty, u[2] * ty, 0, forward[0], forward[1], forward[2], up};
}

std::array<float, 12> mirroredSkyCamera(const FrameView& f) {
    auto sky = skyCamera(f, -1);
    for (int k = 0; k < 3; ++k) {
        sky[static_cast<std::size_t>(k)] = -sky[static_cast<std::size_t>(k)];
    }
    return sky;
}

void FramePlan::collectDraws(const RendererState& s, const ResourceQuery& q, const FrameView& f, std::vector<Draw>& opaque,
                             std::vector<Draw>& layers, std::vector<Draw>& blended) {
    thread_local std::vector<float> depths;
    const auto& meshes = q.meshes();
    // Without projected shadows the game falls back to the models' own shadow groups.
    const bool objectShadows = s.capabilities().projectedShadows;
    for (std::size_t i = 0; s.drawScene_ && i < s.instances_.size(); ++i) {
        const auto& instance = s.instances_[i];
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes.size() || (i < s.visible_.size() && !s.visible_[i])) {
            continue;
        }
        Mat4 model;
        std::copy_n(instance.transform.m.begin(), 12, model.m.begin());
        const Mat4 clip = f.viewProjection * model;
        const GpuMesh& mesh = meshes[static_cast<std::size_t>(instance.mesh)];
        const InView meshInView = mesh.anyCullable ? boxInView(clip, mesh.low, mesh.high) : InView::Inside;
        const std::size_t firstBlended = blended.size();
        for (const auto& p : mesh.primitives) {
            if (p.skinned && (i >= s.jointBase_.size() || s.jointBase_[i] < 0 || !q.skinReady(p.buffers))) {
                continue;
            }
            if (p.cullable && meshInView != InView::Inside &&
                (meshInView == InView::Outside || boxOutside(clip, p.low, p.high))) {
                continue;
            }
            if (p.shadowGroup && (objectShadows ? !scene::objectshadow::showsShadowGroups(scene::objectshadow::inForce(instance.shadow, instance.shadowFacts, s.shadowOptions_))
                                                : !s.shadowOptions_.groups)) {
                continue;
            }
            int material = p.material;
            if (i < s.instanceMaterials_.size() && p.source < s.instanceMaterials_[i].size()) {
                material = s.instanceMaterials_[i][p.source];
                if (material == scene::kHiddenMaterial) {
                    continue;
                }
            }
            const scene::Material* m = material >= 0 && static_cast<std::size_t>(material) < s.materials_.size()
                                           ? &s.materials_[static_cast<std::size_t>(material)]
                                           : nullptr;
            const auto alpha = m ? m->alpha : scene::AlphaMode::Opaque;
            // A shape colour draws the materials that take it blended, as the game turns standard materials to blend mode.
            const bool blends = alpha == scene::AlphaMode::Blend || alpha == scene::AlphaMode::Add || alpha == scene::AlphaMode::Sum ||
                                (instance.shapeColor.has_value() && (!m || m->takesShapeColor));
            const float sortDepth = blends && p.hasBox ? nearestDepth(clip, p.low, p.high, f.nearZ) : clip.m[15];
            (blends ? blended : alpha == scene::AlphaMode::Layer ? layers : opaque)
                .push_back({&p, &instance, m, m ? m->layer : 0, sortDepth, p.source < instance.sun.size() ? instance.sun[p.source] : 1.0f});
        }
        // The game draws an object's blended surfaces of one layer by their groups' sort numbers, the
        // lowest first, and those with the same number from the farthest. A Sim's hair needs this, as
        // its strands write depth. Their depths are handed out again, the farthest to the first, so
        // they keep that order among other objects' surfaces.
        const auto own = blended.begin() + static_cast<std::ptrdiff_t>(firstBlended);
        std::stable_sort(own, blended.end(), [](const Draw& a, const Draw& b) {
            if (a.layer != b.layer) {
                return a.layer < b.layer;
            }
            return a.primitive->alphaSort != b.primitive->alphaSort ? a.primitive->alphaSort < b.primitive->alphaSort : a.depth > b.depth;
        });
        for (auto run = own; run != blended.end();) {
            const auto end = std::find_if(run, blended.end(), [&](const Draw& d) { return d.layer != run->layer; });
            depths.clear();
            for (auto it = run; it != end; ++it) {
                depths.push_back(it->depth);
            }
            std::sort(depths.begin(), depths.end(), std::greater<>());
            for (std::size_t k = 0; run != end; ++run, ++k) {
                run->depth = depths[k];
            }
        }
    }
    // Within a layer, surfaces sharing a material go together, as binding textures again costs more
    // than the sort. The game also sorts by shader and geometry. A lot's roofs go first in the order
    // placed, as their pieces can lie in one plane, where whichever comes last shows.
    std::stable_sort(opaque.begin(), opaque.end(), [](const Draw& a, const Draw& b) {
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        const bool aRoof = a.instance->part == scene::Part::Roof, bRoof = b.instance->part == scene::Part::Roof;
        if (aRoof || bRoof) {
            return aRoof && !bRoof;
        }
        if (a.primitive->skinned != b.primitive->skinned) {
            return b.primitive->skinned;
        }
        if (a.material != b.material) {
            return std::less<>{}(a.material, b.material);
        }
        return a.primitive->buffers != b.primitive->buffers ? a.primitive->buffers < b.primitive->buffers : a.primitive->firstIndex < b.primitive->firstIndex;
    });
    std::stable_sort(blended.begin(), blended.end(),
                     [](const Draw& a, const Draw& b) { return a.layer != b.layer ? a.layer < b.layer : a.depth > b.depth; });
}

DrawUniforms FramePlan::uniformsFor(const RendererState& s, const ResourceQuery& q, const FrameView& f, const Draw& d, int blend) {
    DrawUniforms u;
    SkinnedUniforms& vu = u.vertex;
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), vu.frame.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), vu.frame.model);
    std::copy(f.camera->eye.begin(), f.camera->eye.end(), vu.frame.eye);
    if (const scene::Material* lit = d.material; s.specularHighlights_ && lit && lit->specPower > 0 && !lit->simLighting) {
        std::copy(lit->specCoef.begin(), lit->specCoef.end(), vu.frame.specular);
        vu.frame.specular[3] = lit->specPower;
    }
    const scene::Material* m = d.material;
    const bool skirt = d.instance->part == scene::Part::Skirt;
    const scene::ObjectLighting* lighting = s.objectLighting_ ? &*s.objectLighting_ : nullptr;
    const auto& parts = d.instance->partLightSets;
    const int set = d.primitive->source < parts.size() ? parts[d.primitive->source] : d.instance->lightSet;
    // A portrait lights everything it draws with its own lights, not the room's.
    if (!s.ownLights_ && set >= 0 && static_cast<std::size_t>(set) < s.lightSets_.size()) {
        lighting = &s.lightSets_[static_cast<std::size_t>(set)];
    }
    // A Sim outdoors also gets the lighting state's Sim-only lights.
    if (lighting && !lighting->simLights.empty() && d.instance->lightGroup == scene::LightGroup::Sims) {
        lighting = &simLightingFor(*lighting);
    }
    const bool roof = d.instance->part == scene::Part::Roof && m && m->roofLight && s.objectLighting_;
    // A roof's underside, trim and edges: standard materials, lit as objects outdoors.
    const bool roofEdge = d.instance->part == scene::Part::Roof && m && !m->roofLight && !m->unlit && s.objectLighting_;
    // As the game's standard and Sim materials: on lit surfaces with tangents, the first lights per
    // pixel, through the normal map. Sims' even without one.
    const int normalMap = s.bumpMapping_ && d.primitive->tangents && m && m->normalMap >= 0 && q.hasImage(m->normalMap) ? m->normalMap : -1;
    const bool bumped = s.bumpMapping_ && d.primitive->tangents && m && !m->unlit && (normalMap >= 0 || m->simLighting);
    if (roof || roofEdge || (d.instance->litOutdoors && s.objectLighting_)) {
        lighting = &*s.objectLighting_;
    }
    if (lighting && (d.instance->part == scene::Part::Object || skirt || roof || roofEdge || d.instance->litOutdoors)) {
        setLights(s, u, d, *lighting, skirt, roof, bumped);
    }
    if (d.primitive->skinned) {
        vu.jointBase = static_cast<std::uint32_t>(s.jointBase_[static_cast<std::size_t>(d.instance - s.instances_.data())]);
        vu.morphRow = s.morphRow(static_cast<std::size_t>(d.instance - s.instances_.data()));
        const auto eyes = s.eyesOf(static_cast<std::size_t>(d.instance - s.instances_.data()));
        std::copy(eyes.begin(), eyes.end(), vu.eyes);
    }
    setMaterial(s, q, u, f, d, blend, normalMap, lighting);
    return u;
}

void FramePlan::setLights(const RendererState& s, DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof,
                          bool bumped) {
    SkinnedUniforms& vu = u.vertex;
    const scene::Material* m = d.material;
    for (std::size_t k = 0; k < 3; ++k) {
        vu.frame.ambient[k] = l.ambient[k];
        if (skirt || roof) {
            // The skirt's and roofs' ambient: a quarter of it as a byte (the device's ambient),
            // times the material's ambient (the skirt's own, a roof's four times its diffuse).
            const float device = std::round(std::clamp(l.ambient[k] / 4, 0.0f, 1.0f) * 255) / 255;
            vu.frame.ambient[k] = skirt ? device * d.instance->skirtAmbient : device * 4;
        }
    }
    if (s.highlightObject_ != 0 && d.instance->part == scene::Part::Object && d.instance->object == s.highlightObject_) {
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.ambient[k] += s.highlightAmbient_;
        }
    }
    vu.frame.ambient[3] = skirt ? 3.0f : 1.0f;
    if (skirt) {
        std::copy(d.instance->skirtScale.begin(), d.instance->skirtScale.end(), vu.frame.thumbnail);
        // Which lights are suns, a bit each, for the shadow past the lot's edge. While the light fades
        // into a new state both states' suns are there.
        int suns = 0;
        for (std::size_t i = 0; i < kObjectLights && i < l.lights.size(); ++i) {
            if (l.lights[i].sun) {
                suns |= 1 << i;
            }
        }
        vu.frame.thumbnail[3] = static_cast<float>(suns);
    } else if (roof) {
        // The vertex colour clamped, then doubled by the texture stage: the same as a thumbnail.
        vu.frame.thumbnail[0] = m->color[0];
        vu.frame.thumbnail[1] = m->color[1];
        vu.frame.thumbnail[2] = m->color[2];
        vu.frame.thumbnail[3] = 0.5f;
    } else if (l.thumbnailScale > 0) {
        const auto c = m ? m->color : std::array<float, 4>{1, 1, 1, 1};
        vu.frame.thumbnail[0] = c[0];
        vu.frame.thumbnail[1] = c[1];
        vu.frame.thumbnail[2] = c[2];
        vu.frame.thumbnail[3] = l.thumbnailScale;
    }
    if (bumped && !skirt && !roof && l.thumbnailScale <= 0) {
        const auto chosen = scene::perPixelLights(l, kObjectLights);
        vu.frame.pixelLights[0] = static_cast<float>(chosen.size());
        for (std::size_t k = 0; k < chosen.size(); ++k) {
            vu.frame.pixelLights[k + 1] = static_cast<float>(chosen[k]);
        }
    }
    for (std::size_t i = 0; i < kObjectLights && i < l.lights.size(); ++i) {
        const auto& light = l.lights[i];
        std::copy(light.direction.begin(), light.direction.end(), vu.frame.lightDirections[i]);
        if (light.point) {
            vu.frame.lightDirections[i][3] = light.range * light.range;
            std::copy(light.position.begin(), light.position.end(), vu.frame.lightPositions[i]);
            vu.frame.lightPositions[i][3] = light.falloff;
            if (light.spot) {
                const float width = light.coneInner - light.coneOuter;
                vu.frame.lightCones[i][0] = light.coneOuter;
                vu.frame.lightCones[i][1] = width > 1e-6f ? 1 / width : 1e6f;
                vu.frame.lightCones[i][2] = 1;
            }
        }
        const float share = light.sun && !skirt ? d.sun : 1.0f;
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.lightColors[i][k] = light.color[k] * share;
        }
    }
}

std::size_t FramePlan::endOfLayer(std::span<const Draw> blended, std::size_t from, int layer) {
    while (from < blended.size() && blended[from].layer <= layer) {
        ++from;
    }
    return from;
}

float FramePlan::particleAlphaReference(std::uint8_t draw) {
    return (draw == effects::ParticleBlock::DepthDecal ? 128.0f : 1.0f) / 255;
}

void FramePlan::setMaterial(const RendererState& s, const ResourceQuery& q, DrawUniforms& u, const FrameView& f, const Draw& d, int blend,
                            int normalMap, const scene::ObjectLighting* lighting) {
    const SkinnedUniforms& vu = u.vertex;
    FragmentUniforms& fu = u.fragment;
    const scene::Material* m = d.material;
    const auto c = m ? m->color : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
    std::copy(c.begin(), c.end(), fu.color);
    if (lighting && m && m->takesTextureLight) {
        for (std::size_t i = 0; i < 3; ++i) {
            fu.color[i] *= lighting->textureLight[i];
        }
    }
    if (d.instance->part == scene::Part::Skirt) {
        fu.neighborLight[3] = 2;
    }
    if (d.instance->neighborHouse) {
        fu.neighborLight[0] = s.imposterLight_[0];
        fu.neighborLight[1] = s.imposterLight_[1];
        fu.neighborLight[2] = s.imposterLight_[2];
        fu.neighborLight[3] = 1;
    }
    if (const auto& shape = d.instance->shapeColor; shape && (!m || m->takesShapeColor)) {
        for (std::size_t i = 0; i < 4; ++i) {
            fu.color[i] *= (*shape)[i];
        }
    }
    fu.sunDirection[0] = f.sun[0];
    fu.sunDirection[1] = f.sun[1];
    fu.sunDirection[2] = f.sun[2];
    // A pool's floor or wall takes the outdoors' texture light for its caustics.
    if (m && m->caustics != 0 && s.poolCaustics_) {
        fu.caustics[0] = static_cast<float>(m->caustics);
        const auto light = s.objectLighting_ ? s.objectLighting_->textureLight : std::array<float, 3>{1, 1, 1};
        std::copy(light.begin(), light.end(), fu.caustics + 1);
    }
    TextureRef texture{TextureRef::Kind::White, 0};
    if (m && m->image >= 0 && q.hasImage(m->image)) {
        texture = {TextureRef::Kind::Image, m->image};
        fu.hasTexture = 1;
    }
    fu.alphaCutoff = !m                                     ? 0.0f
                     : m->alpha == scene::AlphaMode::Mask  ? m->alphaReference
                     : m->alpha == scene::AlphaMode::Layer ? 10.0f / 255  // as the game's terrain paint
                     : m->alpha == scene::AlphaMode::Add   ? 1.0f / 255   // as the game's particles
                     : m->alpha == scene::AlphaMode::Sum   ? 0.0f         // its alpha plays no part
                     : m->blendedHair                      ? 60.5f / 255  // the game keeps alpha above 60
                     : blend                               ? 0.01f
                                                           : 0.0f;
    fu.unlit = m && m->unlit ? 1.0f : 0.0f;
    fu.alphaReplicate = m && m->alphaReplicate && fu.hasTexture > 0 ? 1.0f : 0.0f;
    // A lot's floors and walls: lit by their light map alone.
    TextureRef lightMap{TextureRef::Kind::White, 0};
    std::optional<TextureRef> lightMapFrom;
    TextureRef incidence{TextureRef::Kind::White, 0};
    fu.lightMapFade = 1;
    if (const int i = d.instance->lightMap; d.primitive->lightMapped && i >= 0 && q.hasImage(i)) {
        lightMap = {TextureRef::Kind::Image, i};
        if (q.hasFadeFrom(i)) {
            lightMapFrom = TextureRef{TextureRef::Kind::FadeFrom, i};
            fu.lightMapFade = q.lightMapFade();
        }
        fu.lightMap[0] = d.instance->lightMapGain;
        fu.lightMap[1] = d.instance->lightMapOnly ? 1.0f : 0.0f;
        fu.lightMap[2] = d.instance->lightMapMultiply ? 1.0f : 0.0f;
        // Floors and the lot's terrain take the projected shadows of the Sims on their storey. Only on
        // the ground storey does the light map fade them where the buildings shade the sun. Higher
        // storeys get the full shadow colour.
        if (d.instance->part == scene::Part::Floor || d.instance->lightMapMultiply) {
            const int level = d.instance->lightMapMultiply ? s.shadows_.groundLevel : d.instance->level;
            fu.shadowReceiver[0] = 1;
            fu.shadowReceiver[1] = static_cast<float>(level);
            fu.shadowReceiver[2] = level == s.shadows_.groundLevel ? 1.0f : 0.0f;
            fu.shadowReceiver[3] = d.instance->part == scene::Part::Floor && !d.instance->lightMapMultiply ? 1.0f : 0.0f;
        }
        // A floor's or wall's normal map, against its light map's incidence map.
        const int k = d.instance->incidenceMap;
        if (s.bumpMapping_ && s.floorAndWallNormalMapping_ && m && m->normalMap >= 0 && q.hasImage(m->normalMap) && k >= 0 && q.hasImage(k) &&
            !d.instance->lightMapMultiply) {
            normalMap = m->normalMap;
            incidence = {TextureRef::Kind::Image, k};
            fu.lightMap[3] = 1;
        }
    }
    // A lot skirt: the lamps' light around the lot, added to its light.
    if (const int i = d.instance->lightMap; d.instance->part == scene::Part::Skirt && d.instance->skirtLampGain > 0 && i >= 0 && q.hasImage(i)) {
        lightMap = {TextureRef::Kind::Image, i};
        if (q.hasFadeFrom(i)) {
            lightMapFrom = TextureRef{TextureRef::Kind::FadeFrom, i};
            fu.lightMapFade = q.lightMapFade();
        }
        for (std::size_t r = 0; r < 2; ++r) {
            std::copy(d.instance->skirtLampMap[r].begin(), d.instance->skirtLampMap[r].end(), fu.skirtLamps[r]);
        }
        fu.skirtLamps[2][0] = d.instance->skirtLampGain;
    }
    fu.pixelLightCount = static_cast<float>(vu.frame.pixelLights[0]);
    fu.normalMapped = normalMap >= 0 ? 1.0f : 0.0f;
    fu.simLighting = m && m->simLighting && lighting && d.instance->part == scene::Part::Object && vu.frame.thumbnail[3] <= 0 ? 1.0f : 0.0f;
    for (int k = 0; k < static_cast<int>(vu.frame.pixelLights[0]); ++k) {
        const auto& lightColor = vu.frame.lightColors[static_cast<int>(vu.frame.pixelLights[k + 1])];
        std::copy(lightColor, lightColor + 4, fu.pixelLightColors[k]);
    }
    if (m && (m->emissive[0] > 0 || m->emissive[1] > 0 || m->emissive[2] > 0)) {
        std::copy(m->emissive.begin(), m->emissive.end(), fu.emissive);
        fu.emissive[3] = 1;
    }
    // A tiled texture shows one frame at a time, left to right, then over again.
    fu.uvTile[0] = fu.uvTile[1] = 1;
    if (m && m->tiles > 1 && m->tileSpeed > 0) {
        const double frames = std::floor(s.lotTime_ * m->tileSpeed);
        const double frame = frames - std::floor(frames / m->tiles) * m->tiles;
        fu.uvTile[0] = 1.0f / static_cast<float>(m->tiles);
        fu.uvTile[2] = static_cast<float>(frame) * fu.uvTile[0];
    }
    // The environment cube, black for materials without one so the binding rarely changes. A shape
    // colour turns reflections off, as the game's material scripts do.
    TextureRef cube{TextureRef::Kind::BlackCube, 0};
    if (m && m->envCube >= 0 && q.hasEnvCube(m->envCube) && !(d.instance->shapeColor && m->takesShapeColor)) {
        cube = {TextureRef::Kind::EnvCube, m->envCube};
        std::copy(m->envCubeCoef.begin(), m->envCubeCoef.end(), fu.envCube);
        if (lighting && m->envCubeTakesLight) {
            for (std::size_t i = 0; i < 3; ++i) {
                fu.envCube[i] *= lighting->textureLight[i];
            }
        }
        fu.envCube[3] = 1;
        if (m->envCubeLocked) {
            const Camera& camera = *f.camera;
            const Vec3 forward = normalize(sub(camera.target, camera.eye));
            // Looking straight down or up, the world's y stands in for up so right stays defined.
            const Vec3 side = cross(forward, {0, 0, 1});
            const Vec3 right = normalize(dot(side, side) > 1e-8f ? side : cross(forward, {0, 1, 0}));
            const Vec3 up = cross(right, forward);
            std::copy(right.begin(), right.end(), fu.cameraRight);
            std::copy(up.begin(), up.end(), fu.cameraUp);
            std::copy(forward.begin(), forward.end(), fu.cameraForward);
            fu.cameraRight[3] = 1;
        }
    }
    const TextureRef normal = normalMap >= 0 ? TextureRef{TextureRef::Kind::Image, normalMap} : TextureRef{};
    TextureRef mask{TextureRef::Kind::White, 0};
    if (m && m->mask >= 0 && q.hasImage(m->mask)) {
        mask = {TextureRef::Kind::Image, m->mask};
        fu.wallMask[0] = 1;
        fu.wallMask[1] = m->maskMirrored ? 1.0f : 0.0f;
    }
    fu.underwaterShade[0] = m && m->partlyUnderwater ? 1.0f : 0.0f;
    fu.underwaterShade[1] = m && m->seaDepthOverlay ? 0.0f : 1.0f;
    fu.underwaterShade[2] =
        d.instance && (d.instance->part == scene::Part::Skirt || d.instance->skirtSand != 0 || d.instance->name == "terrain") ? 1.0f : 0.0f;
    TextureRef detail{TextureRef::Kind::White, 0};
    if (m && m->terrain) {
        fu.terrainTexture[0] = 1;
        if (m->detail >= 0 && q.hasImage(m->detail)) {
            detail = {TextureRef::Kind::Image, m->detail};
            fu.terrainTexture[1] = 1;
            fu.terrainTexture[2] = m->detailRepeats;
        }
    }
    u.samplers = {{{texture, m && m->clamp ? SamplerKind::Clamped : SamplerKind::Repeat},
                   {lightMap, SamplerKind::Clamped},
                   {normal, SamplerKind::Repeat},
                   {incidence, SamplerKind::Clamped},
                   {lightMapFrom ? *lightMapFrom : lightMap, SamplerKind::Clamped},
                   {cube, SamplerKind::Clamped},
                   {mask, SamplerKind::Clamped},
                   {detail, SamplerKind::Repeat}}};
}

std::vector<std::vector<Draw>> FramePlan::shadowCasters(const RendererState& s, const ResourceQuery& q) {
    std::vector<std::vector<Draw>> out;
    const auto& meshes = q.meshes();
    for (const auto& caster : s.shadows_.casters) {
        if (out.size() == kMaxShadowCasters) {
            break;
        }
        auto& draws = out.emplace_back();
        for (const std::size_t i : caster.instances) {
            if (i >= s.instances_.size() || (i < s.visible_.size() && !s.visible_[i])) {
                continue;
            }
            const auto& instance = s.instances_[i];
            if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes.size() || i >= s.jointBase_.size() || s.jointBase_[i] < 0) {
                continue;
            }
            for (const auto& p : meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
                if (!p.skinned || !q.skinReady(p.buffers)) {
                    continue;
                }
                if (i < s.instanceMaterials_.size() && p.source < s.instanceMaterials_[i].size() &&
                    s.instanceMaterials_[i][p.source] == scene::kHiddenMaterial) {
                    continue;
                }
                draws.push_back({&p, &instance, nullptr, 0, 0, 1});
            }
        }
    }
    return out;
}

SkinnedUniforms FramePlan::shadowCasterUniforms(const RendererState& s, const Draw& d, std::size_t caster) {
    SkinnedUniforms u{};
    const auto& c = s.shadows_.casters[caster];
    const auto camera = c.indoor ? scene::simshadows::indoorCamera(c.root, c.forward) : scene::simshadows::camera(s.shadows_.toSun, c.root);
    std::copy(camera.begin(), camera.end(), u.frame.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), u.frame.model);
    u.jointBase = static_cast<std::uint32_t>(s.jointBase_[static_cast<std::size_t>(d.instance - s.instances_.data())]);
    u.morphRow = s.morphRow(static_cast<std::size_t>(d.instance - s.instances_.data()));
    return u;
}

ShadowUniforms FramePlan::shadowUniforms(const RendererState& s, std::size_t casters) {
    ShadowUniforms u{};
    casters = std::min({casters, s.shadows_.casters.size(), kMaxShadowCasters});
    const std::size_t grounds = std::min(s.neighborGrounds_.size(), kMaxNeighborGrounds);
    for (std::size_t i = 0; i < grounds; ++i) {
        std::copy(s.neighborGrounds_[i].begin(), s.neighborGrounds_[i].end(), u.neighborGround[i]);
    }
    u.neighborGrounds[0] = static_cast<float>(grounds);
    for (std::size_t i = 0; i < casters; ++i) {
        const auto& caster = s.shadows_.casters[i];
        const auto camera = caster.indoor ? scene::simshadows::indoorQuad(caster.root, caster.forward) : scene::simshadows::camera(s.shadows_.toSun, caster.root);
        std::copy(camera.begin(), camera.end(), u.cameras[i]);
        u.casters[i][0] = static_cast<float>(i % kShadowTilesAcross) / kShadowTilesAcross;
        u.casters[i][1] = static_cast<float>(i / kShadowTilesAcross) / kShadowTilesAcross;
        u.casters[i][2] = static_cast<float>(caster.level);
        u.casters[i][3] = caster.indoor ? 1.0f : 0.0f;
    }
    std::copy(s.shadows_.shade.begin(), s.shadows_.shade.end(), u.shade);
    u.shade[3] = static_cast<float>(casters);
    return u;
}

std::array<std::array<float, 4>, 25> FramePlan::outdoorBlurWeights() {
    constexpr int r = scene::simshadows::kOutdoorBlur;
    static_assert(r == 2, "the object blur shader takes 5 x 5 taps");
    std::array<std::array<float, 4>, 25> w{};
    for (int across = -r; across <= r; ++across) {
        for (int up = -r; up <= r; ++up) {
            w[static_cast<std::size_t>((across + r) * (2 * r + 1) + up + r)][0] = static_cast<float>(scene::simshadows::blurWeight(across, up, r)) / 255;
        }
    }
    return w;
}

ShadowBlurUniforms FramePlan::shadowBlurUniforms(const RendererState& s) {
    constexpr int kReach = 3;
    static_assert(scene::simshadows::kOutdoorBlur <= kReach && scene::simshadows::kIndoorBlur == kReach, "the blur shader takes 7 x 7 taps");
    ShadowBlurUniforms u{};
    const int radii[2] = {scene::simshadows::kOutdoorBlur, scene::simshadows::kIndoorBlur};
    for (int k = 0; k < 2; ++k) {
        for (int across = -kReach; across <= kReach; ++across) {
            for (int up = -kReach; up <= kReach; ++up) {
                u.weights[k][(across + kReach) * 7 + up + kReach][0] = static_cast<float>(scene::simshadows::blurWeight(across, up, radii[k])) / 255;
            }
        }
    }
    const std::size_t casters = std::min(s.shadows_.casters.size(), kMaxShadowCasters);
    for (std::size_t i = 0; i < casters; ++i) {
        u.indoor[i][0] = s.shadows_.casters[i].indoor ? 1.0f : 0.0f;
    }
    return u;
}

PoolUniforms FramePlan::poolUniformsFor(const RendererState& s, const FrameView& f, const Draw& d) {
    PoolUniforms u{};
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), u.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), u.model);
    std::copy(f.camera->eye.begin(), f.camera->eye.end(), u.eye);
    // The normal is tilted by repeats times height.
    const float waveX[4] = {kPoolRepeatX, 0, kPoolWaveHeight, kPoolWaveSpeed}, waveY[4] = {0, kPoolRepeatY, kPoolWaveHeight, kPoolWaveSpeed};
    std::copy(std::begin(waveX), std::end(waveX), u.waveX);
    std::copy(std::begin(waveY), std::end(waveY), u.waveY);
    u.waveNormal[0] = -kPoolRepeatX * kPoolWaveHeight;
    u.waveNormal[1] = -kPoolRepeatY * kPoolWaveHeight;
    // The highlight comes from the draw's first light, the sun outdoors.
    if (s.objectLighting_ && !s.objectLighting_->lights.empty()) {
        const auto& sun = s.objectLighting_->lights.front();
        std::copy(sun.direction.begin(), sun.direction.end(), u.light);
        std::copy(sun.color.begin(), sun.color.end(), u.lightColor);
        u.lightColor[3] = 1;
    }
    const float night = d.instance->poolNight;
    u.night[0] = u.night[1] = u.night[2] = night;
    u.night[3] = 1;
    u.time[0] = s.time_;
    // The game builds the water 2.75 lower in its own frame than it shows it.
    u.time[1] = -2.75f;
    if (d.material) {
        std::copy(d.material->color.begin(), d.material->color.end(), u.tint);
    }
    return u;
}

PondUniforms FramePlan::pondUniformsFor(const RendererState& s, const FrameView& f, const Draw& d) {
    PondUniforms u{};
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), u.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), u.model);
    std::copy(f.camera->eye.begin(), f.camera->eye.end(), u.eye);
    const auto light = s.objectLighting_ ? s.objectLighting_->textureLight : std::array<float, 3>{1, 1, 1};
    std::copy(light.begin(), light.end(), u.light);
    u.light[3] = 1;
    // The waterOnTerrain material: the ripple images slide along u in a sine 0.04 times a second
    // and repeat every 4 tiles ($waterbumpMapScale).
    constexpr float kCyclesPerSecond = 0.04f, kTilesPerRepeat = 4;
    u.ripple[0] = static_cast<float>(s.time_);
    u.ripple[1] = 2 * 3.14159265f * kCyclesPerSecond;
    u.ripple[2] = 1 / kTilesPerRepeat;
    return u;
}

const Draw* FramePlan::poolWater(const std::vector<Draw>& blended) {
    // The lot's own pool sets the mirror, and a neighbor's real house only without one.
    const Draw* found = nullptr;
    for (const auto& d : blended) {
        if (d.material && d.material->poolSurface && d.primitive->hasBox) {
            if (!d.instance->neighborHouse) {
                return &d;
            }
            found = found ? found : &d;
        }
    }
    return found;
}

SeaUniforms FramePlan::seaUniformsFor(const RendererState& s, const FrameView& f, const Draw& d) {
    SeaUniforms u{};
    std::copy(f.viewProjection.m.begin(), f.viewProjection.m.end(), u.viewProjection);
    const auto& t = d.instance->transform.m;
    const float model[16] = {t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11], 0, 0, 0, 1};
    std::copy(std::begin(model), std::end(model), u.model);
    std::copy(f.camera->eye.begin(), f.camera->eye.end(), u.eye);
    u.time[0] = s.time_;
    // The lotSkirtSea material's waterTint by day and at night.
    constexpr std::array<float, 3> kDay{-0.3f, 0.1f, 0.1f}, kNight{-0.3f, -0.2f, -0.1f};
    const auto& tint = d.instance->seaNight ? kNight : kDay;
    std::copy(tint.begin(), tint.end(), u.tint);
    u.scale[0] = u.scale[1] = u.scale[2] = 1;
    return u;
}

const Draw* FramePlan::skirtSea(const std::vector<Draw>& blended) {
    for (const auto& d : blended) {
        if (d.material && d.material->skirtSea && d.primitive->hasBox) {
            return &d;
        }
    }
    return nullptr;
}

void FramePlan::poolCasters(const RendererState& s, const ResourceQuery& q, const Mat4& clip, std::vector<Draw>& out) {
    const auto& meshes = q.meshes();
    for (std::size_t i = 0; i < s.instances_.size(); ++i) {
        const auto& instance = s.instances_[i];
        const bool object = instance.part == scene::Part::Object;
        if (instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes.size() ||
            (!object && instance.part != scene::Part::Floor && instance.part != scene::Part::Wall)) {
            continue;
        }
        Mat4 model;
        std::copy_n(instance.transform.m.begin(), 12, model.m.begin());
        const Mat4 toClip = clip * model;
        for (const auto& p : meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
            int material = p.material;
            if (i < s.instanceMaterials_.size() && p.source < s.instanceMaterials_[i].size()) {
                material = s.instanceMaterials_[i][p.source];
            }
            if (material == scene::kHiddenMaterial || p.shadowGroup) {
                continue;
            }
            const scene::Material* m = material >= 0 && static_cast<std::size_t>(material) < s.materials_.size()
                                           ? &s.materials_[static_cast<std::size_t>(material)]
                                           : nullptr;
            // Of floors and walls only the pool's own, and nothing see-through.
            if ((!object && (!m || m->caustics == 0)) || (m && (m->alpha == scene::AlphaMode::Blend || m->alpha == scene::AlphaMode::Add ||
                                                                 m->alpha == scene::AlphaMode::Sum || m->poolSurface))) {
                continue;
            }
            if (p.skinned && (i >= s.jointBase_.size() || s.jointBase_[i] < 0 || !q.skinReady(p.buffers))) {
                continue;
            }
            if (p.hasBox && !p.skinned && boxOutside(toClip, p.low, p.high)) {
                continue;
            }
            out.push_back({&p, &instance, m, 0, 0, 1});
        }
    }
}

PoolLight poolLight(const std::array<float, 12>& t, const std::array<float, 6>& box, const Vec3& toSun) {
    // The sun in the lot's frame: the lot is only turned and moved into the world.
    const Vec3 sun = normalize({t[0] * toSun[0] + t[4] * toSun[1] + t[8] * toSun[2], t[1] * toSun[0] + t[5] * toSun[1] + t[9] * toSun[2],
                                t[2] * toSun[0] + t[6] * toSun[1] + t[10] * toSun[2]});
    const Vec3 forward = scale(sun, -1);
    Vec3 right = cross(forward, {0, 0, 1});
    right = dot(right, right) > 1e-8f ? normalize(right) : Vec3{1, 0, 0};
    const Vec3 up = cross(right, forward);
    float l = std::numeric_limits<float>::max(), r = std::numeric_limits<float>::lowest();
    float near = l, far = r, top = r;
    for (int c = 0; c < 8; ++c) {
        const Vec3 p{c & 1 ? box[3] + 2 : box[0] - 2, c & 2 ? box[4] + 2 : box[1] - 2, c & 4 ? box[5] + 6 : box[2]};
        l = std::min(l, dot(right, p));
        r = std::max(r, dot(right, p));
        top = std::max(top, dot(up, p));
        near = std::min(near, dot(forward, p));
        far = std::max(far, dot(forward, p));
    }
    const float width = std::max(r - l, 1e-3f), depth = std::max(far - near, 1e-3f);
    const float bottom = top - depth;
    Mat4 lot;
    lot.m = {2 / width * right[0], 2 / width * right[1], 2 / width * right[2], -(r + l) / width,
             2 / depth * up[0],    2 / depth * up[1],    2 / depth * up[2],    -(top + bottom) / depth,
             forward[0] / depth,   forward[1] / depth,   forward[2] / depth,   -near / depth,
             0, 0, 0, 1};
    // World to lot: the transpose of the lot's turn, then back by its place.
    Mat4 toLot;
    toLot.m = {t[0], t[4], t[8], -(t[0] * t[3] + t[4] * t[7] + t[8] * t[11]),
               t[1], t[5], t[9], -(t[1] * t[3] + t[5] * t[7] + t[9] * t[11]),
               t[2], t[6], t[10], -(t[2] * t[3] + t[6] * t[7] + t[10] * t[11]),
               0, 0, 0, 1};
    PoolLight out;
    out.toLot = toLot;
    out.clip = lot * toLot;
    Mat4 toTexture;
    toTexture.m = {0.5f, 0, 0, 0.5f, 0, -0.5f, 0, 0.5f, 0, 0, 1, 0, 0, 0, 0, 1};
    out.texture = toTexture * out.clip;
    return out;
}

CausticsUniforms causticsFrame(float seconds, float brightness) {
    // The game's TiledTextureAnimShaderProgram (8, 4, speed 1): the time's fraction picks the row,
    // the fraction of that the column.
    constexpr float kAcross = 8, kDown = 4;
    const float f = seconds - std::floor(seconds);
    const float row = std::floor(f * kDown);
    const float g = f * kDown - row;
    const float column = std::floor(g * kAcross);
    CausticsUniforms u{};
    u.frame[0] = column / kAcross;
    u.frame[1] = row / kDown;
    u.frame[2] = 1 / kAcross;
    u.frame[3] = 1 / kDown;
    u.color[0] = u.color[1] = u.color[2] = brightness;
    u.color[3] = 1;
    return u;
}

} // namespace ts2::render
