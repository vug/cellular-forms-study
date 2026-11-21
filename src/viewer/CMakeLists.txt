# Vulkan 1.4 viewer: library (cf_viewer) + executable (cf_view).

# --- shaders: GLSL -> SPIR-V as a comma-separated word list that the C++ sources #include
set(CF_SHADER_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(CF_SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${CF_SHADER_OUT})
file(GLOB CF_SHADER_INCLUDES CONFIGURE_DEPENDS ${CF_SHADER_DIR}/*.glsl)
file(GLOB CF_SHADER_SOURCES CONFIGURE_DEPENDS
  ${CF_SHADER_DIR}/*.vert ${CF_SHADER_DIR}/*.frag ${CF_SHADER_DIR}/*.comp)
set(CF_SHADER_OUTPUTS)
foreach(shader ${CF_SHADER_SOURCES})
  get_filename_component(name ${shader} NAME)
  set(out ${CF_SHADER_OUT}/${name}.inc)
  add_custom_command(
    OUTPUT ${out}
    COMMAND Vulkan::glslc --target-env=vulkan1.4 -O -Werror -I ${CF_SHADER_DIR} -mfmt=num ${shader} -o ${out}
    DEPENDS ${shader} ${CF_SHADER_INCLUDES}
    COMMENT "glslc ${name}"
    VERBATIM)
  list(APPEND CF_SHADER_OUTPUTS ${out})
endforeach()
add_custom_target(cf_shaders DEPENDS ${CF_SHADER_OUTPUTS})

# --- library
file(GLOB CF_VIEWER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB CF_VIEWER_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.hpp)
add_library(cf_viewer STATIC ${CF_VIEWER_SOURCES} ${CF_VIEWER_HEADERS})
add_dependencies(cf_viewer cf_shaders)
target_include_directories(cf_viewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${CF_SHADER_OUT})
target_link_libraries(cf_viewer PUBLIC cf_core Vulkan::Vulkan glfw imgui stb PRIVATE cf_options)
if(WIN32)
  target_compile_definitions(cf_viewer PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
if(CF_HAVE_CUDA)
  target_link_libraries(cf_viewer PUBLIC cf_cuda)
  target_compile_definitions(cf_viewer PUBLIC CF_HAVE_CUDA=1)
endif()

# --- executable
file(GLOB CF_VIEW_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/apps/cf_view/*.cpp)
if(CF_VIEW_APP_SOURCES)
  add_executable(cf_view ${CF_VIEW_APP_SOURCES})
  target_link_libraries(cf_view PRIVATE cf_viewer cf_options)
endif()
