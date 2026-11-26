# vrcore: game-independent pieces shared by every mod built from this repo.
# An OBJECT library, so DllMain and other unreferenced-but-needed objects are
# always linked into the final DLL (a static library would drop them).
add_library(vrcore OBJECT
  src/config.cpp
  src/d3d9_hooks.cpp
  src/dllmain.cpp
  src/dxvk_interop.cpp
  src/hook.cpp
  src/log.cpp
  src/memory.cpp
  src/process.cpp
  src/ue3/reflection.cpp
  src/ue3/types.cpp
  src/vk.cpp
  src/xr/runtime_loader.cpp
  src/xr/virtual_screen.cpp
  src/xr/xr_context.cpp)

target_include_directories(vrcore PUBLIC include)
target_link_libraries(vrcore PUBLIC minhook vulkan_headers openxr_headers)
target_compile_definitions(vrcore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
# -Wno-missing-field-initializers: `VkFoo foo{VK_STRUCTURE_TYPE_FOO}` zero-fills the rest by design.
target_compile_options(vrcore PUBLIC -Wall -Wextra -Wno-missing-field-initializers)
