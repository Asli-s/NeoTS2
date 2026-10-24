#pragma once

#include "platform/platform.hpp"
#include "render/frame_plan.hpp"
#include "render/gl/gl_api.hpp"
#include "render/gl_subset.hpp"
#include "render/irenderer.hpp"
#include "render/mesh_pack.hpp"
#include "render/shared_state.hpp"
#include "render/terrain_mesh.hpp"
#include "render/texture_fit.hpp"
#include "scene/scene.hpp"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ts2::render {

// Draws a Scene with OpenGL 2.1 (and a few common extensions), for systems that have no more.
// Shaders are GLSL 1.20 made from the same HLSL the SDL GPU renderer uses (see CMakeLists.txt).
//
// With `fixedFunction` it draws without shaders, with the fixed pipeline of OpenGL 1.5, for systems
// whose cards have none: the game's lighting is worked out for each vertex on the CPU (see
// cpu_light.hpp), and bump mapping, specular, environment cubes and the water's reflection are left
// out (see capabilities()). The neighborhood's terrain, roads and water are laid out for it when the
// scene is uploaded (see fixed_layout.hpp), and the terrain and roads use two texture units.
//
// A context is current on one thread at a time, and only the thread that draws makes calls on its own.
// Where the driver can share objects between contexts, prepare() makes its textures and buffers in
// a context of its own on the thread it is called from. Where it can't, prepare() only gathers what to
// upload and swap() uploads it. Everything else that touches the GPU is on the drawing thread.
class GlRenderer final : public RendererState, private ResourceQuery {
public:
    GlRenderer(platform::Window& window, const std::filesystem::path& shaderFolder, bool fixedFunction = false);
    ~GlRenderer() override;

    using IRenderer::upload;
    [[nodiscard]] Capabilities capabilities() const override;
    [[nodiscard]] Prepared prepare(const scene::Scene& scene) const override;
    void upload(Prepared prepared) override;
    [[nodiscard]] Prepared swap(Prepared prepared) override;
    [[nodiscard]] bool uploaded() override { return true; }
    void setSprites(const std::vector<std::pair<int, const scene::Image*>>& images) override;
    [[nodiscard]] bool draw(const Camera& camera) override;
    [[nodiscard]] bool drawToFile(const Camera& camera, unsigned width, unsigned height, const std::filesystem::path& path) override;
    [[nodiscard]] std::optional<scene::Image> drawInstancesToImage(const Camera& camera, unsigned width, unsigned height,
                                                                   const scene::ObjectLighting& lighting) override;
    std::string backend() const override { return fixed_ ? "opengl1" : "opengl"; }
    std::string device() const override;
    void benchmark(const Camera& camera, unsigned width, unsigned height, int frames) override;
    void addMaterials(const scene::Scene& scene) override;
    void addMeshes(const scene::Scene& scene) override;
    void setImages(const scene::Scene& scene, const std::vector<int>& indexes, bool fade = false) override;
    void setLightMapFade(float fade) override;
    void setSkybox(const std::vector<scene::Image>& faces) override;
    void setSingleTextureTerrain(bool on) override { singleTextureTerrain_ = on; }
    void setOverbrightLight(bool on) override { overbright_ = on; }
    void setVertexLightMaps(bool on) override;

private:
    // What the frame plan asks about the uploaded scene (see ResourceQuery).
    const std::vector<GpuMesh>& meshes() const override { return gpu_->meshes; }
    bool hasImage(int index) const override;
    bool hasEnvCube(int index) const override;
    bool hasFadeFrom(int index) const override;
    float lightMapFade() const override { return gpu_->lightMapFade; }
    bool skinReady(std::uint32_t buffers) const override;

