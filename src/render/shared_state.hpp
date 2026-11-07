// What every renderer keeps of the scene on the CPU: its materials and instances, which are shown
// and how they are lit and posed, and the settings. A renderer adds what it uploads to its GPU.
#pragma once

#include "render/irenderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace ts2::render {

class RendererState : public IRenderer {
public:
    void show(std::vector<std::uint8_t> visible) override;
    void setClearColor(float r, float g, float b) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; }
    void showSprites(std::vector<Sprite>& sprites, bool scene = true) override;
    void showParticles(std::vector<scene::ParticleVertex>& vertices, std::vector<ParticleBatch>& batches) override {
        particleVertices_.swap(vertices);
        particleBatches_.swap(batches);
    }
    void setSpriteScale(float scale) override { spriteScale_ = scale; }
    void setJoints(std::size_t instance, const std::vector<scene::Transform>& joints) override;
    void setMorphWeights(std::size_t instance, const std::vector<float>& weights) override;
    void setEyes(std::size_t instance, const std::array<float, 4>& eyes) override;
    // An instance's eye values, all 0 for none.
    std::array<float, 4> eyesOf(std::size_t instance) const;
    // Where an instance's morph weights start in the joint array, in rows of four floats (see
    // skinned.vert.hlsl), kNoMorphWeights for none.
    std::uint32_t morphRow(std::size_t instance) const;
    static constexpr std::uint32_t kNoMorphWeights = 0xFFFFFFFF;
    void setPlace(std::size_t instance, const scene::Transform& transform) override;
    void setMaterials(std::size_t instance, std::vector<int> materials) override;
    void setInstance(const scene::Scene& scene, std::size_t index) override;
    void setLights(const scene::Scene& scene, std::size_t index) override;
    void setImposterLight(const std::array<float, 3>& light) override { imposterLight_ = light; }
    void setNeighborGrounds(const std::vector<std::array<float, 4>>& lots) override { neighborGrounds_ = lots; }
    void setImpostersShown(bool shown) override { impostersShown_ = shown; }
    void setImposterAlpha(std::uint32_t lot, float alpha) override {
        if (alpha >= 1) {
            imposterAlphas_.erase(lot);
        } else {
            imposterAlphas_[lot] = alpha;
        }
    }
    // A lot's imposter's alpha (see setImposterAlpha).
    float imposterAlpha(std::uint32_t lot) const {
        const auto it = imposterAlphas_.find(lot);
        return it == imposterAlphas_.end() ? 1.0f : it->second;
    }
    void setObjectLighting(std::optional<scene::ObjectLighting> lighting) override { objectLighting_ = std::move(lighting); }
    void setTime(float seconds) override { time_ = seconds; }
    void setLotTime(double seconds) override { lotTime_ = seconds; }
    void setBumpMapping(bool on) override { bumpMapping_ = on; }
    void setFloorAndWallNormalMapping(bool on) override { floorAndWallNormalMapping_ = on; }
    void setShadows(Shadows shadows) override {
        shadows_ = std::move(shadows);
        if (!shadowOptions_.sims) {
            shadows_.casters.clear();
        }
        if (!shadowOptions_.objects) {
            shadows_.objects.reset();
        }
    }
    void setShadowOptions(const scene::objectshadow::Options& options) override { shadowOptions_ = options; }
    void setReflections(bool on) override { reflections_ = on; }
    void setSeaReflections(bool on) override { seaReflections_ = on; }
    void setSpecularHighlights(bool on) override { specularHighlights_ = on; }
    void setHighlight(std::uint32_t object, float ambient) override {
        highlightObject_ = object;
        highlightAmbient_ = ambient;
    }
    void setShapeColor(std::size_t instance, std::optional<std::array<float, 4>> color) override {
        if (instance < instances_.size()) {
            instances_[instance].shapeColor = color;
        }
    }

protected:
    // Takes the light sets the scene has gained (they only ever grow).
    void addLightSets(const scene::Scene& scene);
    // Fills a prepared scene's share with the scene's.
    static void copyScene(PreparedBase& to, const scene::Scene& scene);
    // Swaps the drawn scene's share with a prepared one's, and starts the instances' state over.
    void swapScene(PreparedBase& prepared);

    std::vector<scene::Material> materials_;
    std::vector<scene::Instance> instances_;
    std::vector<std::vector<int>> instanceMaterials_;  // see setMaterials
    std::optional<scene::ObjectLighting> objectLighting_;
    std::array<float, 3> imposterLight_{1, 1, 1};
    std::vector<std::array<float, 4>> neighborGrounds_;
    bool impostersShown_ = true;
    std::map<std::uint32_t, float> imposterAlphas_;  // the imposters that fade, by lot id
    std::vector<scene::ObjectLighting> lightSets_;  // objects lit by their room
    std::vector<std::uint8_t> visible_;

    // Skinned instances' joint matrices (3x4, by rows), all in one array: where each instance's
    // start (in matrices, -1 for none) and how many it has.
    std::vector<int> jointBase_, jointCount_;
    // Each instance's morph weights in the same array: where they start (in matrices' room, 12
    // floats, -1 for none) and how many floats they have room for.
    std::vector<int> morphBase_, morphRoom_;
    std::vector<std::array<float, 4>> eyes_;  // by instance, as set
    std::vector<float> jointData_;
    bool jointsChanged_ = false;

    std::vector<Sprite> sprites_;
    float spriteScale_ = 1;
    std::vector<scene::ParticleVertex> particleVertices_;
    std::vector<ParticleBatch> particleBatches_;
    bool drawScene_ = true;
    bool instancesOnly_ = false;  // while drawing a portrait (see drawInstancesToImage)
    float clear_[3] = {0.17f, 0.18f, 0.19f};
    float time_ = 0;
    double lotTime_ = 0;
    bool bumpMapping_ = true;
    bool floorAndWallNormalMapping_ = true;
    std::uint32_t highlightObject_ = 0;
    float highlightAmbient_ = 0;
    Shadows shadows_;
    scene::objectshadow::Options shadowOptions_;
    bool reflections_ = true;
    bool seaReflections_ = true;
    bool specularHighlights_ = true;
    // This frame's pool caustics are drawn: its tile and shadow map are made (a renderer that draws
    // them sets it).
    bool poolCaustics_ = false;

    // What a frame plan reads (see frame_plan.hpp).
    friend struct FramePlan;
};

} // namespace ts2::render
