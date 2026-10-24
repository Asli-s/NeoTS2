#include "render/gl_subset.hpp"

#include <algorithm>
#include <array>
#include <format>

namespace ts2::render {

namespace {

// OpenGL's enums that the rules use.
constexpr unsigned kZero = 0, kOne = 1, kSrcColor = 0x300, kOneMinusSrcColor = 0x301, kSrcAlpha = 0x302, kOneMinusSrcAlpha = 0x303,
                   kDstAlpha = 0x304, kOneMinusDstAlpha = 0x305, kDstColor = 0x306, kOneMinusDstColor = 0x307, kSrcAlphaSaturate = 0x308;
constexpr unsigned kGreater = 0x204, kReplace = 0x1E01, kModulate = 0x2100, kDecal = 0x2101;
constexpr unsigned kTexture2d = 0xDE1, kCullFace = 0xB44, kDepthTest = 0xB71, kBlend = 0xBE2, kAlphaTest = 0xBC0, kScissorTest = 0xC11,
                   kTextureCubeMap = 0x8513;
constexpr unsigned kUnsignedByte = 0x1401, kUnsignedShort = 0x1403, kFloat = 0x1406;

std::string nameOf(unsigned value) {
    static constexpr std::array<std::pair<unsigned, const char*>, 24> names{{
        {kZero, "GL_ZERO"}, {kOne, "GL_ONE"}, {kSrcColor, "GL_SRC_COLOR"}, {kOneMinusSrcColor, "GL_ONE_MINUS_SRC_COLOR"},
        {kSrcAlpha, "GL_SRC_ALPHA"}, {kOneMinusSrcAlpha, "GL_ONE_MINUS_SRC_ALPHA"}, {kDstAlpha, "GL_DST_ALPHA"},
        {kOneMinusDstAlpha, "GL_ONE_MINUS_DST_ALPHA"}, {kDstColor, "GL_DST_COLOR"}, {kOneMinusDstColor, "GL_ONE_MINUS_DST_COLOR"},
        {kSrcAlphaSaturate, "GL_SRC_ALPHA_SATURATE"}, {kGreater, "GL_GREATER"}, {kReplace, "GL_REPLACE"}, {kModulate, "GL_MODULATE"},
        {kDecal, "GL_DECAL"}, {kTexture2d, "GL_TEXTURE_2D"}, {kCullFace, "GL_CULL_FACE"}, {kDepthTest, "GL_DEPTH_TEST"},
        {kBlend, "GL_BLEND"}, {kAlphaTest, "GL_ALPHA_TEST"}, {kScissorTest, "GL_SCISSOR_TEST"}, {kTextureCubeMap, "GL_TEXTURE_CUBE_MAP"},
        {kUnsignedShort, "GL_UNSIGNED_SHORT"}, {kFloat, "GL_FLOAT"},
    }};
    for (const auto& [number, name] : names) {
        if (number == value) {
            return name;
        }
    }
    return std::format("0x{:X}", value);
}

bool oneOf(unsigned value, std::initializer_list<unsigned> list) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

void GlSubset::violate(unsigned targets, const std::string& call, const std::string& what) {
    const std::string who = targets == Both ? "GLdc and ps2gl" : targets == GLdc ? "GLdc" : "ps2gl";
    const std::string text = std::format("OpenGL 1 subset: {}: {} (not in {})", call, what, who);
    {
        std::lock_guard lock(lock_);
        if (!seen_.insert(text).second) {
            return;
        }
    }
    if (report_) {
        report_(text);
    }
}

std::size_t GlSubset::violations() const {
    std::lock_guard lock(lock_);
    return seen_.size();
}

void GlSubset::blend(unsigned srcRgb, unsigned dstRgb, unsigned srcAlpha, unsigned dstAlpha) {
    const std::string call = std::format("glBlendFuncSeparate({}, {}, {}, {})", nameOf(srcRgb), nameOf(dstRgb), nameOf(srcAlpha), nameOf(dstAlpha));
    // GLdc has only glBlendFunc, and the GS has one blend function for colour and alpha.
    if (srcAlpha != srcRgb || dstAlpha != dstRgb) {
        violate(Both, call, "separate factors for alpha; the framebuffer's alpha is left as the colour blend makes it");
    }
    // GLdc: the PVR has no GL_SRC_COLOR source or GL_DST_COLOR destination factor, nor saturate.
    if (!oneOf(srcRgb, {kZero, kOne, kSrcAlpha, kDstColor, kDstAlpha, kOneMinusDstColor, kOneMinusSrcAlpha, kOneMinusDstAlpha})) {
        violate(GLdc, call, std::format("{} is not a source factor", nameOf(srcRgb)));
    }
    if (!oneOf(dstRgb, {kZero, kOne, kSrcAlpha, kSrcColor, kDstAlpha, kOneMinusSrcColor, kOneMinusSrcAlpha, kOneMinusDstAlpha})) {
        violate(GLdc, call, std::format("{} is not a destination factor", nameOf(dstRgb)));
    }
    // ps2gl: three combinations only. The GS blends (A-B)*C+D with C an alpha, so no colour by colour at all.
    const bool known = (srcRgb == kSrcAlpha && dstRgb == kOneMinusSrcAlpha) || (srcRgb == kSrcAlpha && dstRgb == kOne) ||
                       (srcRgb == kOneMinusSrcAlpha && dstRgb == kOne);
    if (!known) {
        violate(Ps2gl, call, "ps2gl blends only SRC_ALPHA/ONE_MINUS_SRC_ALPHA, SRC_ALPHA/ONE and ONE_MINUS_SRC_ALPHA/ONE");
    }
}

void GlSubset::alphaFunc(unsigned func) {
    if (func != kGreater) {
        violate(GLdc, std::format("glAlphaFunc({})", nameOf(func)), "GLdc tests alpha with GL_GREATER only");
    }
}

void GlSubset::texEnv(unsigned mode) {
    if (!oneOf(mode, {kModulate, kDecal, kReplace})) {
        violate(Both, std::format("glTexEnv({})", nameOf(mode)), "only GL_MODULATE, GL_DECAL and GL_REPLACE");
    }
}

void GlSubset::enable(unsigned capability) {
    const std::string call = std::format("glEnable({})", nameOf(capability));
    if (capability == kScissorTest) {
        violate(Ps2gl, call, "ps2gl doesn't take it");
    } else if (!oneOf(capability, {kTexture2d, kCullFace, kDepthTest, kBlend, kAlphaTest})) {
        violate(Both, call, "an unsupported capability (GLdc raises an error, ps2gl says it isn't implemented)");
    }
}

void GlSubset::colorMaskOff() {
    violate(GLdc, "glColorMask", "GLdc ignores it, so a draw meant for depth alone paints");
}

void GlSubset::drawElements(unsigned indexType) {
    violate(Ps2gl, "glDrawElements", "ps2gl's is a placeholder that raises an error; draw with glDrawArrays or a renderer of its own");
    if (indexType != kUnsignedShort) {
        violate(Both, std::format("glDrawElements({})", nameOf(indexType)), "the consoles want 16-bit indices");
    }
}

void GlSubset::array(const char* name, int size, unsigned type, int stride) {
    const int bytes = size * (type == kUnsignedByte ? 1 : 4);
    if (stride != 0 && stride != bytes) {
        violate(Ps2gl, std::format("gl{}Pointer", name), "ps2gl's arrays can't be strided (one array for each attribute)");
    }
}

void GlSubset::textureUnits(unsigned count) {
    if (count > 1) {
        violate(Both, std::format("{} texture units", count), "GLdc doesn't submit a second texture and the GS has one a pass");
    }
}

void GlSubset::texture(unsigned target, unsigned width, unsigned height) {
    const std::string call = std::format("texture {}x{}", width, height);
    if (target != kTexture2d) {
        violate(Both, std::format("texture target {}", nameOf(target)), "only 2D textures (no cube maps)");
        return;
    }
    const auto isPowerOfTwo = [](unsigned v) { return v != 0 && (v & (v - 1)) == 0; };
    if (!isPowerOfTwo(width) || !isPowerOfTwo(height)) {
        violate(Both, call, "sides must be powers of two");
    }
    if (width > 1024 || height > 1024) {
        violate(Both, call, "1024 is the largest side");
    }
    if (width < 8 || height < 8) {
        violate(GLdc, call, "GLdc's smallest side is 8");
    }
}

} // namespace ts2::render
