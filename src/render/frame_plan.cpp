#include "render/frame_plan.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>

namespace ts2::render {

InView boxInView(const Mat4& clip, const std::array<float, 3>& low, const std::array<float, 3>& high) {
    // The low corner in clip space, and the box's three edges from it.
    float base[4], edge[3][4];
    for (int r = 0; r < 4; ++r) {
        const float* row = &clip.m[static_cast<std::size_t>(r) * 4];
        base[r] = row[0] * low[0] + row[1] * low[1] + row[2] * low[2] + row[3];
        for (int k = 0; k < 3; ++k) {
            edge[k][r] = row[k] * (high[k] - low[k]);
        }
    }
    int outside[6] = {};
    for (int c = 0; c < 8; ++c) {
        float p[4];
        for (int r = 0; r < 4; ++r) {
            p[r] = base[r] + (c & 1 ? edge[0][r] : 0) + (c & 2 ? edge[1][r] : 0) + (c & 4 ? edge[2][r] : 0);
        }
        outside[0] += p[0] < -p[3];
        outside[1] += p[0] > p[3];
        outside[2] += p[1] < -p[3];
        outside[3] += p[1] > p[3];
        outside[4] += p[2] < 0;
        outside[5] += p[2] > p[3];
    }
    if (std::any_of(std::begin(outside), std::end(outside), [](int n) { return n == 8; })) {
        return InView::Outside;
    }
    return std::all_of(std::begin(outside), std::end(outside), [](int n) { return n == 0; }) ? InView::Inside : InView::Partly;
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
    f.viewProjection = Mat4::perspective(f.fovY, f.aspect, f.nearZ, camera.farPlane) *
                       Mat4::lookAt(camera.eye, camera.target, {0, 0, 1});
    f.sun = normalize({0.4f, -0.5f, 0.75f});
    return f;
}

std::array<float, 12> skyCamera(const FrameView& f, float up) {
    const Camera& camera = *f.camera;
    const Vec3 forward = normalize(sub(camera.target, camera.eye));
    const Vec3 s = normalize(cross(forward, {0, 0, 1}));
    const Vec3 u = cross(s, forward);
    const float ty = std::tan(f.fovY / 2), tx = ty * f.aspect;
    return {s[0] * tx, s[1] * tx, s[2] * tx, 0, u[0] * ty, u[1] * ty, u[2] * ty, 0, forward[0], forward[1], forward[2], up};
}

void FramePlan::collectDraws(const RendererState& s, const ResourceQuery& q, const FrameView& f, std::vector<Draw>& opaque,
                             std::vector<Draw>& layers, std::vector<Draw>& blended) {
    std::vector<float> depths;
    const auto& meshes = q.meshes();
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
                (meshInView == InView::Outside || boxInView(clip, p.low, p.high) == InView::Outside)) {
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
            const bool blends = alpha == scene::AlphaMode::Blend || alpha == scene::AlphaMode::Add;
            const float sortDepth = blends && p.hasBox ? nearestDepth(clip, p.low, p.high, f.nearZ) : clip.m[15];
            (blends ? blended : alpha == scene::AlphaMode::Layer ? layers : opaque)
                .push_back({&p, &instance, m, m ? m->layer : 0, sortDepth, p.source < instance.sun.size() ? instance.sun[p.source] : 1.0f});
        }
        // The game draws an object's blended surfaces of one layer from its model's last to its first
        // (a Sim's hair underside first, the outer strands last): their depths are handed out again,
        // the farthest to the first, so they keep that order among other objects' surfaces.
        const auto own = blended.begin() + static_cast<std::ptrdiff_t>(firstBlended);
        std::reverse(own, blended.end());
        std::stable_sort(own, blended.end(), [](const Draw& a, const Draw& b) { return a.layer < b.layer; });
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
    std::stable_sort(opaque.begin(), opaque.end(), [](const Draw& a, const Draw& b) { return a.layer < b.layer; });
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
    if (const scene::Material* lit = d.material; lit && lit->specPower > 0 && !lit->simLighting) {
        std::copy(lit->specCoef.begin(), lit->specCoef.end(), vu.frame.specular);
        vu.frame.specular[3] = lit->specPower;
    }
    const scene::Material* m = d.material;
    const bool skirt = d.instance->part == scene::Part::Skirt;
    const scene::ObjectLighting* lighting = s.objectLighting_ ? &*s.objectLighting_ : nullptr;
    const auto& parts = d.instance->partLightSets;
    const int set = d.primitive->source < parts.size() ? parts[d.primitive->source] : d.instance->lightSet;
    if (set >= 0 && static_cast<std::size_t>(set) < s.lightSets_.size()) {
        lighting = &s.lightSets_[static_cast<std::size_t>(set)];
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
    }
    setMaterial(s, q, u, f, d, blend, normalMap, lighting);
    return u;
}

