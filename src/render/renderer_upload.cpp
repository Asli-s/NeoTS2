#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ts2::render {

using namespace render_detail;

namespace render_detail {

namespace {

// An image's own smaller levels are uploaded when it has them all the way down. Otherwise the
// GPU makes them.
bool ownLevels(const scene::Image& image) {
    std::uint32_t w = image.width, h = image.height;
    for (const auto& level : image.smallerLevels) {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        if (level.size() != static_cast<std::size_t>(w) * h * 4) {
            return false;
        }
    }
    return !image.smallerLevels.empty() && w == 1 && h == 1;
}

} // namespace

SDL_GPUTransferBuffer* filledUpload(SDL_GPUDevice* device, const void* data, Uint32 size) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(device, &info);
    void* mapped = buffer ? SDL_MapGPUTransferBuffer(device, buffer, false) : nullptr;
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, buffer);
        return nullptr;
    }
    std::memcpy(mapped, data, size);
    SDL_UnmapGPUTransferBuffer(device, buffer);
    return buffer;
}

SDL_GPUBuffer* filledBuffer(SDL_GPUDevice* device, UploadPass& pass, SDL_GPUBufferUsageFlags usage, const void* data, Uint32 size) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = size;
    Filling<SDL_GPUBuffer> b(device, SDL_CreateGPUBuffer(device, &info), "GPU buffer");
    const SDL_GPUTransferBufferLocation from = pass.stage(data, size);
    if (from.transfer_buffer == nullptr) {
        throw sdlError("GPU upload buffer");
    }
    SDL_GPUBufferRegion to{b.get(), 0, size};
    SDL_UploadToGPUBuffer(pass.copy(), &from, &to, false);
    return b.keep();
}

