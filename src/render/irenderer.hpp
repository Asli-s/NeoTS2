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
    int mostSmoothEdgeSamples = 1;         // what setSmoothEdges can do on this card, 1 for nothing
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
    // Takes the sprites and hands back the last ones in `sprites`, so the caller reuses their storage.
    virtual void showSprites(std::vector<Sprite>& sprites, bool scene = true) = 0;
    // The particles to draw from the next frame on, in order of their layers. Takes the lists and hands back the
    // last ones, so the caller reuses their storage.
    virtual void showParticles(std::vector<scene::ParticleVertex>& vertices, std::vector<ParticleBatch>& batches) = 0;
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
    // The weights a skinned instance's morphs are drawn with from the next frame on, one per morph of
    // its scene mesh (see scene::Mesh::morphs). Without them its morphs move nothing.
    virtual void setMorphWeights(std::size_t instance, const std::vector<float>& weights) = 0;
    // A face instance's eye values from the next frame on (see scene::mixEyes): direction x and y,
    // the left and right eye's stretch. Without them its eyes stay as modelled.
    virtual void setEyes(std::size_t instance, const std::array<float, 4>& eyes) = 0;
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
    // Uploads the scene's lot imposters and boundary curtain again, in place of the uploaded ones:
    // lot view's surroundings at another View Distance.
    virtual void setSurroundings(const scene::Scene& scene) = 0;
    // The light lot imposters' walls, roofs and slices are drawn with from the next frame on: the
    // lighting state's environment colour in lot view, white in the neighborhood.
    virtual void setImposterLight(const std::array<float, 3>& light) = 0;
    // Whether the lots' imposters are drawn, from the next frame on (lot view's Neighbors option). On
    // by default.
    virtual void setImpostersShown(bool shown) = 0;
    // The shape colour alpha a lot's imposter is drawn with from the next frame on (by lot id), as
    // lot view's Fade Distance fades it: its walls and roofs drawn blended without writing depth, and
    // its slices' alpha scaled.
    // 1, the default, draws it as it is.
    virtual void setImposterAlpha(std::uint32_t lot, float alpha) = 0;
    // How objects are lit from the next frame on (see scene::Scene::objectLighting).
    virtual void setObjectLighting(std::optional<scene::ObjectLighting> lighting) = 0;

    // The time animated surfaces are drawn at, in seconds: real time for the neighborhood water, and in a
    // lot real time that stops while the lot is paused for its pools' water, as the game's runs at every speed.
    virtual void setTime(float seconds) = 0;
    // The lot's clock in seconds, which stops while the lot is paused: what animated textures
    // (candle flames) play on.
    virtual void setLotTime(double seconds) = 0;

    // Which shadows draw from the next frame on (the game's Shadows option). All by default.
    virtual void setShadowOptions(const scene::objectshadow::Options& options) = 0;
    // Whether the pools' and the neighborhood water's mirror images are drawn, and a beach sea's. Off,
    // the water shows no reflection (the game's Reflections option). On by default.
    virtual void setReflections(bool on) = 0;
    // How many samples each pixel of the main view takes to smooth the edges of what is drawn: 1, 2, 4 or
    // 8. The next frame draws with it.
    virtual void setSmoothEdges(int samples) { (void)samples; }
    // The lots of neighbors drawn as real houses (x0, x1, y0, y1 in the world), which bring their own ground
    // in place of the ground around the lot. The first kMaxNeighborGrounds count.
    virtual void setNeighborGrounds(const std::vector<std::array<float, 4>>& lots) { (void)lots; }
    // Darkens corners and contacts in the main view, not in the original game. `strength` is the power
    // how open a pixel is gets raised to, with 0 for none.
    virtual void setAmbientOcclusion(float strength) { (void)strength; }
    // Whether a lot's sea shows them too (enableOceanReflection in Graphics Rules).
    virtual void setSeaReflections(bool on) = 0;
    // Whether materials show specular highlights (the game's specHighlights, off at its Low
    // Lighting Quality). On by default.
    virtual void setSpecularHighlights(bool on) = 0;
    // Whether a frame waits for the screen's refresh. Without waiting, frames go out as fast as they
    // are drawn. On by default.
    virtual void setWaitForScreen(bool on) = 0;
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
    // Draws an uploaded instance with this shape colour from the next frame on, none to stop (see
    // scene::Instance::shapeColor).
    virtual void setShapeColor(std::size_t instance, std::optional<std::array<float, 4>> color) = 0;
};

} // namespace ts2::render
