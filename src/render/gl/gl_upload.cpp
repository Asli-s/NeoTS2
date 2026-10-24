#include "diag/log.hpp"
#include "diag/timing.hpp"
#include "render/gl/gl_renderer.hpp"
#include "render/image_levels.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <stdexcept>
#include <thread>
#include <utility>

namespace ts2::render {

void GlRenderer::SceneGpu::release(Graveyard& graveyard) {
    std::lock_guard lock(graveyard.lock);
    const auto give = [&](Texture& t) {
        if (t.id != 0) {
            graveyard.textures.push_back(t.id);
            t.id = 0;
        }
    };
    for (auto& t : textures) give(t);
    textures.clear();
    for (auto& [index, t] : fadingFrom) give(t);
    fadingFrom.clear();
    give(white);
    give(blackCube);
    give(skybox);
    for (auto& t : skyFaces) give(t);
    for (auto& t : envCubes) give(t);
    envCubes.clear();
    for (auto& b : meshBuffers) {
        for (GLuint* id : {&b.vertices, &b.indices}) {
            if (*id != 0) {
                graveyard.buffers.push_back(*id);
                *id = 0;
            }
        }
    }
    meshBuffers.clear();
    meshes.clear();
    for (auto* list : {&terrainPaints, &terrainWeights, &roadTextures, &imposterTextures}) {
        for (auto& t : *list) give(t);
        list->clear();
    }
    for (auto* pair : {&terrainAlphas, &terrainCells}) {
        for (auto& t : *pair) give(t);
    }
    give(terrainLight);
    give(terrainGradient);
    give(ripples);
    give(skyCube);
    for (GLuint* id : {&waterVertices, &waterIndices, &terrainVertices, &terrainIndices, &terrainColors, &roadVertices, &roadIndices, &roadColors, &imposterVertices, &imposterIndices, &curtainVertices, &curtainIndices}) {
        if (*id != 0) {
            graveyard.buffers.push_back(*id);
            *id = 0;
        }
    }
    for (GLuint& id : terrainSliceColors) {
        if (id != 0) {
            graveyard.buffers.push_back(id);
        }
    }
    terrainSliceColors.clear();
    terrainIndexCount = 0;
    waterIndexCount = 0;
    roadRuns.clear();
    imposterDraws.clear();
}

std::size_t GlRenderer::SceneGpu::textureBytes() const {
    std::size_t bytes = white.bytes + blackCube.bytes + skybox.bytes + terrainLight.bytes + terrainGradient.bytes + ripples.bytes + skyCube.bytes;
    for (const auto* list : {&textures, &envCubes, &terrainPaints, &terrainWeights, &roadTextures, &imposterTextures}) {
        for (const Texture& t : *list) {
            bytes += t.bytes;
        }
    }
    for (const Texture& t : skyFaces) {
        bytes += t.bytes;
    }
    for (const auto* pair : {&terrainAlphas, &terrainCells}) {
        for (const Texture& t : *pair) {
            bytes += t.bytes;
        }
    }
    for (const auto& [index, t] : fadingFrom) {
        bytes += t.bytes;
    }
    return bytes;
}

bool GlRenderer::SceneGpu::releaseTextures(Graveyard& graveyard, std::size_t count) {
    std::lock_guard lock(graveyard.lock);
    for (auto* list : {&textures, &imposterTextures}) {
        for (; count > 0 && !list->empty(); --count) {
            if (list->back().id != 0) {
                graveyard.textures.push_back(list->back().id);
            }
            list->pop_back();
        }
    }
    return textures.empty() && imposterTextures.empty();
}

void GlRenderer::markNeeded(const scene::Scene& scene, std::vector<bool>& needed) {
    needed.assign(scene.images.size(), false);
    const auto mark = [&](int index) {
        if (index >= 0 && static_cast<std::size_t>(index) < needed.size()) {
            needed[static_cast<std::size_t>(index)] = true;
        }
    };
    for (const auto& material : scene.materials) {
        mark(material.image);
    }
    if (scene.lightMaps) {
        for (const int floor : scene.lightMaps->floors) {
            mark(floor);
        }
        mark(scene.lightMaps->walls);
    }
}

void GlRenderer::reportScene(const SceneGpu& gpu, const char* when) const {
    if (!subset_ || gpu.textures.size() < 20) {
        return;
    }
    struct Bucket {
        std::size_t count = 0, bytes = 0;
    };
    std::map<unsigned, std::size_t> formats;
    struct Group {
        std::map<std::pair<unsigned, unsigned>, Bucket> sizes;
        std::size_t count = 0, bytes = 0;
    } drawn, idle;
    for (std::size_t i = 0; i < gpu.textures.size(); ++i) {
        const Texture& t = gpu.textures[i];
        if (t.id == 0) {
            continue;
        }
        ++formats[t.bits];
        Group& g = gpu.drawnImages.contains(static_cast<int>(i)) ? drawn : idle;
        Bucket& b = g.sizes[{t.width, t.height}];
        ++b.count;
        b.bytes += t.bytes;
        ++g.count;
        g.bytes += t.bytes;
    }
    const auto describe = [](const Group& g) {
        std::vector<std::pair<std::pair<unsigned, unsigned>, Bucket>> sorted(g.sizes.begin(), g.sizes.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        std::string text;
        for (std::size_t i = 0; i < sorted.size() && i < 5; ++i) {
            text += std::format("{}{}x{} x{} = {:.1f} MiB", text.empty() ? "" : ", ", sorted[i].first.first, sorted[i].first.second, sorted[i].second.count,
                                static_cast<double>(sorted[i].second.bytes) / (1024 * 1024));
        }
        return text;
    };
    constexpr double kMiB = 1024.0 * 1024.0;
    std::string formatText;
    for (const auto& [bits, n] : formats) {
        formatText += std::format("{}{} at {} bits", formatText.empty() ? "" : ", ", n, bits);
    }
    diag::info(std::format("images {} by format: {}", when, formatText));
    // What the drawn images would take at the size the screen needed of each (its sides as the next power of two up,
    // no larger than it is), as 16-bit texels, as 8-bit palettes and as 4-bit ones, each with its smaller levels.
    std::map<std::pair<unsigned, unsigned>, Bucket> needs;
    double bits16 = 0, bits8 = 0, bits4 = 0, unmeasuredBytes = 0;
    std::size_t measured = 0, unmeasured = 0;
    for (std::size_t i = 0; i < gpu.textures.size(); ++i) {
        const Texture& t = gpu.textures[i];
        if (t.id == 0 || !gpu.drawnImages.contains(static_cast<int>(i))) {
            continue;
        }
        const auto need = gpu.neededPixels.find(static_cast<int>(i));
        if (need == gpu.neededPixels.end()) {
            ++unmeasured;
            unmeasuredBytes += static_cast<double>(t.bytes);
            continue;
        }
        const auto side = [](float pixels, unsigned native) {
            return std::clamp(std::bit_ceil(static_cast<unsigned>(std::max(1.0f, std::ceil(pixels)))), 8u, std::max(8u, native));
        };
        const unsigned w = side(need->second[0], t.width), h = side(need->second[1], t.height);
        const double texels = static_cast<double>(w) * h * 4 / 3;
        ++measured;
        bits16 += texels * 2;
        bits8 += texels + 1024;
        bits4 += texels / 2 + 32;
        Bucket& b = needs[{w, h}];
        ++b.count;
        b.bytes += static_cast<std::size_t>(texels * 2);
    }
    if (measured > 0) {
        Group sizes;
        sizes.sizes = needs;
        diag::info(std::format("images {} at the size a {}-pixel-wide screen needed: {} measured, {:.1f} MiB as 16-bit texels, {:.1f} as 8-bit palettes, {:.1f} as "
                               "4-bit palettes ({}); {} not measured, {:.1f} MiB as they are",
                               when, consoleWidth_, measured, bits16 / kMiB, bits8 / kMiB, bits4 / kMiB, describe(sizes), unmeasured, unmeasuredBytes / kMiB));
    }
    diag::info(std::format("images {}: {} on the GPU, {:.1f} MiB. Drawn: {} ({:.1f} MiB; {}). Never drawn: {} ({:.1f} MiB; {})", when,
                           drawn.count + idle.count, static_cast<double>(drawn.bytes + idle.bytes) / kMiB, drawn.count, static_cast<double>(drawn.bytes) / kMiB,
                           describe(drawn), idle.count, static_cast<double>(idle.bytes) / kMiB, describe(idle)));
}

void GlRenderer::reportResident() {
    if (!subset_) {
        return;
    }
    std::size_t sprites = 0;
    for (const Texture& t : spriteTextures_) {
        sprites += t.bytes;
    }
    subset_->resident("scene", gpu_->textureBytes());
    subset_->resident("sprites", sprites);
}

void GlRenderer::GpuPrepared::releaseSome(std::size_t count) {
    if (gpu && graveyard && gpu->releaseTextures(*graveyard, count)) {
        release();
    }
}

void GlRenderer::GpuPrepared::release() {
    if (gpu && graveyard) {
        gpu->release(*graveyard);
    }
    gpu.reset();
    staged.reset();
}

// Deletes what prepared scenes gave up, on the drawing thread.
void GlRenderer::collectGarbage() {
    std::vector<GLuint> textures, buffers;
    {
        std::lock_guard lock(graveyard_->lock);
        textures.swap(graveyard_->textures);
        buffers.swap(graveyard_->buffers);
    }
    if (!textures.empty()) {
        gl_.DeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    }
    if (!buffers.empty()) {
        gl_.DeleteBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    }
}

void GlRenderer::release(Texture& texture) {
    if (texture.id != 0) {
        gl_.DeleteTextures(1, &texture.id);
        texture.id = 0;
    }
}

void GlRenderer::releaseBuffer(GLuint& buffer) {
    if (buffer != 0) {
        gl_.DeleteBuffers(1, &buffer);
        buffer = 0;
    }
}

// A texture of a complete image, with mip levels unless `mips` is false.
GlRenderer::Texture GlRenderer::uploadTexture(const scene::Image& original, bool mips, bool anisotropic) {
    if (fixed_ && !fitsTexture(original.width, original.height, limits_)) {
        // Textures can repeat, which padding would break, so the image is resampled to sides the hardware takes.
        return uploadTexture(stretched(original, fitSide(original.width, limits_), fitSide(original.height, limits_)), mips, anisotropic);
    }
    // To see a palette's worth of colours (and not for light maps, which have no smaller levels): the colours it would keep.
    scene::Image reduced;
    unsigned paletteUsed = 0;
    if (fixed_ && (paletteBits_ != 0 || paletteAuto_) && mips && complete(original)) {
        QuantizeOptions options;
        options.iterations = 3;
        options.dither = dither_;
        if (paletteAuto_) {
            // 4 bits where that costs little, 8 where 4 won't do, and the texels as they are where neither will.
            scene::Image four = quantized(original, 16, options);
            if (imageError(original, four) <= static_cast<double>(paletteError_)) {
                reduced = std::move(four);
                paletteUsed = 4;
            } else if (scene::Image eight = quantized(original, 256, options); imageError(original, eight) <= static_cast<double>(paletteError_) / 2) {
                reduced = std::move(eight);
                paletteUsed = 8;
            }
        } else {
            reduced = quantized(original, 1u << paletteBits_, options);
            paletteUsed = paletteBits_;
        }
    }
    const scene::Image& image = reduced.rgba.empty() ? original : reduced;
    Texture t;
    t.target = GL_TEXTURE_2D;
    checkTexture("image", GL_TEXTURE_2D, image.width, image.height);
    t.mips = mips && (image.width > 1 || image.height > 1);
    gl_.GenTextures(1, &t.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_2D, t.id);
    forgetBindings();
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, t.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    t.wrap = GL_REPEAT;
    if (t.mips && anisotropic && gl_.anisotropy) {
        gl_.TexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, std::min(8.0f, gl_.maxAnisotropy));
    }
    const bool own = t.mips && ownLevels(image);
    // The driver makes the smaller levels as the first is uploaded, unless the image has its own.
    if (t.mips && !own) {
        gl_.TexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    }
    const GLint format = internalFormatFor(image.rgba.data(), std::size_t{image.width} * image.height);
    t.bytes = bytesFor(std::size_t{image.width} * image.height, t.mips, paletteUsed);
    t.bits = paletteUsed != 0 ? paletteUsed : sixteenBit_ ? 16 : 32;
    t.width = image.width;
    t.height = image.height;
    gl_.TexImage2D(GL_TEXTURE_2D, 0, format, static_cast<GLsizei>(image.width), static_cast<GLsizei>(image.height), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, image.rgba.data());
    if (own) {
        std::uint32_t w = image.width, h = image.height;
        for (std::size_t level = 0; level < image.smallerLevels.size(); ++level) {
            w = std::max(1u, w / 2);
            h = std::max(1u, h / 2);
            gl_.TexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level + 1), format, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, image.smallerLevels[level].data());
        }
    }
    return t;
}

