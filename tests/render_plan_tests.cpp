// What every renderer shares: which primitives are in view, in which order they are drawn, and the
// textures and numbers each draw is given. None of it needs a GPU.
#include "test_support.hpp"
#include "render/frame_plan.hpp"
#include "render/mesh_pack.hpp"
#include "render/terrain_mesh.hpp"

#include <algorithm>
#include <iterator>
#include <random>

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
    void setWaitForScreen(bool) override {}
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
    void setSurroundings(const scene::Scene&) override {}

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
    // Both answer as every corner taken against every side does, across boxes of all sizes and places.
    std::minstd_rand random(12345);
    const auto place = [&](float reach) { return static_cast<float>(random() % 2001) / 1000.0f * reach - reach; };
    const auto& m = f.viewProjection.m;
    int seen[3] = {};
    for (int i = 0; i < 20000; ++i) {
        const float reach = i % 3 == 0 ? 5.0f : i % 3 == 1 ? 60.0f : 500.0f;
        const std::array<float, 3> low{place(reach), place(reach), place(reach)};
        const float size = reach * (static_cast<float>(random() % 100) / 100.0f);
        const std::array<float, 3> high{low[0] + size, low[1] + size / 2, low[2] + size * 2};
        int outside[6] = {};
        for (int c = 0; c < 8; ++c) {
            const float x = c & 1 ? high[0] : low[0], y = c & 2 ? high[1] : low[1], z = c & 4 ? high[2] : low[2];
            float p[4];
            for (int r = 0; r < 4; ++r) {
                p[r] = m[r * 4] * x + m[r * 4 + 1] * y + m[r * 4 + 2] * z + m[r * 4 + 3];
            }
            outside[0] += p[0] < -p[3];
            outside[1] += p[0] > p[3];
            outside[2] += p[1] < -p[3];
            outside[3] += p[1] > p[3];
            outside[4] += p[2] < 0;
            outside[5] += p[2] > p[3];
        }
        const bool out = std::any_of(std::begin(outside), std::end(outside), [](int n) { return n == 8; });
        const bool in = std::all_of(std::begin(outside), std::end(outside), [](int n) { return n == 0; });
        const InView view = boxInView(f.viewProjection, low, high);
        CHECK(view == (out ? InView::Outside : in ? InView::Inside : InView::Partly));
        CHECK(boxOutside(f.viewProjection, low, high) == out);
        ++seen[static_cast<int>(view)];
    }
    CHECK(seen[0] > 100);
    CHECK(seen[1] > 100);
    CHECK(seen[2] > 100);
}

// An imposter part's box holds all its positions, not its texture coordinates.
void imposterPartBoxes() {
    scene::PaintedTerrain t;
    t.imposterTextures.push_back({});
    scene::PaintedTerrain::ImposterPart part;
    part.vertices = {1, -2, 3, 90, 91, 92, 93, -4, 5, 0, 94, 95, 96, 97};
    part.indices = {0, 1, 0};
    t.imposters.push_back(part);
    const ImposterMesh mesh = imposterParts(t);
    CHECK(mesh.draws.size() == 1);
    CHECK((mesh.draws[0].low == std::array<float, 3>{-4, -2, 0}));
    CHECK((mesh.draws[0].high == std::array<float, 3>{1, 5, 3}));
}

// A pool's reflection: the view mirrored in the plane and flipped left to right, a point on the
// plane at the near end of depth, one below it past it (not drawn).
void aPoolMirrorsTheViewInItsPlane() {
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    const Mat4 mirror = poolMirror(f.viewProjection, 0.5f);
    auto clip = [](const Mat4& m, float x, float y, float z) {
        std::array<float, 4> r{};
        for (int i = 0; i < 4; ++i) {
            r[i] = m.m[i * 4] * x + m.m[i * 4 + 1] * y + m.m[i * 4 + 2] * z + m.m[i * 4 + 3];
        }
        return r;
    };
    const auto mirrored = clip(mirror, 0.3f, 2, 1.5f), seen = clip(f.viewProjection, 0.3f, 2, -0.5f);
    CHECK(std::fabs(mirrored[0] + seen[0]) < 1e-4f && std::fabs(mirrored[1] - seen[1]) < 1e-4f && std::fabs(mirrored[3] - seen[3]) < 1e-4f);
    const auto onPlane = clip(mirror, 0.3f, 2, 0.5f), below = clip(mirror, 0.3f, 2, 0.4f);
    CHECK(std::fabs(onPlane[2] - onPlane[3]) < 1e-4f && below[2] > below[3]);
    CHECK(mirrored[2] < mirrored[3] && mirrored[2] > 0);
}