SDL_GPUTexture* uploadTexture(SDL_GPUDevice* device, UploadPass& pass, const scene::Image& image, bool mips,
                              std::vector<SDL_GPUTexture*>& withMips) {
    const bool own = mips && ownLevels(image);
    const Uint32 levels = mips ? static_cast<Uint32>(std::floor(std::log2(std::max(image.width, image.height)))) + 1 : 1;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = image.width;
    info.height = image.height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    Filling<SDL_GPUTexture> texture(device, SDL_CreateGPUTexture(device, &info), "GPU texture");
    SDL_GPUTexture* t = texture.get();
    auto copyLevel = [&](const std::vector<std::uint8_t>& pixels, Uint32 level) {
        const SDL_GPUTransferBufferLocation at = pass.stage(pixels.data(), static_cast<Uint32>(pixels.size()));
        if (at.transfer_buffer == nullptr) {
            throw sdlError("GPU upload buffer");
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = std::max(1u, image.width >> level);
        from.rows_per_layer = std::max(1u, image.height >> level);
        SDL_GPUTextureRegion to{};
        to.texture = t;
        to.mip_level = level;
        to.w = from.pixels_per_row;
        to.h = from.rows_per_layer;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
    };
    copyLevel(image.rgba, 0);
    for (Uint32 level = 1; own && level < levels; ++level) {
        copyLevel(image.smallerLevels[level - 1], level);
    }
    if (levels > 1 && !own) {
        withMips.push_back(t);
    }
    return texture.keep();
}

SDL_GPUTexture* uploadCube(SDL_GPUDevice* device, UploadPass& pass, const std::vector<scene::Image>& faces) {
    // A cube's faces must be square, or the GPU refuses it and the whole upload fails.
    if (faces.size() != 6 || faces.front().width == 0 || faces.front().width != faces.front().height) {
        return nullptr;
    }
    const auto& face = faces.front();
    for (const auto& f : faces) {
        if (f.width != face.width || f.height != face.height || !complete(f)) {
            return nullptr;
        }
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_CUBE;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = face.width;
    info.height = face.height;
    info.layer_count_or_depth = 6;
    info.num_levels = 1;
    Filling<SDL_GPUTexture> texture(device, SDL_CreateGPUTexture(device, &info), "GPU texture");
    for (Uint32 l = 0; l < 6; ++l) {
        const SDL_GPUTransferBufferLocation at = pass.stage(faces[l].rgba.data(), static_cast<Uint32>(faces[l].rgba.size()));
        if (at.transfer_buffer == nullptr) {
            throw sdlError("GPU upload buffer");
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = face.width;
        from.rows_per_layer = face.height;
        SDL_GPUTextureRegion to{};
        to.texture = texture.get();
        to.layer = l;
        to.w = face.width;
        to.h = face.height;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
    }
    return texture.keep();
}

} // namespace render_detail

void SdlGpuRenderer::SceneGpu::release(SDL_GPUDevice* device) {
    meshes.clear();
    for (auto* t : textures) {
        if (t) {
            SDL_ReleaseGPUTexture(device, t);
        }
    }
    textures.clear();
    for (const auto& [index, texture] : fadingFrom) {
        if (texture) {
            SDL_ReleaseGPUTexture(device, texture);
        }
    }
    fadingFrom.clear();
    if (white) {
        SDL_ReleaseGPUTexture(device, white);
        white = nullptr;
    }
    for (const auto& set : meshBuffers) {
        for (auto* b : {set.vertices, set.indices, set.skin}) {
            if (b) {
                SDL_ReleaseGPUBuffer(device, b);
            }
        }
    }
    meshBuffers.clear();
    for (auto** b : {&terrainVertices, &terrainIndices, &waterVertices, &waterIndices, &roadVertices, &roadIndices,
                     &imposterVertices, &imposterIndices, &clipVertices, &clipIndices}) {
        if (*b) {
            SDL_ReleaseGPUBuffer(device, *b);
            *b = nullptr;
        }
    }
    for (auto** t : {&terrainPaints, &terrainAlphas, &terrainCells, &terrainLight, &terrainGradient, &skyCube, &skybox, &blackCube, &ripples, &roadTextures}) {
        if (*t) {
            SDL_ReleaseGPUTexture(device, *t);
            *t = nullptr;
        }
    }
    for (auto* t : imposterTextures) {
        SDL_ReleaseGPUTexture(device, t);
    }
    imposterTextures.clear();
    for (auto* t : envCubes) {
        if (t) {
            SDL_ReleaseGPUTexture(device, t);
        }
    }
    envCubes.clear();
    imposterDraws.clear();
    terrainIndexCount = 0;
    waterIndexCount = 0;
    roadIndexCount = 0;
}

IRenderer::Prepared SdlGpuRenderer::prepare(const scene::Scene& scene) const {
    auto p = std::make_unique<GpuPrepared>();
    p->device = device_;
    p->gpu = std::make_unique<SceneGpu>();
    p->gpu->upload(device_, scene, p->withMips);
    copyScene(*p, scene);
    return Prepared(std::move(p));
}

void SdlGpuRenderer::upload(Prepared prepared) {
    (void)swap(std::move(prepared));
}

IRenderer::Prepared SdlGpuRenderer::swap(Prepared prepared) {
    if (!prepared.get()) {
        prepared = Prepared(std::make_unique<GpuPrepared>());
    }
    auto& p = static_cast<GpuPrepared&>(*prepared.get());
    if (!p.gpu) {
        p.gpu = std::make_unique<SceneGpu>();
    }
    p.device = device_;
    std::swap(gpu_, p.gpu);
    swapScene(p);
    // SDL makes mip levels with its own blit pipelines, which it keeps without a lock: only here, on
    // the drawing thread. The fence comes after them and the copies sent before.
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    for (auto* t : p.withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    p.withMips.clear();
    if (uploadFence_) {
        SDL_ReleaseGPUFence(device_, uploadFence_);
    }
    uploadFence_ = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    return prepared;
}

bool SdlGpuRenderer::uploaded() {
    if (uploadFence_ && SDL_QueryGPUFence(device_, uploadFence_)) {
        SDL_ReleaseGPUFence(device_, uploadFence_);
        uploadFence_ = nullptr;
    }
    return uploadFence_ == nullptr;
}

void SdlGpuRenderer::GpuPrepared::releaseSome(std::size_t textures) {
    if (gpu && gpu->releaseTextures(device, textures)) {
        release();
    }
}

bool SdlGpuRenderer::SceneGpu::releaseTextures(SDL_GPUDevice* device, std::size_t count) {
    for (auto* list : {&textures, &imposterTextures}) {
        for (; count > 0 && !list->empty(); --count) {
            if (list->back()) {
                SDL_ReleaseGPUTexture(device, list->back());
            }
            list->pop_back();
        }
    }
    return textures.empty() && imposterTextures.empty();
}

void SdlGpuRenderer::GpuPrepared::release() {
    if (gpu) {
        gpu->release(device);
        gpu.reset();
    }
    withMips.clear();
}

// One copy pass a scene is uploaded in, and the ways of filling buffers and textures in it. Textures
// whose mip levels the GPU still has to make go into `withMips`.
struct SdlGpuRenderer::SceneGpu::Copy {
    SDL_GPUDevice* device;
    UploadPass& pass;
    std::vector<SDL_GPUTexture*>& withMips;
    const scene::Image whiteImage{1, 1, {255, 255, 255, 255}};

    // Where a texture copies from.
    void stage(SDL_GPUTextureTransferInfo& from, const void* data, Uint32 size) {
        const SDL_GPUTransferBufferLocation at = pass.stage(data, size);
        if (at.transfer_buffer == nullptr) {
            throw sdlError("GPU upload buffer");
        }
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
    }
    SDL_GPUBuffer* buffer(SDL_GPUBufferUsageFlags usage, const void* data, Uint32 size) { return filledBuffer(device, pass, usage, data, size); }
    SDL_GPUTexture* texture(const scene::Image& image, bool mips = true) { return uploadTexture(device, pass, image, mips, withMips); }
    // A texture array of `layers` layers, each `w` by `h` texels taken from layerData(layer).
    SDL_GPUTexture* array(SDL_GPUTextureFormat format, Uint32 w, Uint32 h, Uint32 layers, Uint32 levels, Uint32 bytesPerTexel,
                          const std::function<const std::uint8_t*(Uint32)>& layerData) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        info.width = w;
        info.height = h;
        info.layer_count_or_depth = layers;
        info.num_levels = levels;
        Filling<SDL_GPUTexture> tex(device, SDL_CreateGPUTexture(device, &info), "GPU texture");
        for (Uint32 l = 0; l < layers; ++l) {
            SDL_GPUTextureTransferInfo from{};
            stage(from, layerData(l), w * h * bytesPerTexel);
            from.pixels_per_row = w;
            from.rows_per_layer = h;
            SDL_GPUTextureRegion to{};
            to.texture = tex.get();
            to.layer = l;
            to.w = w;
            to.h = h;
            to.d = 1;
            SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
        }
        return tex.keep();
    }
    // A plain 2D texture, with or without mip levels. White when the image is broken.
    SDL_GPUTexture* plain(const scene::Image& given, bool mips) {
        const scene::Image& image = complete(given) ? given : whiteImage;
        const Uint32 levels = mips ? static_cast<Uint32>(std::floor(std::log2(std::max(image.width, image.height)))) + 1 : 1;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | (levels > 1 ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0);
        info.width = image.width;
        info.height = image.height;
        info.layer_count_or_depth = 1;
        info.num_levels = levels;
        Filling<SDL_GPUTexture> tex(device, SDL_CreateGPUTexture(device, &info), "GPU texture");
        SDL_GPUTextureTransferInfo from{};
        stage(from, image.rgba.data(), static_cast<Uint32>(image.rgba.size()));
        from.pixels_per_row = image.width;
        from.rows_per_layer = image.height;
        SDL_GPUTextureRegion to{};
        to.texture = tex.get();
        to.w = image.width;
        to.h = image.height;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
        if (levels > 1) {
            withMips.push_back(tex.get());
        }
        return tex.keep();
    }
};

void SdlGpuRenderer::SceneGpu::upload(SDL_GPUDevice* device, const scene::Scene& scene, std::vector<SDL_GPUTexture*>& withMips) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    UploadPass pass(device, cmd);
    Copy c{device, pass, withMips};
    uploadImages(c, scene);
    uploadMeshes(c, scene);
    if (scene.terrain && scene.terrain->paints() > 0 && !scene.terrain->textures.empty()) {
        const auto& t = *scene.terrain;
        uploadTerrainGrid(c, t);
        uploadTerrainTextures(c, t);
        uploadWater(c, t);
        if (!t.roads.empty() && !t.roadTextures.empty()) {
            uploadRoads(c, t);
        }
        if (!t.imposters.empty()) {
            uploadImposters(c, t);
        }
    }
    if (scene.clip) {
        uploadBoundary(c, scene);
    }
    // A lot's skybox: a cube map of six faces of one size, drawn behind everything.
    skybox = uploadCube(device, pass, scene.skybox);
    envCubes.reserve(scene.envCubes.size());
    for (const auto& faces : scene.envCubes) {
        envCubes.push_back(uploadCube(device, pass, faces));
    }
    blackCube = uploadCube(device, pass, std::vector<scene::Image>(6, scene::Image{1, 1, {0, 0, 0, 255}}));

    pass.end();
    // Waited for here: on another thread this keeps the first frame that draws the scene from
    // waiting for the copies.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        throw sdlError("GPU upload");
    }
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);
}