GlRenderer::Texture GlRenderer::uploadStarter(SceneGpu& gpu, int index, scene::Image image) {
    constexpr unsigned kStart = 32;
    scene::Image& full = gpu.sources[index];
    full = std::move(image);
    if (full.width <= kStart && full.height <= kStart) {
        return uploadTexture(full, true);
    }
    return uploadTexture(stretched(full, std::min(full.width, kStart), std::min(full.height, kStart)), true);
}

void GlRenderer::growTexture(int image, const std::array<float, 2>& texels) {
    constexpr unsigned kPerFrame = 8;
    const auto source = gpu_->sources.find(image);
    if (source == gpu_->sources.end() || grown_ >= kPerFrame || image < 0 || static_cast<std::size_t>(image) >= gpu_->textures.size()) {
        return;
    }
    const scene::Image& full = source->second;
    Texture& current = gpu_->textures[static_cast<std::size_t>(image)];
    const auto side = [&](float pixels, unsigned native) {
        const unsigned asked = std::bit_ceil(static_cast<unsigned>(std::max(1.0f, std::ceil(pixels))));
        return std::clamp(asked, limits_.minSide, std::max(limits_.minSide, std::min(native, limits_.maxSide)));
    };
    const unsigned w = std::max(side(texels[0], full.width), current.width), h = std::max(side(texels[1], full.height), current.height);
    if (w <= current.width && h <= current.height) {
        return;
    }
    Texture fresh = uploadTexture(w == full.width && h == full.height ? full : stretched(full, w, h), true);
    release(current);
    current = fresh;
    ++grown_;
}

