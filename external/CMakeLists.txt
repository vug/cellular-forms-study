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

# --- stb (header-only; stb_image_write for PNG captures)
add_library(stb INTERFACE)
target_include_directories(stb INTERFACE stb)
