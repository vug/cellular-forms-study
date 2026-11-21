# Vulkan compute backend (cf_vkcompute): GLSL compute shaders under shaders/sim.
set(CF_SIM_SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders/sim)
set(CF_SIM_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${CF_SIM_SHADER_OUT})
file(GLOB CF_SIM_SHADER_INCLUDES CONFIGURE_DEPENDS ${CF_SIM_SHADER_DIR}/*.glsl)
file(GLOB CF_SIM_SHADER_SOURCES CONFIGURE_DEPENDS ${CF_SIM_SHADER_DIR}/*.comp)
set(CF_SIM_SHADER_OUTPUTS)
foreach(shader ${CF_SIM_SHADER_SOURCES})
  get_filename_component(name ${shader} NAME)
  set(out ${CF_SIM_SHADER_OUT}/${name}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND Vulkan::glslc --target-env=vulkan1.4 -O -Werror -I ${CF_SIM_SHADER_DIR} -mfmt=num ${shader} -o ${out}
    DEPENDS ${shader} ${CF_SIM_SHADER_INCLUDES}
    COMMENT "glslc sim/${name}"
    VERBATIM)
  list(APPEND CF_SIM_SHADER_OUTPUTS ${out})
endforeach()
add_custom_target(cf_sim_shaders DEPENDS ${CF_SIM_SHADER_OUTPUTS})

file(GLOB CF_VKC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB CF_VKC_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_library(cf_vkcompute STATIC ${CF_VKC_SOURCES} ${CF_VKC_HEADERS})
add_dependencies(cf_vkcompute cf_sim_shaders)
target_include_directories(cf_vkcompute PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CF_SIM_SHADER_OUT})
target_link_libraries(cf_vkcompute PUBLIC cf_core cf_viewer PRIVATE cf_options)

# --- runner: benchmark and comparison against the CPU reference (and CUDA when built)
file(GLOB CF_VKC_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/apps/cf_vkc/*.cpp)
if(CF_VKC_APP_SOURCES)
  add_executable(cf_vkc ${CF_VKC_APP_SOURCES})
  target_link_libraries(cf_vkc PRIVATE cf_vkcompute cf_core cf_options)
endif()
