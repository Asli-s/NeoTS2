#include "scene/builder.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
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

std::array<float, 3> diffuse(const sg::Material& m) {
    auto c = parseFloats(property(m, "stdMatDiffCoef"));
    c.resize(3, 1.0f);
    return {c[0], c[1], c[2]};
}
} // namespace

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
    Material out;
    out.name = name;
    const auto c = diffuse(m);
    out.color = {c[0], c[1], c[2], 1};
    if (const std::string texture = property(m, "stdMatBaseTextureName");
        !texture.empty() && property(m, "stdMatBaseTextureEnabled") == "true") {
        out.image = image(texture);
        baseTextures_[key] = texture;
    }
    // A Sim's face: its textures (skin, eyes, brows, beard...) drawn one over another, as the game
    // composites them.
    if (const int layers = std::atoi(property(m, "numTexturesToComposite").c_str()); layers > 0) {
        std::vector<std::string> textures;
        for (int i = 0; i < layers; ++i) {
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
        if (const auto ref = parseFloats(property(m, "stdMatAlphaRefValue")); !ref.empty()) {
            out.alphaReference = ref[0] / 255.0f;
        }
    }
    if (const auto multiplier = parseFloats(property(m, "stdMatAlphaMultiplier")); !multiplier.empty()) {
        out.color[3] = multiplier[0];
    }
    // Without lighting the material's coefficients don't apply: the texture shows as it is.
    if (const std::string lighting = property(m, "stdMatLightingEnabled"); lighting == "0" || lighting == "false") {
        out.unlit = true;
        out.color = {1, 1, 1, out.color[3]};
    }
    out.doubleSided = property(m, "stdMatCullMode") == "none";
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

int SceneBuilder::textureMaterial(const std::string& texture, float shade, bool doubleSided) {
    const std::string key = lower(texture);
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
    const std::string key = lower(texture) + " terrain";
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
        } catch (const std::exception&) {
        }
    }
    return materials_[key] = cutMaterial(name, covering, cut, mirrored);
}

int SceneBuilder::cutMaterial(const std::string& name, const std::string& covering, const std::optional<Image>& cut,
                              bool mirrored) {
    std::array<float, 3> color{1, 1, 1};
    std::optional<Image> base;
    if (const auto data = assets_.byName(kTxmt, covering + "_txmt")) {
        try {
            const auto m = sg::parseMaterial(*data);
            color = diffuse(m);
            const std::string texture = property(m, "stdMatBaseTextureName");
            if (!texture.empty()) {
                base = assets_.texture(texture);
            }
        } catch (const std::exception&) {
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

int SceneBuilder::layeredImage(const std::vector<std::string>& textures) {
    std::string key = "layers";
    for (const auto& t : textures) {
        key += "|" + lower(t);
    }
    if (const auto it = images_.find(key); it != images_.end()) {
        return it->second;
    }
    std::optional<Image> out;
    for (const auto& t : textures) {
        const auto layer = assets_.texture(t);
        if (!layer || layer->width == 0 || layer->height == 0) {
            continue;
        }
        if (!out) {
            out = *layer;
            for (std::size_t i = 3; i < out->rgba.size(); i += 4) {
                out->rgba[i] = 255;
            }
            continue;
        }
        // Sampled at the bottom layer's size (nearest texel), drawn over it by its alpha.
        for (std::uint32_t y = 0; y < out->height; ++y) {
            const std::uint32_t ly = static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * layer->height) / out->height);
            for (std::uint32_t x = 0; x < out->width; ++x) {
                const std::uint32_t lx = static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * layer->width) / out->width);
                const std::uint8_t* s = &layer->rgba[(static_cast<std::size_t>(ly) * layer->width + lx) * 4];
                std::uint8_t* d = &out->rgba[(static_cast<std::size_t>(y) * out->width + x) * 4];
                const int a = s[3];
                for (int c = 0; c < 3; ++c) {
                    d[c] = static_cast<std::uint8_t>((s[c] * a + d[c] * (255 - a) + 127) / 255);
                }
            }
        }
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

// Moves a mesh group's vertices (and normals, if any) from the bind pose to `pose`: each follows
// up to four joints, weighted (three weights stored, the fourth what is left). Vertices of joints
// the pose lacks stay where they are.
void skin(const sg::Mesh& mesh, const sg::ElementLink& link, const sg::Group& group, const std::map<int, std::string>& joints,
          const Pose& pose, std::vector<float>& positions, std::vector<float>& normals) {
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
    for (std::size_t v = 0; v < vertices; ++v) {
        float p[3] = {0, 0, 0}, n[3] = {0, 0, 0}, used = 0, left = 1;
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
    }
}

} // namespace

