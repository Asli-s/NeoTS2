#include "render/image_levels.hpp"
#include "render/renderer.hpp"
#include "render/terrain_mesh.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>

namespace ts2::render {

using namespace render_detail;

namespace render_detail {

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

void* mapStream(SDL_GPUDevice* device, StreamUpload& stream, Uint32 size) {
    if (size > stream.capacity) {
        if (stream.buffer != nullptr) {
            SDL_ReleaseGPUTransferBuffer(device, stream.buffer);
        }
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = std::max(size, stream.capacity * 2);
        stream.buffer = SDL_CreateGPUTransferBuffer(device, &info);
        stream.capacity = stream.buffer != nullptr ? info.size : 0;
    }
    // Cycling hands out spare memory while the GPU still reads the last frame's copy, so a new frame
    // neither waits for it nor needs a new buffer.
    return stream.buffer != nullptr ? SDL_MapGPUTransferBuffer(device, stream.buffer, true) : nullptr;
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

namespace {

// The GPU's compressed format for an image's DXT levels, or none when the GPU can't sample it. The
// largest level's sides must be whole blocks.
SDL_GPUTextureFormat blockFormat(SDL_GPUDevice* device, const scene::Image& image) {
    if (image.blockLevels.empty() || image.width % 4 != 0 || image.height % 4 != 0) {
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
    const SDL_GPUTextureFormat format = image.blockFormat == scene::kDxt1   ? SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM
                                        : image.blockFormat == scene::kDxt3 ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM
                                        : image.blockFormat == scene::kDxt5 ? SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM
                                                                 : SDL_GPU_TEXTUREFORMAT_INVALID;
    if (format == SDL_GPU_TEXTUREFORMAT_INVALID || !SDL_GPUTextureSupportsFormat(device, format, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_SAMPLER)) {
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
    return format;
}

// An image drawn from its DXT levels as the file stores them, every level its own.
SDL_GPUTexture* uploadBlocks(SDL_GPUDevice* device, UploadPass& pass, const scene::Image& image, SDL_GPUTextureFormat format) {
    const auto levels = static_cast<Uint32>(image.blockLevels.size());
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = image.width;
    info.height = image.height;
    info.layer_count_or_depth = 1;
    info.num_levels = levels;
    Filling<SDL_GPUTexture> texture(device, SDL_CreateGPUTexture(device, &info), "GPU texture");
    for (Uint32 level = 0; level < levels; ++level) {
        const auto& blocks = image.blockLevels[level];
        const SDL_GPUTransferBufferLocation at = pass.stage(blocks.data(), static_cast<Uint32>(blocks.size()));
        if (at.transfer_buffer == nullptr) {
            throw sdlError("GPU upload buffer");
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = std::max(1u, image.width >> level);
        from.rows_per_layer = std::max(1u, image.height >> level);
        SDL_GPUTextureRegion to{};
        to.texture = texture.get();
        to.mip_level = level;
        to.w = from.pixels_per_row;
        to.h = from.rows_per_layer;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, false);
    }
    return texture.keep();
}

} // namespace

SDL_GPUTexture* uploadTexture(SDL_GPUDevice* device, UploadPass& pass, const scene::Image& image, bool mips,
                              std::vector<SDL_GPUTexture*>& withMips) {
    if (const SDL_GPUTextureFormat format = mips ? blockFormat(device, image) : SDL_GPU_TEXTUREFORMAT_INVALID; format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        return uploadBlocks(device, pass, image, format);
    }
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
        for (auto* b : {set.vertices, set.indices, set.skin, set.morphs}) {
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
    static std::atomic<std::uint64_t> serials{0};
    auto p = std::make_unique<GpuPrepared>();
    p->device = device_;
    p->serial = ++serials;
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
    auto* made = dynamic_cast<GpuPrepared*>(prepared.get());
    if (made == nullptr) {
        throw std::logic_error("a scene prepared by another renderer was given to the SDL GPU one");
    }
    auto& p = *made;
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
    if (!data.morphs.empty()) {
        meshBuffers[0].morphs = c.buffer(SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, data.morphs.data(), static_cast<Uint32>(data.morphs.size() * sizeof(float)));
    }
}

// The neighborhood terrain's grid of points and its tiles.
void SdlGpuRenderer::SceneGpu::uploadTerrainGrid(Copy& c, const scene::PaintedTerrain& t) {
    const IndexedMesh m = terrainGrid(t);
    terrainVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, m.vertices.data(), static_cast<Uint32>(m.vertices.size() * sizeof(float)));
    if (!m.indices.empty()) {  // every tile can be hidden (a lot's own ground stands in for it)
        terrainIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, m.indices.data(), static_cast<Uint32>(m.indices.size() * sizeof(std::uint32_t)));
    }
    terrainIndexCount = static_cast<unsigned>(m.indices.size());
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
                       t.gradientDepth, 0, 0, 0};
    terrainGradient = c.plain(t.underwaterGradient.width ? t.underwaterGradient : c.whiteImage, false);
    // Its first column, row by row, for the objects darkened below the water.
    const scene::Image& g = t.underwaterGradient;
    const std::size_t rows = complete(g) && g.width > 0 ? std::min<std::size_t>(g.height, kUnderwaterRows) : 0;
    for (std::size_t r = 0; r < rows; ++r) {
        for (std::size_t k = 0; k < 4; ++k) {
            underwaterRows[r][k] = g.rgba[r * g.width * 4 + k] / 255.0f;
        }
    }
    const bool usable = rows > 0 && std::isfinite(t.litFrom) && std::isfinite(t.gradientDepth) && t.gradientDepth > 0;
    poolStyleSea = t.poolStyleSea;
    underwater = {t.litFrom, t.gradientDepth, usable ? static_cast<float>(rows) : 0.0f, poolStyleSea ? 1.0f : 0.0f};
}

// The water sheet: a quad per wet tile at the water level, texture coordinates in tiles, with its
// ripples and the sky it reflects.
void SdlGpuRenderer::SceneGpu::uploadWater(Copy& c, const scene::PaintedTerrain& t) {
    const IndexedMesh water = waterSheet(t);
    const auto& wv = water.vertices;
    const auto& wi = water.indices;
    if (wi.empty() || t.sky.size() != 6 || !t.ripples.width) {
        return;
    }
    waterVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, wv.data(), static_cast<Uint32>(wv.size() * sizeof(float)));
    waterIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, wi.data(), static_cast<Uint32>(wi.size() * sizeof(std::uint32_t)));
    waterIndexCount = static_cast<unsigned>(wi.size());
    waterLevel = t.waterLevel;
    ripples = c.plain(t.ripples, true);
    skyCube = uploadSkyCube(c, t.sky);
    waterUniforms = {{t.reflectionTint[0], t.reflectionTint[1], t.reflectionTint[2], t.reflectionStrength}, {0, 0},
                     t.reflectionOffset, t.rippleScale, t.rippleSpeed, 0, {0, 0}};
}

// The neighborhood's sky cube map with its mip levels: six faces of one size.
SDL_GPUTexture* SdlGpuRenderer::SceneGpu::uploadSkyCube(Copy& c, const std::vector<scene::Image>& sky) {
    const auto& face = sky.front();
    const Uint32 skyLevels = static_cast<Uint32>(std::floor(std::log2(std::max(face.width, face.height)))) + 1;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_CUBE;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = face.width;
    info.height = face.height;
    info.layer_count_or_depth = 6;
    info.num_levels = skyLevels;
    SDL_GPUTexture* skyCube = made(SDL_CreateGPUTexture(c.device, &info), "GPU texture");
    for (Uint32 l = 0; l < 6; ++l) {
        SDL_GPUTextureTransferInfo from{};
        c.stage(from, sky[l].rgba.data(), static_cast<Uint32>(sky[l].rgba.size()));
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
    return skyCube;
}

// The roads: a quad per tile, split like the terrain's tiles.
void SdlGpuRenderer::SceneGpu::uploadRoads(Copy& c, const scene::PaintedTerrain& t) {
    const IndexedMesh roads = roadQuads(t).mesh;
    const auto& rv = roads.vertices;
    const auto& ri = roads.indices;
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

// The lot imposters: all parts in one buffer, each drawn with its own texture. Textures already
// uploaded are kept.
SDL_GPUTexture* SdlGpuRenderer::SceneGpu::uploadImposterTexture(Copy& c, const scene::Image& image) {
    // High's pictures bring only their DXT5 blocks, a level each.
    if (const SDL_GPUTextureFormat format = image.rgba.empty() && image.blockLevels.size() == 1 ? blockFormat(c.device, image) : SDL_GPU_TEXTUREFORMAT_INVALID;
        format != SDL_GPU_TEXTUREFORMAT_INVALID) {
        return uploadBlocks(c.device, c.pass, image, format);
    }
    scene::Image decoded;
    return c.plain(withPixels(image, decoded), false);
}

void SdlGpuRenderer::SceneGpu::uploadImposters(Copy& c, const scene::PaintedTerrain& t) {
    for (std::size_t i = imposterTextures.size(); i < t.imposterTextures.size(); ++i) {
        imposterTextures.push_back(uploadImposterTexture(c, t.imposterTextures[i]));
    }
    const ImposterMesh parts = imposterParts(t);
    const auto& iv = parts.mesh.vertices;
    const auto& ii = parts.mesh.indices;
    imposterDraws.clear();
    for (const auto& part : parts.draws) {
        ImposterDraw draw{part.kind};
        draw.lot = part.lot;
        draw.texture = imposterTextures[part.texture];
        draw.lightMap = part.lightMap >= 0 ? imposterTextures[static_cast<std::size_t>(part.lightMap)] : nullptr;
        std::copy(part.tint.begin(), part.tint.end(), draw.tint);
        draw.firstIndex = part.firstIndex;
        draw.indexCount = part.indexCount;
        draw.vertexOffset = part.vertexOffset;
        draw.low = part.low;
        draw.high = part.high;
        imposterDraws.push_back(draw);
    }
    imposterVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, iv.data(), static_cast<Uint32>(iv.size() * sizeof(float)));
    imposterIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, ii.data(), static_cast<Uint32>(ii.size() * sizeof(std::uint32_t)));
}

SdlGpuRenderer::GpuImposterTextures::~GpuImposterTextures() {
    for (auto* t : textures) {
        SDL_ReleaseGPUTexture(device, t);
    }
}

IRenderer::ImposterTextures SdlGpuRenderer::prepareImposterTextures(const scene::SharedImages& images) const {
    auto ahead = std::make_unique<GpuImposterTextures>();
    ahead->device = device_;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return {};
    }
    std::vector<SDL_GPUTexture*> withMips;
    UploadPass pass(device_, cmd);
    SceneGpu::Copy c{device_, pass, withMips};
    for (const auto& image : images) {
        ahead->textures.push_back(SceneGpu::uploadImposterTexture(c, image));
    }
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
    return ahead;
}