GlRenderer::Texture GlRenderer::lightPlaceholder() {
    return uploadTexture(scene::Image{1, 1, {255, 255, 255, 255}, {}}, false);
}

void GlRenderer::setVertexLightMaps(bool on) {
    if (on == vertexLightMaps_) {
        return;
    }
    vertexLightMaps_ = on;
    if (!fixed_) {
        return;
    }
    // The light maps are needed on the GPU only by the pass that multiplies them in. The surfaces count as
    // light-mapped only while their texture exists, so with the light taken at the vertices it is a tiny stand-in.
    for (const auto& [index, image] : gpu_->lightImages) {
        if (index < 0 || static_cast<std::size_t>(index) >= gpu_->textures.size()) {
            continue;
        }
        Texture fresh = on ? lightPlaceholder() : uploadTexture(image, false);
        release(gpu_->textures[static_cast<std::size_t>(index)]);
        gpu_->textures[static_cast<std::size_t>(index)] = fresh;
    }
    reportResident();
}

GLint GlRenderer::internalFormatFor(const std::uint8_t* rgba, std::size_t texels) const {
    if (!sixteenBit_) {
        return GL_RGBA8;
    }
    for (std::size_t i = 0; i < texels; ++i) {
        if (rgba[i * 4 + 3] != 255) {
            return GL_RGBA4;
        }
    }
    return GL_RGB5;
}

