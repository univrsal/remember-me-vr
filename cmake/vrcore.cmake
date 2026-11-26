# vrcore_add_mod(<target> PROXY <dll-name> SOURCES <files...>)
#
# Builds a game mod as a proxy DLL named <dll-name>.dll (e.g. dinput8) that
# links the shared vrcore object library. Supported proxies live in
# core/proxy/<dll-name>/ (a .cpp with the forwarding exports and a .def file).
function(vrcore_add_mod target)
  cmake_parse_arguments(ARG "" "PROXY" "SOURCES" ${ARGN})
  if(NOT ARG_PROXY)
    message(FATAL_ERROR "vrcore_add_mod(${target}): PROXY is required")
  endif()
  set(proxy_dir ${PROJECT_SOURCE_DIR}/core/proxy/${ARG_PROXY})
  if(NOT EXISTS ${proxy_dir}/${ARG_PROXY}.def)
    message(FATAL_ERROR "vrcore_add_mod(${target}): no proxy implementation in ${proxy_dir}")
  endif()

  add_library(${target} SHARED
    ${ARG_SOURCES}
    ${proxy_dir}/${ARG_PROXY}.cpp
    ${proxy_dir}/${ARG_PROXY}.def)
  set_target_properties(${target} PROPERTIES PREFIX "" OUTPUT_NAME ${ARG_PROXY})
  target_link_libraries(${target} PRIVATE vrcore)
  # Self-contained DLL: no libgcc/libstdc++/winpthread DLLs needed next to the game.
  target_link_options(${target} PRIVATE -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup)
endfunction()
