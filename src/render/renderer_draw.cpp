#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/timing.hpp"
#include "scene/sim_shadows.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>

namespace ts2::render {

using namespace render_detail;

void SdlGpuRenderer::drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection) {
    if (gpu_->imposterDraws.empty()) {
        return;
    }
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    SDL_GPUBufferBinding vertices{gpu_->imposterVertices, 0};
    SDL_GPUBufferBinding indices{gpu_->imposterIndices, 0};
    struct PartUniforms {
        float tint[3];
        float cutoff, mode, light[3];
    };
    auto draw = [&](SDL_GPUGraphicsPipeline* pipeline, const ImposterDraw& d, SDL_GPUTexture* texture, SDL_GPUSampler* sampler,
                    float cutoff, float mode) {
        SDL_BindGPUGraphicsPipeline(pass, pipeline);
        SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
        const PartUniforms u{{d.tint[0], d.tint[1], d.tint[2]}, cutoff, mode, {imposterLight_[0], imposterLight_[1], imposterLight_[2]}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPUTextureSamplerBinding binding{texture, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, d.vertexOffset, 0);
    };
    // The terrain first (its shader layer comes before the others'): light map, then texture.
    for (const auto& d : gpu_->imposterDraws) {
        if (d.kind != Kind::Terrain) {
            continue;
        }
        if (d.lightMap) {
            draw(imposterLightPipeline_, d, d.lightMap, sampler_, 0, 0);
        }
        draw(imposterTerrainPipeline_, d, d.texture, clampedMips_, 0, 1);
    }
    for (const auto& d : gpu_->imposterDraws) {
        switch (d.kind) {
        case Kind::Wall:
            draw(imposterWallPipeline_, d, d.texture, pointSampler_, 64.0f / 255, 1);
            break;
        case Kind::Slice:
            draw(imposterWallPipeline_, d, d.texture, pointSampler_, 127.0f / 255, 1);
            break;
        case Kind::Roof:
            draw(imposterRoofPipeline_, d, d.texture, clampedMips_, -1, 2);
            break;
        case Kind::Terrain:
            break;
        }
    }
}

bool SdlGpuRenderer::draw(const Camera& camera) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return false;
    }
    SDL_GPUTexture* target = nullptr;
    Uint32 width = 0, height = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &target, &width, &height) || target == nullptr) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    const bool drawn = render(cmd, target, width, height, camera);
    SDL_SubmitGPUCommandBuffer(cmd);
    return drawn;
}

bool SdlGpuRenderer::drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) {
    scene::Image image;
    if (!drawOffscreen(camera, width, height, image)) {
        return false;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_RGBA32,
                                                 image.rgba.data(), static_cast<int>(width * 4));
    const bool saved = surface != nullptr && SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

std::optional<scene::Image> SdlGpuRenderer::drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                           const scene::ObjectLighting& lighting) {
    // Back to the scene's own drawing afterwards, even if drawing throws.
    struct Restore {
        SdlGpuRenderer& r;
        std::optional<scene::ObjectLighting> lighting;
        ~Restore() {
            r.objectLighting_ = std::move(lighting);
            r.instancesOnly_ = false;
        }
    } restore{*this, std::exchange(objectLighting_, lighting)};
    instancesOnly_ = true;
    scene::Image image;
    const bool drawn = drawOffscreen(camera, width, height, image);
    return drawn ? std::optional<scene::Image>(std::move(image)) : std::nullopt;
}

bool SdlGpuRenderer::drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out) {
    if (width == 0 || height == 0 || width > kMaxOffscreen || height > kMaxOffscreen) {
        return false;
    }
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    // The pipelines draw in the window's format. Only 8-bit RGBA or BGRA can be read back as bytes.
    if (format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM) {
        return false;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device_, &info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = width * height * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &transferInfo);
    SDL_GPUCommandBuffer* cmd = target && transfer ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if (cmd == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    if (!render(cmd, target, width, height, camera)) {
        SDL_CancelGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion from{};
    from.texture = target;
    from.w = width;
    from.h = height;
    from.d = 1;
    SDL_GPUTextureTransferInfo to{};
    to.transfer_buffer = transfer;
    to.pixels_per_row = width;
    to.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device_, transfer);
        SDL_ReleaseGPUTexture(device_, target);
        return false;
    }
    SDL_WaitForGPUFences(device_, true, &fence, 1);
    SDL_ReleaseGPUFence(device_, fence);

    const auto* pixels = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (pixels != nullptr) {
        out.width = width;
        out.height = height;
        out.rgba.assign(pixels, pixels + std::size_t{width} * height * 4);
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
            for (std::size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
                std::swap(out.rgba[i], out.rgba[i + 2]);
            }
        }
        SDL_UnmapGPUTransferBuffer(device_, transfer);
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    SDL_ReleaseGPUTexture(device_, target);
    return pixels != nullptr;
}