void GlRenderer::uploadSkyFaces(std::array<Texture, 6>& faces, const std::vector<scene::Image>& images) {
    if (images.size() != 6 || images.front().width == 0 || images.front().width != images.front().height) {
        return;
    }
    for (const auto& image : images) {
        if (image.width != images.front().width || image.height != images.front().height || !complete(image)) {
            return;
        }
    }
    for (std::size_t k = 0; k < 6; ++k) {
        faces[k] = uploadTexture(images[k], false);
    }
}

// A cube map of six faces of one size. Empty when the faces are not that.
GlRenderer::Texture GlRenderer::uploadCube(const std::vector<scene::Image>& faces, bool mips) {
    Texture t;
    if (faces.size() != 6 || faces.front().width == 0 || faces.front().width != faces.front().height) {
        return t;
    }
    const auto& face = faces.front();
    if (face.width > static_cast<std::uint32_t>(gl_.maxTextureSize)) {
        return t;
    }
    for (const auto& f : faces) {
        if (f.width != face.width || f.height != face.height || !complete(f)) {
            return t;
        }
    }
    t.target = GL_TEXTURE_CUBE_MAP;
    checkTexture("cube map", GL_TEXTURE_CUBE_MAP, face.width, face.height);
    t.wrap = GL_CLAMP_TO_EDGE;
    gl_.GenTextures(1, &t.id);
    gl_.ActiveTexture(GL_TEXTURE0);
    gl_.BindTexture(GL_TEXTURE_CUBE_MAP, t.id);
    forgetBindings();
    gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    t.mips = mips && face.width > 1;
    t.bytes = bytesFor(std::size_t{face.width} * face.height * 6, t.mips);
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, t.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (t.mips) {
        gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_GENERATE_MIPMAP, GL_TRUE);
    }
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_.TexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (GLenum l = 0; l < 6; ++l) {
        gl_.TexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + l, 0, GL_RGBA8, static_cast<GLsizei>(face.width), static_cast<GLsizei>(face.height), 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, faces[l].rgba.data());
    }
    return t;
}

