#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"
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
    if (gpu_->imposterDraws.empty() || !impostersShown_) {
        return;
    }
    using Kind = scene::PaintedTerrain::ImposterPart::Kind;
    SDL_GPUBufferBinding vertices{gpu_->imposterVertices, 0};
    SDL_GPUBufferBinding indices{gpu_->imposterIndices, 0};
    struct PartUniforms {
        float tint[3];
        float cutoff, mode, light[3];
        float alpha, unused[3];
    };
    SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_GPUGraphicsPipeline* bound = nullptr;
    auto draw = [&](SDL_GPUGraphicsPipeline* pipeline, const ImposterDraw& d, SDL_GPUTexture* texture, SDL_GPUSampler* sampler,
                    float cutoff, float mode, float alpha = 1) {
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            SDL_PushGPUVertexUniformData(cmd, 0, viewProjection.m.data(), 16 * sizeof(float));
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
            bound = pipeline;
        }
        const PartUniforms u{{d.tint[0], d.tint[1], d.tint[2]}, cutoff, mode, {imposterLight_[0], imposterLight_[1], imposterLight_[2]}, alpha, {}};
        SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof(u));
        SDL_GPUTextureSamplerBinding binding{texture, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUIndexedPrimitives(pass, d.indexCount, 1, d.firstIndex, d.vertexOffset, 0);
    };
    // The terrain first (its shader layer comes before the others'): light map, then texture.
    for (const auto& d : gpu_->imposterDraws) {
        if (d.kind != Kind::Terrain || imposterLeftOut(d.lot) || boxOutside(viewProjection, d.low, d.high)) {
            continue;
        }
        if (d.lightMap) {
            draw(drawing_->imposterLight, d, d.lightMap, sampler_, 0, 0);
        }
        draw(drawing_->imposterTerrain, d, d.texture, clampedMips_, 0, 1);
    }
    // A fading imposter's walls, roofs and slices take its alpha, its terrain doesn't. Slices keep their
    // blend and depth writes, so their alpha test drops more of them as they fade.
    for (const auto& d : gpu_->imposterDraws) {
        if (d.kind == Kind::Terrain || imposterLeftOut(d.lot) || boxOutside(viewProjection, d.low, d.high)) {
            continue;
        }
        const float alpha = imposterAlpha(d.lot);
        switch (d.kind) {
        case Kind::Wall:
            draw(alpha < 1 ? drawing_->imposterFadedWall : drawing_->imposterWall, d, d.texture, pointSampler_, 64.0f / 255, 1, alpha);
            break;
        case Kind::Slice:
            draw(drawing_->imposterWall, d, d.texture, pointSampler_, 127.0f / 255, 1, alpha);
            break;
        case Kind::Roof:
            draw(alpha < 1 ? drawing_->imposterFadedRoof : drawing_->imposterRoof, d, d.texture, clampedMips_, -1, 2, alpha);
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
    const bool saved = surface != nullptr && SDL_SavePNG(surface, diag::utf8(path).c_str());
    SDL_DestroySurface(surface);
    return saved;
}

std::optional<scene::Image> SdlGpuRenderer::drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                           std::optional<scene::ObjectLighting> lighting) {
    // Back to the scene's own drawing afterwards, even if drawing throws.
    struct Restore {
        SdlGpuRenderer& r;
        std::optional<std::optional<scene::ObjectLighting>> lighting;
        ~Restore() {
            if (lighting) {
                r.objectLighting_ = std::move(*lighting);
            }
            r.instancesOnly_ = false;
            r.ownLights_ = false;
        }
    } restore{*this, {}};
    if (lighting) {
        restore.lighting = std::exchange(objectLighting_, std::move(lighting));
        ownLights_ = true;
    }
    instancesOnly_ = true;
    scene::Image image;
    const bool drawn = drawOffscreen(camera, width, height, image);
    return drawn ? std::optional<scene::Image>(std::move(image)) : std::nullopt;
}

SdlGpuRenderer::GpuPictures::~GpuPictures() {
    // The GPU is done with a buffer once the fence is, so one let go earlier waits for it here.
    if (fence != nullptr) {
        SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
    }
    for (auto& t : targets) {
        if (t.transfer != nullptr) {
            pool->giveBack(t.side, t.transfer);
        }
    }
}

SDL_GPUTransferBuffer* SdlGpuRenderer::TransferPool::take(unsigned side) {
    {
        std::lock_guard held(lock);
        if (auto& kept = free[side]; !kept.empty()) {
            SDL_GPUTransferBuffer* buffer = kept.back();
            kept.pop_back();
            return buffer;
        }
    }
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = side * side * 4;
    return SDL_CreateGPUTransferBuffer(device, &info);
}

void SdlGpuRenderer::TransferPool::giveBack(unsigned side, SDL_GPUTransferBuffer* buffer) {
    // As many as are drawn at once are kept of each side.
    constexpr std::size_t kMostKept = 16;
    std::lock_guard held(lock);
    if (device == nullptr) {
        return;
    }
    if (auto& kept = free[side]; kept.size() < kMostKept) {
        kept.push_back(buffer);
        return;
    }
    SDL_ReleaseGPUTransferBuffer(device, buffer);
}

void SdlGpuRenderer::TransferPool::releaseAll(bool forever) {
    std::lock_guard held(lock);
    for (auto& [side, kept] : free) {
        for (auto* buffer : kept) {
            SDL_ReleaseGPUTransferBuffer(device, buffer);
        }
    }
    free.clear();
    if (forever) {
        device = nullptr;
    }
}