// The scene's images, and a white texture for surfaces without one.
void SdlGpuRenderer::SceneGpu::uploadImages(Copy& c, const scene::Scene& scene) {
    // Light maps are drawn as the game draws them: one level, no mipmaps.
    std::set<std::size_t> lightMaps;
    if (scene.lightMaps) {
        for (const int i : scene.lightMaps->floors) lightMaps.insert(static_cast<std::size_t>(i));
        for (const int i : scene.lightMaps->floorIncidence) lightMaps.insert(static_cast<std::size_t>(i));
        lightMaps.insert(static_cast<std::size_t>(scene.lightMaps->walls));
        lightMaps.insert(static_cast<std::size_t>(scene.lightMaps->wallIncidence));
    }
    for (std::size_t i = 0; i < scene.images.size(); ++i) {
        const auto& image = scene.images[i];
        textures.push_back(complete(image) ? c.texture(image, !lightMaps.contains(i)) : nullptr);
    }
    white = c.texture({1, 1, {255, 255, 255, 255}});
}

// All meshes go into one vertex, one index and one skin buffer. Each primitive keeps where its part starts.
void SdlGpuRenderer::SceneGpu::uploadMeshes(Copy& c, const scene::Scene& scene) {
    MeshData data;
    meshBuffers.emplace_back();
    for (const auto& mesh : scene.meshes) {
        meshes.push_back(packMesh(mesh, scene.materials.size(), 0, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: the scene's meshes are too big for one GPU buffer; some are left out");
    }
    if (!data.indices.empty()) {
        meshBuffers[0].vertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, data.vertices.data(), static_cast<Uint32>(data.vertices.size() * sizeof(float)));
        meshBuffers[0].indices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, data.indices.data(), static_cast<Uint32>(data.indices.size() * sizeof(std::uint32_t)));
    }
    if (!data.skin.empty()) {
        meshBuffers[0].skin = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, data.skin.data(), static_cast<Uint32>(data.skin.size()));
    }
}

