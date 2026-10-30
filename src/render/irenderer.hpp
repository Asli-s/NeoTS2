// What the game asks of a renderer. The SDL GPU renderer (Direct3D 12 or Vulkan) and the OpenGL 2
// renderer both implement it, so the game doesn't know which one draws.
#pragma once

#include "render/types.hpp"
#include "scene/scene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ts2::render {

// What a renderer can't or won't do, so the game can leave out the settings that wouldn't work and
// the passes that wouldn't show. A renderer given a setting it lacks leaves it alone.
struct Capabilities {
    bool bumpMapping = true;               // normal maps (setBumpMapping)
    bool floorWallNormalMapping = true;    // normal maps on floors and walls (setFloorAndWallNormalMapping)
    bool waterReflection = true;           // the neighborhood water reflects the sky and ground
    bool terrainLayers = true;             // the neighborhood ground is painted in layers
    bool gpuSkinning = true;               // skinned meshes are posed by the GPU
    bool anisotropic = true;               // textures are filtered anisotropically
    bool threadedPrepare = true;           // prepare() can run on another thread and upload there
    bool offscreen = true;                 // drawToFile and drawInstancesToImage can draw off-screen
    bool projectedShadows = true;          // Sims' projected shadows (setShadows)
    int maxTextureSize = 8192;
};

class IRenderer {
public:
    virtual ~IRenderer() = default;
    IRenderer() = default;
    IRenderer(const IRenderer&) = delete;
    IRenderer& operator=(const IRenderer&) = delete;

    // A renderer's own share of a scene uploaded but not drawn yet. It holds the scene's materials
    // and instances too (see prepare).
    class PreparedBase {
    public:
        virtual ~PreparedBase() = default;
        // Gives back up to `textures` of its textures, and the rest of it with the last ones. A GPU
        // may take a while to free what is given back, for hundreds at once.
        virtual void releaseSome(std::size_t textures) = 0;

        std::vector<scene::Material> materials;
        std::vector<scene::Instance> instances;
        std::optional<scene::ObjectLighting> objectLighting;
        std::vector<scene::ObjectLighting> lightSets;
    };

    // A scene uploaded but not drawn yet (see prepare). It is only handed back to the renderer that made it.
    class Prepared {
    public:
        Prepared() = default;
        explicit Prepared(std::unique_ptr<PreparedBase> impl) : impl_(std::move(impl)) {}
        Prepared(Prepared&&) noexcept = default;
        Prepared& operator=(Prepared&&) noexcept = default;
        void releaseSome(std::size_t textures) {
            if (impl_) {
                impl_->releaseSome(textures);
            }
        }
        [[nodiscard]] PreparedBase* get() const { return impl_.get(); }

    private:
        std::unique_ptr<PreparedBase> impl_;
    };

    [[nodiscard]] virtual Capabilities capabilities() const = 0;

    // Uploads a scene's images and meshes in place of what was uploaded before.
    void upload(const scene::Scene& scene) { upload(prepare(scene)); }
    // Uploads a scene's images and meshes without drawing them. A renderer whose
    // capabilities().threadedPrepare is set can do it on another thread, so a scene is uploaded as it is built.
    [[nodiscard]] virtual Prepared prepare(const scene::Scene& scene) const = 0;
    // Draws a prepared scene from now on, in place of what was uploaded before.
    virtual void upload(Prepared prepared) = 0;
    // The same, handing back the scene drawn so far, to be drawn again later without uploading it anew.
    [[nodiscard]] virtual Prepared swap(Prepared prepared) = 0;
    // Whether the GPU has done what the last upload gave it. A picture read back before then waits
    // for all of it (see drawInstancesToImage).
    [[nodiscard]] virtual bool uploaded() = 0;

    // Which uploaded instances to draw, one flag each. All of them when empty (as after upload).
    virtual void show(std::vector<std::uint8_t> visible) = 0;

