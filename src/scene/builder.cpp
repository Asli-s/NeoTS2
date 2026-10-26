#include "scene/builder.hpp"

#include "diag/log.hpp"
#include "scene/texture_blend.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sg = ts2::scenegraph;

namespace ts2::scene {

namespace {
constexpr std::uint32_t kCres = 0xE519C933;
constexpr std::uint32_t kShpe = 0xFC6EB1F7;
constexpr std::uint32_t kGmnd = 0x7BA3838C;
constexpr std::uint32_t kGmdc = 0xAC4F8687;
constexpr std::uint32_t kTxmt = 0x49596978;

std::vector<float> parseFloats(const std::string& text) {
    std::vector<float> out;
    std::stringstream ss(text);
    std::string part;
    while (std::getline(ss, part, ',')) {
        try {
            out.push_back(std::stof(part));
        } catch (const std::exception&) {
            out.push_back(1.0f);
        }
    }
    return out;
}

std::string property(const sg::Material& m, const char* key) {
    const auto it = m.properties.find(key);
    return it != m.properties.end() ? it->second : std::string();
}

// A whole-number property, 0 when missing or not a number, kept within +-1000 (the game's layers
// are in -32..32 steps of 18 with small additions).
int intProperty(const sg::Material& m, const char* key) {
    const std::string text = property(m, key);
    int v = 0;
    const bool read = std::from_chars(text.data(), text.data() + text.size(), v).ec == std::errc{};
    return read ? std::clamp(v, -1000, 1000) : 0;
}

std::array<float, 3> diffuse(const sg::Material& m) {
    auto c = parseFloats(property(m, "stdMatDiffCoef"));
    c.resize(3, 1.0f);
    return {c[0], c[1], c[2]};
}
// The game's older material types are its standard material with some settings fixed, as its script
// of deprecated material definitions sets them: whether the base texture is used, blending or alpha
// test with the diffuse's alpha as the alpha multiplier, no lighting, the normal map (on only for
// the Bump types), and the diffuse times the material's deprecatedStdMatInvDiffuseCoeffMultiplier.
void asStandardMaterial(sg::Material& m) {
    struct Old {
        const char* type;
        bool texture, blend, test, unlit, additive, bump;
    };
    static constexpr Old kOld[] = {
        {"Phong", false, false, false, false, false, false},
        {"PhongAlpha", false, true, false, false, false, false},
        {"PhongTexture", true, false, false, false, false, false},
        {"PhongTextureMask", true, false, true, false, false, false},
        {"PhongTextureAlpha", true, true, false, false, false, false},
        {"PhongTextureSpec", true, false, false, false, false, false},
        {"PhongTextureSpecMask", true, false, true, false, false, false},
        {"UnlitTexture", true, false, false, true, false, false},
        {"UnlitTextureAlpha", true, true, false, true, false, false},
        {"AdditiveTextureBlend", true, false, false, true, true, false},
        {"AdditiveTextureAlphaBlend", true, false, false, true, true, false},
        {"PhongTextureBump", true, false, false, false, false, true},
        {"PhongTextureMaskBump", true, false, true, false, false, true},
        {"PhongTextureCubeMapReflection", true, false, false, false, false, false},
        {"PhongAlphaCubeMapReflection", false, true, false, false, false, false},
    };
    const auto old = std::find_if(std::begin(kOld), std::end(kOld), [&](const Old& o) { return m.type == o.type; });
    if (old == std::end(kOld)) {
        return;
    }
    auto& p = m.properties;
    auto diff = parseFloats(property(m, "stdMatDiffCoef"));
    diff.resize(4, 1.0f);
    const auto multiplier = parseFloats(property(m, "deprecatedStdMatInvDiffuseCoeffMultiplier"));
    const float scale = multiplier.empty() ? 1.0f : multiplier[0];
    p["stdMatBaseTextureEnabled"] = old->texture ? "true" : "false";
    p["stdMatNormalMapTextureEnabled"] = old->bump ? "true" : "false";
    if (old->additive) {
        p["stdMatAlphaBlendMode"] = "additive";
        const auto strength = parseFloats(property(m, "blendStrength"));
        p["stdMatAlphaMultiplier"] = std::to_string(strength.empty() ? 1.0f : strength[0]);
    } else if (old->blend || old->test) {
        p["stdMatAlphaMultiplier"] = std::to_string(diff[3]);
    }
    if (old->blend) {
        p["stdMatAlphaBlendMode"] = "blend";
    }
    if (old->test) {
        p["stdMatAlphaTestEnabled"] = "true";
        const std::string ref = property(m, "alphaRefValue");
        p["stdMatAlphaRefValue"] = ref.empty() ? "0" : ref;
    }
    if (old->unlit) {
        p["stdMatLightingEnabled"] = "0";
    }
    p["stdMatDiffCoef"] = std::to_string(diff[0] * scale) + "," + std::to_string(diff[1] * scale) + "," + std::to_string(diff[2] * scale) + "," +
                          std::to_string(diff[3]);
}

// The vertices' colors (rgb into `colors`, alpha into `alphas`), when the link has one per vertex.
void vertexColors(const sg::Mesh& mesh, const sg::ElementLink& link, std::size_t vertices, Primitive& p) {
    const sg::Element* e = mesh.find(link, sg::kColors);
    if (e == nullptr || e->blockFormat != 4 || e->data.size() < vertices * 4) {
        return;
    }
    p.colors.resize(vertices * 3);
    p.alphas.resize(vertices);
    for (std::size_t i = 0; i < vertices; ++i) {
        const std::uint8_t* c = &e->data[i * 4];
        p.colors[i * 3] = static_cast<float>(c[2]) / 255;
        p.colors[i * 3 + 1] = static_cast<float>(c[1]) / 255;
        p.colors[i * 3 + 2] = static_cast<float>(c[0]) / 255;
        p.alphas[i] = static_cast<float>(c[3]) / 255;
    }
}
} // namespace

std::optional<EnvCubeSettings> envCubeSettings(const sg::Material& m) {
    const std::string mode = property(m, "stdMatEnvCubeMode");
    EnvCubeSettings out;
    out.texture = property(m, "stdMatEnvCubeTextureName");
    if (mode.empty() || mode == "none" || out.texture.empty()) {
        return std::nullopt;
    }
    auto coef = parseFloats(property(m, "stdMatEnvCubeCoef"));
    coef.resize(3, 0.0f);
    if (!std::all_of(coef.begin(), coef.begin() + 3, [](float c) { return std::isfinite(c); }) ||
        coef[0] + coef[1] + coef[2] < 0.003f) {
        return std::nullopt;
    }
    out.coef = {coef[0], coef[1], coef[2]};
    const std::string locked = property(m, "stdMatEnvCubeLockedToCamera");
    out.lockedToCamera = locked == "true" || intProperty(m, "stdMatEnvCubeLockedToCamera") != 0;
    return out;
}

std::optional<SpecularSettings> specularSettings(const sg::Material& m) {
    const auto power = parseFloats(property(m, "stdMatSpecPower"));
    auto coef = parseFloats(property(m, "stdMatSpecCoef"));
    coef.resize(3, 0.0f);
    if (power.empty() || !std::isfinite(power[0]) || power[0] <= 1 ||
        !std::all_of(coef.begin(), coef.begin() + 3, [](float c) { return std::isfinite(c); }) ||
        coef[0] + coef[1] + coef[2] <= 0.3f) {
        return std::nullopt;
    }
    return SpecularSettings{{coef[0], coef[1], coef[2]}, power[0] * 0.4f};
}

bool isWallShadowGroup(const std::string& group) {
    const std::string low = lower(group);
    return low == "wallshadow" || low == "northwallshadow" || low == "southwallshadow" || low == "eastwallshadow" ||
           low == "westwallshadow";
}

void addVertex(Primitive& p, const std::array<float, 3>& position, const std::array<float, 3>& normal,
               const std::array<float, 2>& uv) {
    p.indices.push_back(static_cast<std::uint32_t>(p.positions.size() / 3));
    p.positions.insert(p.positions.end(), position.begin(), position.end());
    p.normals.insert(p.normals.end(), normal.begin(), normal.end());
    p.uvs.insert(p.uvs.end(), uv.begin(), uv.end());
}

void addQuad(Primitive& p, const std::array<std::array<float, 3>, 4>& c, const std::array<float, 3>& facing,
             const std::array<std::array<float, 2>, 4>& uv) {
    // The quad's normal from all its corners (Newell), so that a quad with two corners in one place
    // (a triangle) is still wound the right way.
    float nx = 0, ny = 0, nz = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto& a = c[i];
        const auto& b = c[(i + 1) % 4];
        nx += (a[1] - b[1]) * (a[2] + b[2]);
        ny += (a[2] - b[2]) * (a[0] + b[0]);
        nz += (a[0] - b[0]) * (a[1] + b[1]);
    }
    const bool flip = nx * facing[0] + ny * facing[1] + nz * facing[2] < 0;
    const int order[2][6] = {{0, 1, 2, 0, 2, 3}, {0, 2, 1, 0, 3, 2}};
    for (const int i : order[flip ? 1 : 0]) {
        addVertex(p, c[static_cast<std::size_t>(i)], facing, uv[static_cast<std::size_t>(i)]);
    }
}