// The neighborhood terrain's grid of points and its tiles.
void SdlGpuRenderer::SceneGpu::uploadTerrainGrid(Copy& c, const scene::PaintedTerrain& t) {
    // Vertices: position, normal, grid position.
    std::vector<float> v(static_cast<std::size_t>(t.columns) * t.rows * 8);
    for (std::uint32_t y = 0; y < t.rows; ++y) {
        for (std::uint32_t x = 0; x < t.columns; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * t.columns + x;
            float* p = &v[i * 8];
            p[0] = static_cast<float>(x) * t.tileSize;
            p[1] = static_cast<float>(y) * t.tileSize;
            p[2] = t.heights[i];
            std::copy_n(&t.normals[i * 3], 3, p + 3);
            p[6] = static_cast<float>(x);
            p[7] = static_cast<float>(y);
        }
    }
    // Two triangles per tile, split from its (x, y) corner to its (x + 1, y + 1) corner.
    std::vector<std::uint32_t> idx;
    idx.reserve(static_cast<std::size_t>(t.columns - 1) * (t.rows - 1) * 6);
    for (std::uint32_t y = 0; y + 1 < t.rows; ++y) {
        for (std::uint32_t x = 0; x + 1 < t.columns; ++x) {
            const std::uint32_t a = y * t.columns + x, b = a + 1, d = a + t.columns, e = d + 1;
            if (!t.hiddenTiles.empty() && t.hiddenTiles[static_cast<std::size_t>(y) * (t.columns - 1) + x]) {
                continue;
            }
            idx.insert(idx.end(), {a, b, e, a, e, d});
        }
    }
    terrainVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, v.data(), static_cast<Uint32>(v.size() * sizeof(float)));
    if (!idx.empty()) {  // every tile can be hidden (a lot's own ground stands in for it)
        terrainIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, idx.data(), static_cast<Uint32>(idx.size() * sizeof(std::uint32_t)));
    }
    terrainIndexCount = static_cast<unsigned>(idx.size());
}