void SdlGpuRenderer::addImposters(const scene::Scene& scene, ImposterTextures ahead) {
    if (!gpu_ || !scene.terrain || scene.terrain->imposterTextures.size() < gpu_->imposterTextures.size()) {
        setSurroundings(scene);
        return;
    }
    SceneGpu& g = *gpu_;
    // Textures uploaded ahead stand for the gained ones, so that only the parts go up now.
    if (auto* given = dynamic_cast<GpuImposterTextures*>(ahead.get());
        given != nullptr && given->device == device_ && g.imposterTextures.size() + given->textures.size() == scene.terrain->imposterTextures.size()) {
        g.imposterTextures.insert(g.imposterTextures.end(), given->textures.begin(), given->textures.end());
        given->textures.clear();
    }
    for (auto** b : {&g.imposterVertices, &g.imposterIndices}) {
        if (*b) {
            SDL_ReleaseGPUBuffer(device_, *b);
            *b = nullptr;
        }
    }
    g.imposterDraws.clear();
    if (scene.terrain->imposters.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips;
    UploadPass pass(device_, cmd);
    SceneGpu::Copy c{device_, pass, withMips};
    g.uploadImposters(c, *scene.terrain);
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::setSurroundings(const scene::Scene& scene) {
    if (!gpu_) {
        return;
    }
    SceneGpu& g = *gpu_;
    // SDL frees what a frame in flight still uses once that frame is done.
    for (auto** b : {&g.imposterVertices, &g.imposterIndices, &g.clipVertices, &g.clipIndices}) {
        if (*b) {
            SDL_ReleaseGPUBuffer(device_, *b);
            *b = nullptr;
        }
    }
    for (auto* t : g.imposterTextures) {
        SDL_ReleaseGPUTexture(device_, t);
    }
    g.imposterTextures.clear();
    g.imposterDraws.clear();
    const bool imposters = scene.terrain && !scene.terrain->imposters.empty();
    if (!imposters && !scene.clip) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips;
    UploadPass pass(device_, cmd);
    SceneGpu::Copy c{device_, pass, withMips};
    if (imposters) {
        g.uploadImposters(c, *scene.terrain);
    }
    if (scene.clip) {
        g.uploadBoundary(c, scene);
    }
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
}

// A lot skirt's boundary curtain: a wall on each edge, facing inwards.
void SdlGpuRenderer::SceneGpu::uploadBoundary(Copy& c, const scene::Scene& scene) {
    const IndexedMesh curtain = boundaryCurtain(*scene.clip);
    const auto& cv = curtain.vertices;
    const auto& ci = curtain.indices;
    clipVertices = c.buffer(SDL_GPU_BUFFERUSAGE_VERTEX, cv.data(), static_cast<Uint32>(cv.size() * sizeof(float)));
    clipIndices = c.buffer(SDL_GPU_BUFFERUSAGE_INDEX, ci.data(), static_cast<Uint32>(ci.size() * sizeof(std::uint32_t)));
}

void SdlGpuRenderer::setTerrainLightMap(const scene::Image& lightMap) {
    if (gpu_->terrainLight == nullptr || !complete(lightMap)) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    SDL_GPUTexture* fresh = nullptr;
    {
        std::vector<SDL_GPUTexture*> withMips;
        UploadPass pass(device_, cmd);
        SceneGpu::Copy c{device_, pass, withMips};
        fresh = c.array(SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, lightMap.width, lightMap.height, 1, 1, 4,
                        [&](Uint32) { return lightMap.rgba.data(); });
        pass.end();
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    // SDL frees a released texture once the frames drawing with it are done.
    SDL_ReleaseGPUTexture(device_, std::exchange(gpu_->terrainLight, fresh));
}

void SdlGpuRenderer::setNeighborhoodSky(const std::vector<scene::Image>& faces) {
    if (gpu_->skyCube == nullptr || faces.size() != 6 || faces.front().width == 0 || faces.front().width != faces.front().height ||
        !std::all_of(faces.begin(), faces.end(), [&](const scene::Image& f) {
            return f.width == faces.front().width && f.height == faces.front().height && complete(f);
        })) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    SDL_GPUTexture* fresh = nullptr;
    {
        std::vector<SDL_GPUTexture*> withMips;
        UploadPass pass(device_, cmd);
        SceneGpu::Copy c{device_, pass, withMips};
        fresh = SceneGpu::uploadSkyCube(c, faces);
        pass.end();
    }
    SDL_GenerateMipmapsForGPUTexture(cmd, fresh);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTexture(device_, std::exchange(gpu_->skyCube, fresh));
}

} // namespace ts2::render
