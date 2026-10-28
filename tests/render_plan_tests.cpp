// What every renderer shares: which primitives are in view, in which order they are drawn, and the
// textures and numbers each draw is given. None of it needs a GPU.
#include "test_support.hpp"
#include "render/frame_plan.hpp"
#include "render/mesh_pack.hpp"

namespace {

using namespace ts2::render;
namespace scene = ts2::scene;

// A renderer that draws nothing, to set a scene's CPU state up.
class StateOnly final : public RendererState, public ResourceQuery {
public:
    Capabilities capabilities() const override { return {}; }
    Prepared prepare(const scene::Scene&) const override { return {}; }
    void upload(Prepared) override {}
    Prepared swap(Prepared) override { return {}; }
    bool uploaded() override { return true; }
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>&) override {}
    bool draw(const Camera&) override { return true; }
    bool drawToFile(const Camera&, unsigned, unsigned, const std::filesystem::path&) override { return true; }
    std::optional<scene::Image> drawInstancesToImage(const Camera&, unsigned, unsigned, const scene::ObjectLighting&) override { return {}; }
    std::string backend() const override { return "none"; }
    std::string device() const override { return "none"; }
    void benchmark(const Camera&, unsigned, unsigned, int) override {}
    void addMaterials(const scene::Scene&) override {}
    void addMeshes(const scene::Scene&) override {}
    void setImages(const scene::Scene&, const std::vector<int>&, bool) override {}
    void setLightMapFade(float) override {}
    void setSkybox(const std::vector<scene::Image>&) override {}

    // Takes the scene as an upload would: its CPU state, and its meshes laid out.
    void load(const scene::Scene& s) {
        Fill p;
        copyScene(p, s);
        swapScene(p);
        MeshData data;
        for (const auto& mesh : s.meshes) {
            meshes_.push_back(packMesh(mesh, s.materials.size(), 0, data));
        }
    }
    const std::vector<GpuMesh>& meshes() const override { return meshes_; }
    bool hasImage(int i) const override { return i >= 0 && i < images; }
    bool hasEnvCube(int) const override { return false; }
    bool hasFadeFrom(int) const override { return false; }
    float lightMapFade() const override { return 1; }
    bool skinReady(std::uint32_t) const override { return true; }

    int images = 0;  // how many of the scene's images are uploaded
    std::vector<GpuMesh> meshes_;

private:
    struct Fill : PreparedBase {
        void releaseSome(std::size_t) override {}
    };
};

// A square of two triangles, 2 across, standing in the x-z plane at the origin.
scene::Mesh quad(int material) {
    scene::Mesh m;
    scene::Primitive p;
    p.material = material;
    p.positions = {-1, 0, -1, 1, 0, -1, 1, 0, 1, -1, 0, 1};
    p.indices = {0, 1, 2, 0, 2, 3};
    m.primitives.push_back(p);
    return m;
}

scene::Instance at(int mesh, float x, float y, float z) {
    scene::Instance i;
    i.mesh = mesh;
    i.transform.m[3] = x;
    i.transform.m[7] = y;
    i.transform.m[11] = z;
    return i;
}

// A camera ten units back from the origin on the y axis, looking at it.
Camera front() {
    Camera c;
    c.eye = {0, -10, 0};
    c.target = {0, 0, 0};
    c.fovY = 0.8f;
    c.farPlane = 100;
    return c;
}

void boxesAgainstTheView() {
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    CHECK(boxInView(f.viewProjection, {-1, -1, -1}, {1, 1, 1}) == InView::Inside);
    CHECK(boxInView(f.viewProjection, {500, -1, -1}, {502, 1, 1}) == InView::Outside);
    // Behind the camera.
    CHECK(boxInView(f.viewProjection, {-1, -30, -1}, {1, -20, 1}) == InView::Outside);
    CHECK(boxInView(f.viewProjection, {-1, -1, -1}, {500, 1, 1}) == InView::Partly);
}

