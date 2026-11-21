# CUDA backend (cf_cuda). Sources are C++20: nvcc 13.5 has no C++23 mode with MSVC.
file(GLOB CF_CUDA_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cu)
file(GLOB CF_CUDA_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)

add_library(cf_cuda STATIC ${CF_CUDA_SOURCES} ${CF_CUDA_HEADERS})
target_include_directories(cf_cuda PUBLIC ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cf_cuda PUBLIC cf_core CUDA::cudart)
set_target_properties(cf_cuda PROPERTIES CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON)
target_compile_options(cf_cuda PRIVATE
  # No fused multiply-add: every float operation is then a single IEEE operation, the same as
  # in the MSVC build of the CPU reference, which is what makes the two backends agree bit for
  # bit. (A faster, non-matching mode would drop this flag.)
  $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false>
  # CCCL (CUB, Thrust) needs the conforming preprocessor.
  $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/Zc:preprocessor>
  $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/utf-8>)
target_compile_definitions(cf_cuda PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)

# --- GPU batch runner
file(GLOB CF_GPU_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/apps/cf_gpu/*.cpp)
if(CF_GPU_APP_SOURCES)
  add_executable(cf_gpu ${CF_GPU_APP_SOURCES})
  target_link_libraries(cf_gpu PRIVATE cf_cuda cf_core cf_options)
endif()

# --- tests: CUDA against the CPU reference (uses the harness in tests/)
file(GLOB CF_CUDA_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/cuda/*.cpp)
if(CF_BUILD_TESTS AND CF_CUDA_TEST_SOURCES)
  add_executable(cf_cuda_tests ${CF_CUDA_TEST_SOURCES} ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)
  target_include_directories(cf_cuda_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(cf_cuda_tests PRIVATE cf_cuda cf_core cf_options)
  add_test(NAME cf_cuda_tests COMMAND cf_cuda_tests)
endif()
