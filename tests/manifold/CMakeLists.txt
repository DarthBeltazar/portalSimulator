add_executable(manifold_tests
    test_se3_closed_loop.cpp
    test_traverse.cpp
)
target_link_libraries(manifold_tests PRIVATE manifold_core Catch2::Catch2WithMain)

# Must run before catch_discover_tests below: discovery invokes the built exe as part of
# its own POST_BUILD step to enumerate tests, and POST_BUILD commands run in registration
# order — if the ASan DLL copy were registered second, discovery would try to launch the
# exe before the DLL is in place and fail with STATUS_DLL_NOT_FOUND.
copy_asan_runtime_dll(manifold_tests)

include(Catch)
catch_discover_tests(manifold_tests)