// The terrain's paints, where each shows, its light map and its underwater gradient.
void SdlGpuRenderer::SceneGpu::uploadTerrainTextures(Copy& c, const scene::PaintedTerrain& t) {
    // The paint textures share one array, so each must be the first one's size. Others are drawn white.
    const auto& first = complete(t.textures.front()) ? t.textures.front() : c.whiteImage;
    std::vector<std::uint8_t> blank;
    auto layer = [&](Uint32 l) {
        const auto& image = t.textures[l];
        if (complete(image) && image.width == first.width && image.height == first.height) {
            return image.rgba.data();
        }
        blank.assign(static_cast<std::size_t>(first.width) * first.height * 4, 255);
        return static_cast<const std::uint8_t*>(blank.data());
    };
    const Uint32 levels = static_cast<Uint32>(std::floor(std::log2(std::max(first.width, first.height)))) + 1;
    terrainPaints = c.array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, first.width, first.height, static_cast<Uint32>(t.textures.size()),
                            levels, 4, layer);
    if (levels > 1) {
        c.withMips.push_back(terrainPaints);
    }
    const Uint32 paints = static_cast<Uint32>(t.paints());
    const std::size_t map = static_cast<std::size_t>(t.alphaSize) * t.alphaSize;
    terrainAlphas = c.array(SDL_GPU_TEXTUREFORMAT_R8_UNORM, t.alphaSize, t.alphaSize, paints, 1, 1,
                            [&](Uint32 l) { return t.alphas.data() + l * map; });
    const std::size_t cells = static_cast<std::size_t>(t.cellsX) * t.cellsY;
    terrainCells = c.array(SDL_GPU_TEXTUREFORMAT_R8_UNORM, t.cellsX, t.cellsY, paints, 1, 1,
                           [&](Uint32 l) { return t.cellTextures.data() + l * cells; });
    // The light map, white (no change) when there is none.
    const scene::Image& light = complete(t.lightMap) ? t.lightMap : c.whiteImage;
    terrainLight = c.array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, light.width, light.height, 1, 1, 4,
                           [&](Uint32) { return light.rgba.data(); });
    terrainUniforms = {static_cast<float>(t.alphaSize), t.textureRepeat, static_cast<float>(t.cellTiles), static_cast<float>(paints),
                       static_cast<float>(t.cellsX), static_cast<float>(t.cellsY), t.lightMap.width ? t.lightMapScale : 1.0f, t.litFrom,
                       t.gradientDepth, 0, {0, 0}};
    terrainGradient = c.plain(t.underwaterGradient.width ? t.underwaterGradient : c.whiteImage, false);
}