SDL_GPUTexture* SdlGpuRenderer::depthFor(DepthBuffer& depth, unsigned width, unsigned height) {
    if (depth.texture == nullptr || depth.width != width || depth.height != height) {
        if (depth.texture) {
            SDL_ReleaseGPUTexture(device_, depth.texture);
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        depth.texture = SDL_CreateGPUTexture(device_, &info);
        depth.width = depth.texture ? width : 0;
        depth.height = depth.texture ? height : 0;
    }
    return depth.texture;
}

bool SdlGpuRenderer::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height,
                      const Camera& camera) {
    diag::Timer timer("record frame");
    SDL_GPUTexture* depthTexture = depthFor(instancesOnly_ ? portraitDepth_ : windowDepth_, width, height);
    if (depthTexture == nullptr) {
        return false;
    }

    uploadSpriteVertices(cmd, width, height);
    uploadJoints(cmd);

    Frame f;
    static_cast<FrameView&>(f) = FrameView::make(camera, width, height);
    f.cmd = cmd;
    f.width = width;
    f.height = height;
    f.extras = drawScene_ && !instancesOnly_;
    f.water = f.extras && gpu_->terrainIndexCount > 0 && gpu_->waterIndexCount > 0 && reflectionReady(width, height);

    if (f.water) {
        drawReflection(f);
    }
    // The shadows' data stays with the command buffer for every scene draw after it.
    if (f.extras) {
        updateObjectShadows(f);
    }
    ShadowUniforms shadows = FramePlan::shadowUniforms(*this, f.extras ? drawShadows(f) : 0);
    if (f.extras) {
        std::copy(objectGridArea_.begin(), objectGridArea_.end(), shadows.objectGrid);
    }
    SDL_PushGPUFragmentUniformData(cmd, 1, &shadows, sizeof(shadows));

    SDL_GPUColorTargetInfo color{};
    color.texture = target;
    color.clear_color = instancesOnly_ ? SDL_FColor{0, 0, 0, 0} : SDL_FColor{clear_[0], clear_[1], clear_[2], 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depthTexture;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    f.pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    // A lot's sky, behind everything (the game's skybox screen effect).
    if (f.extras && gpu_->skybox != nullptr) {
        drawSky(f, f.pass, gpu_->skybox, 1);
    }
    if (f.extras && gpu_->clipVertices != nullptr) {
        drawBoundary(f);
    }

    std::vector<Draw> opaque, layers, blended;
    FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
    drawList(f, opaque, 0);
    if (f.extras && gpu_->terrainVertices) {
        if (gpu_->terrainIndexCount > 0) {
            drawTerrain(f, f.pass, f.viewProjection, false);
        }
        if (gpu_->roadIndexCount > 0) {
            drawRoads(f);
        }
        drawImposters(cmd, f.pass, f.viewProjection);
    }
    drawList(f, layers, 1);
    if (f.water) {
        drawWater(f);
    }
    drawList(f, blended, 1);
    if (!instancesOnly_ && !sprites_.empty() && spriteVertices_) {
        drawSprites(f);
    }
    SDL_EndGPURenderPass(f.pass);
    return true;
}

// Makes the water's reflection textures at the size drawn at. False when they couldn't be made (out of
// video memory), to be tried again next frame.
bool SdlGpuRenderer::reflectionReady(unsigned width, unsigned height) {
    if (reflection_ == nullptr || reflectionDepth_ == nullptr || reflectionWidth_ != width || reflectionHeight_ != height) {
        for (auto** t : {&reflection_, &reflectionDepth_}) {
            if (*t) {
                SDL_ReleaseGPUTexture(device_, *t);
            }
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        reflection_ = SDL_CreateGPUTexture(device_, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        reflectionDepth_ = SDL_CreateGPUTexture(device_, &info);
        if (reflection_ && reflectionDepth_) {
            reflectionWidth_ = width;
            reflectionHeight_ = height;
        }
    }
    return reflection_ && reflectionDepth_;
}

// The neighborhood water's reflection: the sky and the terrain above the water, mirrored in it.
void SdlGpuRenderer::drawReflection(const Frame& f) {
    SDL_GPUColorTargetInfo rc{};
    rc.texture = reflection_;
    rc.load_op = SDL_GPU_LOADOP_CLEAR;
    rc.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo rd{};
    rd.texture = reflectionDepth_;
    rd.clear_depth = 0;
    rd.load_op = SDL_GPU_LOADOP_CLEAR;
    rd.store_op = SDL_GPU_STOREOP_DONT_CARE;
    rd.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    rd.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* mirror = SDL_BeginGPURenderPass(f.cmd, &rc, 1, &rd);
    // The sky, by each pixel's ray mirrored in the water.
    drawSky(f, mirror, gpu_->skyCube, -1);
    // The terrain, mirrored about the water's surface.
    Mat4 flip;
    flip.m = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 2 * gpu_->waterLevel, 0, 0, 0, 1};
    drawTerrain(f, mirror, f.viewProjection * flip, true);
    SDL_EndGPURenderPass(mirror);
}

// A sky cube map behind everything, looked up by each pixel's ray. `up` is -1 for the rays mirrored
// in the water.
void SdlGpuRenderer::drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, float up) {
    const std::array<float, 12> skyCamera = render::skyCamera(f, up);
    SDL_BindGPUGraphicsPipeline(on, skyPipeline_);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, skyCamera.data(), sizeof(skyCamera));
    SDL_GPUTextureSamplerBinding skyBinding{cube, sampler_};
    SDL_BindGPUFragmentSamplers(on, 0, &skyBinding, 1);
    SDL_DrawGPUPrimitives(on, 3, 1, 0, 0);
}