int SceneBuilder::model(const std::string& name, const std::vector<sg::MaterialOverride>& overrides) {
    std::string key = lower(name);
    for (const auto& o : overrides) {
        key += "|" + lower(o.group) + "=" + lower(o.material);
    }
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    return models_[key] = buildModel(name, overrides, nullptr);
}

int SceneBuilder::posedModel(const std::string& name, const std::vector<sg::MaterialOverride>& overrides, const Pose& pose,
                             const std::string& poseName) {
    std::string key = lower(name) + "|pose=" + poseName;
    for (const auto& o : overrides) {
        key += "|" + lower(o.group) + "=" + lower(o.material);
    }
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    return models_[key] = buildModel(name, overrides, &pose);
}

int SceneBuilder::savedPoseModel(const std::string& name, const std::vector<sg::MaterialOverride>& overrides,
                                 std::span<const cobject::SavedJoint> joints, std::string& boneKey) {
    boneKey = name;
    if (joints.empty()) {
        return model(name, overrides);
    }
    auto [known, added] = restJoints_.try_emplace(lower(name));
    if (added) {
        if (const auto cres = assets_.byName(kCres, name + "_cres")) {
            try {
                for (const auto& n : sg::parseResourceNode(*cres).nodes) {
                    if (n.kind == sg::SceneNode::Kind::Transform) {
                        known->second.try_emplace(lower(n.name), RestJoint{n.translation, n.rotation});
                    }
                }
            } catch (const std::exception&) {
                // model() reports the broken model.
            }
        }
    }
    // Only joints that moved count, so that objects saved at rest share the model's own mesh.
    constexpr float kSame = 1e-4f;
    auto near = [&](float a, float b) { return std::fabs(a - b) <= kSame; };
    Pose places;
    for (const auto& j : joints) {
        const std::string low = lower(j.name);
        const auto rest = known->second.find(low);
        if (rest == known->second.end() || !std::ranges::all_of(j.translation, [](float v) { return std::isfinite(v); }) ||
            !std::ranges::all_of(j.rotation, [](float v) { return std::isfinite(v); })) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < 3; ++i) {
            same = same && near(j.translation[i], rest->second.translation[i]);
        }
        // q and -q are the same turn.
        bool sameTurn = true, oppositeTurn = true;
        for (std::size_t i = 0; i < 4; ++i) {
            sameTurn = sameTurn && near(j.rotation[i], rest->second.rotation[i]);
            oppositeTurn = oppositeTurn && near(j.rotation[i], -rest->second.rotation[i]);
        }
        if (!same || !(sameTurn || oppositeTurn)) {
            places.insert_or_assign(low, Transform::of(j.translation, j.rotation));
        }
    }
    if (places.empty()) {
        return model(name, overrides);
    }
    std::string key = lower(name) + "|joints";
    for (const auto& [joint, place] : places) {
        key += "|" + joint;
        for (const float v : place.m) {
            char text[32];
            const auto end = std::to_chars(text, text + sizeof text, v).ptr;
            key += ",";
            key.append(text, end);
        }
    }
    boneKey = key;
    for (const auto& o : overrides) {
        key += "|" + lower(o.group) + "=" + lower(o.material);
    }
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    return models_[key] = buildModel(name, overrides, nullptr, false, &places, boneKey);
}

int SceneBuilder::skinnedModel(const std::string& name, const std::vector<sg::MaterialOverride>& overrides) {
    std::string key = lower(name) + "|skinned";
    for (const auto& o : overrides) {
        key += "|" + lower(o.group) + "=" + lower(o.material);
    }
    if (const auto it = models_.find(key); it != models_.end()) {
        return it->second;
    }
    return models_[key] = buildModel(name, overrides, nullptr, true);
}

const Transform* SceneBuilder::bone(const std::string& model, const std::string& node) const {
    const auto m = bones_.find(lower(model));
    if (m == bones_.end()) {
        return nullptr;
    }
    const auto b = m->second.find(lower(node));
    return b != m->second.end() ? &b->second : nullptr;
}