void SdlGpuRenderer::releasePictureTargets() {
    // SDL frees what a frame in flight still uses once that frame is done.
    for (auto& [side, square] : pictureTargets_) {
        SDL_ReleaseGPUTexture(device_, square.color);
        SDL_ReleaseGPUTexture(device_, square.depth.texture);
    }
    pictureTargets_.clear();
    transferPool_->releaseAll(false);
}

bool SdlGpuRenderer::makePreparedLevels(Prepared& prepared, std::size_t textures) {
    auto* p = dynamic_cast<GpuPrepared*>(prepared.get());
    if (p == nullptr || p->withMips.empty()) {
        return true;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return false;
    }
    // SDL makes mip levels with pipelines it keeps without a lock: only here, on the drawing thread.
    const std::size_t count = std::min(textures, p->withMips.size());
    for (std::size_t i = 0; i < count; ++i) {
        SDL_GenerateMipmapsForGPUTexture(cmd, p->withMips[p->withMips.size() - 1 - i]);
    }
    p->withMips.resize(p->withMips.size() - count);
    SDL_SubmitGPUCommandBuffer(cmd);
    return p->withMips.empty();
}

IRenderer::Pictures SdlGpuRenderer::startPreparedImages(Prepared& prepared, const std::vector<PreparedShot>& shots) {
    auto* p = dynamic_cast<GpuPrepared*>(prepared.get());
    const SDL_GPUTextureFormat format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    // The pipelines draw in the window's format. Only 8-bit RGBA or BGRA can be read back as bytes.
    if (p == nullptr || !p->gpu || p->device != device_ ||
        (format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && format != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)) {
        return {};
    }
    auto pictures = std::make_unique<GpuPictures>();
    pictures->device = device_;
    pictures->pool = transferPool_;
    pictures->swapRedBlue = format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    pictures->targets.resize(shots.size());
    pictureScene_ = p->serial;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return {};
    }
    // SDL makes mip levels with pipelines it keeps without a lock: only here, on the drawing thread.
    for (auto* t : p->withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    p->withMips.clear();
    // The prepared scene is drawn in place of the drawn one, which is put back as it was even if
    // drawing throws.
    struct Lend {
        SdlGpuRenderer& r;
        GpuPrepared& p;
        InstanceState kept;
        std::vector<ParticleBatch> particles;  // the drawn scene's, which pictures leave out
        Lend(SdlGpuRenderer& renderer, GpuPrepared& prepared) : r(renderer), p(prepared) {
            swap();
            r.instancesOnly_ = true;
            r.picturing_ = true;
            std::swap(r.particleBatches_, particles);
        }
        ~Lend() {
            std::swap(r.particleBatches_, particles);
            r.picturing_ = false;
            r.instancesOnly_ = false;
            r.pictureDepth_ = nullptr;
            r.picturePools_ = nullptr;
            swap();
        }
        Lend(const Lend&) = delete;
        Lend& operator=(const Lend&) = delete;
        void swap() {
            std::swap(r.gpu_, p.gpu);
            r.lendScene(p, kept);
        }
    } lend(*this, *p);
    try {
        for (std::size_t i = 0; i < shots.size(); ++i) {
            const auto& shot = shots[i];
            auto& target = pictures->targets[i];
            if (shot.width == 0 || shot.height == 0 || shot.width > kMaxOffscreen || shot.height > kMaxOffscreen) {
                continue;
            }
            // Drawn into the top left of a kept square target, the view widened to the square so that the
            // shot's pixels are its own.
            unsigned side = 64;
            while (side < shot.width || side < shot.height) {
                side *= 2;
            }
            auto& square = pictureTargets_[side];
            if (square.color == nullptr) {
                SDL_GPUTextureCreateInfo info{};
                info.type = SDL_GPU_TEXTURETYPE_2D;
                info.format = format;
                info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                info.width = side;
                info.height = side;
                info.layer_count_or_depth = 1;
                info.num_levels = 1;
                square.color = SDL_CreateGPUTexture(device_, &info);
            }
            Camera camera = shot.camera;
            if (camera.orthographic[1] != camera.orthographic[0]) {
                auto& o = camera.orthographic;
                o[1] = o[0] + (o[1] - o[0]) * static_cast<float>(side) / static_cast<float>(shot.width);
                o[2] = o[3] - (o[3] - o[2]) * static_cast<float>(side) / static_cast<float>(shot.height);
            }
            target.side = side;
            target.transfer = transferPool_->take(side);
            pictureDepth_ = &square.depth;
            picturePools_ = shot.pools.empty() ? nullptr : &shot.pools;
            show(shot.shown);
            if (square.color == nullptr || target.transfer == nullptr || !render(cmd, square.color, side, side, camera)) {
                if (target.transfer != nullptr) {
                    transferPool_->giveBack(side, target.transfer);
                }
                target = {};
                continue;
            }
            target.width = shot.width;
            target.height = shot.height;
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureRegion from{};
            from.texture = square.color;
            from.w = shot.width;
            from.h = shot.height;
            from.d = 1;
            SDL_GPUTextureTransferInfo to{};
            to.transfer_buffer = target.transfer;
            to.pixels_per_row = shot.width;
            to.rows_per_layer = shot.height;
            SDL_DownloadFromGPUTexture(copy, &from, &to);
            SDL_EndGPUCopyPass(copy);
        }
    } catch (...) {
        SDL_CancelGPUCommandBuffer(cmd);
        throw;
    }
    pictures->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (pictures->fence == nullptr) {
        // Its buffers go back for the next pictures only once the GPU is done with them.
        SDL_WaitForGPUIdle(device_);
        return {};
    }
    return pictures;
}

