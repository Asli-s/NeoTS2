// What every renderer shares: which primitives are in view, in which order they are drawn, and the
// textures and numbers each draw is given. None of it needs a GPU.
#include "test_support.hpp"
#include "render/cpu_light.hpp"
#include "render/fixed_layout.hpp"
#include "render/frame_plan.hpp"
#include "render/gl_subset.hpp"
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

void nightDimsOnlyTheMaterialsThatDimAtNight() {
    scene::Scene s;
    scene::Material dim, plain;
    dim.nightDimmed = true;
    s.materials = {dim, plain};
    s.meshes = {quad(0), quad(1)};
    s.instances = {at(0, 0, 0, 0), at(1, 0, 0, 0)};
    StateOnly r;
    r.load(s);
    r.setNight(true);
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

void vertexLightIsWorkedOutOnTheCpu() {
    VertexUniforms u{};
    u.ambient[0] = u.ambient[1] = u.ambient[2] = 0.2f;
    u.ambient[3] = 1;
    // A directional light toward +z, and a point light 2 away on +x with a range of 3 (squared: 9).
    u.lightDirections[0][2] = 1;
    u.lightColors[0][0] = u.lightColors[0][1] = u.lightColors[0][2] = 0.5f;
    u.lightPositions[1][0] = 2;
    u.lightDirections[1][3] = 9;
    u.lightColors[1][0] = u.lightColors[1][1] = u.lightColors[1][2] = 1;

    const auto facingUp = gameLight(u, {0, 0, 1}, {0, 0, 0}, false);
    CHECK(facingUp[3] == 1 && std::fabs(facingUp[0] - 0.7f) < 1e-6f);
    // A normal need not be unit length. Facing the point light (no falloff set: all of it), the
    // directional one no longer reaches it.
    const auto longNormal = gameLight(u, {0, 0, 5}, {0, 0, 0}, false);
    CHECK(std::fabs(longNormal[0] - 0.7f) < 1e-6f);
    const auto toPoint = gameLight(u, {5, 0, 0}, {0, 0, 0}, false);
    CHECK(std::fabs(toPoint[0] - 1.2f) < 1e-6f);
    // Out of the point light's range, and facing away from the directional one, only the ambient is left.
    const auto far = gameLight(u, {-1, 0, -1}, {-5, 0, 0}, false);
    CHECK(std::fabs(far[0] - 0.2f) < 1e-6f);

    // A light lit per pixel is left out, unless all of them are summed here.
    u.pixelLights[0] = 1;
    u.pixelLights[1] = 0;
    CHECK(std::fabs(gameLight(u, {0, 0, 1}, {0, 0, 0}, false)[0] - 0.2f) < 1e-6f);
    CHECK(std::fabs(gameLight(u, {0, 0, 1}, {0, 0, 0}, true)[0] - 0.7f) < 1e-6f);

    // A thumbnail's light is clamped and doubled, and an instance not lit this way has none.
    u.pixelLights[0] = 0;
    u.thumbnail[0] = u.thumbnail[1] = u.thumbnail[2] = 1;
    u.thumbnail[3] = 1;
    const auto thumbnail = gameLight(u, {0, 0, 1}, {0, 0, 0}, false);
    CHECK(thumbnail[3] == 2 && std::fabs(thumbnail[0] - 1.4f) < 1e-6f);
    u.ambient[3] = 0;
    CHECK(gameLight(u, {0, 0, 1}, {0, 0, 0}, false)[3] == 0);
}

void terrainIsLaidOutForTheFixedPipeline() {
    // Two paints' worth of alpha maps over a 2 x 2 map with one cell: the cell shows texture 3 for the
    // first layer and texture 4 for the second.
    scene::PaintedTerrain t;
    t.columns = t.rows = 3;
    t.alphaSize = 2;
    t.alphas = {255, 0, 128, 255, 0, 255, 127, 0};
    t.cellTiles = 2;
    t.cellsX = t.cellsY = 1;
    t.cellTextures = {3, 4};
    t.textureRepeat = 4;
    t.lightMapScale = 8;
    const auto three = fixedPaintWeights(t, 3), four = fixedPaintWeights(t, 4), other = fixedPaintWeights(t, 9);
    CHECK(three.size() == 16 && three[0] == 255 && three[4] == 0 && three[8] == 128 && three[12] == 255 && three[3] == 255);
    CHECK(four[0] == 0 && four[4] == 255 && four[8] == 127 && four[12] == 0);
    CHECK(other[0] == 0 && other[12] == 0);

    // The grid's vertices carry the three textures' coordinates, and its colours are white without a gradient.
    IndexedMesh grid;
    grid.vertices = {10, 20, 5, 0, 0, 1, 2, 1};
    const auto vertices = fixedTerrainVertices(grid, t);
    CHECK(vertices.size() == kFixedTerrainFloats && vertices[0] == 10 && vertices[3] == 0.5f && vertices[4] == 0.25f);
    CHECK(vertices[5] == 1 && vertices[6] == 0.5f && vertices[7] == 0.25f && vertices[8] == 0.125f);
    CHECK(fixedTerrainColors(grid, t) == std::vector<std::uint8_t>(4, 255));

    // A gradient dark at depth: a vertex at the surface takes its top row, a deep one its bottom row.
    t.underwaterGradient = scene::Image{1, 2, {200, 200, 200, 255, 0, 0, 0, 255}, {}};
    t.litFrom = 10;
    t.gradientDepth = 10;
    grid.vertices = {0, 0, 10, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0};
    const auto colors = fixedTerrainColors(grid, t);
    CHECK(colors.size() == 8 && colors[0] == 200 && colors[4] == 0);
}

void terrainPassesCanBeColouredAtTheirVertices() {
    // One layer fully shown, by a cell that shows texture 3, over a 2 x 2 alpha map.
    scene::PaintedTerrain t;
    t.columns = t.rows = 3;
    t.alphaSize = 2;
    t.alphas = {255, 255, 255, 255};
    t.cellTiles = 2;
    t.cellsX = t.cellsY = 1;
    t.cellTextures = {3};
    t.lightMapScale = 2;
    t.litFrom = 0;
    IndexedMesh grid;
    grid.vertices = {0, 0, 5, 0, 0, 1, 1, 1,  0, 0, -20, 0, 0, 1, 1, 1};  // one above the water, one far under it
    const auto shown = fixedSliceColors(grid, t, 3), other = fixedSliceColors(grid, t, 4);
    CHECK(shown.size() == 8 && shown[0] == 255 && shown[3] == 255 && shown[4] == 255);
    CHECK(other[0] == 0 && other[4] == 0 && other[3] == 255);

    // A light map of one texel multiplies in above the water only, and a gradient dark at the bottom
    // darkens what is deep.
    t.lightMap = scene::Image{1, 1, {100, 100, 100, 255}, {}};
    const auto lit = fixedSliceColors(grid, t, 3);
    CHECK(lit[0] == 100 && lit[4] == 255);
    t.underwaterGradient = scene::Image{1, 2, {255, 255, 255, 255, 0, 0, 0, 255}, {}};
    t.gradientDepth = 10;
    const auto deep = fixedSliceColors(grid, t, 3);
    CHECK(deep[4] == 0 && deep[0] == 100);

    // Road vertices take the light map at their place (here the one texel everywhere).
    RoadMesh roads;
    roads.mesh.vertices = {64, 32, 1, 0, 0, 0};
    const auto road = fixedRoadColors(roads, t, 128);
    CHECK(road.size() == 4 && road[0] == 100 && road[3] == 255);
    t.lightMap = {};
    CHECK(fixedRoadColors(roads, t, 128)[0] == 255);
}

void theConsoleSubsetReportsWhatTheLibrariesCannotDo() {
    // OpenGL's enums, as numbers.
    constexpr unsigned zero = 0, one = 1, srcColor = 0x300, srcAlpha = 0x302, oneMinusSrcAlpha = 0x303, dstColor = 0x306;
    constexpr unsigned greater = 0x204, gequal = 0x206, texture2d = 0xDE1, cubeMap = 0x8513, floats = 0x1406, ushort = 0x1403;
    std::vector<std::string> reports;
    GlSubset subset([&](const std::string& text) { reports.push_back(text); });
    const auto has = [&](const char* needle) { return !reports.empty() && reports.back().find(needle) != std::string::npos; };

    // Over, with the alpha blended the same way: fine for both.
    subset.blend(srcAlpha, oneMinusSrcAlpha, srcAlpha, oneMinusSrcAlpha);
    CHECK(reports.empty());
    // Alpha blended differently needs glBlendFuncSeparate, which neither has.
    subset.blend(srcAlpha, oneMinusSrcAlpha, one, oneMinusSrcAlpha);
    CHECK(reports.size() == 1 && has("GLdc and ps2gl"));
    // A colour by the colour behind it: GLdc has it, the GS can't.
    subset.blend(dstColor, zero, dstColor, zero);
    CHECK(reports.size() == 2 && has("(not in ps2gl)"));
    // And GL_SRC_COLOR as a source is the other way round (GLdc lacks it, and so does ps2gl's short list).
    subset.blend(srcColor, zero, srcColor, zero);
    CHECK(reports.size() == 4 && subset.violations() == 4);
    // Each is reported once.
    subset.blend(srcColor, zero, srcColor, zero);
    subset.blend(srcAlpha, oneMinusSrcAlpha, one, oneMinusSrcAlpha);
    CHECK(reports.size() == 4);

    subset.alphaFunc(greater);
    CHECK(reports.size() == 4);
    subset.alphaFunc(gequal);
    CHECK(reports.size() == 5 && has("GLdc"));
    subset.enable(texture2d);
    CHECK(reports.size() == 5);
    subset.enable(cubeMap);
    CHECK(reports.size() == 6 && has("GL_TEXTURE_CUBE_MAP"));

    subset.texture(texture2d, 256, 64);
    CHECK(reports.size() == 6);
    subset.texture(texture2d, 100, 64);
    CHECK(reports.size() == 7 && has("powers of two"));
    subset.texture(texture2d, 2048, 64);
    CHECK(reports.size() == 8 && has("1024"));
    subset.texture(texture2d, 4, 64);
    CHECK(reports.size() == 9 && has("smallest"));
    subset.textureUnits(1);
    CHECK(reports.size() == 9);
    subset.textureUnits(2);
    CHECK(reports.size() == 10);

    // One array for each attribute is fine to ps2gl; an interleaved one is strided.
    subset.array("Vertex", 3, floats, 12);
    subset.array("Vertex", 3, floats, 0);
    CHECK(reports.size() == 10);
    subset.array("Vertex", 3, floats, 68);
    CHECK(reports.size() == 11 && has("ps2gl"));

    // Indexed drawing: ps2gl's is a stub, and the others want 16-bit indices.
    subset.drawElements(ushort);
    CHECK(reports.size() == 12 && has("placeholder"));
    subset.drawElements(0x1405);
    CHECK(reports.size() == 13 && has("16-bit"));
}

void roadsTakeTheirLightMapCoordinatesFromTheirPlace() {
    RoadMesh roads;
    roads.mesh.vertices = {64, 32, 1, 0.25f, 0.75f, 2};
    const auto vertices = fixedRoadVertices(roads, 128);
    CHECK(vertices.size() == kFixedRoadFloats && vertices[0] == 64 && vertices[3] == 0.25f && vertices[5] == 0.5f && vertices[6] == 0.25f);
    const scene::Image white{1, 1, {255, 255, 255, 255}, {}}, black{1, 1, {0, 0, 0, 255}, {}};
    const auto average = averageColor({white, black});
    CHECK(std::fabs(average[0] - 0.5f) < 1e-6f);
    CHECK(averageColor({})[0] == 1);
}

} // namespace

std::vector<Test> renderPlanTests() {
    return {
        {"Boxes are tested against the view", boxesAgainstTheView},
        {"A frame view takes its fov and near plane as the game does", aFrameViewTakesItsFovAndNearPlane},
        {"Draws are sorted as the game sorts them", drawsAreSortedAsTheGameSortsThem},
        {"A hidden material is not drawn", aMaterialSetToHiddenIsNotDrawn},
        {"Texture references follow what is uploaded", textureReferencesFollowWhatIsUploaded},
        {"Night dims only the materials that dim at night", nightDimsOnlyTheMaterialsThatDimAtNight},
        {"Packed meshes share their buffers", packedMeshesAreLaidOutInTheSharedBuffers},
        {"Vertex light is worked out on the CPU", vertexLightIsWorkedOutOnTheCpu},
        {"Terrain is laid out for the fixed pipeline", terrainIsLaidOutForTheFixedPipeline},
        {"Terrain passes can be coloured at their vertices", terrainPassesCanBeColouredAtTheirVertices},
        {"The console subset reports what the libraries can't do", theConsoleSubsetReportsWhatTheLibrariesCannotDo},
        {"Roads take their light map coordinates from their place", roadsTakeTheirLightMapCoordinatesFromTheirPlace},
    };
}