int SceneBuilder::material(const std::string& name) {
    const std::string key = lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    const auto data = assets_.byName(kTxmt, name + "_txmt");
    if (!data) {
        // A counter's top is named "<top>-cutout-<mask texture>" while something such as a sink sits
        // in it ("-cutout-0" for nothing): the top with the mask's hole cut out, made while playing.
        if (const auto cut = key.find("-cutout-"); cut != std::string::npos) {
            const std::string top = name.substr(0, cut), mask = name.substr(cut + 8);
            const int m = mask == "0" ? -1 : cutMaterial(name, top, assets_.texture(mask), false);
            return materials_[key] = m >= 0 ? m : material(top);
        }
        ++missingMaterials;
        return materials_[key] = -1;
    }
    sg::Material m;
    try {
        m = sg::parseMaterial(*data);
    } catch (const std::exception& ex) {
        assets_.warnings.push_back("material " + name + ": " + ex.what());
        ++missingMaterials;
        return materials_[key] = -1;
    }
    asStandardMaterial(m);
    Material out;
    out.name = name;
    const int stdLayer = intProperty(m, "stdMatLayer");
    // Untextured and unlit, its color and see-through alpha are its vertices' (counters' shadows).
    if (m.type == "VertexAlpha") {
        out.vertexColor = true;
        out.unlit = true;
        out.alpha = AlphaMode::Blend;
        out.layer = stdLayer * 18 + 17;
        out.doubleSided = property(m, "stdMatCullMode") == "none";
        return addMaterial(key, std::move(out));
    }
    const auto c = diffuse(m);
    out.color = {c[0], c[1], c[2], 1};
    if (const std::string texture = property(m, "stdMatBaseTextureName"), enabled = property(m, "stdMatBaseTextureEnabled");
        !texture.empty() && (enabled == "true" || enabled == "1")) {
        out.image = image(texture);
        baseTextures_[key] = texture;
    }
    // A Sim's face: its textures (skin, eyes, brows, beard...) drawn one over another, as the game
    // composites them. Each layer is a property of its own, so a count past the material's
    // properties (a broken file) can't name more.
    const long layerCount = std::strtol(property(m, "numTexturesToComposite").c_str(), nullptr, 10);
    if (const std::size_t layers = std::min(static_cast<std::size_t>(std::max(layerCount, 0L)), m.properties.size());
        layers > 0) {
        std::vector<std::string> textures;
        for (std::size_t i = 0; i < layers; ++i) {
            if (std::string t = property(m, ("baseTexture" + std::to_string(i)).c_str()); !t.empty()) {
                textures.push_back(std::move(t));
            }
        }
        if (const int composite = layeredImage(textures); composite >= 0) {
            out.image = composite;
        }
    }
    auto enabled = [&](const char* key) {
        const std::string v = property(m, key);
        return v == "true" || v == "1";
    };
    if (const std::string blend = property(m, "stdMatAlphaBlendMode"); !blend.empty() && blend != "none") {
        out.alpha = AlphaMode::Blend;
    } else if (enabled("stdMatAlphaTestEnabled")) {
        out.alpha = AlphaMode::Mask;
        // The game keeps alpha at least the reference out of 255. Half a step under it, so an alpha
        // that is 255 in the texture but a hair under 1 once interpolated still passes a 255
        // reference (a crib's bedding, a grill).
        if (const auto ref = parseFloats(property(m, "stdMatAlphaRefValue")); !ref.empty()) {
            out.alphaReference = (ref[0] - 0.5f) / 255.0f;
        }
    }
    if (const auto multiplier = parseFloats(property(m, "stdMatAlphaMultiplier")); !multiplier.empty()) {
        out.color[3] = multiplier[0];
    }
    // Without a texture the material's own alpha stands in for the texture's (window glass is half
    // see-through this way).
    if (out.image < 0) {
        if (const auto alpha = parseFloats(property(m, "stdMatUntexturedDiffAlpha")); !alpha.empty()) {
            out.color[3] *= alpha[0];
        }
    }
    // The game's floor shader takes 0.125 to 64 tiles a repeat.
    for (std::size_t i = 0; i < 2; ++i) {
        if (const auto tiles = parseFloats(property(m, i == 0 ? "floorMaterialScaleU" : "floorMaterialScaleV")); !tiles.empty()) {
            out.floorTiles[i] = std::isfinite(tiles[0]) ? std::clamp(tiles[0], 0.125f, 64.0f) : 1.0f;
        }
    }
    // Without lighting the material's coefficients don't apply: the texture shows as it is.
    if (const std::string lighting = property(m, "stdMatLightingEnabled"); lighting == "0" || lighting == "false") {
        out.unlit = true;
        out.color = {1, 1, 1, out.color[3]};
        // The standard material darkens such a texture at night, unless its texture moves or it glows.
        const std::string anim = property(m, "stdMatTextureCoordAnimMode");
        const auto glow = parseFloats(property(m, "stdMatEmissiveCoef"));
        out.nightDimmed = out.image >= 0 && (anim.empty() || anim == "none") && std::none_of(glow.begin(), glow.end(), [](float c) { return c > 0; });
    }
    // As the game's standard material: a normal map only when lit (the scripts' default bump map is
    // reggrid-bump). The Sims' own material types bump-map through other scripts, not drawn yet.
    const bool standard = m.type == "StandardMaterial" || m.type == "PhongTextureBump" || m.type == "PhongTextureMaskBump";
    // A model drawn by an effect (a plumbob): its material script lights it as the standard material
    // does, with its emissive, its specular and a pass adding its environment cube.
    const bool particleModel = m.type == "StandardParticleModelMaterial";
    if (particleModel && !out.unlit) {
        const auto glow = parseFloats(property(m, "stdMatEmissiveCoef"));
        for (std::size_t i = 0; i < 3 && i < glow.size(); ++i) {
            out.emissive[i] = std::isfinite(glow[i]) ? std::clamp(glow[i], 0.0f, 4.0f) : 0.0f;
        }
    }
    if (standard && enabled("stdMatNormalMapTextureEnabled") && !out.unlit) {
        const std::string bump = property(m, "stdMatNormalMapTextureName");
        out.normalMap = normalMap(bump.empty() ? "reggrid-bump" : bump);
    }
    // Floors bump-map like standard materials, through their incidence map.
    if (m.type == "Floor" && enabled("stdMatNormalMapTextureEnabled")) {
        const std::string bump = property(m, "stdMatNormalMapTextureName");
        out.normalMap = normalMap(bump.empty() ? "reggrid-bump" : bump);
    }
    if (m.type == "Wallpaper" || m.type == "WallpaperBump") {
        out.normalMap = wallNormalMap(m);
    }
    // The Sims' materials. Those not composited take their own normal map, as the Sim shader does
    // (a composited outfit's bump composite is not made yet).
    out.simLighting = m.type == "SimSkin" || m.type == "SimStandardMaterial" || m.type == "SimHair";
    if (m.type == "SimStandardMaterial" && enabled("stdMatNormalMapTextureEnabled") && !out.unlit) {
        const std::string bump = property(m, "stdMatNormalMapTextureName");
        out.normalMap = normalMap(bump.empty() ? "reggrid-bump" : bump);
    }
    if (standard || particleModel) {
        if (const auto cube = envCubeSettings(m); cube) {
            out.envCube = envCube(cube->texture);
            out.envCubeCoef = cube->coef;
            out.envCubeLocked = cube->lockedToCamera;
        }
        if (const auto spec = specularSettings(m); spec && !out.unlit) {
            out.specCoef = spec->coef;
            out.specPower = spec->power;
        }
    }
    out.doubleSided = property(m, "stdMatCullMode") == "none";
    // As the game's material scripts number it: a Sim's hair blends 3 above its opaque layer instead
    // of 17, so it is drawn before other blended surfaces of the same stdMatLayer.
    if (m.type == "SimHair") {
        out.layer = stdLayer * 18 + (out.alpha == AlphaMode::Blend ? 3 : 0);
    } else {
        out.layer = stdLayer * 18 + intProperty(m, "stdMatAddLayer") + (out.alpha == AlphaMode::Blend ? 17 : 0);
    }
    return addMaterial(key, std::move(out));
}