std::optional<std::vector<scene::Image>> SdlGpuRenderer::takePreparedImages(PicturesBase& given) {
    auto* pictures = dynamic_cast<GpuPictures*>(&given);
    if (pictures == nullptr || pictures->fence == nullptr) {
        return std::vector<scene::Image>{};
    }
    if (!SDL_QueryGPUFence(device_, pictures->fence)) {
        return std::nullopt;
    }
    std::vector<scene::Image> out(pictures->targets.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto& target = pictures->targets[i];
        if (target.transfer == nullptr) {
            continue;
        }
        const auto* pixels = static_cast<const std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, target.transfer, false));
        if (pixels == nullptr) {
            continue;
        }
        auto& image = out[i];
        image.width = target.width;
        image.height = target.height;
        image.rgba.assign(pixels, pixels + std::size_t{image.width} * image.height * 4);
        SDL_UnmapGPUTransferBuffer(device_, target.transfer);
        if (pictures->swapRedBlue) {
            for (std::size_t k = 0; k + 3 < image.rgba.size(); k += 4) {
                std::swap(image.rgba[k], image.rgba[k + 2]);
            }
        }
    }
    return out;
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

// A multisampled target for the main view. Null when it can't be made (out of video memory).
SDL_GPUTexture* SdlGpuRenderer::smoothTarget(DepthBuffer& buffer, bool isDepth, unsigned width, unsigned height) {
    if (buffer.texture == nullptr || buffer.width != width || buffer.height != height) {
        if (buffer.texture) {
            SDL_ReleaseGPUTexture(device_, buffer.texture);
        }
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = isDepth ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT : SDL_GetGPUSwapchainTextureFormat(device_, window_);
        info.usage = isDepth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        info.sample_count = smoothSamples_ >= 8 ? SDL_GPU_SAMPLECOUNT_8 : smoothSamples_ >= 4 ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_2;
        buffer.texture = SDL_CreateGPUTexture(device_, &info);
        buffer.width = buffer.texture ? width : 0;
        buffer.height = buffer.texture ? height : 0;
    }
    return buffer.texture;
}

