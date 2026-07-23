# Phase 2 render module — Embree CPU path (docs/phase2-rendering.md §7 step 5).
add_library(render_core STATIC
    camera.cpp
    scene.cpp
    renderer.cpp
)
target_include_directories(render_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(render_core PUBLIC manifold_core Eigen3::Eigen embree TBB::tbb)

# Vulkan RT sub-step (docs/phase2-rendering.md §7 step 8): headless compute-only GPU path, kept
# as its own target rather than folded into render_core — a separate concern (GPU cross-check,
# not the CPU reference), and keeps Embree's ASan-gate scoping (DECISIONS.md #0005) from having
# to reason about an unrelated dependency set too.
add_library(render_vulkan STATIC
    vulkan_context.cpp
    vma_impl.cpp
    portal_hop_gpu.cpp
    portal_traverse_gpu.cpp
    acceleration_structure.cpp
    triangle_query_gpu.cpp
    full_scene_gpu.cpp
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

set(PORTAL_SHADER_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/shaders/portal_common.slangh)

# portal_hop.slang (docs/DECISIONS.md #0007): the single-hop differential test's GPU-side port of
# manifold::stepThroughNearestPortal. sm_6_0 is enough here — this shader is plain arithmetic,
# no RayQuery<> (that lands with the full scene shader once acceleration structures exist).
compile_slang_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/portal_hop.slang main sm_6_0 PORTAL_HOP_SPV ${PORTAL_SHADER_COMMON})
add_custom_target(portal_hop_shader DEPENDS ${PORTAL_HOP_SPV})
add_dependencies(render_vulkan portal_hop_shader)
target_compile_definitions(render_vulkan PUBLIC PORTAL_HOP_SPV_PATH="${PORTAL_HOP_SPV}")

# portal_traverse.slang: the multi-hop differential test's GPU-side port of
# manifold::detail::traverseImpl, validating the accumulated-transform composition math that
# portal_hop.slang alone never exercises (docs/DECISIONS.md #0007's follow-up note).
compile_slang_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/portal_traverse.slang main sm_6_0 PORTAL_TRAVERSE_SPV ${PORTAL_SHADER_COMMON})
add_custom_target(portal_traverse_shader DEPENDS ${PORTAL_TRAVERSE_SPV})
add_dependencies(render_vulkan portal_traverse_shader)
target_compile_definitions(render_vulkan PUBLIC PORTAL_TRAVERSE_SPV_PATH="${PORTAL_TRAVERSE_SPV}")

# triangle_query.slang (docs/phase2-rendering.md §7 step 8's "triangle in a TLAS, ray-queried"
# milestone): sm_6_5, not sm_6_0 like the plain-arithmetic portal shaders -- inline ray tracing's
# RayQuery<> type requires it (confirmed empirically with slangc + spirv-val before writing the
# host-side acceleration_structure.cpp).
compile_slang_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle_query.slang main sm_6_5 TRIANGLE_QUERY_SPV)
add_custom_target(triangle_query_shader DEPENDS ${TRIANGLE_QUERY_SPV})
add_dependencies(render_vulkan triangle_query_shader)
target_compile_definitions(render_vulkan PUBLIC TRIANGLE_QUERY_SPV_PATH="${TRIANGLE_QUERY_SPV}")

# full_scene.slang (docs/phase2-rendering.md §7 step 8's remaining bullet): the full-scene shader
# combining the portal-hop loop with scene-geometry ray query -- the one shader needing both
# sm_6_5 (RayQuery<>, DECISIONS.md #0007) and portal_common.slangh (DECISIONS.md #0006/#0007's
# shared disk-crossing math), so unlike triangle_query.slang above it takes the common-header
# dependency too.
compile_slang_shader(${CMAKE_CURRENT_SOURCE_DIR}/shaders/full_scene.slang main sm_6_5 FULL_SCENE_SPV ${PORTAL_SHADER_COMMON})
add_custom_target(full_scene_shader DEPENDS ${FULL_SCENE_SPV})
add_dependencies(render_vulkan full_scene_shader)
target_compile_definitions(render_vulkan PUBLIC FULL_SCENE_SPV_PATH="${FULL_SCENE_SPV}")