int SceneBuilder::colorMaterial(const std::string& name, const std::array<float, 3>& color, bool doubleSided) {
    const std::string key = lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Material m;
    m.name = name;
    m.color = {color[0], color[1], color[2], 1};
    m.doubleSided = doubleSided;
    return addMaterial(key, std::move(m));
}

int SceneBuilder::blendedColorMaterial(const std::string& name, const std::array<float, 4>& color, int layer) {
    const std::string key = lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Material m;
    m.name = name;
    m.color = color;
    m.alpha = AlphaMode::Blend;
    m.layer = layer;
    m.unlit = true;
    return addMaterial(key, std::move(m));
}

int SceneBuilder::particleMaterial(const std::string& texture, const std::array<float, 4>& color, bool added, int layer) {
    char key[256];
    std::snprintf(key, sizeof key, "%.200s particle %g %g %g %g %d %d", lower(texture).c_str(), color[0], color[1], color[2], color[3],
                  added ? 1 : 0, layer);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Material m;
    m.name = texture;
    m.image = image(texture);
    if (m.image < 0) {
        return -1;
    }
    m.color = color;
    m.alpha = added ? AlphaMode::Add : AlphaMode::Blend;
    m.unlit = true;
    m.doubleSided = true;
    m.clamp = true;
    m.layer = layer;
    return addMaterial(key, std::move(m));
}

int SceneBuilder::textureMaterial(const std::string& texture, float shade, bool doubleSided) {
    const std::string key = lower(texture) + " texture " + std::to_string(shade) + (doubleSided ? " both sides" : "");
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Material m;
    m.name = texture;
    m.color = {shade, shade, shade, 1};
    m.image = image(texture);
    m.doubleSided = doubleSided;
    return addMaterial(key, std::move(m));
}

int SceneBuilder::textureCellMaterial(const std::string& texture, int columns, int rows, int column, int row,
                                      const std::array<float, 3>& color) {
    return textureRegionMaterial(texture, static_cast<double>(column) / columns, static_cast<double>(row) / rows,
                                 static_cast<double>(column + 1) / columns, static_cast<double>(row + 1) / rows, 128, 128,
                                 color);
}

int SceneBuilder::textureRegionMaterial(const std::string& texture, double u0, double v0, double u1, double v1,
                                        std::uint32_t width, std::uint32_t height, const std::array<float, 3>& color) {
    std::ostringstream name;
    name << texture << " region " << u0 << "," << v0 << "," << u1 << "," << v1 << " " << color[0] << "," << color[1] << "," << color[2];
    const std::string key = lower(name.str());
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    Material m;
    m.name = name.str();
    m.color = {color[0], color[1], color[2], 1};
    if (const auto tex = assets_.texture(texture); tex && tex->width > 0 && tex->height > 0) {
        Image region{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto sx = std::min<std::size_t>(tex->width - 1, static_cast<std::size_t>((u0 + (u1 - u0) * (x + 0.5) / width) * tex->width));
                const auto sy = std::min<std::size_t>(tex->height - 1, static_cast<std::size_t>((v0 + (v1 - v0) * (y + 0.5) / height) * tex->height));
                std::copy_n(tex->rgba.begin() + static_cast<std::ptrdiff_t>((sy * tex->width + sx) * 4), 4,
                            region.rgba.begin() + static_cast<std::ptrdiff_t>((static_cast<std::size_t>(y) * width + x) * 4));
            }
        }
        m.image = addImage(std::move(region));
    }
    return addMaterial(key, std::move(m));
}

namespace {

// The color of an image at fractions (u, v) of its size, repeating, between its pixels.
std::array<float, 4> sampleRepeating(const Image& image, double u, double v) {
    const double x = (u - std::floor(u)) * image.width - 0.5, y = (v - std::floor(v)) * image.height - 0.5;
    const double fx = x - std::floor(x), fy = y - std::floor(y);
    auto wrap = [](double c, std::uint32_t n) {
        const auto i = static_cast<std::int64_t>(std::floor(c)) % static_cast<std::int64_t>(n);
        return static_cast<std::size_t>(i < 0 ? i + n : i);
    };
    const std::size_t x0 = wrap(x, image.width), x1 = wrap(x + 1, image.width);
    const std::size_t y0 = wrap(y, image.height), y1 = wrap(y + 1, image.height);
    std::array<float, 4> c{};
    for (std::size_t k = 0; k < 4; ++k) {
        auto at = [&](std::size_t px, std::size_t py) { return static_cast<double>(image.rgba[(py * image.width + px) * 4 + k]); };
        const double top = at(x0, y0) * (1 - fx) + at(x1, y0) * fx, bottom = at(x0, y1) * (1 - fx) + at(x1, y1) * fx;
        c[k] = static_cast<float>(top * (1 - fy) + bottom * fy);
    }
    return c;
}

} // namespace