GlRenderer::MeshBuffers GlRenderer::uploadMeshBuffers(MeshData& data) {
    MeshBuffers b;
    if (data.indices.empty()) {
        return b;
    }
    if (fixed_) {
        b.vertexCount = data.vertices.size() / kVertexFloats;
        // Only what the GPU draws with: positions, texture coordinates and light-map coordinates. The normals,
        // colours and alpha light the vertices on the CPU, from the copy kept below.
        b.vertices = uploadBlocks(data.vertices.data(), b.vertexCount, kVertexFloats, {{{0, 3}}, {{6, 2}}, {{9, 2}}});
    } else {
        b.vertices = uploadBuffer(GL_ARRAY_BUFFER, data.vertices.data(), data.vertices.size() * sizeof(float));
    }
    b.indices = uploadIndices(data.indices, b.wideIndices);
    if (fixed_ && (subset_ || detail_ > 0)) {
        b.cpuIndices = data.indices;
    }
    // Skinned primitives are posed on the CPU from the vertices as they were made. Without shaders
    // every primitive is lit on the CPU from them too.
    if (!data.skin.empty() || fixed_) {
        b.cpuVertices = std::move(data.vertices);
    }
    if (!data.skin.empty()) {
        b.cpuSkin = std::move(data.skin);
    }
    return b;
}

// What a scene is uploaded from, gathered on any thread.
std::unique_ptr<GlRenderer::Staged> GlRenderer::stage(const scene::Scene& scene, bool copyImages) {
    auto s = std::make_unique<Staged>();
    if (copyImages) {
        s->images = scene.images;
    } else {
        s->borrowed = &scene.images;
    }
    if (scene.lightMaps) {
        for (const int i : scene.lightMaps->floors) s->noMips.insert(static_cast<std::size_t>(i));
        for (const int i : scene.lightMaps->floorIncidence) s->noMips.insert(static_cast<std::size_t>(i));
        s->noMips.insert(static_cast<std::size_t>(scene.lightMaps->walls));
        s->noMips.insert(static_cast<std::size_t>(scene.lightMaps->wallIncidence));
    }
    reserveMeshData(scene.meshes, 0, s->meshData);
    for (const auto& mesh : scene.meshes) {
        s->meshes.push_back(packMesh(mesh, scene.materials.size(), 0, s->meshData));
    }
    if (s->meshData.tooBig) {
        diag::warning("renderer: the scene's meshes are too big for one GPU buffer; some are left out");
    }
    markNeeded(scene, s->needed);
    if (scene.lightMaps) {
        s->lightMaps.assign(scene.lightMaps->floors.begin(), scene.lightMaps->floors.end());
        s->lightMaps.push_back(scene.lightMaps->walls);
    }
    s->skybox = scene.skybox;
    s->envCubes = scene.envCubes;
    if (scene.terrain && scene.terrain->paints() > 0 && !scene.terrain->textures.empty()) {
        s->terrain = scene.terrain;
    }
    s->clip = scene.clip;
    return s;
}

