// What the SPIR-V build path needs from the shaders. Both macros are empty for Direct3D 12.

// A texture and its sampler at the same register number are one combined image sampler.
#if defined(__spirv__) || defined(NEOTS2_SPIRV)
#define COMBINED [[vk::combinedImageSampler]]
#else
#define COMBINED
#endif

// A vertex input read from the attribute at this location, which must match its TEXCOORD number.
// Without it Vulkan numbers the inputs in the order they are declared.
#if defined(__spirv__) || defined(NEOTS2_SPIRV)
#define LOCATION(n) [[vk::location(n)]]
#else
#define LOCATION(n)
#endif