int SceneBuilder::terrainMaterial(const std::string& texture, int repeats, float shade, bool paint) {
    const std::string key = lower(texture) + " terrain " + std::to_string(repeats) + " " + std::to_string(shade) + (paint ? " paint" : "");
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    const auto base = assets_.texture(texture);
    if (!base || base->width == 0 || base->height == 0) {
        return -1;
    }
    const auto detail = assets_.texture(texture + "_detail");
    constexpr std::uint32_t kSize = 1024;
    Image baked{kSize, kSize, std::vector<std::uint8_t>(static_cast<std::size_t>(kSize) * kSize * 4)};
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const double u = (x + 0.5) / kSize, v = (y + 0.5) / kSize;
            auto c = sampleRepeating(*base, u, v);
            if (detail && detail->width > 0 && detail->height > 0) {
                const auto d = sampleRepeating(*detail, u * repeats, v * repeats);
                const float a = d[3] / 255;
                for (std::size_t k = 0; k < 3; ++k) {
                    c[k] = d[k] * a + c[k] * (1 - a);
                }
            }
            auto* out = &baked.rgba[(static_cast<std::size_t>(y) * kSize + x) * 4];
            for (std::size_t k = 0; k < 3; ++k) {
                out[k] = static_cast<std::uint8_t>(std::clamp(c[k] + 0.5f, 0.0f, 255.0f));
            }
            out[3] = 255;
        }
    }
    Material m;
    m.name = texture + " terrain";
    m.color = {shade, shade, shade, 1};
    m.image = addImage(std::move(baked));
    m.alpha = paint ? AlphaMode::Layer : AlphaMode::Opaque;
    return addMaterial(key, std::move(m));
}

int SceneBuilder::maskedMaterial(const std::string& covering, const std::string& mask, bool mirrored) {
    const std::string name = covering + " cut by " + mask + (mirrored ? " mirrored" : "");
    const std::string key = lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    std::optional<Image> cut;
    if (const auto data = assets_.byName(kTxmt, mask + "_txmt")) {
        try {
            const std::string texture = property(sg::parseMaterial(*data), "stdMatBaseTextureName");
            if (!texture.empty()) {
                cut = assets_.texture(texture);
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("walls: a cutout material that can't be read is left out: ") + ex.what());
        }
    }
    return materials_[key] = cutMaterial(name, covering, cut, mirrored);
}

int SceneBuilder::cutMaterial(const std::string& name, const std::string& covering, const std::optional<Image>& cut,
                              bool mirrored) {
    std::array<float, 3> color{1, 1, 1};
    std::optional<Image> base;
    int normals = -1;
    if (const auto data = assets_.byName(kTxmt, covering + "_txmt")) {
        try {
            const auto m = sg::parseMaterial(*data);
            color = diffuse(m);
            normals = wallNormalMap(m);
            const std::string texture = property(m, "stdMatBaseTextureName");
            if (!texture.empty()) {
                base = assets_.texture(texture);
            }
        } catch (const std::exception& ex) {
            diag::warnOnce(std::string("walls: a wall covering material that can't be read is left out: ") + ex.what());
        }
    }
    if (!base || !cut || cut->width == 0 || cut->height == 0 || base->width == 0 || base->height == 0) {
        return -1;
    }
    // At least as fine as the mask, so that small textures (a painted wall's is 4 pixels square)
    // keep the hole's shape.
    if (base->width < cut->width || base->height < cut->height) {
        const std::uint32_t w = std::max(base->width, cut->width), h = std::max(base->height, cut->height);
        Image finer{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto c = sampleRepeating(*base, (x + 0.5) / w, (y + 0.5) / h);
                for (std::size_t k = 0; k < 4; ++k) {
                    finer.rgba[(static_cast<std::size_t>(y) * w + x) * 4 + k] = static_cast<std::uint8_t>(std::clamp(c[k] + 0.5f, 0.0f, 255.0f));
                }
            }
        }
        base = std::move(finer);
    }
    for (std::uint32_t y = 0; y < base->height; ++y) {
        for (std::uint32_t x = 0; x < base->width; ++x) {
            const std::uint32_t mx = (mirrored ? base->width - 1 - x : x) * cut->width / base->width;
            const std::uint32_t my = y * cut->height / base->height;
            base->rgba[(static_cast<std::size_t>(y) * base->width + x) * 4 + 3] =
                cut->rgba[(static_cast<std::size_t>(my) * cut->width + mx) * 4 + 3] < 128 ? 0 : 255;
        }
    }
    Material m;
    m.name = name;
    m.color = {color[0], color[1], color[2], 1};
    m.image = addImage(std::move(*base));
    m.alpha = AlphaMode::Mask;
    m.normalMap = normals;
    return addMaterial(lower(name), std::move(m));
}

int SceneBuilder::addMesh(const std::string& name, Surfaces surfaces) {
    Mesh mesh;
    mesh.name = name;
    for (auto& [materialIndex, p] : surfaces) {
        if (p.indices.empty()) {
            continue;
        }
        p.material = materialIndex;
        mesh.primitives.push_back(std::move(p));
    }
    if (mesh.primitives.empty()) {
        return -1;
    }
    scene_.meshes.push_back(std::move(mesh));
    return static_cast<int>(scene_.meshes.size() - 1);
}

Instance* SceneBuilder::place(int mesh, const std::string& name, const Transform& transform, Part part, int level) {
    if (mesh < 0) {
        return nullptr;
    }
    Instance& instance = scene_.instances.emplace_back();
    instance.mesh = mesh;
    instance.name = name;
    instance.transform = transform;
    instance.part = part;
    instance.level = level;
    return &instance;
}

int SceneBuilder::image(const std::string& texture) {
    const std::string key = lower(texture);
    if (const auto it = images_.find(key); it != images_.end()) {
        return it->second;
    }
    auto img = assets_.texture(texture);
    return images_[key] = img ? addImage(std::move(*img)) : -1;
}

int SceneBuilder::envCube(const std::string& texture) {
    const std::string key = lower(texture);
    if (const auto it = envCubes_.find(key); it != envCubes_.end()) {
        return it->second;
    }
    auto faces = assets_.textureFaces(texture);
    const bool whole = faces.size() == 6 && std::all_of(faces.begin(), faces.end(), [&](const Image& f) {
        return f.width > 0 && f.width == f.height && f.width == faces.front().width;
    });
    if (!whole) {
        assets_.warnings.push_back("environment cube " + texture + ": not six square faces of one size");
        return envCubes_[key] = -1;
    }
    scene_.envCubes.push_back(std::move(faces));
    return envCubes_[key] = static_cast<int>(scene_.envCubes.size() - 1);
}

int SceneBuilder::wallNormalMap(const sg::Material& m) {
    // The game hands a wall its covering's stdMatNormalMapTextureName whenever the covering has a
    // stdMatNormalMapTextureEnabled property, whatever its value.
    const std::string bump = property(m, "stdMatNormalMapTextureName");
    return m.properties.contains("stdMatNormalMapTextureEnabled") && !bump.empty() ? normalMap(bump) : -1;
}