int SceneBuilder::buildModel(const std::string& name, const std::vector<sg::MaterialOverride>& overrides, const Pose* pose) {
    const auto cres = assets_.byName(kCres, name + "_cres");
    if (!cres) {
        ++missingModels;
        return -1;
    }
    std::vector<std::pair<sg::Shape, Transform>> placements;
    sg::ResourceNode tree;
    try {
        tree = sg::parseResourceNode(*cres);
    } catch (const std::exception& ex) {
        assets_.warnings.push_back("model " + name + ": " + ex.what());
        ++missingModels;
        return -1;
    }
    // The model's joints by the index meshes skin to.
    std::map<int, std::string> joints;
    for (const auto& n : tree.nodes) {
        if (n.kind == sg::SceneNode::Kind::Transform && n.joint >= 0 && n.joint < 0x7FFFFFFF) {
            joints.emplace(n.joint, lower(n.name));
        }
    }
    std::vector<Transform> world(tree.nodes.size());
    std::vector<bool> seen(tree.nodes.size(), false);
    auto walk = [&](auto&& self, std::uint32_t index, const Transform& parent, int depth) -> void {
        if (index >= tree.nodes.size() || seen[index] || depth > 64) {
            return;
        }
        seen[index] = true;
        const auto& node = tree.nodes[index];
        world[index] = parent * Transform::of(node.translation, node.rotation);
        if (node.kind == sg::SceneNode::Kind::ShapeRef) {
            for (const auto l : node.shapeLinks) {
                if (l < tree.container.links.size() && tree.container.links[l].type == kShpe) {
                    if (const auto data = assets_.byKey(tree.container.links[l])) {
                        try {
                            placements.emplace_back(sg::parseShape(*data), world[index]);
                        } catch (const std::exception&) {
                        }
                    }
                }
            }
        }
        for (const auto c : node.children) {
            self(self, c, world[index], depth + 1);
        }
    };
    if (!tree.nodes.empty()) {
        walk(walk, 0, Transform{}, 0);
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
                walk(walk, i, Transform{}, 0);
            }
        }
        auto& bones = bones_[lower(name)];
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
                    } catch (const std::exception&) {
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
            const auto gmnd = assets_.byName(kGmnd, nodeName);
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
                if (part == shape.parts.end() || (!drawShadows_ && lowGroup.find("shadow") != std::string::npos) ||
                    group.primitive != sg::kTriangleList || group.link >= data->links.size() || group.indices.empty()) {
                    continue;
                }
                std::string materialName = part->material;
                for (const auto& o : overrides) {
                    if (lower(o.group) == lowGroup) {
                        materialName = o.material;
                    }
                }
                if (lower(materialName).find("_null") != std::string::npos) {
                    continue;
                }
                const auto& link = data->links[group.link];
                Primitive p;
                p.positions = sg::vertexFloats(*data, link, sg::kPositions);
                const std::size_t vertices = p.positions.size() / 3;
                const auto highest = *std::max_element(group.indices.begin(), group.indices.end());
                if (vertices == 0 || highest >= vertices) {
                    continue;
                }
                p.normals = sg::vertexFloats(*data, link, sg::kNormals);
                if (pose != nullptr) {
                    skin(*data, link, group, joints, *pose, p.positions, p.normals);
                }
                for (std::size_t i = 0; i + 2 < p.positions.size(); i += 3) {
                    xf.apply(&p.positions[i], true);
                }
                if (p.normals.size() == p.positions.size()) {
                    for (std::size_t i = 0; i + 2 < p.normals.size(); i += 3) {
                        xf.apply(&p.normals[i], false);
                    }
                } else {
                    p.normals.clear();
                }
                p.uvs = sg::vertexFloats(*data, link, sg::kTextureCoordinates);
                if (p.uvs.size() / 2 != vertices) {
                    p.uvs.clear();
                }
                p.indices.assign(group.indices.begin(), group.indices.end());
                p.material = material(materialName);
                mesh.primitives.push_back(std::move(p));
            }
        }
    }
    if (mesh.primitives.empty()) {
        return -1;
    }
    scene_.meshes.push_back(std::move(mesh));
    return static_cast<int>(scene_.meshes.size() - 1);
}

} // namespace ts2::scene
