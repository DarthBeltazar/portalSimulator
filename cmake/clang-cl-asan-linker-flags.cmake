# Chainloaded via VCPKG_CHAINLOAD_TOOLCHAIN_FILE by the clang-cl-sanitize preset.
#
# clang-cl's own driver auto-links the ASan/UBSan runtime when it does the linking itself,
# but CMake+Ninja invokes lld-link directly for the final link step, bypassing that — the
# link fails with "undefined symbol: __asan_init" otherwise. This must run as part of
# toolchain-file processing (not from CMakeLists.txt) because CMake's internal
# compiler-works sanity check performs a full link at project()-time, before anything in
# CMakeLists.txt executes.
#
# The runtime lib directory is resolved from clang-cl itself via -print-resource-dir, so
# this keeps working across LLVM upgrades without a hardcoded version number.

find_program(_clang_cl_exe NAMES clang-cl)
if(_clang_cl_exe)
  execute_process(
    COMMAND "${_clang_cl_exe}" /clang:-print-resource-dir
    OUTPUT_VARIABLE _clang_resource_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  set(_clang_rt_dir "${_clang_resource_dir}/lib/windows")
  if(EXISTS "${_clang_rt_dir}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT
      "${CMAKE_EXE_LINKER_FLAGS_INIT} /libpath:\"${_clang_rt_dir}\" /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib clang_rt.asan_dynamic-x86_64.lib")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT
      "${CMAKE_SHARED_LINKER_FLAGS_INIT} /libpath:\"${_clang_rt_dir}\" /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib clang_rt.asan_dynamic-x86_64.lib")
  endif()
endif()
