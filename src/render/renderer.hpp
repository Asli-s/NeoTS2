#pragma once

#include "render/math.hpp"
#include "scene/scene.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUCommandBuffer;

namespace ts2::render {

// Where the camera is and what it looks at, in game coordinates (z up).
struct Camera {
    Vec3 eye{10, -10, 10};
    Vec3 target{0, 0, 0};
    float fovY = 0.7f;  // radians
};

// Draws a Scene with the GPU through SDL: opaque surfaces first, then alpha-tested ones, then
// blended ones. Shaders are loaded from files in `shaderFolder`.
class Renderer {
public:
    Renderer(SDL_Window* window, const std::filesystem::path& shaderFolder);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Uploads a scene's images and meshes; replaces what was uploaded before.
    void upload(const scene::Scene& scene);

    // Which uploaded instances to draw, one flag each; all of them when empty (as after upload).
    void show(std::vector<std::uint8_t> visible);

    // Draws one frame. Returns false when the window can't be drawn to right now (minimized).
    bool draw(const Camera& camera);

    // Draws one frame off-screen and saves it as a PNG.
    bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path);

    // The name of the GPU backend in use (e.g. "direct3d12").
    std::string backend() const;

private:
    struct GpuPrimitive {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        unsigned count = 0;
        int material = -1;
    };
    struct GpuMesh {
        std::vector<GpuPrimitive> primitives;
    };

    void release();
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height, const Camera& camera);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTexture* depth_ = nullptr;
    unsigned depthWidth_ = 0, depthHeight_ = 0;
    SDL_GPUSampler* sampler_ = nullptr;
    SDL_GPUTexture* white_ = nullptr;  // stands in for surfaces without a texture
    // Pipelines by [two-sided][blend]: opaque and alpha-tested surfaces write depth, blended ones don't.
    SDL_GPUGraphicsPipeline* pipelines_[2][2] = {};
    std::vector<SDL_GPUTexture*> textures_;
    std::vector<GpuMesh> meshes_;
    std::vector<scene::Material> materials_;
    std::vector<scene::Instance> instances_;
    std::vector<std::uint8_t> visible_;
};

} // namespace ts2::render
