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
  src/process.cpp)

target_include_directories(vrcore PUBLIC include)
target_link_libraries(vrcore PUBLIC minhook vulkan_headers)
target_compile_definitions(vrcore PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(vrcore PUBLIC -Wall -Wextra)