    // A texture object and how it is sampled right now. A GL 2 texture carries its sampler's state,
    // so the wrap mode is set when a draw needs another than the last one used.
    struct Texture {
        GLuint id = 0;
        GLenum target = GL_TEXTURE_2D;
        bool mips = false;
        GLint wrap = GL_REPEAT;
        bool nearest = false;  // sampled without filtering
        std::size_t bytes = 0;  // the memory it takes (RGBA, or 16 bits a texel; with its smaller levels)
        unsigned width = 0, height = 0;
        unsigned bits = 0;  // bits a texel it is kept in (4 or 8: with a palette)
    };
    // Objects to delete on the drawing thread: a prepared scene can be let go of on any thread.
    struct Graveyard {
        std::mutex lock;
        std::vector<GLuint> textures, buffers;
    };
    // One vertex and one index buffer: the uploaded scene's meshes, then each batch of meshes added
    // later (see addMeshes). Skinned primitives are posed on the CPU from `vertices` and `skin`.
    struct MeshBuffers {
        GLuint vertices = 0;  // without shaders, one block of tightly packed values for each attribute (see uploadBlocks)
        GLuint indices = 0;
        std::size_t vertexCount = 0;  // the vertices in the buffer, which sizes its blocks
        bool wideIndices = true;      // 32-bit indices (see uploadIndices)
        std::vector<float> cpuVertices;
        std::vector<std::uint8_t> cpuSkin;
        std::vector<std::uint32_t> cpuIndices;  // only kept for the strict mode's measuring (see noteDensity)
    };
    // What a scene is drawn with on the GPU.
    struct SceneGpu {
        std::vector<Texture> textures;  // id 0 where an image is broken
        Texture white;                  // stands in for surfaces without a texture
        std::vector<GpuMesh> meshes;
        std::vector<MeshBuffers> meshBuffers;
        std::vector<Texture> envCubes;
        Texture blackCube;
        Texture skybox;  // a lot's sky
        // Without shaders, the pixels of the floor and wall light maps by image index, and of those being faded from,
        // because their light is taken from them at the vertices (see drawListFixed).
        std::map<int, scene::Image> lightImages, lightFrom;
        std::set<int> drawnImages;  // the images some draw has used (kept only for the strict mode's report)
        // And the most pixels one repeat of an image covered across and down, at the size the screen would be on a console
        // (see noteDensity): how big the image needs to be.
        std::map<int, std::array<float, 2>> neededPixels;
        // With NEOTS2_GL1_TEXTURE_DETAIL, the full images the textures are made from at the sizes the screen asks for.
        std::map<int, scene::Image> sources;
        std::array<Texture, 6> skyFaces;  // without shaders, a lot's sky as six pictures: a cube map is more than the consoles take
        // Light maps being faded from, by the image they are replacing (see setImages), and how far
        // the fade has gone (0 .. 1).
        std::map<int, Texture> fadingFrom;
        float lightMapFade = 1;

        // A neighborhood's painted terrain (see scene::PaintedTerrain). OpenGL 2 has no texture arrays:
        // each paint texture is its own, and the alpha maps and cells' textures of four paints share one.
        GLuint terrainVertices = 0, terrainIndices = 0;
        unsigned terrainIndexCount = 0;
        // Without shaders: how many vertices the terrain, roads and imposters have (their buffers are in blocks) and whether
        // each index buffer is 32-bit.
        unsigned terrainVertexCount = 0, roadVertexCount = 0, imposterVertexCount = 0;
        bool terrainWide = true, roadWide = true, imposterWide = true, waterWide = true, curtainWide = true;
        // Without shaders (see fixed_layout.hpp): the grid's gradient colours, and the weight each
        // paint texture in terrainSlices shows with (a texture each, in that order).
        GLuint terrainColors = 0;
        std::vector<Texture> terrainWeights;
        // And for drawing with one texture a pass: the grid's colours in each paint texture's pass, and the roads' vertices'.
        std::vector<GLuint> terrainSliceColors;
        GLuint roadColors = 0;
        std::array<float, 4> waterFlat{};  // the colour and opacity of the water without its reflection
        std::vector<Texture> terrainPaints;
        std::vector<std::uint8_t> terrainSlices;  // the paint textures some cell shows
        std::array<Texture, 2> terrainAlphas, terrainCells;
        Texture terrainLight, terrainGradient;
        TerrainUniforms terrainUniforms{};
        // Its roads, one texture each, drawn in runs of quads that share one.
        GLuint roadVertices = 0, roadIndices = 0;
        std::vector<Texture> roadTextures;
        std::vector<RoadMesh::Run> roadRuns;
        float lightMapSize = 1280;
        // Its lots' imposters.
        GLuint imposterVertices = 0, imposterIndices = 0;
        std::vector<Texture> imposterTextures;
        std::vector<ImposterMesh::Part> imposterDraws;
        // A lot skirt's boundary curtain.
        GLuint curtainVertices = 0, curtainIndices = 0;
        // The neighborhood water's sheet, its ripples and the sky it reflects.
        GLuint waterVertices = 0, waterIndices = 0;
        unsigned waterIndexCount = 0;
        Texture ripples, skyCube;
        float waterLevel = 0;
        WaterUniforms waterUniforms{};

