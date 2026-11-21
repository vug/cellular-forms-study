cmake_minimum_required(VERSION 3.28)
project(cellular_forms VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------------------------
# Cellular Forms study. Targets:
#   cf_core    static library: the model, CPU reference backend, state files   (src/core)
#   cf_tests   unit and property tests                                         (tests)
#   cf_oracle  test helper that talks to the Python reference                  (tests/oracle_tool.cpp)
#   cf_run     batch CLI                                                       (apps/cf_run)
#   cf_render  CPU preview renderer                                            (apps/cf_render)
#   cf_export  PLY export for Blender                                          (apps/cf_export)
#   cf_cuda    CUDA backend                                                    (src/cuda)
#   cf_view    Vulkan 1.4 viewer                                               (src/viewer, apps/cf_view)
# Host code is C++23. CUDA sources are C++20: nvcc 13.5 has no C++23 mode with MSVC
# (docs/progress/PROGRESS.md, toolchain entry).
# ---------------------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)  # no C++ modules here; skips the dependency scan
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(CF_BUILD_TESTS "Build the tests" ON)
option(CF_BUILD_CUDA "Build the CUDA backend (needs nvcc)" ON)
option(CF_BUILD_VIEWER "Build the Vulkan viewer (needs the Vulkan SDK)" ON)
# Experiments for the speed investigation (docs/progress/PROGRESS.md). Both change results, so
# neither belongs in a normal build: tests that compare against recorded checksums or against
# the CPU reference fail with them.
set(CF_GRID_REFINE "1" CACHE STRING "Experiment: grid cells per radius of influence in the repulsion search (1 = the model)")
option(CF_CUDA_FMA "Experiment: let nvcc fuse multiply-adds in the CUDA backend" OFF)

# Compile options for our own code (not for third-party code under external/).
add_library(cf_options INTERFACE)
if(MSVC)
  # /fp:precise is the default and does not contract a*b+c into FMA, which keeps results
  # independent of how the compiler would choose to fuse expressions.
  target_compile_options(cf_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /utf-8 /fp:precise /Zc:preprocessor /MP>)
  target_compile_definitions(cf_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(cf_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion -ffp-contract=off>)
endif()

find_package(Threads REQUIRED)

# ------------------------------------------------------------------------------- core (M1)
file(GLOB CF_CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.cpp)
file(GLOB CF_CORE_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/core/*.hpp)
add_library(cf_core STATIC ${CF_CORE_SOURCES} ${CF_CORE_HEADERS})
target_include_directories(cf_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(cf_core PUBLIC Threads::Threads PRIVATE cf_options)
target_compile_definitions(cf_core PUBLIC CF_GRID_REFINE=${CF_GRID_REFINE})

function(cf_add_app name)
  file(GLOB app_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/apps/${name}/*.cpp)
  if(app_sources)
    add_executable(${name} ${app_sources})
    target_link_libraries(${name} PRIVATE cf_core cf_options)
  endif()
endfunction()
cf_add_app(cf_run)
cf_add_app(cf_render)
cf_add_app(cf_export)  # PLY export for Blender (M7)
cf_add_app(cf_describe)  # shape descriptors as JSON (M6)

if(CF_BUILD_TESTS)
  enable_testing()
  file(GLOB CF_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
  list(FILTER CF_TEST_SOURCES EXCLUDE REGEX "oracle_tool\\.cpp$")
  if(CF_TEST_SOURCES)
    add_executable(cf_tests ${CF_TEST_SOURCES})
    target_link_libraries(cf_tests PRIVATE cf_core cf_options)
    add_test(NAME cf_tests COMMAND cf_tests)
  endif()
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/oracle_tool.cpp)
    add_executable(cf_oracle ${CMAKE_SOURCE_DIR}/tests/oracle_tool.cpp)
    target_link_libraries(cf_oracle PRIVATE cf_core cf_options)
  endif()
endif()

# ------------------------------------------------------------------------------- CUDA (M3)
set(CF_HAVE_CUDA OFF)
if(CF_BUILD_CUDA AND EXISTS ${CMAKE_SOURCE_DIR}/src/cuda/CMakeLists.txt)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    # The GPU driver on the development PC is older than the CUDA 13.5 toolkit and cannot JIT
    # its PTX, so build native code for the RTX 4090 (sm_89). Override for another GPU.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
      set(CMAKE_CUDA_ARCHITECTURES 89 CACHE STRING "CUDA architectures")
    endif()
    enable_language(CUDA)
    set(CMAKE_CUDA_STANDARD 20)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    set(CMAKE_CUDA_EXTENSIONS OFF)
    find_package(CUDAToolkit REQUIRED)
    set(CF_HAVE_CUDA ON)
    add_subdirectory(src/cuda)
  else()
    message(STATUS "CUDA compiler not found: the CUDA backend is skipped")
  endif()
endif()

# ------------------------------------------------------------------------------- viewer (M2, M4)
if(CF_BUILD_VIEWER AND EXISTS ${CMAKE_SOURCE_DIR}/src/viewer/CMakeLists.txt)
  find_package(Vulkan COMPONENTS glslc)
  if(Vulkan_FOUND AND Vulkan_glslc_FOUND AND EXISTS ${CMAKE_SOURCE_DIR}/external/glfw/CMakeLists.txt)
    add_subdirectory(external)
    add_subdirectory(src/viewer)
    if(EXISTS ${CMAKE_SOURCE_DIR}/src/vkcompute/CMakeLists.txt)
      add_subdirectory(src/vkcompute)  # Vulkan compute backend (M9)
    endif()
  else()
    message(STATUS "Vulkan SDK, glslc or the external/ submodules not found: the viewer is skipped")
  endif()
endif()

message(STATUS "cellular_forms: CUDA backend ${CF_HAVE_CUDA}, C++ compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