void aFrameViewTakesItsFovAndNearPlane() {
    Camera camera = front();
    FrameView f = FrameView::make(camera, 1000, 500);
    CHECK(std::fabs(f.aspect - 2.0f) < 1e-6f);
    CHECK(std::fabs(f.nearZ - 0.01f) < 1e-6f);
    CHECK(std::fabs(f.fovY - 0.8f) < 1e-6f);
    camera.fovX = 1.0f;
    camera.nearPlane = 0.5f;
    f = FrameView::make(camera, 1000, 500);
    CHECK(std::fabs(f.nearZ - 0.5f) < 1e-6f);
    CHECK(std::fabs(f.fovY - 2 * std::atan(std::tan(0.5f) / 2.0f)) < 1e-6f);
    CHECK(std::fabs(f.sun[0] * f.sun[0] + f.sun[1] * f.sun[1] + f.sun[2] * f.sun[2] - 1) < 1e-5f);
}

void drawsAreSortedAsTheGameSortsThem() {
    scene::Scene s;
    scene::Material opaque, blend;
    opaque.layer = 0;
    blend.alpha = scene::AlphaMode::Blend;
    s.materials = {opaque, blend};
    s.meshes = {quad(0), quad(1)};
    // Two blended quads at different distances, an opaque one, one out of view, one not shown.
    s.instances = {at(1, 0, 0, 0), at(1, 0, 5, 0), at(0, 0, 0, 0), at(0, 900, 0, 0), at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    r.show({1, 1, 1, 1, 0});
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> opaqueDraws, layers, blended;
    FramePlan::collectDraws(r, r, f, opaqueDraws, layers, blended);
    CHECK(opaqueDraws.size() == 1);  // the culled and the hidden quads are left out
    CHECK(layers.empty());
    CHECK(blended.size() == 2);
    // Blended: the farthest from the camera first.
    CHECK(blended.size() == 2 && blended[0].instance->transform.m[7] == 5 && blended[1].instance->transform.m[7] == 0);
    CHECK(blended.size() == 2 && blended[0].depth > blended[1].depth);
}

void aMaterialSetToHiddenIsNotDrawn() {
    scene::Scene s;
    s.materials = {scene::Material{}};
    s.meshes = {quad(0)};
    s.instances = {at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    r.setMaterials(0, {scene::kHiddenMaterial});
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> a, b, c;
    FramePlan::collectDraws(r, r, f, a, b, c);
    CHECK(a.empty() && b.empty() && c.empty());
    r.setMaterials(0, {});
    FramePlan::collectDraws(r, r, f, a, b, c);
    CHECK(a.size() == 1);
}

void textureReferencesFollowWhatIsUploaded() {
    scene::Scene s;
    scene::Material m;
    m.image = 3;
    m.clamp = true;
    m.color = {0.5f, 0.25f, 1, 1};
    s.materials = {m};
    s.meshes = {quad(0)};
    s.instances = {at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;

    r.images = 0;  // its image didn't upload: drawn plain white
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    CHECK(draws.size() == 1);
    DrawUniforms u = FramePlan::uniformsFor(r, r, f, draws[0], 0);
    CHECK(u.samplers[0].texture.kind == TextureRef::Kind::White);
    CHECK(u.fragment.hasTexture == 0);
    CHECK(u.samplers[5].texture.kind == TextureRef::Kind::BlackCube);

    r.images = 4;
    u = FramePlan::uniformsFor(r, r, f, draws[0], 0);
    CHECK(u.samplers[0].texture.kind == TextureRef::Kind::Image && u.samplers[0].texture.index == 3);
    CHECK(u.samplers[0].sampler == SamplerKind::Clamped);
    CHECK(u.fragment.hasTexture == 1);
    CHECK(u.fragment.color[0] == 0.5f && u.fragment.color[1] == 0.25f);
    CHECK(u.samplers[2].texture.kind == TextureRef::Kind::White && u.samplers[2].sampler == SamplerKind::Repeat);
}

void textureLightScalesOnlyTheMaterialsThatTakeIt() {
    scene::Scene s;
    scene::Material dim, plain;
    dim.takesTextureLight = true;
    s.materials = {dim, plain};
    s.meshes = {quad(0), quad(1)};
    s.instances = {at(0, 0, 0, 0), at(1, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    scene::ObjectLighting night;
    night.textureLight = {0.35f, 0.35f, 0.7f};
    r.setObjectLighting(night);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    CHECK(draws.size() == 2);
    const DrawUniforms a = FramePlan::uniformsFor(r, r, f, draws[0], 0);
    const DrawUniforms b = FramePlan::uniformsFor(r, r, f, draws[1], 0);
    CHECK(std::fabs(a.fragment.color[0] - 0.35f) < 1e-6f && std::fabs(a.fragment.color[2] - 0.7f) < 1e-6f);
    CHECK(b.fragment.color[0] == 1.0f);
}

// A candle flame's texture holds 4 frames side by side, shown 10 a second on the lot's clock, left to
// right and over again, as the game steps it. Its colour is added whatever its alpha, so nothing of
// it is cut away.
void tiledTexturesStepThroughTheirFrames() {
    scene::Scene s;
    scene::Material flame;
    flame.tiles = 4;
    flame.tileSpeed = 10;
    flame.alpha = scene::AlphaMode::Sum;
    flame.color[3] = 0;
    s.materials = {flame};
    s.meshes = {quad(0)};
    s.instances = {at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    CHECK(blended.size() == 1);
    if (blended.size() != 1) return;
    r.setLotTime(0.25);
    const DrawUniforms third = FramePlan::uniformsFor(r, r, f, blended[0], 1);
    CHECK(third.fragment.uvTile[0] == 0.25f && third.fragment.uvTile[1] == 1 && third.fragment.uvTile[2] == 0.5f && third.fragment.uvTile[3] == 0);
    CHECK(third.fragment.alphaCutoff == 0);
    r.setLotTime(0.45);
    CHECK(FramePlan::uniformsFor(r, r, f, blended[0], 1).fragment.uvTile[2] == 0);
}

void packedMeshesAreLaidOutInTheSharedBuffers() {
    scene::Mesh m = quad(0);
    scene::Mesh bad = quad(0);
    bad.primitives[0].indices = {0, 1, 9};  // reads past its vertices
    MeshData data;
    const GpuMesh first = packMesh(m, 1, 0, data);
    const GpuMesh skipped = packMesh(bad, 1, 0, data);
    const GpuMesh second = packMesh(m, 1, 0, data);
    CHECK(first.primitives.size() == 1 && skipped.primitives.empty() && second.primitives.size() == 1);
    CHECK(first.primitives[0].firstVertex == 0 && second.primitives[0].firstVertex == 4);
    CHECK(second.primitives[0].firstIndex == 6);
    CHECK(data.vertices.size() == 8 * kVertexFloats && data.indices.size() == 12);
    CHECK(first.anyCullable && first.low[0] == -1 && first.high[2] == 1);
}

} // namespace

std::vector<Test> renderPlanTests() {
    return {
        {"Boxes are tested against the view", boxesAgainstTheView},
        {"A frame view takes its fov and near plane as the game does", aFrameViewTakesItsFovAndNearPlane},
        {"Draws are sorted as the game sorts them", drawsAreSortedAsTheGameSortsThem},
        {"A hidden material is not drawn", aMaterialSetToHiddenIsNotDrawn},
        {"Texture references follow what is uploaded", textureReferencesFollowWhatIsUploaded},
        {"The texture light scales only the materials that take it", textureLightScalesOnlyTheMaterialsThatTakeIt},
        {"Tiled textures step through their frames", tiledTexturesStepThroughTheirFrames},
        {"Packed meshes share their buffers", packedMeshesAreLaidOutInTheSharedBuffers},
    };
}