bool SdlGpuRenderer::render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height,
                      const Camera& camera) {
    diag::Timer timer("record frame");
    // Everything before the main view's pass is drawn with one sample.
    drawing_ = &single_;
    occlusionBound_ = noOcclusion_;
    bool smooth = smoothSamples_ > 1 && !instancesOnly_;
    if (smooth && (smoothTarget(smoothColor_, false, width, height) == nullptr || smoothTarget(smoothDepth_, true, width, height) == nullptr)) {
        diag::warnOnce("smooth edges: no video memory for its targets, so edges stay as they are");
        smooth = false;
    }
    SDL_GPUTexture* depthTexture = smooth ? smoothDepth_.texture
                                          : depthFor(pictureDepth_ != nullptr ? *pictureDepth_ : instancesOnly_ ? portraitDepth_ : windowDepth_, width, height);
    if (depthTexture == nullptr) {
        return false;
    }

    uploadSpriteVertices(cmd, width, height);
    uploadParticleVertices(cmd);
    uploadJoints(cmd);

    Frame f;
    static_cast<FrameView&>(f) = FrameView::make(camera, width, height);
    f.cmd = cmd;
    f.width = width;
    f.height = height;
    f.extras = drawScene_ && !instancesOnly_;
    f.water = f.extras && gpu_->terrainIndexCount > 0 && gpu_->waterIndexCount > 0 && reflectionReady(width, height);

    // The shadows' data stays with the command buffer for every scene draw after it.
    if (f.extras) {
        updateObjectShadows(f);
    }
    ShadowUniforms shadows = FramePlan::shadowUniforms(*this, f.extras ? drawShadows(f) : 0);
    if (f.extras) {
        std::copy(objectGridArea_.begin(), objectGridArea_.end(), shadows.objectGrid);
        std::copy(objectFloorsArea_.begin(), objectFloorsArea_.begin() + 12, &shadows.objectFloorsToLot[0][0]);
        std::copy(objectFloorsArea_.begin() + 12, objectFloorsArea_.end(), shadows.objectFloors);
    }
    if (gpu_ && gpu_->terrainVertices) {
        std::copy(gpu_->underwater.begin(), gpu_->underwater.end(), shadows.underwater);
        for (std::size_t r = 0; r < kUnderwaterRows; ++r) {
            std::copy(gpu_->underwaterRows[r].begin(), gpu_->underwaterRows[r].end(), shadows.underwaterGradient[r]);
        }
    }

    // The draw lists are kept between frames, so they reuse their storage.
    // A picture's pools light their floors and walls with their caustics whether or not the shot shows
    // their water, as the game's bake draws them. Draws take the caustics as they are collected, and
    // the caustics need only the water's instance and material.
    if (picturePools_ != nullptr) {
        poolCaustics_ = false;
        const auto& meshes = gpu_->meshes;
        for (std::size_t i = 0; i < instances_.size() && i < picturePools_->size() && !poolCaustics_; ++i) {
            const auto& instance = instances_[i];
            if (!instance.poolWater || !(*picturePools_)[i] || instance.mesh < 0 || static_cast<std::size_t>(instance.mesh) >= meshes.size()) {
                continue;
            }
            for (const auto& primitive : meshes[static_cast<std::size_t>(instance.mesh)].primitives) {
                if (primitive.material >= 0 && static_cast<std::size_t>(primitive.material) < materials_.size() &&
                    materials_[static_cast<std::size_t>(primitive.material)].poolSurface) {
                    const Draw water{&primitive, &instance, &materials_[static_cast<std::size_t>(primitive.material)], 0, 0, 1};
                    poolCaustics_ = drawPoolCaustics(f, water, shadows, pictureCausticsScene_ != pictureScene_);
                    pictureCausticsScene_ = poolCaustics_ ? pictureScene_ : pictureCausticsScene_;
                    break;
                }
            }
        }
    }
    thread_local std::vector<Draw> opaque, layers, blended;
    opaque.clear();
    layers.clear();
    blended.clear();
    FramePlan::collectDraws(*this, *this, f, opaque, layers, blended);
    const Draw* pool = f.extras || picturePools_ != nullptr ? FramePlan::poolWater(blended) : nullptr;
    const Draw* sea = f.extras ? FramePlan::skirtSea(blended) : nullptr;
    // A beach sea drawn with the pools' caustics (not in the original game) plays them where no
    // pool does.
    const Draw* caustics = pool != nullptr ? pool : sea != nullptr && sea->material->causticsImage >= 0 ? sea : nullptr;
    if (picturePools_ == nullptr) {
        poolCaustics_ = caustics != nullptr && drawPoolCaustics(f, *caustics, shadows);
        // A picture drawn next draws its own again.
        pictureCausticsScene_ = poolCaustics_ ? 0 : pictureCausticsScene_;
    }
    SDL_PushGPUFragmentUniformData(cmd, 1, &shadows, sizeof(shadows));
    if (f.water) {
        drawReflection(f);
    }
    poolReflected_ = false;
    if (pool != nullptr && single_.pool != nullptr && mirrorReady(poolReflection_, width, height)) {
        const float mirrorHeight = pool->instance->transform.m[11] + pool->primitive->low[2] - kPoolMirrorDrop;
        if (picturePools_ != nullptr) {
            // A picture's mirror image shows the sky and the lot with its walls up over the whole
            // view, as the game's bake draws it.
            auto shown = std::exchange(visible_, *picturePools_);
            drawMirror(f, mirrorHeight, pool->instance->poolMirrorKeep, poolReflection_, Mirrored::Pool, {-1, 1, -1, 1});
            visible_ = std::move(shown);
        } else if (reflections_) {
            drawMirror(f, mirrorHeight, pool->instance->poolMirrorKeep, poolReflection_, Mirrored::Pool,
                       poolMirrorArea(f, blended, width, height));
        } else {
            clearMirror(f, poolReflection_);
        }
        poolReflected_ = true;
    }
    // The beach sea's mirror image, at the sea's own height. The game redraws it only when the
    // camera moves, which shows the same picture.
    seaReflected_ = false;
    // A neighborhood's pool-style sea (not in the original game) shows the water sheet's reflection.
    const bool hoodSea = gpu_ && gpu_->poolStyleSea;
    seaFromWater_ = hoodSea && f.water && reflections_ && sea != nullptr && single_.sea != nullptr;
    seaReflected_ = seaFromWater_;
    if (!hoodSea && reflections_ && seaReflections_ && sea != nullptr && single_.sea != nullptr && mirrorReady(seaReflection_, width, height)) {
        drawMirror(f, sea->instance->transform.m[11] + sea->primitive->low[2], 0, seaReflection_, Mirrored::Sea, {-1, 1, -1, 1});
        seaReflected_ = true;
    }

    const bool occluded = f.extras && occlusionReady(width, height);
    // The opaque surfaces' uniforms are made once for both passes when occlusion draws them too.
    thread_local std::vector<DrawUniforms> opaqueUniforms;
    opaqueUniforms.clear();
    bool overDepth = false;
    if (occluded) {
        opaqueUniforms.reserve(opaque.size());
        for (const Draw& d : opaque) {
            opaqueUniforms.push_back(FramePlan::uniformsFor(*this, *this, f, d, 0));
        }
        drawOcclusion(f, opaque, opaqueUniforms);
        occlusionBound_ = occlusion_.blurred;
        // With one sample the main view starts from occlusion's depth. That needs every opaque surface
        // to leave the same pixels there as in the main view, which the water surfaces drawn their own
        // way and the ground cut away under neighbors' real houses don't.
        overDepth = !smooth && !picturing_ && pictureDepth_ == nullptr && shadows.neighborGrounds[0] == 0 &&
                    std::none_of(opaque.begin(), opaque.end(), [](const Draw& d) {
                        return d.material && (d.material->pondWater || d.material->poolSurface || d.material->skirtSea);
                    });
        shadows.ambientOcclusion[0] = 1;
        SDL_PushGPUFragmentUniformData(cmd, 1, &shadows, sizeof(shadows));
    }

    SDL_GPUColorTargetInfo color{};
    color.texture = smooth ? smoothColor_.texture : target;
    color.resolve_texture = smooth ? target : nullptr;
    color.clear_color = instancesOnly_ ? SDL_FColor{0, 0, 0, 0} : SDL_FColor{clear_[0], clear_[1], clear_[2], 1.0f};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = smooth ? SDL_GPU_STOREOP_RESOLVE : SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = overDepth ? occlusion_.depth : depthTexture;
    depth.clear_depth = 0;
    depth.load_op = overDepth ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    // The main view's depth stays in its buffer after the pass, for tools that read it, such as ReShade.
    depth.store_op = instancesOnly_ ? SDL_GPU_STOREOP_DONT_CARE : SDL_GPU_STOREOP_STORE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    drawing_ = smooth ? &smooth_ : picturing_ ? &picture_ : &single_;
    f.pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);
    // A lot's sky, behind everything (the game's skybox screen effect).
    if (f.extras && gpu_->skybox != nullptr && camera.orthographic[1] <= camera.orthographic[0]) {
        drawSky(f, f.pass, gpu_->skybox, skyCamera(f, 1));
    }
    if (f.extras && gpu_->clipVertices != nullptr) {
        drawBoundary(f);
    }

    drawList(f, opaque, 0, opaqueUniforms, overDepth);
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
    // What is blended over the surfaces wasn't in the occlusion's depth pass.
    if (occluded) {
        shadows.ambientOcclusion[0] = 0;
        SDL_PushGPUFragmentUniformData(cmd, 1, &shadows, sizeof(shadows));
    }
    if (f.water && !hoodSea) {
        drawWater(f);
    }
    if (f.extras) {
        drawBlended(f, blended);
    } else {
        drawList(f, blended, 1);
    }
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