void GlRenderer::uploadStaged(SceneGpu& gpu, Staged& staged) {
    diag::Timer timer("upload scene");
    // The images are the staged copy's own, to be let go as each is on the GPU, or the scene's (see stage).
    const std::vector<scene::Image>& images = staged.imageList();
    const bool owned = staged.borrowed == nullptr;
    gpu.textures.reserve(images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        const scene::Image& image = images[i];
        const bool wanted = !fixed_ || (i < staged.needed.size() && staged.needed[i]);
        if (wanted && fixed_ && detail_ > 0 && !staged.noMips.contains(i) && complete(image)) {
            gpu.textures.push_back(uploadStarter(gpu, static_cast<int>(i), owned ? std::move(staged.images[i]) : scene::Image(image)));
            if (owned) {
                staged.images[i] = scene::Image{};
            }
            continue;
        }
        const bool lightMap = fixed_ && std::find(staged.lightMaps.begin(), staged.lightMaps.end(), static_cast<int>(i)) != staged.lightMaps.end();
        if (lightMap && vertexLightMaps_ && complete(image)) {
            gpu.lightImages[static_cast<int>(i)] = owned ? std::move(staged.images[i]) : scene::Image(image);
            gpu.textures.push_back(lightPlaceholder());
            if (owned) {
                staged.images[i] = scene::Image{};
            }
            continue;
        }
        gpu.textures.push_back(wanted && complete(image) ? uploadTexture(image, !staged.noMips.contains(i)) : Texture{});
        // Without shaders the light that floors and walls take is taken from their light maps on the CPU.
        if (lightMap) {
            gpu.lightImages[static_cast<int>(i)] = image;
        }
        // The pixels are on the GPU: the copy made to carry them here isn't needed any more.
        if (owned) {
            staged.images[i] = scene::Image{};
        }
    }
    gpu.white = uploadTexture({1, 1, {255, 255, 255, 255}, {}}, false);
    gpu.meshes = std::move(staged.meshes);
    gpu.meshBuffers.push_back(uploadMeshBuffers(staged.meshData));
    if (fixed_) {
        uploadSkyFaces(gpu.skyFaces, staged.skybox);
    } else {
        gpu.skybox = uploadCube(staged.skybox);
    }
    // Without shaders there are no environment cubes: empty ones keep the scene's indexes.
    for (const auto& faces : staged.envCubes) {
        gpu.envCubes.push_back(fixed_ ? Texture{} : uploadCube(faces));
    }
    if (!fixed_) {
        gpu.blackCube = uploadCube(std::vector<scene::Image>(6, scene::Image{1, 1, {0, 0, 0, 255}, {}}));
    }
    if (staged.terrain) {
        uploadTerrain(gpu, *staged.terrain);
    }
    if (staged.clip) {
        const IndexedMesh curtain = boundaryCurtain(*staged.clip);
        gpu.curtainVertices = uploadBuffer(GL_ARRAY_BUFFER, curtain.vertices.data(), curtain.vertices.size() * sizeof(float));
        gpu.curtainIndices = uploadIndices(curtain.indices, gpu.curtainWide);
    }
}

void GlRenderer::forgetBindings() {
    if (onDrawingThread()) {
        bound_ = {};
        arrayBuffer_ = elementBuffer_ = ~0u;
    }
}

// Makes contexts that share their textures and buffers with the drawing one, for prepare() to use
// on other threads. Made with the drawing context current, which they then share with.
void GlRenderer::makeSharedContexts(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        platform::GlContextPtr shared = window_->createGlContext(true);
        if (shared == nullptr) {
            break;
        }
        pool_.push_back(SharedContext{std::move(shared), false});
    }
    // Making one current again is what made them current here.
    window_->makeCurrent(context_.get());
    // Some systems (EGL on Wayland, macOS) can't make a context current on the window from another thread.
    // One is tried on a thread now, and the pool is given up when that fails.
    if (!pool_.empty()) {
        bool works = false;
        std::thread([&] {
            works = window_->makeCurrent(pool_.front().context.get());
            if (works) {
                window_->makeCurrent(nullptr);
            }
        }).join();
        if (!works) {
            destroySharedContexts();
            closing_ = false;
            window_->makeCurrent(context_.get());
        }
    }
}

void GlRenderer::destroySharedContexts() {
    std::unique_lock lock(poolLock_);
    // Waits for the contexts a thread is uploading in, and lets none be taken after this.
    closing_ = true;
    poolFree_.notify_all();
    poolFree_.wait(lock, [&] {
        for (const auto& shared : pool_) {
            if (shared.busy) {
                return false;
            }
        }
        return true;
    });
    pool_.clear();
}

// Uploads a scene on this thread in a context of its own. The objects it makes are the drawing context's
// too, once the driver is done with them (waited for here).
std::unique_ptr<GlRenderer::SceneGpu> GlRenderer::uploadShared(const scene::Scene& scene) const {
    SharedContext* mine = nullptr;
    {
        std::unique_lock lock(poolLock_);
        poolFree_.wait(lock, [&] {
            if (closing_) {
                return true;
            }
            for (auto& shared : pool_) {
                if (!shared.busy) {
                    mine = &shared;
                    return true;
                }
            }
            return false;
        });
        if (mine == nullptr) {
            return nullptr;
        }
        mine->busy = true;
    }
    std::unique_ptr<SceneGpu> gpu;
    if (window_->makeCurrent(mine->context.get())) {
        // This touches nothing but what this thread made: the caches of what is bound are the drawing thread's.
        auto* self = const_cast<GlRenderer*>(this);
        // The scene is there until this returns, so its images need not be copied.
        auto staged = stage(scene, false);
        gpu = std::make_unique<SceneGpu>();
        try {
            self->uploadStaged(*gpu, *staged);
            self->gl_.Finish();
        } catch (...) {
            gpu->release(*graveyard_);
            gpu.reset();
        }
        window_->makeCurrent(nullptr);
    }
    {
        std::lock_guard lock(poolLock_);
        mine->busy = false;
    }
    poolFree_.notify_all();
    return gpu;
}

