#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ts2::render {

using namespace render_detail;

void SdlGpuRenderer::releaseSprites() {
    for (auto* t : spriteTextures_) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    spriteTextures_.clear();
    spriteSizes_.clear();
}

void SdlGpuRenderer::setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) {
    if (images.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        return;
    }
    UploadPass pass(device_, cmd);
    for (const auto& [index, image] : images) {
        if (index < 0 || image == nullptr || image->width == 0 || image->height == 0 ||
            image->rgba.size() != static_cast<std::size_t>(image->width) * image->height * 4) {
            continue;
        }
        const auto i = static_cast<std::size_t>(index);
        if (i >= spriteTextures_.size()) {
            spriteTextures_.resize(i + 1, nullptr);
            spriteSizes_.resize(i + 1, {0, 0});
        }
        // A new texture when the slot has none yet or the picture changed size.
        if (!spriteTextures_[i] || spriteSizes_[i][0] != image->width || spriteSizes_[i][1] != image->height) {
            if (spriteTextures_[i]) {
                SDL_ReleaseGPUTexture(device_, spriteTextures_[i]);
            }
            SDL_GPUTextureCreateInfo info{};
            info.type = SDL_GPU_TEXTURETYPE_2D;
            info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width = image->width;
            info.height = image->height;
            info.layer_count_or_depth = 1;
            info.num_levels = 1;
            spriteTextures_[i] = SDL_CreateGPUTexture(device_, &info);
            spriteSizes_[i] = {image->width, image->height};
            if (!spriteTextures_[i]) {
                spriteSizes_[i] = {0, 0};
            }
        }
        // Out of GPU memory: this picture is skipped rather than the whole frame lost.
        const SDL_GPUTransferBufferLocation at =
            spriteTextures_[i] ? pass.stage(image->rgba.data(), static_cast<Uint32>(image->rgba.size())) : SDL_GPUTransferBufferLocation{};
        if (at.transfer_buffer == nullptr) {
            continue;
        }
        SDL_GPUTextureTransferInfo from{};
        from.transfer_buffer = at.transfer_buffer;
        from.offset = at.offset;
        from.pixels_per_row = image->width;
        from.rows_per_layer = image->height;
        SDL_GPUTextureRegion to{};
        to.texture = spriteTextures_[i];
        to.w = image->width;
        to.h = image->height;
        to.d = 1;
        SDL_UploadToGPUTexture(pass.copy(), &from, &to, true);
    }
    pass.end();
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::addMaterials(const scene::Scene& scene) {
    if (scene.materials.size() <= materials_.size() && scene.images.size() <= gpu_->textures.size() &&
        scene.envCubes.size() <= gpu_->envCubes.size()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips;
    {
        UploadPass pass(device_, cmd);
        for (std::size_t i = gpu_->textures.size(); i < scene.images.size(); ++i) {
            const auto& image = scene.images[i];
            gpu_->textures.push_back(complete(image) ? uploadTexture(device_, pass, image, true, withMips) : nullptr);
        }
        for (std::size_t i = gpu_->envCubes.size(); i < scene.envCubes.size(); ++i) {
            gpu_->envCubes.push_back(uploadCube(device_, pass, scene.envCubes[i]));
        }
        pass.end();
    }
    for (auto* t : withMips) {
        SDL_GenerateMipmapsForGPUTexture(cmd, t);
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    if (scene.materials.size() > materials_.size()) {
        materials_.insert(materials_.end(), scene.materials.begin() + static_cast<std::ptrdiff_t>(materials_.size()), scene.materials.end());
    }
}

void SdlGpuRenderer::addMeshes(const scene::Scene& scene) {
    if (scene.meshes.size() <= gpu_->meshes.size()) {
        return;
    }
    const auto set = static_cast<std::uint32_t>(gpu_->meshBuffers.size());
    MeshData data;
    std::vector<GpuMesh> added;
    for (std::size_t i = gpu_->meshes.size(); i < scene.meshes.size(); ++i) {
        added.push_back(packMesh(scene.meshes[i], scene.materials.size(), set, data));
    }
    if (data.tooBig) {
        diag::warning("renderer: added meshes are too big for one GPU buffer; some are left out");
    }
    // Kept before they are filled, so that release() frees them should the upload fail halfway.
    MeshBuffers& buffers = gpu_->meshBuffers.emplace_back();
    if (!data.indices.empty()) {
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            throw sdlError("GPU command buffer");
        }
        UploadPass pass(device_, cmd);
        buffers.vertices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.vertices.data(),
                                        static_cast<Uint32>(data.vertices.size() * sizeof(float)));
        buffers.indices = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_INDEX, data.indices.data(),
                                       static_cast<Uint32>(data.indices.size() * sizeof(std::uint32_t)));
        if (!data.skin.empty()) {
            buffers.skin = filledBuffer(device_, pass, SDL_GPU_BUFFERUSAGE_VERTEX, data.skin.data(), static_cast<Uint32>(data.skin.size()));
        }
        pass.end();
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    gpu_->meshes.insert(gpu_->meshes.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
}

void SdlGpuRenderer::setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade) {
    if (indexes.empty()) {
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    std::vector<SDL_GPUTexture*> withMips;
    {
        UploadPass pass(device_, cmd);
        for (const int i : indexes) {
            const auto at = static_cast<std::size_t>(i);
            if (i < 0 || at >= scene.images.size() || at >= gpu_->textures.size() || !complete(scene.images[at])) {
                continue;
            }
            SDL_GPUTexture* fresh = uploadTexture(device_, pass, scene.images[at], false, withMips);
            // SDL frees a released texture once the frames drawing with it are done.
            SDL_GPUTexture* old = std::exchange(gpu_->textures[at], fresh);
            auto& from = gpu_->fadingFrom[i];
            if (fade && from == nullptr) {
                from = old;  // the light seen when the fade began
            } else if (old != nullptr) {
                SDL_ReleaseGPUTexture(device_, old);
            }
            if (!fade && from != nullptr) {
                SDL_ReleaseGPUTexture(device_, from);
            }
            if (!fade) {
                gpu_->fadingFrom.erase(i);
            }
        }
        pass.end();
    }
    SDL_SubmitGPUCommandBuffer(cmd);
}

void SdlGpuRenderer::setLightMapFade(float fade) {
    gpu_->lightMapFade = std::clamp(fade, 0.0f, 1.0f);
    if (gpu_->lightMapFade >= 1) {
        for (const auto& [index, texture] : gpu_->fadingFrom) {
            if (texture != nullptr) {
                SDL_ReleaseGPUTexture(device_, texture);
            }
        }
        gpu_->fadingFrom.clear();
    }
}

void SdlGpuRenderer::setSkybox(const std::vector<scene::Image>& faces) {
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (cmd == nullptr) {
        throw sdlError("GPU command buffer");
    }
    SDL_GPUTexture* fresh = nullptr;
    {
        UploadPass pass(device_, cmd);
        fresh = uploadCube(device_, pass, faces);
        pass.end();
    }
    SDL_SubmitGPUCommandBuffer(cmd);
    if (gpu_->skybox != nullptr) {
        SDL_ReleaseGPUTexture(device_, gpu_->skybox);
    }
    gpu_->skybox = fresh;
}

void SdlGpuRenderer::uploadJoints(SDL_GPUCommandBuffer* cmd) {
    if (!jointsChanged_ || jointData_.empty()) {
        return;
    }
    if (jointData_.size() > jointCapacity_) {
        if (jointBuffer_) {
            SDL_ReleaseGPUBuffer(device_, jointBuffer_);
        }
        jointCapacity_ = std::max(jointData_.size(), jointCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = static_cast<Uint32>(jointCapacity_ * sizeof(float));
        jointBuffer_ = SDL_CreateGPUBuffer(device_, &info);
        if (jointBuffer_ == nullptr) {
            jointCapacity_ = 0;
        }
    }
    // Out of GPU memory: the poses are sent again next frame.
    const auto bytes = static_cast<Uint32>(jointData_.size() * sizeof(float));
    SDL_GPUTransferBuffer* tb = jointBuffer_ ? filledUpload(device_, jointData_.data(), bytes) : nullptr;
    if (tb == nullptr) {
        return;
    }
    jointsChanged_ = false;
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{tb, 0};
    SDL_GPUBufferRegion to{jointBuffer_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, true);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

void SdlGpuRenderer::uploadSpriteVertices(SDL_GPUCommandBuffer* cmd, unsigned, unsigned) {
    if (sprites_.empty()) {
        return;
    }
    std::vector<float> v;
    v.reserve(sprites_.size() * 6 * kSpriteFloats);
    for (const auto& s : sprites_) {
        const float corners[6][4] = {{s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y0, s.u1, s.v0}, {s.x1, s.y1, s.u1, s.v1},
                                     {s.x0, s.y0, s.u0, s.v0}, {s.x1, s.y1, s.u1, s.v1}, {s.x0, s.y1, s.u0, s.v1}};
        for (const auto& c : corners) {
            v.insert(v.end(), {c[0], c[1], c[2], c[3], s.color[0], s.color[1], s.color[2], s.color[3]});
        }
    }
    const auto count = static_cast<unsigned>(v.size() / kSpriteFloats);
    if (count > spriteCapacity_) {
        if (spriteVertices_) {
            SDL_ReleaseGPUBuffer(device_, spriteVertices_);
        }
        spriteCapacity_ = std::max(count, spriteCapacity_ * 2);
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        info.size = spriteCapacity_ * kSpriteFloats * static_cast<Uint32>(sizeof(float));
        spriteVertices_ = SDL_CreateGPUBuffer(device_, &info);
        if (spriteVertices_ == nullptr) {
            spriteCapacity_ = 0;
        }
    }
    const auto bytes = static_cast<Uint32>(v.size() * sizeof(float));
    SDL_GPUTransferBuffer* tb = spriteVertices_ ? filledUpload(device_, v.data(), bytes) : nullptr;
    if (tb == nullptr) {
        return;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation from{tb, 0};
    SDL_GPUBufferRegion to{spriteVertices_, 0, bytes};
    SDL_UploadToGPUBuffer(copy, &from, &to, false);
    SDL_EndGPUCopyPass(copy);
    SDL_ReleaseGPUTransferBuffer(device_, tb);
}

} // namespace ts2::render