    // Adds or replaces sprite images by index (a slot past the end grows the list).
    virtual void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) = 0;
    // The color the frame starts from (RGB, 0 to 1).
    virtual void setClearColor(float r, float g, float b) = 0;
    // What to draw over the scene, in order, from the next frame on, and whether to draw the scene.
    virtual void showSprites(std::vector<Sprite> sprites, bool scene = true) = 0;
    // How many times larger than their pictures the sprites are drawn (the UI's size, 1 as the game
    // draws it). Above 1 their pictures are enlarged sharply.
    virtual void setSpriteScale(float scale) = 0;

    // Draws one frame. Returns false when the window can't be drawn to right now (minimized, or no
    // depth buffer).
    [[nodiscard]] virtual bool draw(const Camera& camera) = 0;

    // Draws one frame off-screen and saves it as a PNG.
    [[nodiscard]] virtual bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) = 0;
    // Draws only the shown instances off-screen over a clear (transparent) background, without the
    // sky, ground, water or sprites, objects lit by `lighting`, and returns the picture (rows from
    // the top). Nullopt when it can't be drawn. For Sim portraits.
    [[nodiscard]] virtual std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                                            const scene::ObjectLighting& lighting) = 0;

    // The name of the graphics API in use (e.g. "direct3d12").
    virtual std::string backend() const = 0;
    // The graphics card and its driver, as the driver names them (for the log).
    virtual std::string device() const = 0;
    // Draws `frames` frames off screen, each waited for, with timings on: their times go to the timing
    // report ("record frame" for building a frame, "gpu frame" for building and drawing it).
    virtual void benchmark(const Camera& camera, unsigned width, unsigned height, int frames) = 0;

    // The joint matrices a skinned instance is drawn with from the next frame on (see
    // scene::Mesh::skin). Skinned instances without them aren't drawn.
    virtual void setJoints(std::size_t instance, const std::vector<scene::Transform>& joints) = 0;
    // Moves an uploaded instance.
    virtual void setPlace(std::size_t instance, const scene::Transform& transform) = 0;
    // Draws an uploaded instance with these materials, one per primitive of its scene mesh (indexes
    // into the scene's materials, scene::kHiddenMaterial for primitives not drawn). Empty for its mesh's own.
    virtual void setMaterials(std::size_t instance, std::vector<int> materials) = 0;
    // Uploads the materials and images the drawn scene has gained since it was uploaded (both only
    // ever grow, in the scene's order).
    virtual void addMaterials(const scene::Scene& scene) = 0;
    // The same for its meshes (after their materials).
    virtual void addMeshes(const scene::Scene& scene) = 0;
    // Replaces an uploaded instance with the scene's, or adds it as the next one. Its materials are
    // its mesh's own again. Light sets the scene has gained are taken too.
    virtual void setInstance(const scene::Scene& scene, std::size_t index) = 0;
    // Lights an uploaded instance with the light sets and sun the scene's has now.
    virtual void setLights(const scene::Scene& scene, std::size_t index) = 0;
    // Uploads the scene's images at these indexes again, in place of the uploaded ones, without
    // smaller levels (as light maps are drawn). With `fade`, light maps are drawn fading from the
    // ones they replace (see setLightMapFade).
    virtual void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false) = 0;
    // How far light maps have faded to the ones setImages put in (0 .. 1, at 1 the old ones are let go).
    virtual void setLightMapFade(float fade) = 0;
    // Draws a lot's sky with these six faces from the next frame on (see scene::Scene::skybox). None
    // when they are not six faces of one size.
    virtual void setSkybox(const std::vector<scene::Image>& faces) = 0;
    // The light lot imposters' walls, roofs and slices are drawn with from the next frame on: the
    // lighting state's environment colour in lot view, white in the neighborhood.
    virtual void setImposterLight(const std::array<float, 3>& light) = 0;
    // How objects are lit from the next frame on (see scene::Scene::objectLighting).
    virtual void setObjectLighting(std::optional<scene::ObjectLighting> lighting) = 0;

    // The time animated surfaces (the neighborhood water) are drawn at, in seconds.
    virtual void setTime(float seconds) = 0;
    // The lot's clock in seconds, which stops while the lot is paused: what animated textures
    // (candle flames) play on.
    virtual void setLotTime(double seconds) = 0;

    // Whether materials with a normal map are drawn with it (the game's bumpMapping, on at its High
    // Material Detail). On by default.
    virtual void setBumpMapping(bool on) = 0;
    // Whether floors and walls are bump-mapped too (the game's floorAndWallNormalMapping, on at its
    // High Lighting Quality). It also needs bump mapping. On by default.
    virtual void setFloorAndWallNormalMapping(bool on) = 0;
    // The Sims' projected shadows drawn from the next frame on (the game's, with its Shadows option
    // at High). None by default.
    virtual void setShadows(Shadows shadows) = 0;
    // Brightens a lot object (by its game object id, 0 for none) by adding `ambient` to its ambient light.
    virtual void setHighlight(std::uint32_t object, float ambient) = 0;
};

} // namespace ts2::render
