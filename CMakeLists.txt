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

set(VRMOD_GAME "rememberme" CACHE STRING "Game module to build (directory under games/)")

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
FetchContent_MakeAvailable(minhook vulkan_headers openxr_sdk)

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
# core/src/xr/runtime_loader.cpp) instead of linking Khronos' loader.
add_library(openxr_headers INTERFACE)
target_include_directories(openxr_headers SYSTEM INTERFACE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(openxr_headers INTERFACE XR_NO_PROTOTYPES XR_USE_GRAPHICS_API_VULKAN XR_USE_PLATFORM_WIN32)

include(cmake/vrcore.cmake)

add_subdirectory(core)
add_subdirectory(games/${VRMOD_GAME})
add_subdirectory(tests)