// The boundary curtain, into depth only: nothing beyond it shows from inside.
void SdlGpuRenderer::drawBoundary(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, clipPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    SDL_GPUBufferBinding vertices{gpu_->clipVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->clipIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, 24, 1, 0, 0, 0);
}

void SdlGpuRenderer::drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected) {
    SDL_BindGPUGraphicsPipeline(on, terrainPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
    TerrainUniforms u = gpu_->terrainUniforms;
    u.reflected = reflected ? 1.0f : 0.0f;
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &u, sizeof(u));
    // The light map wraps: the game's lighting stage sets no addressing, leaving D3D's default.
    SDL_GPUTextureSamplerBinding bindings[5] = {{gpu_->terrainPaints, sampler_},      {gpu_->terrainAlphas, spriteSampler_},
                                                {gpu_->terrainCells, spriteSampler_}, {gpu_->terrainLight, sampler_},
                                                {gpu_->terrainGradient, spriteSampler_}};
    SDL_BindGPUFragmentSamplers(on, 0, bindings, 5);
    SDL_GPUBufferBinding vertices{gpu_->terrainVertices, 0};
    SDL_BindGPUVertexBuffers(on, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->terrainIndices, 0};
    SDL_BindGPUIndexBuffer(on, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(on, gpu_->terrainIndexCount, 1, 0, 0, 0);
}

// Draws primitives with the scene pipelines (`blend` 1 for the blending ones).
void SdlGpuRenderer::drawList(const Frame& f, const std::vector<Draw>& list, int blend) {
    // Each binding SDL is given takes new GPU descriptors, so textures and joints are bound only when
    // they change (and again after a pipeline switch).
    SDL_GPUGraphicsPipeline* bound = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 10> boundSamplers{};
    bool jointsBound = false;
    const MeshBuffers* boundBuffers = nullptr;
    for (const auto& d : list) {
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        if (buffers.vertices == nullptr || buffers.indices == nullptr) {
            continue;
        }
        if (&buffers != boundBuffers) {
            const SDL_GPUBufferBinding indices{buffers.indices, 0};
            SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            boundBuffers = &buffers;
        }
        const scene::Material* m = d.material;
        const bool skinned = d.primitive->skinned;
        const int kind = !blend || !m                          ? blend
                         : m->alpha == scene::AlphaMode::Add ? 2
                         : m->alpha == scene::AlphaMode::Sum ? 3
                                                             : blend;
        SDL_GPUGraphicsPipeline* pipeline = (skinned ? skinnedPipelines_ : pipelines_)[m && m->doubleSided ? 1 : 0][kind];
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(f.pass, pipeline);
            bound = pipeline;
            boundSamplers = {};
            jointsBound = false;
        }
        if (skinned && !jointsBound) {
            SDL_BindGPUVertexStorageBuffers(f.pass, 0, &jointBuffer_, 1);
            jointsBound = true;
        }
        const DrawUniforms u = FramePlan::uniformsFor(*this, *this, f, d, blend);
        std::array<SDL_GPUTextureSamplerBinding, 10> samplers;
        for (std::size_t k = 0; k < u.samplers.size(); ++k) {
            samplers[k] = {textureFor(u.samplers[k].texture), samplerFor(u.samplers[k].sampler)};
        }
        samplers[u.samplers.size()] = {shadowBlurred_, shadowSampler_};
        samplers[u.samplers.size() + 1] = {objectPictures_, shadowSampler_};
        samplers[u.samplers.size() + 2] = {objectPlaces_, shadowPoint_};
        samplers[u.samplers.size() + 3] = {objectGrid_, shadowPoint_};
        if (skinned) {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex, sizeof(u.vertex));
        } else {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.vertex.frame, sizeof(u.vertex.frame));
        }
        SDL_PushGPUFragmentUniformData(f.cmd, 0, &u.fragment, sizeof(u.fragment));
        const auto same = [](const SDL_GPUTextureSamplerBinding& a, const SDL_GPUTextureSamplerBinding& b) {
            return a.texture == b.texture && a.sampler == b.sampler;
        };
        if (!std::equal(samplers.begin(), samplers.end(), boundSamplers.begin(), same)) {
            SDL_BindGPUFragmentSamplers(f.pass, 0, samplers.data(), static_cast<Uint32>(samplers.size()));
            boundSamplers = samplers;
        }
        // A skinned primitive's two vertex streams start where its own part of each buffer does. The
        // others share one binding, with the first vertex given to the draw.
        const auto& p = *d.primitive;
        if (skinned) {
            const SDL_GPUBufferBinding vertices[2] = {{buffers.vertices, p.firstVertex * kVertexFloats * static_cast<Uint32>(sizeof(float))},
                                                      {buffers.skin, p.firstSkin}};
            SDL_BindGPUVertexBuffers(f.pass, 0, vertices, 2);
        } else {
            const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
            SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
        }
        SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, skinned ? 0 : static_cast<Sint32>(p.firstVertex), 0);
    }
}

