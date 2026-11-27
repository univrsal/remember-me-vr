cmake_minimum_required(VERSION 3.24)

project(vrmod LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR
    "This project builds 32-bit Windows DLLs. Configure with a preset, e.g.\n"
    "  cmake --preset mingw-i686-release")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VRMOD_GAME "rememberme" CACHE STRING "Game module to build (directory under src/games/)")

include(FetchContent)

# Third-party dependencies, pinned to tags. SOURCE_SUBDIR points at a directory
# that doesn't exist so only the sources are fetched; we define the targets
# ourselves below instead of running the dependencies' own CMake projects.
FetchContent_Declare(minhook
  GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
  GIT_TAG        v1.3.4
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _fetch_only)
FetchContent_Declare(vulkan_headers
  GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
  GIT_TAG        v1.4.369
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _fetch_only)
FetchContent_Declare(openxr_sdk
  GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
  GIT_TAG        release-1.1.65
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _fetch_only)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.91.9
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  _fetch_only)
FetchContent_MakeAvailable(minhook vulkan_headers openxr_sdk imgui)

# Dear ImGui with its D3D9 renderer (the in-game config menu). Input comes from
# the mod, not imgui's Win32 backend; no clipboard/IME/shell helpers, so the DLL
# imports nothing new.
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp)
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PUBLIC
  IMGUI_DISABLE_WIN32_DEFAULT_CLIPBOARD_FUNCTIONS
  IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
  IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

add_library(minhook STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(vulkan_headers INTERFACE)
target_include_directories(vulkan_headers SYSTEM INTERFACE ${vulkan_headers_SOURCE_DIR}/include)
target_compile_definitions(vulkan_headers INTERFACE VK_NO_PROTOTYPES)

# Headers only: vrcore negotiates with the OpenXR runtime itself (see
# src/core/src/xr/runtime_loader.cpp) instead of linking Khronos' loader.
add_library(openxr_headers INTERFACE)
target_include_directories(openxr_headers SYSTEM INTERFACE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(openxr_headers INTERFACE XR_NO_PROTOTYPES XR_USE_GRAPHICS_API_VULKAN XR_USE_PLATFORM_WIN32)

include(meta/cmake/vrcore.cmake)

add_subdirectory(src/core core)
add_subdirectory(src/games/${VRMOD_GAME} games/${VRMOD_GAME})
add_subdirectory(src/tests tests)
