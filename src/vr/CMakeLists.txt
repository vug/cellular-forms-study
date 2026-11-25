# cf_vr: the live form in an OpenXR headset (src/vr, apps/cf_vr). Uses the viewer library
# (renderer, live simulation) and the OpenXR loader from external/openxr.

# --- its own small shaders (the pointer rays), compiled like the viewer's
set(CF_VR_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${CF_VR_SHADER_OUT})
file(GLOB CF_VR_SHADER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert ${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag)
set(CF_VR_SHADER_OUTPUTS)
foreach(shader ${CF_VR_SHADER_SOURCES})
  get_filename_component(name ${shader} NAME)
  set(out ${CF_VR_SHADER_OUT}/${name}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND Vulkan::glslc --target-env=vulkan1.4 -O -Werror -mfmt=num ${shader} -o ${out}
    DEPENDS ${shader}
    COMMENT "glslc ${name}"
    VERBATIM)
  list(APPEND CF_VR_SHADER_OUTPUTS ${out})
endforeach()
add_custom_target(cf_vr_shaders DEPENDS ${CF_VR_SHADER_OUTPUTS})

file(GLOB CF_VR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp
     ${CMAKE_SOURCE_DIR}/apps/cf_vr/*.cpp)
add_executable(cf_vr ${CF_VR_SOURCES})
add_dependencies(cf_vr cf_vr_shaders)
target_include_directories(cf_vr PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CF_VR_SHADER_OUT})
target_compile_definitions(cf_vr PRIVATE XR_USE_GRAPHICS_API_VULKAN XR_USE_PLATFORM_WIN32)
target_link_libraries(cf_vr PRIVATE cf_viewer openxr_loader cf_options)
