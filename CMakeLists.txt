cmake_minimum_required(VERSION 3.28)
project(portalSimulator CXX)

find_package(Eigen3 CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)
find_package(rapidcheck CONFIG REQUIRED)
find_package(embree CONFIG REQUIRED)
find_package(Vulkan REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(vk-bootstrap CONFIG REQUIRED)

enable_testing()

# Offline Slang -> SPIR-V compilation (docs/phase2-rendering.md §7 step 8). vcpkg's
# shader-slang port installs slangc.exe as a host tool alongside the library, on
# CMAKE_PROGRAM_PATH via the vcpkg toolchain file, so a plain find_program locates it without
# needing the system Vulkan SDK.
find_program(SLANGC_EXECUTABLE NAMES slangc REQUIRED)

# Compiles a .slang shader to SPIR-V at build time and returns the output path in OUT_VAR.
# Mirrors this project's other build-time codegen (Catch2 test discovery, ASan DLL copying):
# a named custom command, not a hidden step, so `ninja -t targets` shows it.
function(compile_slang_shader source_file entry_point profile out_var)
  get_filename_component(_name ${source_file} NAME_WE)
  set(_spv "${CMAKE_CURRENT_BINARY_DIR}/${_name}.spv")
  add_custom_command(
    OUTPUT "${_spv}"
    COMMAND "${SLANGC_EXECUTABLE}" "${source_file}" -profile "${profile}" -target spirv
            -entry "${entry_point}" -capability spvRayQueryKHR -o "${_spv}"
    DEPENDS "${source_file}"
    COMMENT "Compiling ${source_file} -> ${_spv} (Slang)"
    VERBATIM
  )
  set(${out_var} "${_spv}" PARENT_SCOPE)
endfunction()

# Whether this configure is the clang-cl ASan+UBSan preset. Shared by copy_asan_runtime_dll()
# below and by tests/render/CMakeLists.txt, which uses it to decide whether a Catch2 binary
# that links Embree/TBB prebuilt (non-instrumented) binaries can safely be run under ctest —
# see DECISIONS.md #0005.
set(PORTAL_SIM_ASAN_ACTIVE FALSE)
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"
   AND CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
  set(PORTAL_SIM_ASAN_ACTIVE TRUE)
endif()

# The ASan runtime DLL isn't on PATH by default, so instrumented binaries fail to launch
# ("cannot open shared object file: clang_rt.asan_dynamic-x86_64.dll") unless it's found
# next to the executable or on PATH. Copy it alongside each target that needs it instead
# of requiring every dev/CI invocation to fix up PATH manually. Resolved dynamically via
# clang-cl itself, same as the linker flags in cmake/clang-cl-asan-linker-flags.cmake.
function(copy_asan_runtime_dll target)
  if(PORTAL_SIM_ASAN_ACTIVE)
    find_program(_clang_cl_exe NAMES clang-cl)
    execute_process(
      COMMAND "${_clang_cl_exe}" /clang:-print-resource-dir
      OUTPUT_VARIABLE _clang_resource_dir
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(_asan_dll "${_clang_resource_dir}/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
    if(EXISTS "${_asan_dll}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_asan_dll}" "$<TARGET_FILE_DIR:${target}>"
      )
    endif()
  endif()
endfunction()

add_executable(portalSimulator main.cpp)
target_link_libraries(portalSimulator PRIVATE Eigen3::Eigen)
copy_asan_runtime_dll(portalSimulator)

add_subdirectory(src/manifold)
add_subdirectory(src/render)
add_subdirectory(tests/manifold)
add_subdirectory(tests/render)
