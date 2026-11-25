# Third-party code, all git submodules. Built without our warning flags.

# --- GLFW (window and input for the viewer)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(glfw EXCLUDE_FROM_ALL)

# --- Dear ImGui (no CMake of its own): core + GLFW and Vulkan backends
add_library(imgui STATIC
  imgui/imgui.cpp
  imgui/imgui_draw.cpp
  imgui/imgui_tables.cpp
  imgui/imgui_widgets.cpp
  imgui/imgui_demo.cpp
  imgui/backends/imgui_impl_glfw.cpp
  imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)
target_compile_features(imgui PUBLIC cxx_std_17)

# --- OpenXR loader (for cf_vr): the loader only, linked statically; no API layers, tests or
# conformance suite. The SDK repository carries its generated sources, so no Python is needed.
if(CF_BUILD_VR AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/openxr/CMakeLists.txt)
  set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
  set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
  add_subdirectory(openxr EXCLUDE_FROM_ALL)
endif()

# --- stb (header-only; stb_image_write for PNG captures)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE stb)

# --- nlohmann/json (header-only; the viewer's gallery reads gallery.json with it)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json SYSTEM INTERFACE json/single_include)