        void release(Graveyard& graveyard);
        // The memory its textures take.
        [[nodiscard]] std::size_t textureBytes() const;
        // Releases up to `count` textures. True when none are left.
        bool releaseTextures(Graveyard& graveyard, std::size_t count);
    };
    // What prepare gathers on any thread, for swap to upload on the drawing thread.
    struct Staged {
        std::vector<scene::Image> images;
        // Or, when the scene stays put until it is uploaded (see uploadShared), its own images, without a copy.
        const std::vector<scene::Image>* borrowed = nullptr;
        [[nodiscard]] const std::vector<scene::Image>& imageList() const { return borrowed != nullptr ? *borrowed : images; }
        std::set<std::size_t> noMips;  // light maps, drawn without smaller levels
        std::vector<GpuMesh> meshes;
        MeshData meshData;
        std::vector<scene::Image> skybox;
        std::vector<std::vector<scene::Image>> envCubes;
        std::vector<int> lightMaps;  // the images that light floors and walls
        std::vector<bool> needed;    // the images something is drawn with (see markNeeded)
        std::optional<scene::PaintedTerrain> terrain;
        std::optional<scene::Scene::BoundaryClip> clip;
    };
    // A scene prepared: staged until swap uploads it, then on the GPU.
    struct GpuPrepared final : PreparedBase {
        ~GpuPrepared() override { release(); }
        void releaseSome(std::size_t textures) override;
        void release();
        std::shared_ptr<Graveyard> graveyard;
        std::unique_ptr<Staged> staged;
        std::unique_ptr<SceneGpu> gpu;
    };

    // A shader program and where its inputs are.
    struct Program {
        GLuint id = 0;
        GLint vertex = -1, fragment = -1;  // the vertex and fragment shaders' constants
    };

    // Contexts sharing objects with the drawing one, for prepare() to upload in on other threads.
    struct SharedContext {
        platform::GlContextPtr context;
        bool busy = false;
    };
    void makeSharedContexts(unsigned count);
    void destroySharedContexts();
    // Uploads `scene` in a shared context. Null when there is none to be had (the caller stages it instead).
    std::unique_ptr<SceneGpu> uploadShared(const scene::Scene& scene) const;
    bool onDrawingThread() const { return std::this_thread::get_id() == drawingThread_; }
    // Forgets what is bound, which an upload on the drawing thread changes (another thread has its own context).
    void forgetBindings();

    // Gathering and uploading (gl_upload.cpp).
    // `copyImages` copies the scene's images into it; without, it points at them, and must be uploaded before the scene goes.
    static std::unique_ptr<Staged> stage(const scene::Scene& scene, bool copyImages = true);
    void uploadStaged(SceneGpu& gpu, Staged& staged);
    Texture uploadTexture(const scene::Image& image, bool mips, bool anisotropic = true);
    Texture uploadRaw(unsigned width, unsigned height, const std::uint8_t* rgba, bool nearest);
    GLuint uploadBuffer(GLenum target, const void* data, std::size_t bytes);
    // Without shaders: uploads the vertices (`stride` floats each) as one block of tightly packed values
    // for each attribute, the attributes given as their first float and how many, one after the other. The
    // arrays then have no stride, as the consoles' OpenGL libraries want.
    GLuint uploadBlocks(const float* data, std::size_t count, std::size_t stride, std::initializer_list<std::array<std::size_t, 2>> attributes);
    // Uploads indices, as 16-bit where they all fit and the renderer has no shaders; `wide` says whether they are 32-bit.
    GLuint uploadIndices(const std::vector<std::uint32_t>& indices, bool& wide);
    void uploadTerrain(SceneGpu& gpu, const scene::PaintedTerrain& terrain);
    Texture uploadCube(const std::vector<scene::Image>& faces, bool mips = false);
    MeshBuffers uploadMeshBuffers(MeshData& data);
    void release(Texture& texture);
    void releaseBuffer(GLuint& buffer);
    void collectGarbage();