// The neighborhood water's reflection. With the Reflections option off the game's water shows only
// the sky.
void SdlGpuRenderer::drawReflection(const Frame& f) {
    const MirrorTarget target{reflection_, reflectionDepth_, reflectionWidth_, reflectionHeight_};
    if (reflections_) {
        drawMirror(f, gpu_->waterLevel, 0, target, Mirrored::Neighborhood, {-1, 1, -1, 1});
        return;
    }
    SDL_GPUColorTargetInfo color{};
    color.texture = reflection_;
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = reflectionDepth_;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* mirror = SDL_BeginGPURenderPass(f.cmd, &color, 1, &depth);
    drawSky(f, mirror, gpu_->skyCube, mirroredSkyCamera(f));
    SDL_EndGPURenderPass(mirror);
}

// Makes a mirror image's textures at the size drawn at.
bool SdlGpuRenderer::mirrorReady(MirrorTarget& target, unsigned width, unsigned height) {
    if (target.color == nullptr || target.depth == nullptr || target.width != width || target.height != height) {
        for (auto** t : {&target.color, &target.depth}) {
            if (*t) {
                SDL_ReleaseGPUTexture(device_, *t);
                *t = nullptr;
            }
        }
        target.width = target.height = 0;
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D;
        info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = width;
        info.height = height;
        info.layer_count_or_depth = 1;
        info.num_levels = 1;
        target.color = SDL_CreateGPUTexture(device_, &info);
        info.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
        target.depth = SDL_CreateGPUTexture(device_, &info);
        if (target.color && target.depth) {
            target.width = width;
            target.height = height;
        }
    }
    return target.color && target.depth;
}

// A mirror image with nothing in it, for water with the Reflections option off.
void SdlGpuRenderer::clearMirror(const Frame& f, const MirrorTarget& target) {
    SDL_GPUColorTargetInfo color{};
    color.texture = target.color;
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_EndGPURenderPass(SDL_BeginGPURenderPass(f.cmd, &color, 1, nullptr));
}

