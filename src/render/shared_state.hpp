// What every renderer keeps of the scene on the CPU: its materials and instances, which are shown
// and how they are lit and posed, and the settings. A renderer adds what it uploads to its GPU.
#pragma once

#include "render/irenderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ts2::render {

class RendererState : public IRenderer {
public:
    void show(std::vector<std::uint8_t> visible) override;
    void setClearColor(float r, float g, float b) override { clear_[0] = r; clear_[1] = g; clear_[2] = b; }
    void showSprites(std::vector<Sprite> sprites, bool scene = true) override;
    void setJoints(std::size_t instance, const std::vector<scene::Transform>& joints) override;
    void setPlace(std::size_t instance, const scene::Transform& transform) override;
    void setMaterials(std::size_t instance, std::vector<int> materials) override;
    void setInstance(const scene::Scene& scene, std::size_t index) override;
    void setLights(const scene::Scene& scene, std::size_t index) override;
    void setImposterLight(const std::array<float, 3>& light) override { imposterLight_ = light; }
    void setNight(bool night) override { night_ = night; }
    void setObjectLighting(std::optional<scene::ObjectLighting> lighting) override { objectLighting_ = std::move(lighting); }
    void setTime(float seconds) override { time_ = seconds; }
    void setBumpMapping(bool on) override { bumpMapping_ = on; }
    void setFloorAndWallNormalMapping(bool on) override { floorAndWallNormalMapping_ = on; }
    void setHighlight(std::uint32_t object, float ambient) override {
        highlightObject_ = object;
        highlightAmbient_ = ambient;
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
    bool night_ = false;
    std::vector<scene::ObjectLighting> lightSets_;  // objects lit by their room
    std::vector<std::uint8_t> visible_;

    // Skinned instances' joint matrices (3x4, by rows), all in one array: where each instance's
    // start (in matrices, -1 for none) and how many it has.
    std::vector<int> jointBase_, jointCount_;
    std::vector<float> jointData_;
    bool jointsChanged_ = false;

    std::vector<Sprite> sprites_;
    bool drawScene_ = true;
    bool instancesOnly_ = false;  // while drawing a portrait (see drawInstancesToImage)
    float clear_[3] = {0.17f, 0.18f, 0.19f};
    float time_ = 0;
    bool bumpMapping_ = true;
    bool floorAndWallNormalMapping_ = true;
    std::uint32_t highlightObject_ = 0;
    float highlightAmbient_ = 0;

    // What a frame plan reads (see frame_plan.hpp).
    friend struct FramePlan;
};

} // namespace ts2::render