// The water sheet: a quad per wet tile at the water level, texture coordinates in tiles, with its
// ripples and the sky it reflects.
void SdlGpuRenderer::SceneGpu::uploadWater(Copy& c, const scene::PaintedTerrain& t) {
    std::vector<float> wv;
    std::vector<std::uint32_t> wi;
    const std::uint32_t tilesX = t.columns - 1;
    for (std::size_t i = 0; i < t.waterTiles.size(); ++i) {
        if (!t.waterTiles[i] || (!t.hiddenTiles.empty() && t.hiddenTiles[i])) {
            continue;
        }
        const auto x = static_cast<float>(i % tilesX), y = static_cast<float>(i / tilesX);
        const auto base = static_cast<std::uint32_t>(wv.size() / 5);
        for (const auto [cx, cy] : {std::pair{x, y}, std::pair{x + 1, y}, std::pair{x + 1, y + 1}, std::pair{x, y + 1}}) {
            wv.insert(wv.end(), {cx * t.tileSize, cy * t.tileSize, t.waterLevel, cx, cy});
        }
        wi.insert(wi.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    if (wi.empty() || t.sky.size() != 6 || !t.ripples.width) {
        return;
    }
    waterVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, wv.data(), static_cast<Uint32>(wv.size() * sizeof(float)));
    waterIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, wi.data(), static_cast<Uint32>(wi.size() * sizeof(std::uint32_t)));
    waterIndexCount = static_cast<unsigned>(wi.size());
    waterLevel = t.waterLevel;
    ripples = c.plain(t.ripples, true);
    // The sky cube map: six faces of one size.
    const auto& face = t.sky.front();
    const Uint32 skyLevels = static_cast<Uint32>(std::floor(std::log2(std::max(face.width, face.height)))) + 1;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_CUBE;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = face.width;
    info.height = face.height;
    info.layer_count_or_depth = 6;
    info.num_levels = skyLevels;
    skyCube = made(SDL_CreateGPUTexture(c.device, &info), "GPU texture");
    for (Uint32 l = 0; l < 6; ++l) {
        SDL_GPUTextureTransferInfo from{};
        c.stage(from, t.sky[l].rgba.data(), static_cast<Uint32>(t.sky[l].rgba.size()));
        from.pixels_per_row = face.width;
        from.rows_per_layer = face.height;
        SDL_GPUTextureRegion to{};
        to.texture = skyCube;
        to.layer = l;
        to.w = face.width;
        to.h = face.height;
        to.d = 1;
        SDL_UploadToGPUTexture(c.pass.copy(), &from, &to, false);
    }
    c.withMips.push_back(skyCube);
    waterUniforms = {{t.reflectionTint[0], t.reflectionTint[1], t.reflectionTint[2], t.reflectionStrength}, {0, 0},
                     t.reflectionOffset, t.rippleScale, t.rippleSpeed, 0, {0, 0}};
}