    // Programs (gl_shaders.cpp).
    Program loadProgram(const std::filesystem::path& folder, const char* vertex, const char* fragment, const char* vertexBlock,
                        const char* fragmentBlock, std::initializer_list<const char*> samplers);
    GLuint compile(GLenum stage, const std::filesystem::path& file);
    void makePrograms(const std::filesystem::path& folder);

    // Frames (gl_draw.cpp).
    [[nodiscard]] bool render(unsigned width, unsigned height, const Camera& camera);
    void drawList(const FrameView& f, const std::vector<Draw>& list, int blend);
    void drawSprites(unsigned width, unsigned height);
    // Drawing without shaders (gl_fixed.cpp).
    void drawListFixed(const FrameView& f, const std::vector<Draw>& list, int blend);
    // The fixed pipeline's calls, each checked against the console subset first when that is on (see subset_).
    void fxEnable(GLenum capability);
    void fxBlendFunc(GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha);
    void fxAlphaFunc(GLenum func, GLclampf reference);
    void fxTexEnv(GLint mode);
    void fxDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices);
    void fxVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void fxColorPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void fxTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
    void checkTexture(const char* what, GLenum target, unsigned width, unsigned height) const {
        if (subset_) {
            subset_->texture(what, target, width, height, sixteenBit_ ? 2 : 4);
        }
    }
    // Without shaders: uploads the six pictures of a sky, which is drawn as a box.
    void uploadSkyFaces(std::array<Texture, 6>& faces, const std::vector<scene::Image>& images);
    // The internal format of texels: 8 bits a channel, or 16 bits a texel (4-4-4-4 where any is see-through, else 5-5-5)
    // with NEOTS2_GL1_TEXTURE_BITS=16, as the consoles keep them.
    // A tiny white stand-in for a light map's texture, which only needs to exist while the light is taken at the vertices.
    Texture lightPlaceholder();
    [[nodiscard]] GLint internalFormatFor(const std::uint8_t* rgba, std::size_t texels) const;
    // The memory texels take, in `bits` a texel (0: as the renderer keeps them), with the smaller levels, and a palette's
    // colours for 8 bits or fewer.
    [[nodiscard]] std::size_t bytesFor(std::size_t texels, bool mips, unsigned bits = 0) const {
        const std::size_t perTexel = bits != 0 ? bits : sixteenBit_ ? 16 : 32;
        return texels * perTexel / 8 * (mips ? 4 : 3) / 3 + (perTexel <= 8 ? (std::size_t{1} << perTexel) * 4 : 0);
    }
    // Tells the strict mode how much texture memory is in use now.
    void reportResident();
    // Tells (in the log, with the strict mode) what a scene's images were: how many, how big, and which were ever drawn.
    void reportScene(const SceneGpu& gpu, const char* when) const;
    // Strict mode: notes how many pixels one repeat of `image` covers in this draw, across and down.
    // The texels this draw asks of the image across and down: the pixels one repeat covers times `scale`, no more than `cap`.
    // Kept as the most any draw asked, for the report.
    std::optional<std::array<float, 2>> noteDensity(const GpuPrimitive& primitive, const MeshBuffers& buffers, const Mat4& clip, int image, float scale,
                                                    float cap);
    // With NEOTS2_GL1_TEXTURE_DETAIL: remakes the texture of `image` bigger if a draw asks for more `texels` (across and
    // down) than it has. A few a frame, so a view of new things doesn't stall.
    void growTexture(int image, const std::array<float, 2>& texels);
    // Uploads an image at its starting size and keeps it, to grow from (see growTexture).
    Texture uploadStarter(SceneGpu& gpu, int index, scene::Image image);
    // Without shaders the images drawn with are the materials' own and the floors' and walls' light maps; the normal
    // maps and the like are never looked at, so they are not uploaded. Marks those in `needed`.
    static void markNeeded(const scene::Scene& scene, std::vector<bool>& needed);
    void beginFixedSprites(unsigned width, unsigned height);
    void fixedSpritePointers(std::size_t vertices);
    void fxTexEnvScale(int scale);
    void endFixed();
    void setClientArrays(unsigned mask);
    void fixedTextureUnits(unsigned units);
    void fixedWorldMatrices(const FrameView& f);
    void drawSkyFixed(const FrameView& f);
    void drawBoundaryFixed(const FrameView& f);
    void drawTerrainFixed(const FrameView& f);
    void drawRoadsFixed(const FrameView& f);
    void drawImpostersFixed(const FrameView& f);
    void drawWaterFixed(const FrameView& f);
    void bindTexture(unsigned unit, Texture& texture, GLint wrap, bool nearest = false);
    void drawSky(const FrameView& f, const Texture& cube, float up);
    void drawBoundary(const FrameView& f);
    void drawTerrain(const FrameView& f, const Mat4& viewProjection, bool reflected);
    void drawWater(const FrameView& f, unsigned width, unsigned height);
    // The water's reflection: the sky and the terrain above the water, mirrored in it.
    [[nodiscard]] bool reflectionReady(unsigned width, unsigned height);
    void drawReflection(const FrameView& f, unsigned width, unsigned height);
    void releaseReflection();
    void bindFramebuffer(GLuint framebuffer);
    void drawRoads(const FrameView& f);
    void drawImposters(const FrameView& f);
    // A draw's state: how it blends, whether it tests and writes depth (and how), and what it writes.
    struct State {
        int blend = 0;  // 0 none, 1 over, 2 add, 3 imposter light (multiply), 4 premultiplied over, 5 sum
        bool cull = false;
        bool depthTest = true;
        bool depthWrite = true;
        GLenum depthFunc = GL_GEQUAL;
        bool colorWrite = true;
        bool operator==(const State&) const = default;
    };
    void setState(const State& state);
    void attributes(unsigned location, int size, GLsizei stride, std::size_t offset);
    Texture& textureFor(const TextureRef& ref);
    void setAttributes(unsigned mask);
    void useProgram(const Program& program);
    // Poses a skinned primitive's vertices on the CPU into the streaming buffer.
    void poseSkinned(const GpuPrimitive& p, const MeshBuffers& buffers, std::size_t instance);
    [[nodiscard]] bool drawOffscreen(const Camera& camera, unsigned width, unsigned height, scene::Image& out);
    // Draws one frame into the off-screen target (made at that size), left bound.
    [[nodiscard]] bool renderToTarget(const Camera& camera, unsigned width, unsigned height);
    void bindWindow();
    void invalidateState();
    void releaseTarget();

    bool fixed_ = false;  // draws without shaders
    // With NEOTS2_GL1_STRICT set (and no shaders), what the console OpenGL libraries can't do is reported as it is used.
    std::unique_ptr<GlSubset> subset_;
    platform::Window* window_ = nullptr;
    platform::GlContextPtr context_;
    std::thread::id drawingThread_;
    mutable std::mutex poolLock_;
    mutable std::condition_variable poolFree_;
    mutable std::vector<SharedContext> pool_;
    bool closing_ = false;  // the pool is being destroyed (under poolLock_): no context is lent any more
    gl::Api gl_;
    std::shared_ptr<Graveyard> graveyard_ = std::make_shared<Graveyard>();
    std::string vendor_, renderer_, version_;

    Program scene_, ui_, terrain_, road_, imposter_, sky_, clip_, water_;
    GLuint skyVertices_ = 0;  // the corners of the sky's triangle (without shaders: the sky cube's triangles)
    GLuint streamVertices_ = 0;  // a posed skinned primitive's vertices
    GLuint colorStream_ = 0;     // the colours a draw's vertices are lit to, without shaders
    std::vector<std::uint8_t> litColors_;
    std::vector<float> positions_, blocks_;  // a posed primitive's positions alone, and the sprites' in blocks, on their way to the GPU
    bool texEnvScaled_ = false;              // the texture environment multiplies by more than 1
    // Without shaders: the sides textures are put to (see texture_fit.hpp), NEOTS2_GL1_MAX_TEXTURE to try smaller ones, and
    // the share of its texture each sprite fills, by sprite.
    TextureLimits limits_, spriteLimits_;
    unsigned consoleWidth_ = 640;          // how wide the console's screen is, for measuring how big textures need to be
    unsigned viewWidth_ = 0, viewHeight_ = 0;  // the size of the frame being drawn
    float objectSide_ = 256, simSide_ = 512;  // and the same for things and for Sims (NEOTS2_GL1_OBJECT_SIDE, NEOTS2_GL1_SIM_SIDE)
    unsigned paletteBits_ = 0;                // NEOTS2_GL1_PALETTE: 4 or 8, to see textures with only 16 or 256 colours (0: all of them)
    bool paletteAuto_ = false;                // or "auto": each texture 4-bit if that shows little (see paletteError_), else 8-bit if that does, else not at all
    float paletteError_ = 8;                  // NEOTS2_GL1_PALETTE_ERROR: how much a palette may change a texture (RMS levels) in "auto"; half that for 8 bits
    bool dither_ = false;                     // NEOTS2_GL1_DITHER: palettes with dithering
    float surfaceSide_ = 128;  // the most texels a floor, wall, roof or the ground around a lot is given across (NEOTS2_GL1_SURFACE_SIDE)
    float detail_ = 0;      // NEOTS2_GL1_TEXTURE_DETAIL: texels for each pixel the screen needs (0: textures are made as they are)
    unsigned grown_ = 0;    // textures remade bigger in this frame
    bool sixteenBit_ = false;
    std::vector<std::array<float, 2>> spriteFill_;
    unsigned clientArrays_ = 0;  // which vertex arrays are enabled, without shaders (see setClientArrays)
    unsigned textureUnits_ = 1;  // how many texture units are on, without shaders (see fixedTextureUnits)
    bool singleTextureTerrain_ = true;  // see setSingleTextureTerrain
    bool overbright_ = true;            // see setOverbrightLight
    bool vertexLightMaps_ = true;       // see setVertexLightMaps
    GLuint spriteVertices_ = 0;
    std::size_t streamCapacity_ = 0, spriteCapacity_ = 0;  // bytes

    std::vector<Texture> spriteTextures_;
    std::vector<float> scratch_;  // what is sent to a streaming buffer
    std::vector<std::array<unsigned, 2>> spriteSizes_;
    void releaseSprites();

    // What is set in the GL state now, so that draws only change what they must.
    GLuint program_ = 0;
    unsigned attributeMask_ = 0;
    std::optional<State> state_;
    GLuint arrayBuffer_ = 0, elementBuffer_ = 0;
    std::array<std::pair<GLuint, GLint>, 8> bound_{};  // texture and how it is sampled (wrap, plus 0x10000 unfiltered) by unit

    // The off-screen target of drawOffscreen, remade when its size changes.
    GLuint targetFramebuffer_ = 0, targetColor_ = 0, targetDepth_ = 0;
    unsigned targetWidth_ = 0, targetHeight_ = 0;
    // The water's reflection, the size of the target drawn to.
    GLuint reflectionFramebuffer_ = 0, reflectionDepth_ = 0;
    Texture reflection_;
    unsigned reflectionWidth_ = 0, reflectionHeight_ = 0;
    GLuint framebuffer_ = 0;  // what is drawn to: 0 for the window
    static constexpr unsigned kMaxOffscreen = 8192;

    std::unique_ptr<SceneGpu> gpu_ = std::make_unique<SceneGpu>();
};

} // namespace ts2::render