void FramePlan::setLights(const RendererState& s, DrawUniforms& u, const Draw& d, const scene::ObjectLighting& l, bool skirt, bool roof,
                          bool bumped) {
    SkinnedUniforms& vu = u.vertex;
    const scene::Material* m = d.material;
    for (std::size_t k = 0; k < 3; ++k) {
        // The skirt's and roofs' ambient: a quarter of it as a byte (the device's ambient),
        // times the material's ambient (the skirt's own, a roof's four times its diffuse).
        const float device = std::round(std::clamp(l.ambient[k] / 4, 0.0f, 1.0f) * 255) / 255;
        vu.frame.ambient[k] = skirt ? device * d.instance->skirtAmbient : roof ? device * 4 : l.ambient[k];
    }
    if (s.highlightObject_ != 0 && d.instance->part == scene::Part::Object && d.instance->object == s.highlightObject_) {
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.ambient[k] += s.highlightAmbient_;
        }
    }
    vu.frame.ambient[3] = skirt ? 3.0f : 1.0f;
    if (skirt) {
        std::copy(d.instance->skirtScale.begin(), d.instance->skirtScale.end(), vu.frame.thumbnail);
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
        std::copy(l.lights[i].direction.begin(), l.lights[i].direction.end(), vu.frame.lightDirections[i]);
        if (l.lights[i].point) {
            vu.frame.lightDirections[i][3] = l.lights[i].range * l.lights[i].range;
            std::copy(l.lights[i].position.begin(), l.lights[i].position.end(), vu.frame.lightPositions[i]);
            vu.frame.lightPositions[i][3] = l.lights[i].falloff;
            if (l.lights[i].spot) {
                const float width = l.lights[i].coneInner - l.lights[i].coneOuter;
                vu.frame.lightCones[i][0] = l.lights[i].coneOuter;
                vu.frame.lightCones[i][1] = width > 1e-6f ? 1 / width : 1e6f;
                vu.frame.lightCones[i][2] = 1;
            }
        }
        const float share = l.lights[i].sun && !skirt ? d.sun : 1.0f;
        for (std::size_t k = 0; k < 3; ++k) {
            vu.frame.lightColors[i][k] = l.lights[i].color[k] * share;
        }
    }
}

void FramePlan::setMaterial(const RendererState& s, const ResourceQuery& q, DrawUniforms& u, const FrameView& f, const Draw& d, int blend,
                            int normalMap, const scene::ObjectLighting* lighting) {
    const SkinnedUniforms& vu = u.vertex;
    FragmentUniforms& fu = u.fragment;
    const scene::Material* m = d.material;
    const auto c = m ? m->color : std::array<float, 4>{0.8f, 0.8f, 0.8f, 1};
    std::copy(c.begin(), c.end(), fu.color);
    if (s.night_ && m && m->nightDimmed) {
        fu.color[0] *= 0.35f;
        fu.color[1] *= 0.35f;
        fu.color[2] *= 0.7f;
    }
    fu.sunDirection[0] = f.sun[0];
    fu.sunDirection[1] = f.sun[1];
    fu.sunDirection[2] = f.sun[2];
    TextureRef texture{TextureRef::Kind::White, 0};
    if (m && m->image >= 0 && q.hasImage(m->image)) {
        texture = {TextureRef::Kind::Image, m->image};
        fu.hasTexture = 1;
    }
    fu.alphaCutoff = !m                                     ? 0.0f
                     : m->alpha == scene::AlphaMode::Mask  ? m->alphaReference
                     : m->alpha == scene::AlphaMode::Layer ? 10.0f / 255  // as the game's terrain paint
                     : m->alpha == scene::AlphaMode::Add   ? 1.0f / 255   // as the game's particles
                     : blend                               ? 0.01f
                                                           : 0.0f;
    fu.unlit = m && m->unlit ? 1.0f : 0.0f;
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
        // A floor's or wall's normal map, against its light map's incidence map.
        const int k = d.instance->incidenceMap;
        if (s.bumpMapping_ && s.floorAndWallNormalMapping_ && m && m->normalMap >= 0 && q.hasImage(m->normalMap) && k >= 0 && q.hasImage(k) &&
            !d.instance->lightMapMultiply) {
            normalMap = m->normalMap;
            incidence = {TextureRef::Kind::Image, k};
            fu.lightMap[3] = 1;
        }
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
    // The environment cube, black for materials without one so the binding rarely changes.
    TextureRef cube{TextureRef::Kind::BlackCube, 0};
    if (m && m->envCube >= 0 && q.hasEnvCube(m->envCube)) {
        cube = {TextureRef::Kind::EnvCube, m->envCube};
        std::copy(m->envCubeCoef.begin(), m->envCubeCoef.end(), fu.envCube);
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
    u.samplers = {{{texture, m && m->clamp ? SamplerKind::Clamped : SamplerKind::Repeat},
                   {lightMap, SamplerKind::Clamped},
                   {normal, SamplerKind::Repeat},
                   {incidence, SamplerKind::Clamped},
                   {lightMapFrom ? *lightMapFrom : lightMap, SamplerKind::Clamped},
                   {cube, SamplerKind::Clamped}}};
}

} // namespace ts2::render
