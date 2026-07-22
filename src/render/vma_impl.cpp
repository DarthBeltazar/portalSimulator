// VMA is a header-only library gated by VMA_IMPLEMENTATION: exactly one translation unit must
// define it before including vk_mem_alloc.h. See vulkan_context.hpp for why
// VMA_STATIC_VULKAN_FUNCTIONS/VMA_DYNAMIC_VULKAN_FUNCTIONS are set the way they are (must match
// here, since this is the first inclusion of the header in the program).
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
