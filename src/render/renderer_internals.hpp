// What the renderer's files share: GPU resource helpers and the vertex layout sizes. renderer.cpp
// creates and ends the renderer, renderer_pipelines.cpp its shaders and pipelines, renderer_scene.cpp
// keeps what is shown, renderer_upload.cpp uploads scenes and renderer_draw.cpp draws frames.
#pragma once

#include "render/image_levels.hpp"
#include "render/renderer.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <cstring>
#include <utility>
#include <vector>

namespace ts2::render::render_detail {

constexpr std::uint32_t kSpriteFloats = 8;  // position, texture coordinates, color

// Windows draws with Direct3D 12 and DXIL shaders; macOS draws with Metal and MSL shaders.
// The build makes only that kind, so the device must not fall back to the other.
#ifdef _WIN32
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
constexpr const char* kShaderExtension = ".dxil";
constexpr const char* kGraphicsApi = "Direct3D 12";
#elif defined(__APPLE__)
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_MSL;
constexpr const char* kShaderExtension = ".msl";
constexpr const char* kShaderEntrypoint = "main0";
constexpr const char* kGraphicsApi = "Metal";
#else
constexpr SDL_GPUShaderFormat kShaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
constexpr const char* kShaderExtension = ".spv";
constexpr const char* kGraphicsApi = "Vulkan";
#endif

#ifndef __APPLE__
constexpr const char* kShaderEntrypoint = "main";
#endif

inline std::runtime_error sdlError(const char* what) {
    return std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

// The resource SDL made. Throws when it couldn't (out of video memory) rather than uploading into null.
template <class T>
T* made(T* resource, const char* what) {
    if (resource == nullptr) {
        throw sdlError(what);
    }
    return resource;
}

inline void releaseResource(SDL_GPUDevice* device, SDL_GPUTexture* t) { SDL_ReleaseGPUTexture(device, t); }
inline void releaseResource(SDL_GPUDevice* device, SDL_GPUBuffer* b) { SDL_ReleaseGPUBuffer(device, b); }

// A texture or buffer being filled. It is released unless kept, so an upload that throws leaves nothing behind.
template <class T>
class Filling {
public:
    Filling(SDL_GPUDevice* device, T* resource, const char* what) : device_(device), resource_(made(resource, what)) {}
    Filling(const Filling&) = delete;
    Filling& operator=(const Filling&) = delete;
    ~Filling() {
        if (resource_) {
            releaseResource(device_, resource_);
        }
    }
    T* get() const { return resource_; }
    T* keep() { return std::exchange(resource_, nullptr); }

private:
    SDL_GPUDevice* device_;
    T* resource_;
};

// An upload buffer holding a copy of `size` bytes. Null when the GPU is out of memory.
SDL_GPUTransferBuffer* filledUpload(SDL_GPUDevice* device, const void* data, Uint32 size);

// A copy pass for uploading many pictures and buffers. If the upload stops with an error before
// end(), the pass is closed and its command buffer cancelled. The upload buffers are released
// either way.
class UploadPass {
public:
    UploadPass(SDL_GPUDevice* device, SDL_GPUCommandBuffer* cmd) : device_(device), cmd_(cmd), copy_(SDL_BeginGPUCopyPass(cmd)) {}
    UploadPass(const UploadPass&) = delete;
    UploadPass& operator=(const UploadPass&) = delete;
    ~UploadPass() {
        unmap();
        if (copy_) {
            SDL_EndGPUCopyPass(copy_);
            SDL_CancelGPUCommandBuffer(cmd_);
        }
        for (auto* t : transfers_) {
            SDL_ReleaseGPUTransferBuffer(device_, t);
        }
    }

    SDL_GPUCopyPass* copy() const { return copy_; }

    // A copy of `size` bytes in an upload buffer. A null buffer when the GPU is out of memory. Small
    // pieces share big buffers: making one takes as long as copying a few hundred kilobytes.
    SDL_GPUTransferBufferLocation stage(const void* data, Uint32 size) {
        constexpr Uint32 kShared = 32u << 20;
        constexpr Uint32 kAlign = 512;  // where Direct3D 12 can copy a texture from
        if (size > kShared / 4) {
            SDL_GPUTransferBuffer* own = filledUpload(device_, data, size);
            if (own) {
                transfers_.push_back(own);
            }
            return {own, 0};
        }
        Uint32 at = (used_ + kAlign - 1) / kAlign * kAlign;
        if (mapped_ == nullptr || at + size > kShared) {
            unmap();
            SDL_GPUTransferBufferCreateInfo info{};
            info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            info.size = kShared;
            shared_ = SDL_CreateGPUTransferBuffer(device_, &info);
            if (shared_ == nullptr) {
                return {nullptr, 0};
            }
            transfers_.push_back(shared_);
            mapped_ = static_cast<std::uint8_t*>(SDL_MapGPUTransferBuffer(device_, shared_, false));
            if (mapped_ == nullptr) {
                return {nullptr, 0};
            }
            at = 0;
        }
        std::memcpy(mapped_ + at, data, size);
        used_ = at + size;
        return {shared_, at};
    }

    // Closes the copy pass. The caller submits the command buffer.
    void end() {
        unmap();
        SDL_EndGPUCopyPass(copy_);
        copy_ = nullptr;
    }

private:
    void unmap() {
        if (mapped_) {
            SDL_UnmapGPUTransferBuffer(device_, shared_);
            mapped_ = nullptr;
        }
    }

    SDL_GPUDevice* device_;
    SDL_GPUCommandBuffer* cmd_;
    SDL_GPUCopyPass* copy_;
    std::vector<SDL_GPUTransferBuffer*> transfers_;
    SDL_GPUTransferBuffer* shared_ = nullptr;  // the one being filled
    std::uint8_t* mapped_ = nullptr;
    Uint32 used_ = 0;
};

// Defined in renderer_upload.cpp, as is filledUpload.

// A GPU buffer filled with `size` bytes in an upload pass.
SDL_GPUBuffer* filledBuffer(SDL_GPUDevice* device, UploadPass& pass, SDL_GPUBufferUsageFlags usage, const void* data, Uint32 size);

// A texture of a complete image, with mip levels unless `mips` is false. Those the GPU still has
// to make (SDL_GenerateMipmapsForGPUTexture, after the copy pass) go into `withMips`.
SDL_GPUTexture* uploadTexture(SDL_GPUDevice* device, UploadPass& pass, const scene::Image& image, bool mips,
                              std::vector<SDL_GPUTexture*>& withMips);

// A cube map of six faces of one size (a lot's skybox). Null when the faces are not that.
SDL_GPUTexture* uploadCube(SDL_GPUDevice* device, UploadPass& pass, const std::vector<scene::Image>& faces);

} // namespace ts2::render::render_detail