// Draws the scene mirrored in water, the sky and what lies above `plane`, as the game's reflections do.
// A pool's leaves out the pools' and ponds' water. The beach sea's shows only the skirt's ground and
// sand, the lot's terrain and the neighborhood's plants (see scene::Instance::seaReflected). The
// neighborhood water's shows terrain, roads, plants, decorations, bridges and effects. None shows lot
// imposters. Only `area` is drawn (see poolMirrorArea).
void SdlGpuRenderer::drawMirror(const Frame& f, float plane, float keepBelow, const MirrorTarget& target, Mirrored what,
                                const std::array<float, 4>& area) {
    Camera mirrored = *f.camera;
    mirrored.eye[2] = 2 * plane - mirrored.eye[2];
    mirrored.target[2] = 2 * plane - mirrored.target[2];
    Frame r = f;
    r.camera = &mirrored;
    r.viewProjection = poolMirror(f.viewProjection, plane, keepBelow);
    r.depthClip = true;

    thread_local std::vector<Draw> opaque, layers, blended;
    opaque.clear();
    layers.clear();
    blended.clear();
    Frame culled = r;
    culled.viewProjection = narrowedTo(r.viewProjection, area);
    FramePlan::collectDraws(*this, *this, culled, opaque, layers, blended);
    for (auto* list : {&opaque, &layers, &blended}) {
        if (what != Mirrored::Pool) {
            std::erase_if(*list, [](const Draw& d) { return !d.instance->seaReflected; });
        } else {
            std::erase_if(*list, [](const Draw& d) { return d.instance->poolWater || (d.material && d.material->pondWater); });
        }
    }

    SDL_GPUColorTargetInfo color{};
    color.texture = target.color;
    color.clear_color = SDL_FColor{0, 0, 0, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = target.depth;
    depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    r.pass = SDL_BeginGPURenderPass(f.cmd, &color, 1, &depth);
    // Clip space y runs up, the target's rows down.
    const auto pixel = [](float at, unsigned size) { return std::clamp(static_cast<int>((at + 1) / 2 * static_cast<float>(size)), 0, static_cast<int>(size)); };
    const int left = pixel(area[0], f.width), right = pixel(area[1], f.width) + 1, top = static_cast<int>(f.height) - pixel(area[3], f.height) - 1,
              bottom = static_cast<int>(f.height) - pixel(area[2], f.height);
    const SDL_Rect scissor{std::max(left, 0), std::max(top, 0), std::min(right, static_cast<int>(f.width)) - std::max(left, 0),
                           std::min(bottom, static_cast<int>(f.height)) - std::max(top, 0)};
    SDL_SetGPUScissor(r.pass, &scissor);
    // A lot's show its own sky (the game's $skyboxCubeMap in lot view), the neighborhood's its sky cube.
    SDL_GPUTexture* sky = what == Mirrored::Neighborhood ? gpu_->skyCube : gpu_->skybox;
    if (sky != nullptr) {
        drawSky(r, r.pass, sky, mirroredSkyCamera(f));
    }
    drawList(r, opaque, 0);
    if (what == Mirrored::Neighborhood) {
        drawTerrain(r, r.pass, r.viewProjection, true);
        if (gpu_->roadIndexCount > 0) {
            drawRoads(r);
        }
    }
    drawList(r, layers, 1);
    if (what == Mirrored::Neighborhood) {
        drawBlended(r, blended);
    } else {
        drawList(r, blended, 1);
    }
    SDL_EndGPURenderPass(r.pass);
}

bool SdlGpuRenderer::drawPoolCaustics(const Frame& f, const Draw& pool, ShadowUniforms& shadows, bool passes) {
    const int flipbook = pool.material->causticsImage;
    if (!objectLighting_ || objectLighting_->lights.empty() || flipbook < 0 || !hasImage(flipbook)) {
        return false;
    }
    const auto& t = pool.instance->transform.m;
    const std::array<float, 12> place{t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10], t[11]};
    const Vec3 toSun = normalize(objectLighting_->lights.front().direction);
    const PoolLight light = poolLight(place, pool.instance->poolBox, toSun);
    if (!passes) {
        std::copy(light.texture.m.begin(), light.texture.m.end(), shadows.poolShadow);
        std::copy(light.toLot.m.begin(), light.toLot.m.end(), shadows.toLot);
        std::copy(toSun.begin(), toSun.end(), shadows.poolSun);
        return true;
    }

    // The tile: this frame of the flipbook, at the time of day's brightness.
    SDL_GPUColorTargetInfo tile{};
    tile.texture = causticsTile_;
    tile.load_op = SDL_GPU_LOADOP_DONT_CARE;
    tile.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(f.cmd, &tile, 1, nullptr);
    SDL_BindGPUGraphicsPipeline(pass, causticsPipeline_);
    const CausticsUniforms frame = causticsFrame(time_, pool.instance->poolNight);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, &frame, sizeof(frame));
    const SDL_GPUTextureSamplerBinding book{textureFor({TextureRef::Kind::Image, flipbook}), sampler_};
    SDL_BindGPUFragmentSamplers(pass, 0, &book, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);

    // The shadow map: what the game draws into its pool render target, seen from the sun.
    thread_local std::vector<Draw> casters;
    casters.clear();
    FramePlan::poolCasters(*this, *this, light.clip, casters);
    SDL_GPUColorTargetInfo color{};
    color.texture = poolShadow_;
    color.clear_color = SDL_FColor{1, 1, 1, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    color.store_op = SDL_GPU_STOREOP_STORE;
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = poolShadowDepth_;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    pass = SDL_BeginGPURenderPass(f.cmd, &color, 1, &depth);
    SDL_GPUGraphicsPipeline* bound = nullptr;
    for (const auto& d : casters) {
        const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
        if (buffers.vertices == nullptr || buffers.indices == nullptr) {
            continue;
        }
        const bool skinned = d.primitive->skinned;
        SDL_GPUGraphicsPipeline* pipeline = skinned ? poolShadowSkinnedPipeline_ : poolShadowPipeline_;
        if (pipeline != bound) {
            SDL_BindGPUGraphicsPipeline(pass, pipeline);
            bound = pipeline;
            if (skinned) {
                SDL_BindGPUVertexStorageBuffers(pass, 0, &jointBuffer_, 1);
            }
        }
        SkinnedUniforms u{};
        std::copy(light.clip.m.begin(), light.clip.m.end(), u.frame.viewProjection);
        const auto& m = d.instance->transform.m;
        const float model[16] = {m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], 0, 0, 0, 1};
        std::copy(std::begin(model), std::end(model), u.frame.model);
        const auto& p = *d.primitive;
        const SDL_GPUBufferBinding indices{buffers.indices, 0};
        SDL_BindGPUIndexBuffer(pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        if (skinned) {
            u.jointBase = static_cast<std::uint32_t>(jointBase_[static_cast<std::size_t>(d.instance - instances_.data())]);
            u.morphRow = morphRow(static_cast<std::size_t>(d.instance - instances_.data()));
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
            const SDL_GPUBufferBinding vertices[2] = {{buffers.vertices, p.firstVertex * kVertexFloats * static_cast<Uint32>(sizeof(float))},
                                                      {buffers.skin, p.firstSkin}};
            SDL_BindGPUVertexBuffers(pass, 0, vertices, 2);
            bindSkinStorage(pass, buffers);
        } else {
            SDL_PushGPUVertexUniformData(f.cmd, 0, &u.frame, sizeof(u.frame));
            const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
            SDL_BindGPUVertexBuffers(pass, 0, &vertices, 1);
        }
        SDL_DrawGPUIndexedPrimitives(pass, p.count, 1, p.firstIndex, skinned ? 0 : static_cast<Sint32>(p.firstVertex), 0);
    }
    SDL_EndGPURenderPass(pass);

    std::copy(light.texture.m.begin(), light.texture.m.end(), shadows.poolShadow);
    std::copy(light.toLot.m.begin(), light.toLot.m.end(), shadows.toLot);
    std::copy(toSun.begin(), toSun.end(), shadows.poolSun);
    return true;
}

