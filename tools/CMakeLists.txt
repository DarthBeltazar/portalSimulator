# Visual demo tools (target repo layout's /tools/: "scene generators, visualizers, reference
# comparisons", CLAUDE.md). Not part of the test suite -- run manually to look at a portal scene.
add_executable(demo_scene
    demo_scene.cpp
    demo_scene_common.cpp
    image_io.cpp
)
target_link_libraries(demo_scene PRIVATE render_core render_vulkan manifold_core)

copy_asan_runtime_dll(demo_scene)

# Real-time fly-through viewer (interactive_viewer.cpp): Win32 + GDI only, no new dependency --
# Windows-specific, so only built there.
if(WIN32)
  add_executable(interactive_viewer
      interactive_viewer.cpp
      demo_scene_common.cpp
  )
  target_link_libraries(interactive_viewer PRIVATE render_core manifold_core user32 gdi32)

  copy_asan_runtime_dll(interactive_viewer)

  # GPU counterpart (docs/DECISIONS.md #0011): same window/input shell, dispatches
  # render::PersistentGpuRenderer (Vulkan RT) per frame instead of render::renderEmbree.
  add_executable(interactive_viewer_gpu
      interactive_viewer_gpu.cpp
      demo_scene_common.cpp
  )
  target_link_libraries(interactive_viewer_gpu PRIVATE render_core render_vulkan manifold_core user32 gdi32)

  copy_asan_runtime_dll(interactive_viewer_gpu)
endif()
