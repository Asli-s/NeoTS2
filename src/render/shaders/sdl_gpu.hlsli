// Vulkan has no separate textures and samplers in SDL_GPU: each pair is one combined image sampler
// (the texture and sampler at the same register number share a binding).
#ifdef __spirv__
#define COMBINED [[vk::combinedImageSampler]]
#else
#define COMBINED
#endif