// The Sims' shadow pictures: each Sim drawn from the sun into its tile, then every tile blurred.
// Returns how many Sims cast a shadow.
std::size_t SdlGpuRenderer::drawShadows(const Frame& f) {
    if (jointBuffer_ == nullptr) {
        return 0;
    }
    const auto casters = FramePlan::shadowCasters(*this, *this);
    if (casters.empty()) {
        return 0;
    }
    constexpr float kSide = scene::simshadows::kResolution;
    SDL_GPUColorTargetInfo pictures{};
    pictures.texture = shadowPictures_;
    pictures.clear_color = SDL_FColor{1, 1, 1, 0};
    pictures.load_op = SDL_GPU_LOADOP_CLEAR;
    pictures.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(f.cmd, &pictures, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, shadowCasterPipeline_);
    SDL_BindGPUVertexStorageBuffers(pass, 0, &jointBuffer_, 1);
    for (std::size_t c = 0; c < casters.size(); ++c) {
        const SDL_GPUViewport tile{static_cast<float>(c % kShadowTilesAcross) * kSide, static_cast<float>(c / kShadowTilesAcross) * kSide, kSide, kSide, 0, 1};
        SDL_SetGPUViewport(pass, &tile);
        for (const auto& d : casters[c]) {
            const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
            if (buffers.vertices == nullptr || buffers.indices == nullptr || buffers.skin == nullptr) {
                continue;
            }
            const SkinnedUniforms u = FramePlan::shadowCasterUniforms(*this, d, c);
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
            const SDL_GPUBufferBinding indices{buffers.indices, 0};
            SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
            const auto& p = *d.primitive;
            const SDL_GPUBufferBinding vertices[2] = {{buffers.vertices, p.firstVertex * kVertexFloats * static_cast<Uint32>(sizeof(float))},
                                                      {buffers.skin, p.firstSkin}};
            SDL_BindGPUVertexBuffers(pass, 0, vertices, 2);
            SDL_DrawGPUIndexedPrimitives(pass, p.count, 1, p.firstIndex, 0, 0);
        }
    }
    SDL_EndGPURenderPass(pass);

    // Only the rows of tiles in use are blurred.
    SDL_GPUColorTargetInfo blurred{};
    blurred.texture = shadowBlurred_;
    blurred.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blurred.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(f.cmd, &blurred, 1, nullptr);
    const std::size_t rows = (casters.size() + kShadowTilesAcross - 1) / kShadowTilesAcross;
    const SDL_GPUViewport used{0, 0, kSide * kShadowTilesAcross, kSide * static_cast<float>(rows), 0, 1};
    SDL_SetGPUViewport(pass, &used);
    SDL_BindGPUGraphicsPipeline(pass, shadowBlurPipeline_);
    const ShadowBlurUniforms weights = FramePlan::shadowBlurUniforms();
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &weights, sizeof(weights));
    const SDL_GPUTextureSamplerBinding source{shadowPictures_, shadowPoint_};
    SDL_BindGPUFragmentSamplers(pass, 0, &source, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return casters.size();
}