int SceneBuilder::normalMap(const std::string& texture) {
    const std::string key = lower(texture) + " normals";
    if (const auto it = images_.find(key); it != images_.end()) {
        return it->second;
    }
    auto img = assets_.normalMap(texture);
    return images_[key] = img ? addImage(std::move(*img)) : -1;
}

int SceneBuilder::layeredImage(const std::vector<std::string>& textures) {
    std::string key = "layers";
    for (const auto& t : textures) {
        key += "|" + lower(t);
    }
    if (const auto it = images_.find(key); it != images_.end()) {
        return it->second;
    }
    // As the game composites: at the smallest layer's width and height, each layer taken from its
    // own mip level of that size rather than stretched.
    std::uint32_t width = 0, height = 0;
    for (const auto& t : textures) {
        if (const auto size = assets_.textureSize(t)) {
            width = width == 0 ? size->first : std::min(width, size->first);
            height = height == 0 ? size->second : std::min(height, size->second);
        }
    }
    std::optional<Image> out;
    for (const auto& t : textures) {
        const auto layer = width > 0 && height > 0 ? assets_.texture(t, width, height) : std::nullopt;
        if (!layer) {
            continue;
        }
        if (!out) {
            out = *layer;
            for (std::size_t i = 3; i < out->rgba.size(); i += 4) {
                out->rgba[i] = 255;
            }
            continue;
        }
        textureblend::drawOver(*out, *layer);
    }
    return images_[key] = out ? addImage(std::move(*out)) : -1;
}

int SceneBuilder::layeredMaterial(const std::string& name, const std::string& material, const std::vector<std::string>& under) {
    const std::string key = lower(name);
    if (const auto it = materials_.find(key); it != materials_.end()) {
        return it->second;
    }
    const int base = this->material(material);
    if (base < 0) {
        return -1;
    }
    Material m = scene_.materials[static_cast<std::size_t>(base)];
    m.name = name;
    if (const auto texture = baseTextures_.find(lower(material)); texture != baseTextures_.end()) {
        std::vector<std::string> layers = under;
        layers.push_back(texture->second);
        if (const int composite = layeredImage(layers); composite >= 0) {
            m.image = composite;
        }
    }
    return addMaterial(key, std::move(m));
}

int SceneBuilder::addImage(Image image) {
    scene_.images.push_back(std::move(image));
    return static_cast<int>(scene_.images.size() - 1);
}

int SceneBuilder::addMaterial(const std::string& key, Material m) {
    scene_.materials.push_back(std::move(m));
    return materials_[key] = static_cast<int>(scene_.materials.size() - 1);
}

namespace {

// Moves a mesh group's vertices (and normals and tangents, if any) from the bind pose to `pose`:
// each follows up to four joints, weighted (three weights stored, the fourth what is left).
// Vertices of joints the pose lacks stay where they are. Tangents keep their length, as the game's
// vertex shader turns them by the weighted joints' matrices without normalizing.
void skin(const sg::Mesh& mesh, const sg::ElementLink& link, const sg::Group& group, const std::map<int, std::string>& joints,
          const Pose& pose, std::vector<float>& positions, std::vector<float>& normals, std::vector<float>& tangents) {
    const std::size_t vertices = positions.size() / 3;
    const sg::Element* indices = mesh.find(link, sg::kBlendIndices);
    const sg::Element* weightElement = mesh.find(link, sg::kBlendWeights);
    if (indices == nullptr || weightElement == nullptr || group.bones.empty() || indices->data.size() < vertices * 4 ||
        !link.aliases[0].empty()) {
        return;
    }
    const std::vector<float> weights = sg::floats(*weightElement);
    const std::size_t perVertex = weights.size() / std::max<std::size_t>(vertices, 1);
    // Each of the group's joints: from the bind pose to the pose.
    std::vector<std::optional<Transform>> moves(group.bones.size());
    for (std::size_t b = 0; b < group.bones.size(); ++b) {
        const std::uint16_t j = group.bones[b];
        const auto name = joints.find(j);
        if (j >= mesh.inverseBinds.size() || name == joints.end()) {
            continue;
        }
        const auto at = pose.find(name->second);
        if (at != pose.end()) {
            const auto& inverse = mesh.inverseBinds[j];
            moves[b] = at->second * Transform::of(inverse.translation, inverse.rotation);
        }
    }
    const bool withNormals = normals.size() == positions.size();
    const bool withTangents = tangents.size() == positions.size();
    for (std::size_t v = 0; v < vertices; ++v) {
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, t[3] = {0, 0, 0}, used = 0, left = 1;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint8_t b = indices->data[v * 4 + k];
            float w = k < perVertex ? weights[v * perVertex + k] : left;
            left -= k < perVertex ? w : 0;
            if (b == 0xFF || b >= moves.size() || !moves[b] || w <= 0) {
                continue;
            }
            float q[3] = {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]};
            moves[b]->apply(q, true);
            for (int i = 0; i < 3; ++i) {
                p[i] += w * q[i];
            }
            if (withNormals) {
                float m[3] = {normals[v * 3], normals[v * 3 + 1], normals[v * 3 + 2]};
                moves[b]->apply(m, false);
                for (int i = 0; i < 3; ++i) {
                    n[i] += w * m[i];
                }
            }
            if (withTangents) {
                float m[3] = {tangents[v * 3], tangents[v * 3 + 1], tangents[v * 3 + 2]};
                moves[b]->apply(m, false);
                for (int i = 0; i < 3; ++i) {
                    t[i] += w * m[i];
                }
            }
            used += w;
        }
        if (used <= 0) {
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            positions[v * 3 + i] = p[i] / used;
        }
        if (withNormals) {
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0) {
                for (int i = 0; i < 3; ++i) {
                    normals[v * 3 + i] = n[i] / len;
                }
            }
        }
        if (withTangents) {
            for (int i = 0; i < 3; ++i) {
                tangents[v * 3 + i] = t[i] / used;
            }
        }
    }
}