IRenderer::Prepared GlRenderer::prepare(const scene::Scene& scene) const {
    auto p = std::make_unique<GpuPrepared>();
    p->graveyard = graveyard_;
    // On the drawing thread there is no need: swap uploads what is staged there and then.
    if (!pool_.empty() && !onDrawingThread()) {
        p->gpu = uploadShared(scene);
    }
    if (!p->gpu) {
        p->staged = stage(scene);
    }
    copyScene(*p, scene);
    return Prepared(std::move(p));
}

void GlRenderer::upload(Prepared prepared) {
    (void)swap(std::move(prepared));
}

IRenderer::Prepared GlRenderer::swap(Prepared prepared) {
    if (!prepared.get()) {
        prepared = Prepared(std::make_unique<GpuPrepared>());
    }
    auto* made = dynamic_cast<GpuPrepared*>(prepared.get());
    if (made == nullptr) {
        throw std::logic_error("a scene prepared by another renderer was given to the OpenGL 2 one");
    }
    auto& p = *made;
    p.graveyard = graveyard_;
    if (!p.gpu) {
        p.gpu = std::make_unique<SceneGpu>();
        if (p.staged) {
            uploadStaged(*p.gpu, *p.staged);
            p.staged.reset();
        }
    }
    reportScene(*gpu_, "of the scene left");
    std::swap(gpu_, p.gpu);
    swapScene(p);
    collectGarbage();
    reportResident();
    return prepared;
}

void GlRenderer::addMaterials(const scene::Scene& scene) {
    std::vector<bool> needed;
    if (fixed_) {
        markNeeded(scene, needed);
    }
    for (std::size_t i = gpu_->textures.size(); i < scene.images.size(); ++i) {
        const auto& image = scene.images[i];
        const bool wanted = complete(image) && (!fixed_ || needed[i]);
        gpu_->textures.push_back(wanted ? (fixed_ && detail_ > 0 ? uploadStarter(*gpu_, static_cast<int>(i), image) : uploadTexture(image, true)) : Texture{});
    }
    if (fixed_) {
        // A new material can use an image that was passed over before.
        for (std::size_t i = 0; i < gpu_->textures.size() && i < scene.images.size(); ++i) {
            if (needed[i] && gpu_->textures[i].id == 0 && complete(scene.images[i])) {
                gpu_->textures[i] = uploadTexture(scene.images[i], true);
            }
        }
    }
    for (std::size_t i = gpu_->envCubes.size(); i < scene.envCubes.size(); ++i) {
        gpu_->envCubes.push_back(fixed_ ? Texture{} : uploadCube(scene.envCubes[i]));
    }
    if (scene.materials.size() > materials_.size()) {
        materials_.insert(materials_.end(), scene.materials.begin() + static_cast<std::ptrdiff_t>(materials_.size()), scene.materials.end());
    }
    reportResident();
}

