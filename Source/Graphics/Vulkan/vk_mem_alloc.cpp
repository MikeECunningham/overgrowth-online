//-----------------------------------------------------------------------------
//           Name: vk_mem_alloc.cpp
//      Developer: Overgrowth Online (crxhvrd)
//    Description: The Vulkan Memory Allocator's implementation (VMA 3.3.0,
//                 MIT, Libraries/vma-3.3.0). Its Vulkan functions come from
//                 volk (vmaImportVulkanFunctionsFromVolk), never the loader's
//                 exports: vulkan-1.dll is loaded at run time
//        License: Apache-2.0, like the rest of the engine (see LICENSE)
//-----------------------------------------------------------------------------
#include <volk.h>

#if defined(__linux__)
#include <cstdlib>

// VMA's default allocator assumes C++17 when libstdc++ advertises aligned_alloc,
// even if the application is compiled as C++14. Use the POSIX allocator directly;
// unlike aligned_alloc it does not require size to be a multiple of alignment.
static void* OgVmaAlignedAlloc(size_t size, size_t alignment) {
    if (alignment < sizeof(void*)) {
        alignment = sizeof(void*);
    }
    void* allocation = NULL;
    return posix_memalign(&allocation, alignment, size) == 0 ? allocation : NULL;
}
#define VMA_SYSTEM_ALIGNED_MALLOC(size, alignment) OgVmaAlignedAlloc((size), (alignment))
#define VMA_SYSTEM_ALIGNED_FREE(ptr) free(ptr)
#endif

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#define VMA_IMPLEMENTATION
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