// A pool's water surface over the pool's reflection (see pool.vert.hlsl).
void SdlGpuRenderer::drawPoolSurface(const Frame& f, const Draw& d) {
    const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
    const PoolUniforms u = FramePlan::poolUniformsFor(*this, f, d);
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->pool);
    SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
    const SDL_GPUTextureSamplerBinding samplers[2] = {{poolReflection_.color, clampedSampler_},
                                                      {textureFor({TextureRef::Kind::Image, d.material->normalMap}), sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, samplers, 2);
    const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    const auto& p = *d.primitive;
    SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, static_cast<Sint32>(p.firstVertex), 0);
}

// A beach lot's sea over its mirror image (see sea.vert.hlsl).
void SdlGpuRenderer::drawSkirtSea(const Frame& f, const Draw& d) {
    const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
    SeaUniforms u = FramePlan::seaUniformsFor(*this, f, d);
    // A neighborhood's pool-style sea (not in the original game) is made more opaque, so what lies deep
    // is barely seen as under the game's water sheet, and its mirror image darkened so it keeps the
    // pool-style blue. Its waves still show as its opacity changes with them, and they shift and blur
    // its mirror image as murky ponds drawn like pools do.
    if (seaFromWater_) {
        constexpr float kHoodSea[4] = {0.55f, 0.7f, 0.64f, 0.2f};
        std::copy(std::begin(kHoodSea), std::end(kHoodSea), u.scale);
        std::fill(u.tint, u.tint + 3, 0.0f);
        u.time[1] = 1;
    }
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->sea);
    SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
    const SDL_GPUTextureSamplerBinding samplers[2] = {{seaFromWater_ ? reflection_ : seaReflection_.color, clampedSampler_},
                                                      {textureFor({TextureRef::Kind::Image, d.material->normalMap}), sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, samplers, 2);
    const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    const auto& p = *d.primitive;
    SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, static_cast<Sint32>(p.firstVertex), 0);
}

// The water in a lot's ponds (see pond.vert.hlsl).
void SdlGpuRenderer::drawPondWater(const Frame& f, const Draw& d) {
    const MeshBuffers& buffers = gpu_->meshBuffers[d.primitive->buffers];
    const PondUniforms u = FramePlan::pondUniformsFor(*this, f, d);
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->pond);
    SDL_PushGPUVertexUniformData(f.cmd, 0, &u, sizeof(u));
    SDL_PushGPUFragmentUniformData(f.cmd, 0, u.light, sizeof(u.light));
    const scene::Material& m = *d.material;
    const SDL_GPUTextureSamplerBinding samplers[3] = {{textureFor({TextureRef::Kind::EnvCube, m.envCube}), clampedSampler_},
                                                      {textureFor({TextureRef::Kind::Image, m.ripples[0]}), sampler_},
                                                      {textureFor({TextureRef::Kind::Image, m.ripples[1]}), sampler_}};
    SDL_BindGPUFragmentSamplers(f.pass, 0, samplers, 3);
    const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    const auto& p = *d.primitive;
    SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, static_cast<Sint32>(p.firstVertex), 0);
}

// A sky cube map behind everything, looked up by each pixel's ray (see skyCamera).
void SdlGpuRenderer::drawSky(const Frame& f, SDL_GPURenderPass* on, SDL_GPUTexture* cube, const std::array<float, 12>& camera) {
    SDL_BindGPUGraphicsPipeline(on, drawing_->sky);
    SDL_PushGPUFragmentUniformData(f.cmd, 0, camera.data(), sizeof(camera));
    SDL_GPUTextureSamplerBinding skyBinding{cube, sampler_};
    SDL_BindGPUFragmentSamplers(on, 0, &skyBinding, 1);
    SDL_DrawGPUPrimitives(on, 3, 1, 0, 0);
}