// Gives a mesh group's vertices their joints and weights for skinning when drawn: up to four joints
// each (three weights stored, the fourth what is left), as entries of the mesh's skin (`skin`,
// added to as needed, `placement` is where the group's shape sits). Weights of joints the model
// doesn't name are dropped and the rest scaled up to one. A vertex left without any follows no
// joint. Returns false (and adds nothing) when the group has no skinning data.
bool skinData(const sg::Mesh& mesh, const sg::ElementLink& link, const sg::Group& group, const std::map<int, std::string>& joints,
              const std::map<std::string, Transform>& rests, const Transform& placement, std::vector<SkinJoint>& skin,
              Primitive& p) {
    const std::size_t vertices = p.positions.size() / 3;
    const sg::Element* indices = mesh.find(link, sg::kBlendIndices);
    const sg::Element* weightElement = mesh.find(link, sg::kBlendWeights);
    if (indices == nullptr || weightElement == nullptr || group.bones.empty() || indices->data.size() < vertices * 4 ||
        !link.aliases[0].empty()) {
        return false;
    }
    const std::vector<float> weights = sg::floats(*weightElement);
    const std::size_t perVertex = weights.size() / std::max<std::size_t>(vertices, 1);
    auto entry = [&](const std::string& joint, const Transform& inverseBind) {
        for (std::size_t i = 0; i < skin.size(); ++i) {
            if (skin[i].joint == joint && skin[i].inverseBind.m == inverseBind.m && skin[i].placement.m == placement.m) {
                return static_cast<int>(i);
            }
        }
        if (skin.size() >= 255) {
            return -1;
        }
        const auto rest = rests.find(joint);
        skin.push_back({joint, inverseBind, placement, rest != rests.end() ? std::optional(rest->second) : std::nullopt});
        return static_cast<int>(skin.size() - 1);
    };
    // The group's joints as skin entries.
    std::vector<int> entries(group.bones.size(), -1);
    for (std::size_t b = 0; b < group.bones.size(); ++b) {
        const std::uint16_t j = group.bones[b];
        const auto name = joints.find(j);
        if (j < mesh.inverseBinds.size() && name != joints.end()) {
            const auto& inverse = mesh.inverseBinds[j];
            entries[b] = entry(name->second, Transform::of(inverse.translation, inverse.rotation));
        }
    }
    p.joints.assign(vertices * 4, 255);
    p.weights.assign(vertices * 4, 0.0f);
    for (std::size_t v = 0; v < vertices; ++v) {
        float used = 0, left = 1;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::uint8_t b = indices->data[v * 4 + k];
            const float w = k < perVertex ? weights[v * perVertex + k] : left;
            left -= k < perVertex ? w : 0;
            if (b == 0xFF || b >= entries.size() || entries[b] < 0 || w <= 0) {
                continue;
            }
            p.joints[v * 4 + k] = static_cast<std::uint8_t>(entries[b]);
            p.weights[v * 4 + k] = w;
            used += w;
        }
        if (used > 0) {
            for (std::size_t k = 0; k < 4; ++k) {
                p.weights[v * 4 + k] /= used;
            }
        } else if (const int none = entry("", Transform{}); none >= 0) {
            p.joints[v * 4] = static_cast<std::uint8_t>(none);
            p.weights[v * 4] = 1;
        }
    }
    return true;
}

} // namespace

