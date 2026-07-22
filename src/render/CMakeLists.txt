# Phase 2 render module — Embree CPU path (docs/phase2-rendering.md §7 step 5).
add_library(render_core STATIC
    camera.cpp
    scene.cpp
    renderer.cpp
)
target_include_directories(render_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(render_core PUBLIC manifold_core Eigen3::Eigen embree)

# Vulkan RT sub-step (docs/phase2-rendering.md §7 step 8): headless compute-only GPU path, kept
# as its own target rather than folded into render_core — a separate concern (GPU cross-check,
# not the CPU reference), and keeps Embree's ASan-gate scoping (DECISIONS.md #0005) from having
# to reason about an unrelated dependency set too.
add_library(render_vulkan STATIC
    vulkan_context.cpp
    vma_impl.cpp
)
target_include_directories(render_vulkan PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(render_vulkan PUBLIC
    manifold_core
    Eigen3::Eigen
    Vulkan::Headers
    volk::volk
    GPUOpen::VulkanMemoryAllocator
    vk-bootstrap::vk-bootstrap
)