bool SdlGpuRenderer::hasImage(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->textures.size() && gpu_->textures[static_cast<std::size_t>(index)] != nullptr;
}

bool SdlGpuRenderer::hasEnvCube(int index) const {
    return index >= 0 && static_cast<std::size_t>(index) < gpu_->envCubes.size() && gpu_->envCubes[static_cast<std::size_t>(index)] != nullptr;
}

bool SdlGpuRenderer::hasFadeFrom(int index) const {
    const auto from = gpu_->fadingFrom.find(index);
    return from != gpu_->fadingFrom.end() && from->second != nullptr;
}

bool SdlGpuRenderer::skinReady(std::uint32_t buffers) const {
    return jointBuffer_ != nullptr && gpu_->meshBuffers[buffers].skin != nullptr;
}

SDL_GPUTexture* SdlGpuRenderer::textureFor(const TextureRef& ref) const {
    const auto at = static_cast<std::size_t>(ref.index);
    switch (ref.kind) {
    case TextureRef::Kind::Image:
        return gpu_->textures[at];
    case TextureRef::Kind::BlackCube:
        return gpu_->blackCube;
    case TextureRef::Kind::EnvCube:
        return gpu_->envCubes[at];
    case TextureRef::Kind::FadeFrom:
        return gpu_->fadingFrom.at(ref.index);
    case TextureRef::Kind::White:
        break;
    }
    return gpu_->white;
}

void SdlGpuRenderer::drawRoads(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, roadPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    const float road[4] = {gpu_->lightMapSize, 0, 0, 0};
    SDL_PushGPUFragmentUniformData(f.cmd, 0, road, sizeof(road));
    SDL_GPUTextureSamplerBinding bindings[2] = {{gpu_->roadTextures, clampedMips_}, {gpu_->terrainLight, sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, bindings, 2);
    SDL_GPUBufferBinding vertices{gpu_->roadVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->roadIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, gpu_->roadIndexCount, 1, 0, 0, 0);
}

void SdlGpuRenderer::drawWater(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, waterPipeline_);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    WaterUniforms w = gpu_->waterUniforms;
    w.screen[0] = static_cast<float>(f.width);
    w.screen[1] = static_cast<float>(f.height);
    w.time = time_;
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &w, sizeof(w));
    SDL_GPUTextureSamplerBinding bindings[2] = {{reflection_, spriteSampler_}, {gpu_->ripples, sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, bindings, 2);
    SDL_GPUBufferBinding vertices{gpu_->waterVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->waterIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, gpu_->waterIndexCount, 1, 0, 0, 0);
}

// Sprites in order, one draw per run of the same texture.
void SdlGpuRenderer::drawSprites(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, spritePipeline_);
    const float screen[4] = {static_cast<float>(f.width), static_cast<float>(f.height), 0, 0};
    SDL_PushGPUVertexUniformData(f.cmd, 0, screen, sizeof(screen));
    SDL_GPUBufferBinding vertices{spriteVertices_, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    for (std::size_t i = 0; i < sprites_.size();) {
        const int t = sprites_[i].texture;
        std::size_t j = i;
        while (j < sprites_.size() && sprites_[j].texture == t) {
            ++j;
        }
        const bool has = t >= 0 && static_cast<std::size_t>(t) < spriteTextures_.size() && spriteTextures_[static_cast<std::size_t>(t)];
        if (!has && !gpu_->white) {
            i = j;
            continue;
        }
        SDL_GPUTextureSamplerBinding binding{has ? spriteTextures_[static_cast<std::size_t>(t)] : gpu_->white, spriteSampler_};
        SDL_BindGPUFragmentSamplers(f.pass, 0, &binding, 1);
        const auto size = has ? spriteSizes_[static_cast<std::size_t>(t)] : std::array<unsigned, 2>{1, 1};
        const float picture[4] = {static_cast<float>(size[0]), static_cast<float>(size[1]), spriteScale_, 0};
        SDL_PushGPUFragmentUniformData(f.cmd, 0, picture, sizeof(picture));
        SDL_DrawGPUPrimitives(f.pass, static_cast<Uint32>((j - i) * 6), 1, static_cast<Uint32>(i * 6), 0);
        i = j;
    }
}

} // namespace ts2::render