// Only the part of a pool's mirror image that its surface reads is drawn. The surface's corners,
// moved as far as the ripples move what they read, fall inside it, flipped left to right. The
// narrowed view stretches that part over the clip volume and keeps depth.
void aPoolMirrorIsDrawnOnlyWhereRead() {
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    CHECK((poolMirrorArea(f, {}, 1280, 800) == std::array<float, 4>{-1, 1, -1, 1}));
    GpuPrimitive p;
    p.hasBox = true;
    p.low = {-0.2f, 1.8f, -0.5f};
    p.high = {0.2f, 2.2f, -0.5f};
    scene::Material m;
    m.poolSurface = true;
    const scene::Instance instance;
    const Draw d{&p, &instance, &m, 0, 0, 1};
    const auto area = poolMirrorArea(f, std::span(&d, 1), 1280, 800);
    CHECK(area[1] - area[0] < 2 && area[3] - area[2] < 2);
    const Mat4 narrow = narrowedTo(f.viewProjection, area);
    for (int c = 0; c < 4; ++c) {
        const float x = c & 1 ? p.high[0] : p.low[0], y = c & 2 ? p.high[1] : p.low[1], z = p.low[2];
        const float* v = f.viewProjection.m.data();
        const float w = v[12] * x + v[13] * y + v[14] * z + v[15];
        const float sx = -(v[0] * x + v[1] * y + v[2] * z + v[3]) / w, sy = (v[4] * x + v[5] * y + v[6] * z + v[7]) / w;
        const float shift = 0.2f / w;
        CHECK(sx - shift >= area[0] && sx + shift <= area[1] && sy - shift >= area[2] && sy + shift <= area[3]);
        const float* n = narrow.m.data();
        const float nx = (n[0] * x + n[1] * y + n[2] * z + n[3]) / w;
        CHECK(std::fabs(nx - (-2 * sx - (area[0] + area[1])) / (area[1] - area[0])) < 1e-4f);
    }
    for (int k = 8; k < 16; ++k) {
        CHECK(narrow.m[k] == f.viewProjection.m[k]);
    }
}

