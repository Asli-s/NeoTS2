#include "render/renderer.hpp"
#include "render/renderer_internals.hpp"

#include "diag/log.hpp"
#include "diag/timing.hpp"

#include <SDL3/SDL.h>

#include <format>
#include <stdexcept>
#include <string>

namespace ts2::render {

using namespace render_detail;

Renderer::Renderer(SDL_Window* window, const std::filesystem::path& shaderFolder) : window_(window) {
    device_ = SDL_CreateGPUDevice(kShaderFormat, false, nullptr);
    if (device_ == nullptr) {
        // SDL's own reason goes to the log only: it means little to players.
        diag::error(std::string("no GPU device: ") + SDL_GetError());
        throw std::runtime_error(std::string("NeoTS2 needs a graphics card and driver that support ") + kGraphicsApi +
                                 ", and none was found.\n\n"
                                 "Update your graphics driver and try again. NeoTS2 can't run over Remote Desktop or in "
                                 "most virtual machines.");
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window_)) {
        const auto error = sdlError("can't draw to the window");
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
        throw error;
    }
    // The destructor doesn't run when the constructor throws, so what was made is released here.
    try {
        create(shaderFolder);
    } catch (...) {
        destroy();
        throw;
    }
}

Renderer::~Renderer() {
    destroy();
}

void Renderer::destroy() {
    if (device_ == nullptr) {
        return;
    }
    release();
    releaseSprites();
    if (uploadFence_) {
        SDL_ReleaseGPUFence(device_, uploadFence_);
        uploadFence_ = nullptr;
    }
    if (spriteVertices_) {
        SDL_ReleaseGPUBuffer(device_, spriteVertices_);
    }
    if (spritePipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device_, spritePipeline_);
    }
    for (auto* s : {clampedMips_, pointSampler_, clampedSampler_}) {
        if (s) {
            SDL_ReleaseGPUSampler(device_, s);
        }
    }
    for (auto* p : {clipPipeline_, terrainPipeline_, skyPipeline_, waterPipeline_, roadPipeline_, imposterLightPipeline_, imposterTerrainPipeline_,
                    imposterWallPipeline_, imposterRoofPipeline_}) {
        if (p) {
            SDL_ReleaseGPUGraphicsPipeline(device_, p);
        }
    }
    for (auto* t : {reflection_, reflectionDepth_}) {
        if (t) {
            SDL_ReleaseGPUTexture(device_, t);
        }
    }
    if (spriteSampler_) {
        SDL_ReleaseGPUSampler(device_, spriteSampler_);
    }
    for (auto* set : {&pipelines_, &skinnedPipelines_}) {
        for (auto& row : *set) {
            for (auto* p : row) {
                if (p) {
                    SDL_ReleaseGPUGraphicsPipeline(device_, p);
                }
            }
        }
    }
    if (jointBuffer_) {
        SDL_ReleaseGPUBuffer(device_, jointBuffer_);
    }
    if (sampler_) {
        SDL_ReleaseGPUSampler(device_, sampler_);
    }
    for (const DepthBuffer* d : {&windowDepth_, &portraitDepth_}) {
        if (d->texture) {
            SDL_ReleaseGPUTexture(device_, d->texture);
        }
    }
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
    device_ = nullptr;
}

std::string Renderer::backend() const {
    const char* name = SDL_GetGPUDeviceDriver(device_);
    return name ? name : "";
}

std::string Renderer::device() const {
    const SDL_PropertiesID properties = SDL_GetGPUDeviceProperties(device_);
    const char* name = SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_NAME_STRING, "unknown card");
    const char* driver = SDL_GetStringProperty(properties, SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING, "unknown");
    return std::format("{}, driver {} ({})", name, driver, backend());
}

void Renderer::benchmark(const Camera& camera, unsigned width, unsigned height, int frames) {
    if (width == 0 || height == 0 || width > kMaxOffscreen || height > kMaxOffscreen) {
        return;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GetGPUSwapchainTextureFormat(device_, window_);
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    SDL_GPUTexture* target = SDL_CreateGPUTexture(device_, &info);
    if (target == nullptr) {
        return;
    }
    const bool wasOn = diag::timingsOn();
    // The first frames warm caches up and aren't counted.
    for (int f = -10; f < frames; ++f) {
        diag::setTimings(f >= 0);
        diag::Timer timer("gpu frame");
        SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
        if (cmd == nullptr) {
            break;
        }
        if (!render(cmd, target, width, height, camera)) {
            SDL_CancelGPUCommandBuffer(cmd);
            break;
        }
        if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd)) {
            SDL_WaitForGPUFences(device_, true, &fence, 1);
            SDL_ReleaseGPUFence(device_, fence);
        }
    }
    diag::setTimings(wasOn);
    SDL_ReleaseGPUTexture(device_, target);
}

void Renderer::release() {
    gpu_->release(device_);
}

} // namespace ts2::render