void GlRenderer::addMeshes(const scene::Scene& scene) {
    if (scene.meshes.size() <= gpu_->meshes.size()) {
        return;
    }
    const auto set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
    MeshData data;
    reserveMeshData(scene.meshes, gpu_->meshes.size(), data);
    std::vector<GpuMesh> added;
    for (std::size_t i = gpu_->meshes.size(); i < scene.meshes.size(); ++i) {
        added.push_back(packMesh(scene.meshes[i], scene.materials.size(), set, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: added meshes are too big for one GPU buffer; some are left out");
    }
    gpu_->meshBuffers.push_back(uploadMeshBuffers(data));
    gpu_->meshes.insert(gpu_->meshes.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
}

void GlRenderer::setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade) {
    for (const int i : indexes) {
        const auto at = static_cast<std::size_t>(i);
        if (i < 0 || at >= scene.images.size() || at >= gpu_->textures.size() || !complete(scene.images[at])) {
            continue;
        }
        if (fixed_) {
            // The light the vertices take: the new map, and the one it is fading in from, if any.
            if (fade && !gpu_->lightFrom.contains(i) && gpu_->lightImages.contains(i)) {
                gpu_->lightFrom[i] = gpu_->lightImages[i];
            } else if (!fade) {
                gpu_->lightFrom.erase(i);
            }
            gpu_->lightImages[i] = scene.images[at];
        }
        // Light maps are drawn as the game draws them: one level, no mipmaps.
        Texture fresh = fixed_ && vertexLightMaps_ ? lightPlaceholder() : uploadTexture(scene.images[at], false);
        Texture old = std::exchange(gpu_->textures[at], fresh);
        auto& from = gpu_->fadingFrom[i];
        if (fade && from.id == 0) {
            from = old;  // the light seen when the fade began
        } else {
            release(old);
        }
        if (!fade) {
            release(from);
            gpu_->fadingFrom.erase(i);
        }
    }
}

void GlRenderer::setLightMapFade(float fade) {
    gpu_->lightMapFade = std::clamp(fade, 0.0f, 1.0f);
    if (gpu_->lightMapFade >= 1) {
        gpu_->lightFrom.clear();
        for (auto& [index, texture] : gpu_->fadingFrom) {
            release(texture);
        }
        gpu_->fadingFrom.clear();
    }
}

void GlRenderer::setSkybox(const std::vector<scene::Image>& faces) {
    if (fixed_) {
        for (auto& t : gpu_->skyFaces) {
            release(t);
        }
        uploadSkyFaces(gpu_->skyFaces, faces);
        return;
    }
    Texture fresh = uploadCube(faces);
    release(gpu_->skybox);
    gpu_->skybox = fresh;
}

void GlRenderer::releaseSprites() {
    for (auto& t : spriteTextures_) {
        release(t);
    }
    spriteTextures_.clear();
    spriteSizes_.clear();
    spriteFill_.clear();
}

void GlRenderer::setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) {
    for (const auto& [index, image] : images) {
        if (index < 0 || image == nullptr || !complete(*image)) {
            continue;
        }
        const auto i = static_cast<std::size_t>(index);
        if (i >= spriteTextures_.size()) {
            spriteTextures_.resize(i + 1);
            spriteSizes_.resize(i + 1, {0, 0});
        }
        if (i >= spriteFill_.size()) {
            spriteFill_.resize(i + 1, {1.0f, 1.0f});
        }
        // A sprite is clamped, so without shaders it is padded up to sides the hardware takes (or stretched if it
        // is too large), and its texture coordinates are multiplied by the share of the texture it fills.
        const scene::Image* source = image;
        scene::Image fitted;
        spriteFill_[i] = {1.0f, 1.0f};
        if (fixed_ && !fitsTexture(image->width, image->height, spriteLimits_)) {
            const ClampedFit fit = fitClamped(image->width, image->height, spriteLimits_);
            fitted = fit.stretch ? stretched(*image, fit.width, fit.height) : padded(*image, fit.width, fit.height);
            spriteFill_[i] = {fit.u, fit.v};
            source = &fitted;
        }
        const GLint format = internalFormatFor(source->rgba.data(), std::size_t{source->width} * source->height);
        gl_.ActiveTexture(GL_TEXTURE0);
        // A new texture when the slot has none yet or the picture changed size, otherwise its pixels are replaced.
        Texture& t = spriteTextures_[i];
        if (t.id == 0 || spriteSizes_[i][0] != source->width || spriteSizes_[i][1] != source->height) {
            release(t);
            t = Texture{};
            t.bytes = bytesFor(std::size_t{source->width} * source->height, false);
            t.width = source->width;
            t.height = source->height;
            checkTexture("sprite", GL_TEXTURE_2D, source->width, source->height);
            gl_.GenTextures(1, &t.id);
            gl_.BindTexture(GL_TEXTURE_2D, t.id);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            t.wrap = GL_CLAMP_TO_EDGE;
            gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_.TexImage2D(GL_TEXTURE_2D, 0, format, static_cast<GLsizei>(source->width), static_cast<GLsizei>(source->height), 0, GL_RGBA,
                           GL_UNSIGNED_BYTE, source->rgba.data());
            spriteSizes_[i] = {source->width, source->height};
        } else {
            gl_.BindTexture(GL_TEXTURE_2D, t.id);
            gl_.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
            gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(source->width), static_cast<GLsizei>(source->height), GL_RGBA,
                              GL_UNSIGNED_BYTE, source->rgba.data());
        }
    }
    bound_ = {};
    reportResident();
}

bool GlRenderer::hasImage(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(index)].id != 0;
}

bool GlRenderer::hasEnvCube(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->envCubes.size() && gpu_->envCubes[static_cast<std::size_t>(index)].id != 0;
}

bool GlRenderer::hasFadeFrom(int index) const {
    const auto from = gpu_->fadingFrom.find(index);
    return from != gpu_->fadingFrom.end() && from->second.id != 0;
}

bool GlRenderer::skinReady(std::uint32_t buffers) const {
    return !gpu_->meshBuffers[buffers].cpuSkin.empty();
}

} // namespace ts2::render
