#pragma once

#include "render/math.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUBuffer;
struct SDL_GPUTexture;
struct SDL_GPUSampler;
struct SDL_GPUGraphicsPipeline;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;

namespace ts2::render {

// Where the camera is and what it looks at, in game coordinates (z up).
struct Camera {
    Vec3 eye{10, -10, 10};
    Vec3 target{0, 0, 0};
    float fovY = 0.7f;  // radians
    float fovX = 0;  // radians across the screen; when set, it is used instead of fovY
    float farPlane = 500;  // nothing farther is drawn
    float nearPlane = 0;  // 0: a ten-thousandth of farPlane
};

// A textured rectangle drawn over the scene, in screen pixels from the top left; texture -1 draws
// the plain color.
struct Sprite {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    int texture = -1;
    float color[4] = {1, 1, 1, 1};
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

    // Uploads the images sprites are drawn with; replaces those uploaded before.
    void uploadSprites(const std::vector<scene::Image>& images);
    // Adds or replaces sprite images by index (a slot past the end grows the list).
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images);
    // The color the frame starts from (RGB, 0 to 1).
    void setClearColor(float r, float g, float b) { clear_[0] = r; clear_[1] = g; clear_[2] = b; }
    // What to draw over the scene, in order, from the next frame on; and whether to draw the scene.
    void showSprites(std::vector<Sprite> sprites, bool scene = true);

    // Draws one frame. Returns false when the window can't be drawn to right now (minimized).
    bool draw(const Camera& camera);

    // Draws one frame off-screen and saves it as a PNG.
    bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path);

    // The name of the GPU backend in use (e.g. "direct3d12").
    std::string backend() const;

    // The time animated surfaces (the neighborhood water) are drawn at, in seconds.
    void setTime(float seconds) { time_ = seconds; }

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
    void releaseSprites();
    void uploadSpriteVertices(SDL_GPUCommandBuffer* cmd, unsigned width, unsigned height);
    void render(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* target, unsigned width, unsigned height, const Camera& camera);
    void drawImposters(SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass, const Mat4& viewProjection);

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

    SDL_GPUGraphicsPipeline* spritePipeline_ = nullptr;
    SDL_GPUSampler* spriteSampler_ = nullptr;  // clamped, as sprites are drawn whole
    std::vector<SDL_GPUTexture*> spriteTextures_;
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    std::vector<Sprite> sprites_;
    bool drawScene_ = true;
    float clear_[3] = {0.17f, 0.18f, 0.19f};
    SDL_GPUBuffer* spriteVertices_ = nullptr;
    unsigned spriteCapacity_ = 0;  // vertices the buffer holds

    // A neighborhood's painted terrain (see scene::PaintedTerrain).
    struct TerrainUniforms {
        float alphaSize, textureRepeat, cellTiles, layers, cellsX, cellsY, lightMapScale, litFrom;
        float gradientDepth, reflected, padding[2];
    };
    struct WaterUniforms {
        float tint[4];
        float screen[2];
        float offset, scale, speed, time, padding[2];
    };
    SDL_GPUGraphicsPipeline* terrainPipeline_ = nullptr;
    SDL_GPUBuffer* terrainVertices_ = nullptr;
    SDL_GPUBuffer* terrainIndices_ = nullptr;
    unsigned terrainIndexCount_ = 0;
    SDL_GPUTexture* terrainPaints_ = nullptr;
    SDL_GPUTexture* terrainAlphas_ = nullptr;
    SDL_GPUTexture* terrainCells_ = nullptr;
    SDL_GPUTexture* terrainLight_ = nullptr;
    SDL_GPUTexture* terrainGradient_ = nullptr;
    TerrainUniforms terrainUniforms_{};

    // Its water: the sheet, the sky cube map, the ripple map, and the reflection drawn each frame.
    SDL_GPUGraphicsPipeline* skyPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* waterPipeline_ = nullptr;
    SDL_GPUBuffer* waterVertices_ = nullptr;
    SDL_GPUBuffer* waterIndices_ = nullptr;
    unsigned waterIndexCount_ = 0;
    SDL_GPUTexture* skyCube_ = nullptr;
    SDL_GPUTexture* ripples_ = nullptr;
    SDL_GPUTexture* reflection_ = nullptr;
    SDL_GPUTexture* reflectionDepth_ = nullptr;
    unsigned reflectionWidth_ = 0, reflectionHeight_ = 0;
    float waterLevel_ = 0;
    WaterUniforms waterUniforms_{};

    // Its roads.
    SDL_GPUGraphicsPipeline* roadPipeline_ = nullptr;
    SDL_GPUSampler* clampedMips_ = nullptr;  // clamped, with mip levels (road textures)
    SDL_GPUBuffer* roadVertices_ = nullptr;
    SDL_GPUBuffer* roadIndices_ = nullptr;
    unsigned roadIndexCount_ = 0;
    SDL_GPUTexture* roadTextures_ = nullptr;
    float lightMapSize_ = 1280;

    // Its lots' imposters (see scene::PaintedTerrain::ImposterPart): pipelines for the terrain's
    // light map, the terrain's texture, walls and slices, and roofs.
    struct ImposterDraw {
        scene::PaintedTerrain::ImposterPart::Kind kind;
        SDL_GPUTexture* texture = nullptr;
        SDL_GPUTexture* lightMap = nullptr;
        float tint[3] = {1, 1, 1};
        std::uint32_t firstIndex = 0, indexCount = 0;
        std::int32_t vertexOffset = 0;
    };
    SDL_GPUGraphicsPipeline* imposterLightPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterTerrainPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterWallPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* imposterRoofPipeline_ = nullptr;
    SDL_GPUSampler* pointSampler_ = nullptr;  // nearest texel, no mip levels (imposter walls and slices)
    SDL_GPUBuffer* imposterVertices_ = nullptr;
    SDL_GPUBuffer* imposterIndices_ = nullptr;
    std::vector<SDL_GPUTexture*> imposterTextures_;
    std::vector<ImposterDraw> imposterDraws_;
    float time_ = 0;
};

} // namespace ts2::render