int SceneBuilder::buildModel(const std::string& name, const std::vector<sg::MaterialOverride>& overrides, const Pose* pose,
                             bool skinned, const Pose* places, const std::string& boneKey) {
    const auto cres = assets_.byName(kCres, name + "_cres");
    std::vector<std::pair<sg::Shape, Transform>> placements;
    sg::ResourceNode tree;
    if (!cres) {
        // Effects name a shape itself (e.g. "..._shpe"), drawn where the effect places it.
        const auto shpe = assets_.byName(kShpe, name);
        if (!shpe) {
            ++missingModels;
            return -1;
        }
        try {
            placements.emplace_back(sg::parseShape(*shpe), Transform{});
        } catch (const std::exception& ex) {
            assets_.warnings.push_back("shape " + name + ": " + ex.what());
            ++missingModels;
            return -1;
        }
    } else {
        try {
            tree = sg::parseResourceNode(*cres);
        } catch (const std::exception& ex) {
            assets_.warnings.push_back("model " + name + ": " + ex.what());
            ++missingModels;
            return -1;
        }
    }
    // The model's joints by the index meshes skin to, and each joint's rest place in the model.
    std::map<int, std::string> joints;
    std::map<std::string, Transform> rests;
    for (const auto& n : tree.nodes) {
        if (n.kind == sg::SceneNode::Kind::Transform && n.joint >= 0 && n.joint < 0x7FFFFFFF) {
            joints.emplace(n.joint, lower(n.name));
            rests.try_emplace(lower(n.name), Transform::of(n.translation, n.rotation));
        }
    }
    std::vector<Transform> world(tree.nodes.size());
    std::vector<bool> seen(tree.nodes.size(), false);
    // The nearest named joint above each node that draws shapes.
    std::set<std::string> shapeJoints;
    auto walk = [&](auto&& self, std::uint32_t index, const Transform& parent, const std::string& under, int depth) -> void {
        if (index >= tree.nodes.size() || seen[index] || depth > 64) {
            return;
        }
        seen[index] = true;
        const auto& node = tree.nodes[index];
        const Transform rest = Transform::of(node.translation, node.rotation);
        const Transform* placed = nullptr;
        if (places != nullptr && node.kind == sg::SceneNode::Kind::Transform) {
            const auto found = places->find(lower(node.name));
            placed = found != places->end() ? &found->second : nullptr;
        }
        world[index] = parent * (placed != nullptr ? *placed : rest);
        if (node.kind == sg::SceneNode::Kind::ShapeRef) {
            if (!node.shapeLinks.empty()) {
                shapeJoints.insert(under);
            }
            for (const auto l : node.shapeLinks) {
                if (l < tree.container.links.size() && tree.container.links[l].type == kShpe) {
                    if (const auto data = assets_.byKey(tree.container.links[l])) {
                        try {
                            placements.emplace_back(sg::parseShape(*data), world[index]);
                        } catch (const std::exception& ex) {
                            diag::warnOnce(std::string("models: a shape that can't be read is left out: ") + ex.what());
                        }
                    }
                }
            }
        }
        const std::string next = node.kind == sg::SceneNode::Kind::Transform && !node.name.empty() ? lower(node.name) : under;
        for (const auto c : node.children) {
            self(self, c, world[index], next, depth + 1);
        }
    };
    if (!tree.nodes.empty()) {
        walk(walk, 0, Transform{}, {}, 0);
        // Nodes the root didn't reach: start from those no node lists as a child.
        std::vector<bool> isChild(tree.nodes.size(), false);
        for (const auto& n : tree.nodes) {
            for (const auto c : n.children) {
                if (c < isChild.size()) {
                    isChild[c] = true;
                }
            }
        }
        for (std::uint32_t i = 0; i < tree.nodes.size(); ++i) {
            if (!seen[i] && !isChild[i]) {
                walk(walk, i, Transform{}, {}, 0);
            }
        }
        auto& bones = bones_[boneKey.empty() ? lower(name) : boneKey];
        for (std::uint32_t i = 0; i < tree.nodes.size(); ++i) {
            if (seen[i] && !tree.nodes[i].name.empty()) {
                bones.try_emplace(lower(tree.nodes[i].name), world[i]);
            }
        }
    }
    if (placements.empty()) {
        // No usable tree: draw the first shape the model links to, untransformed.
        for (const auto& link : tree.container.links) {
            if (link.type == kShpe) {
                if (const auto data = assets_.byKey(link)) {
                    try {
                        placements.emplace_back(sg::parseShape(*data), Transform{});
                        break;
                    } catch (const std::exception& ex) {
                        diag::warnOnce(std::string("models: a shape that can't be read is left out: ") + ex.what());
                    }
                }
            }
        }
    }
    if (placements.empty()) {
        ++missingModels;
        return -1;
    }
    Mesh mesh;
    mesh.name = name;
    bool skinnedGroups = false;
    for (const auto& [shape, xf] : placements) {
        // With object detail on High the game draws level of detail 0, or the next one a shape has
        // (LODConfig.txt, Graphics Rules "lodOverride 0").
        std::uint32_t detail = UINT32_MAX;
        for (std::size_t i = 0; i < shape.geometryNodes.size(); ++i) {
            detail = std::min(detail, i < shape.detailLevels.size() ? shape.detailLevels[i] : 0u);
        }
        for (std::size_t item = 0; item < shape.geometryNodes.size(); ++item) {
            const std::string& nodeName = shape.geometryNodes[item];
            if ((item < shape.detailLevels.size() ? shape.detailLevels[item] : 0u) != detail) {
                continue;
            }
            // A mirror's bounding box only bounds its reflection and isn't drawn.
            if (lower(nodeName).find("boundingbox") != std::string::npos) {
                continue;
            }
            const auto linked = item < shape.geometryLinks.size() ? shape.geometryLinks[item] : std::nullopt;
            const auto gmnd = !linked                                  ? assets_.byName(kGmnd, nodeName)
                              : *linked < shape.container.links.size() ? assets_.byKey(shape.container.links[*linked])
                                                                       : std::nullopt;
            if (!gmnd) {
                continue;
            }
            std::optional<sg::Mesh> data;
            try {
                for (const auto& link : sg::parseContainer(*gmnd).links) {
                    if (link.type == kGmdc) {
                        if (const auto bytes = assets_.byKey(link)) {
                            data = sg::parseMesh(*bytes);
                        }
                    }
                }
            } catch (const std::exception& ex) {
                assets_.warnings.push_back("mesh of " + name + ": " + ex.what());
            }
            if (!data) {
                continue;
            }
            for (const auto& group : data->groups) {
                const std::string lowGroup = lower(group.name);
                const auto part = std::find_if(shape.parts.begin(), shape.parts.end(),
                                               [&](const sg::ShapePart& p) { return lower(p.group) == lowGroup; });
                if (part == shape.parts.end() || (!drawShadows_ && isWallShadowGroup(lowGroup)) ||
                    group.primitive != sg::kTriangleList || group.link >= data->links.size() || group.indices.empty()) {
                    continue;
                }
                std::string materialName = part->material;
                for (const auto& o : overrides) {
                    if (lower(o.group) == lowGroup) {
                        materialName = o.material;
                    }
                }
                // A "_null" material hides its group. The one exception is blended glass with some
                // opacity (a greenhouse roof's faint panes), drawn as its material says.
                if (lower(materialName).find("_null") != std::string::npos) {
                    const int m = material(materialName);
                    const Material* glass = m >= 0 ? &scene_.materials[static_cast<std::size_t>(m)] : nullptr;
                    if (glass == nullptr || glass->alpha != AlphaMode::Blend || glass->color[3] <= 0) {
                        continue;
                    }
                }
                const auto& link = data->links[group.link];
                Primitive p;
                p.group = lowGroup;
                p.positions = sg::vertexFloats(*data, link, sg::kPositions);
                const std::size_t vertices = p.positions.size() / 3;
                const auto highest = *std::max_element(group.indices.begin(), group.indices.end());
                if (vertices == 0 || highest >= vertices) {
                    continue;
                }
                p.normals = sg::vertexFloats(*data, link, sg::kNormals);
                // Tangents are stored one per vertex, not through the link's aliases (in every mesh of the
                // game their count is the link's vertex count).
                if (const sg::Element* t = data->find(link, sg::kTangents); t != nullptr && t->blockFormat == 2) {
                    p.tangents = sg::floats(*t);
                    if (p.tangents.size() != p.positions.size()) {
                        p.tangents.clear();
                    }
                }
                if (pose != nullptr) {
                    skin(*data, link, group, joints, *pose, p.positions, p.normals, p.tangents);
                }
                // A skinned group is placed by its joints' matrices when drawn.
                const bool placed = skinned && skinData(*data, link, group, joints, rests, xf, mesh.skin, p);
                if (!placed) {
                    for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                        xf.apply(&p.positions[i], true);
                    }
                }
                if (p.normals.size() == p.positions.size()) {
                    for (std::size_t i = 0; !placed && i + 2 < p.normals.size(); i += 3) {
                        xf.apply(&p.normals[i], false);
                    }
                } else {
                    p.normals.clear();
                }
                for (std::size_t i = 0; !placed && i + 2 < p.tangents.size(); i += 3) {
                    xf.apply(&p.tangents[i], false);
                }
                p.uvs = sg::vertexFloats(*data, link, sg::kTextureCoordinates);
                if (p.uvs.size() / 2 != vertices) {
                    p.uvs.clear();
                }
                p.indices.assign(group.indices.begin(), group.indices.end());
                p.material = material(materialName);
                if (p.material >= 0 && scene_.materials[static_cast<std::size_t>(p.material)].vertexColor) {
                    vertexColors(*data, link, vertices, p);
                }
                p.origin = {xf.m[3], xf.m[7], xf.m[11]};
                skinnedGroups = skinnedGroups || !group.bones.empty();
                mesh.primitives.push_back(std::move(p));
            }
        }
    }
    if (mesh.primitives.empty()) {
        return -1;
    }
    if (!skinnedGroups && shapeJoints.size() == 1 && !shapeJoints.begin()->empty()) {
        followJoints_.try_emplace(boneKey.empty() ? lower(name) : boneKey, *shapeJoints.begin());
    }
    scene_.meshes.push_back(std::move(mesh));
    return static_cast<int>(scene_.meshes.size() - 1);
}

const std::string* SceneBuilder::followJoint(const std::string& model) const {
    const auto it = followJoints_.find(lower(model));
    return it != followJoints_.end() ? &it->second : nullptr;
}

} // namespace ts2::scene