// The sun's view of an 11 by 7 tile pool, fitted as the game fits it, against the game's own: across and
// in depth to a hundredth, up and down within 0.004 of the scale (0.04 of a tile).
void aPoolsSunViewIsFittedAsTheGames() {
    const std::array<float, 12> place{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    const auto light = poolLight(place, {12.925f, 4.925f, -2.63f, 24.075f, 12.075f, 0.36f}, normalize({1.5f, 1.2f, 2}));
    const std::array<float, 16> game{-0.06876f, 0.08595f, 0, 0.54147f, -0.05773f, -0.04618f, 0.07101f, 1.30939f,
                                     -0.02772f, -0.02218f, -0.03697f, 1.27035f, 0, 0, 0, 1};
    for (int i = 0; i < 16; ++i) {
        const float tolerance = i == 7 ? 0.005f : 0.0015f;
        CHECK(std::fabs(light.clip.m[i] - game[i]) < tolerance);
    }
    // The shadow map's texture coordinates: x from 0 to 1 across, y down.
    CHECK(std::fabs(light.texture.m[3] - (0.5f * game[3] + 0.5f)) < 0.002f && std::fabs(light.texture.m[7] - (-0.5f * game[7] + 0.5f)) < 0.003f);
    // A flipbook frame: the time's fraction picks the row (of 4), its fraction the column (of 8).
    const auto frame = causticsFrame(10.40625f, 0.75f);
    CHECK(frame.frame[0] == 5.0f / 8 && frame.frame[1] == 1.0f / 4 && frame.color[0] == 0.75f);
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

// A shape colour blends a standard material and scales its colour and alpha, as the game's standard
// material script draws one. The TextureAlpha material script has none, so it draws as it is.
void shapeColourOnlyChangesTheMaterialsThatTakeIt() {
    scene::Scene s;
    scene::Material standard, groundShadow;
    groundShadow.alpha = scene::AlphaMode::Blend;
    groundShadow.takesShapeColor = false;
    s.materials = {standard, groundShadow};
    scene::Mesh tree = quad(0);
    tree.primitives.push_back(quad(1).primitives[0]);
    s.meshes = {tree};
    s.instances = {at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    r.setShapeColor(0, std::array<float, 4>{1, 1, 1, 0.25f});
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    CHECK(draws.empty() && blended.size() == 2);
    for (const Draw& d : blended) {
        const float alpha = FramePlan::uniformsFor(r, r, f, d, 0).fragment.color[3];
        CHECK(alpha == (d.material->takesShapeColor ? 0.25f : 1.0f));
    }
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

// The game's hair state keeps only alpha above 60 out of 255, where other blended surfaces keep
// nearly everything.
void blendedHairDropsLowAlpha() {
    scene::Scene s;
    scene::Material hair;
    hair.alpha = scene::AlphaMode::Blend;
    hair.blendedHair = true;
    scene::Material glass;
    glass.alpha = scene::AlphaMode::Blend;
    s.materials = {hair, glass};
    s.meshes = {quad(0), quad(1)};
    s.instances = {at(0, 0, 0, 0), at(1, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    CHECK(blended.size() == 2);
    for (const Draw& d : blended) {
        const float cutoff = FramePlan::uniformsFor(r, r, f, d, 1).fragment.alphaCutoff;
        CHECK(cutoff == (d.material->blendedHair ? 60.5f / 255 : 0.01f));
    }
}

// A hair's groups may lie in the model in any order. The sort numbers decide, the lowest first and
// -1 before all, wherever each group is.
void blendedGroupsDrawByTheirSortNumbers() {
    scene::Scene s;
    scene::Material hair;
    hair.alpha = scene::AlphaMode::Blend;
    s.materials = {hair};
    scene::Mesh m = quad(0);
    const scene::Primitive first = m.primitives[0];
    m.primitives.clear();
    for (const int number : {-1, 1, 2, 4, 3}) {
        scene::Primitive p = first;
        p.alphaSort = number;
        m.primitives.push_back(p);
    }
    s.meshes = {m};
    s.instances = {at(0, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    std::vector<int> order;
    for (const Draw& d : blended) {
        order.push_back(d.primitive->alphaSort);
    }
    CHECK((order == std::vector<int>{-1, 1, 2, 3, 4}));
}

// Opaque surfaces sharing a material draw together, so textures are bound less often. Roofs come
// first in the order placed, and a lower layer stays before a higher one.
void opaqueDrawsGroupByMaterial() {
    scene::Scene s;
    s.materials.resize(3);
    s.materials[2].layer = -18;
    s.meshes = {quad(1), quad(0), quad(1), quad(0), quad(1), quad(2)};
    for (int m = 0; m < 6; ++m) {
        s.instances.push_back(at(m, static_cast<float>(m), 0, 0));
    }
    s.instances[0].part = scene::Part::Roof;
    s.instances[1].part = scene::Part::Roof;
    for (std::size_t i = 2; i < 6; ++i) {
        s.instances[i].part = scene::Part::Object;
    }
    StateOnly r;
    r.load(s);
    const Camera camera = front();
    const FrameView f = FrameView::make(camera, 1280, 800);
    std::vector<Draw> draws, layers, blended;
    FramePlan::collectDraws(r, r, f, draws, layers, blended);
    std::vector<int> order;
    for (const Draw& d : draws) {
        order.push_back(static_cast<int>(d.instance->transform.m[3]));
    }
    CHECK((order == std::vector<int>{5, 0, 1, 3, 2, 4}));
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
        {"An imposter part's box holds its positions", imposterPartBoxes},
        {"A pool mirrors the view in its plane", aPoolMirrorsTheViewInItsPlane},
        {"A pool's mirror image is drawn only where read", aPoolMirrorIsDrawnOnlyWhereRead},
        {"A pool's sun view is fitted as the game's", aPoolsSunViewIsFittedAsTheGames},
        {"A frame view takes its fov and near plane as the game does", aFrameViewTakesItsFovAndNearPlane},
        {"Draws are sorted as the game sorts them", drawsAreSortedAsTheGameSortsThem},
        {"A hidden material is not drawn", aMaterialSetToHiddenIsNotDrawn},
        {"Texture references follow what is uploaded", textureReferencesFollowWhatIsUploaded},
        {"The texture light scales only the materials that take it", textureLightScalesOnlyTheMaterialsThatTakeIt},
        {"A shape colour only changes the materials that take it", shapeColourOnlyChangesTheMaterialsThatTakeIt},
        {"Tiled textures step through their frames", tiledTexturesStepThroughTheirFrames},
        {"Blended hair drops low alpha", blendedHairDropsLowAlpha},
        {"Opaque draws group by material", opaqueDrawsGroupByMaterial},
        {"Blended groups draw by their sort numbers", blendedGroupsDrawByTheirSortNumbers},
        {"Packed meshes share their buffers", packedMeshesAreLaidOutInTheSharedBuffers},
    };
}