// The boundary curtain, into depth only: nothing beyond it shows from inside.
void SdlGpuRenderer::drawBoundary(const Frame& f) {
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->clip);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    SDL_GPUBufferBinding vertices{gpu_->clipVertices, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    SDL_GPUBufferBinding indices{gpu_->clipIndices, 0};
    SDL_BindGPUIndexBuffer(f.pass, &indices, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    SDL_DrawGPUIndexedPrimitives(f.pass, 24, 1, 0, 0, 0);
}

void SdlGpuRenderer::drawTerrain(const Frame& f, SDL_GPURenderPass* on, const Mat4& viewProjection, bool reflected) {
    SDL_BindGPUGraphicsPipeline(on, drawing_->terrain);
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
void SdlGpuRenderer::drawList(const Frame& f, std::span<const Draw> list, int blend, std::span<const DrawUniforms> ready, bool overDepth) {
    // Each binding SDL is given takes new GPU descriptors, so textures and joints are bound only when
    // they change (and again after a pipeline switch).
    SDL_GPUGraphicsPipeline* bound = nullptr;
    std::array<SDL_GPUTextureSamplerBinding, 16> boundSamplers{};
    bool jointsBound = false;
    const MeshBuffers* boundBuffers = nullptr;
    DrawUniforms made;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const Draw& d = list[i];
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
        if (m && m->pondWater) {
            if (f.pass != nullptr && hasEnvCube(m->envCube) && hasImage(m->ripples[0]) && hasImage(m->ripples[1])) {
                drawPondWater(f, d);
                bound = nullptr;
                boundSamplers = {};
                jointsBound = false;
            }
            continue;
        }
        // Without its mirror image the beach sea is drawn as below, with its sky cube.
        if (m && m->skirtSea && seaReflected_ && f.pass != nullptr && m->normalMap >= 0 && hasImage(m->normalMap)) {
            drawSkirtSea(f, d);
            bound = nullptr;
            boundSamplers = {};
            jointsBound = false;
            continue;
        }
        if (m && m->poolSurface) {
            if (poolReflected_ && f.pass != nullptr && m->normalMap >= 0 && hasImage(m->normalMap)) {
                drawPoolSurface(f, d);
                bound = nullptr;
                boundSamplers = {};
                jointsBound = false;
            }
            continue;
        }
        const bool skinned = d.primitive->skinned;
        const int kind = overDepth                             ? kOpaqueOverDepth
                         : !blend || !m                          ? blend
                         : m->alpha == scene::AlphaMode::Add ? 2
                         : m->alpha == scene::AlphaMode::Sum ? 3
                         : m->blendedHair                    ? 4
                                                             : blend;
        const auto& set = f.depthClip ? (skinned ? clippedSkinnedPipelines_ : clippedPipelines_) : (skinned ? drawing_->skinnedScene : drawing_->scene);
        SDL_GPUGraphicsPipeline* pipeline = set[m && m->doubleSided ? 1 : 0][kind];
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
        if (ready.empty()) {
            made = FramePlan::uniformsFor(*this, *this, f, d, blend);
        }
        const DrawUniforms& u = ready.empty() ? made : ready[i];
        std::array<SDL_GPUTextureSamplerBinding, 16> samplers;
        for (std::size_t k = 0; k < u.samplers.size(); ++k) {
            samplers[k] = {textureFor(u.samplers[k].texture), samplerFor(u.samplers[k].sampler)};
        }
        samplers[u.samplers.size()] = {shadowBlurred_, shadowSampler_};
        samplers[u.samplers.size() + 1] = {objectPictures_, shadowSampler_};
        samplers[u.samplers.size() + 2] = {objectPlaces_, shadowPoint_};
        samplers[u.samplers.size() + 3] = {objectGrid_, shadowPoint_};
        samplers[u.samplers.size() + 4] = {causticsTile_, sampler_};
        samplers[u.samplers.size() + 5] = {poolShadow_, shadowPoint_};
        samplers[u.samplers.size() + 6] = {occlusionBound_, shadowPoint_};
        samplers[u.samplers.size() + 7] = {objectFloors_, shadowPoint_};
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
            bindSkinStorage(f.pass, buffers);
        } else {
            const SDL_GPUBufferBinding vertices{buffers.vertices, 0};
            SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
        }
        SDL_DrawGPUIndexedPrimitives(f.pass, p.count, 1, p.firstIndex, skinned ? 0 : static_cast<Sint32>(p.firstVertex), 0);
    }
}

// How a particle draws among the other blended surfaces of its layer is not known yet, so they go after them.
void SdlGpuRenderer::drawBlended(const Frame& f, const std::vector<Draw>& blended) {
    const std::span<const Draw> all(blended);
    std::size_t done = 0;
    for (const ParticleBatch& batch : particleBatches_) {
        const std::size_t end = FramePlan::endOfLayer(all, done, batch.layer);
        drawList(f, all.subspan(done, end - done), 1);
        done = end;
        drawParticles(f, batch);
    }
    drawList(f, all.subspan(done), 1);
}

void SdlGpuRenderer::drawParticles(const Frame& f, const ParticleBatch& batch) {
    const auto drawn = static_cast<std::uint64_t>(batch.firstVertex) + batch.vertices;
    if (particleBuffer_ == nullptr || !hasImage(batch.image) || drawn > particleVertices_.size() || batch.draw >= drawing_->particles.size()) {
        return;
    }
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->particles[batch.draw]);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), sizeof(f.viewProjection.m));
    const float reference[4] = {FramePlan::particleAlphaReference(batch.draw), 0, 0, 0};
    SDL_PushGPUFragmentUniformData(f.cmd, 0, reference, sizeof(reference));
    const SDL_GPUBufferBinding vertices{particleBuffer_, 0};
    SDL_BindGPUVertexBuffers(f.pass, 0, &vertices, 1);
    const SDL_GPUTextureSamplerBinding binding{textureFor({TextureRef::Kind::Image, batch.image}), samplerFor(SamplerKind::Clamped)};
    SDL_BindGPUFragmentSamplers(f.pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(f.pass, batch.vertices, 1, batch.firstVertex, 0);
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
            bindSkinStorage(pass, buffers);
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
    const ShadowBlurUniforms weights = FramePlan::shadowBlurUniforms(*this);
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

void SdlGpuRenderer::bindSkinStorage(SDL_GPURenderPass* pass, const MeshBuffers& buffers) {
    SDL_GPUBuffer* const storage[2] = {jointBuffer_, buffers.morphs != nullptr ? buffers.morphs : noMorphs_};
    SDL_BindGPUVertexStorageBuffers(pass, 0, storage, 2);
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
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->road);
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
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->water);
    SDL_PushGPUVertexUniformData(f.cmd, 0, f.viewProjection.m.data(), 16 * sizeof(float));
    WaterUniforms w = gpu_->waterUniforms;
    w.screen[0] = static_cast<float>(f.width);
    w.screen[1] = static_cast<float>(f.height);
    w.time = time_;
    // The game's water material without a reflection: the sky at this tint and strength, unrippled.
    if (!reflections_) {
        w.tint[0] = 0.4f;
        w.tint[1] = 0.6f;
        w.tint[2] = 0.9f;
        w.tint[3] = 0.6f;
        w.offset = 0;
    }
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
    SDL_BindGPUGraphicsPipeline(f.pass, drawing_->sprite);
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
