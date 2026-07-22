# Phase 2 render module — Embree CPU path (docs/phase2-rendering.md §7 step 5).
add_library(render_core STATIC
    camera.cpp
    scene.cpp
    renderer.cpp
)
target_include_directories(render_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(render_core PUBLIC manifold_core Eigen3::Eigen embree)