// The roads: a quad per tile, split like the terrain's tiles.
void SdlGpuRenderer::SceneGpu::uploadRoads(Copy& c, const scene::PaintedTerrain& t) {
    std::vector<float> rv;
    std::vector<std::uint32_t> ri;
    rv.reserve(t.roads.size() * 24);
    ri.reserve(t.roads.size() * 6);
    for (const auto& road : t.roads) {
        const auto base = static_cast<std::uint32_t>(rv.size() / 6);
        for (const auto& corner : road.corners) {
            rv.insert(rv.end(), {corner[0], corner[1], corner[2], corner[3], corner[4], static_cast<float>(road.texture)});
        }
        ri.insert(ri.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
    }
    roadVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, rv.data(), static_cast<Uint32>(rv.size() * sizeof(float)));
    roadIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, ri.data(), static_cast<Uint32>(ri.size() * sizeof(std::uint32_t)));
    roadIndexCount = static_cast<unsigned>(ri.size());
    const auto& road = t.roadTextures.front();
    const Uint32 roadLevels = static_cast<Uint32>(std::floor(std::log2(std::max(road.width, road.height)))) + 1;
    roadTextures = c.array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, road.width, road.height, static_cast<Uint32>(t.roadTextures.size()),
                           roadLevels, 4, [&](Uint32 l) { return t.roadTextures[l].rgba.data(); });
    if (roadLevels > 1) {
        c.withMips.push_back(roadTextures);
    }
    lightMapSize = t.lightMapScale * t.tileSize;
}

// The lot imposters: all parts in one buffer, each drawn with its own texture.
void SdlGpuRenderer::SceneGpu::uploadImposters(Copy& c, const scene::PaintedTerrain& t) {
    for (const auto& image : t.imposterTextures) {
        imposterTextures.push_back(c.plain(image, false));
    }
    std::vector<float> iv;
    std::vector<std::uint32_t> ii;
    for (const auto& part : t.imposters) {
        if (part.texture >= imposterTextures.size()) {
            continue;
        }
        ImposterDraw draw{part.kind};
        draw.texture = imposterTextures[part.texture];
        const auto lightMap = static_cast<std::size_t>(part.lightMap);
        draw.lightMap = part.lightMap >= 0 && lightMap < imposterTextures.size() ? imposterTextures[lightMap] : nullptr;
        std::copy_n(part.lightMapTint.data(), 3, draw.tint);
        draw.firstIndex = static_cast<std::uint32_t>(ii.size());
        draw.indexCount = static_cast<std::uint32_t>(part.indices.size());
        draw.vertexOffset = static_cast<std::int32_t>(iv.size() / 7);
        iv.insert(iv.end(), part.vertices.begin(), part.vertices.end());
        ii.insert(ii.end(), part.indices.begin(), part.indices.end());
        imposterDraws.push_back(draw);
    }
    imposterVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, iv.data(), static_cast<Uint32>(iv.size() * sizeof(float)));
    imposterIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, ii.data(), static_cast<Uint32>(ii.size() * sizeof(std::uint32_t)));
}

// A lot skirt's boundary curtain: a wall on each edge, facing inwards.
void SdlGpuRenderer::SceneGpu::uploadBoundary(Copy& c, const scene::Scene& scene) {
    const auto& b = *scene.clip;
    const float corners[4][2] = {{b.x0, b.y0}, {b.x1, b.y0}, {b.x1, b.y1}, {b.x0, b.y1}};
    std::vector<float> cv;
    std::vector<std::uint32_t> ci;
    for (int i = 0; i < 4; ++i) {
        const auto& p = corners[i];
        const auto& q = corners[(i + 1) % 4];
        const auto base = static_cast<std::uint32_t>(cv.size() / 3);
        cv.insert(cv.end(), {p[0], p[1], b.bottom, q[0], q[1], b.bottom, q[0], q[1], b.top, p[0], p[1], b.top});
        ci.insert(ci.end(), {base, base + 3, base + 2, base, base + 2, base + 1});
    }
    clipVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, cv.data(), static_cast<Uint32>(cv.size() * sizeof(float)));
    clipIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, ci.data(), static_cast<Uint32>(ci.size() * sizeof(std::uint32_t)));
}

} // namespace ts2::render
