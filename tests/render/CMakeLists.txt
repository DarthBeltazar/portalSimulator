add_executable(render_tests
    test_corridor_brightness.cpp
    test_corridor_render.cpp
    test_vulkan_context_smoke.cpp
    test_portal_hop_gpu_differential.cpp
    test_portal_traverse_gpu_differential.cpp
    test_acceleration_structure_smoke.cpp
)
target_link_libraries(render_tests PRIVATE render_core render_vulkan manifold_core Catch2::Catch2WithMain)

copy_asan_runtime_dll(render_tests)

include(Catch)
catch_discover_tests(render_tests)

# test_shadow_through_portal.cpp is the one test that actually drives render::Scene/renderEmbree
# end to end, which pulls in Embree/TBB's prebuilt (non-ASan-instrumented) binaries. Under
# clang-cl-sanitize this aborts with an ASan bad-free during TBB/CRT static-teardown before any
# TEST_CASE body runs -- even Catch2's own --list-tests discovery crashes -- a toolchain/allocator
# mismatch between the instrumented host process and Embree/TBB's plain-MSVC-built binaries, not
# a first-party bug (see DECISIONS.md #0005). Build this target under every preset, so it's still
# compile-checked, but only register it with ctest -- which invokes the binary to discover tests --
# under a toolchain where that discovery is known not to crash.
add_executable(render_tests_embree
    test_shadow_through_portal.cpp
)
target_link_libraries(render_tests_embree PRIVATE render_core manifold_core Catch2::Catch2WithMain)

copy_asan_runtime_dll(render_tests_embree)

if(NOT PORTAL_SIM_ASAN_ACTIVE)
  catch_discover_tests(render_tests_embree)
endif()

# test_gpu_vs_embree.cpp (docs/phase2-rendering.md §7 step 8's final bullet, criterion 3) drives
# both render_core's renderEmbree and render_vulkan's renderVulkan on the same scene, so it links
# both -- same Embree/TBB-teardown ASan incompatibility as render_tests_embree above (DECISIONS.md
# #0005), so it gets the identical build-everywhere/discover-only-under-msvc-debug scoping.
add_executable(render_tests_gpu_vs_embree
    test_gpu_vs_embree.cpp
)
target_link_libraries(render_tests_gpu_vs_embree PRIVATE render_core render_vulkan manifold_core Catch2::Catch2WithMain)

copy_asan_runtime_dll(render_tests_gpu_vs_embree)

if(NOT PORTAL_SIM_ASAN_ACTIVE)
  catch_discover_tests(render_tests_gpu_vs_embree)
endif()
