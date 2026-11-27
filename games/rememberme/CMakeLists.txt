vrcore_add_mod(RememberMeVR
  PROXY dinput8
  SOURCES game.cpp scene_view_hooks.cpp free_cam.cpp renderer_hooks.cpp device_state_hooks.cpp sensen_stereo.cpp draw_fixes.cpp mirror.cpp vr_gamepad.cpp)

# Ship the default config next to the DLL in the build tree.
add_custom_command(TARGET RememberMeVR POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_CURRENT_SOURCE_DIR}/RememberMeVR.ini $<TARGET_FILE_DIR:RememberMeVR>/RememberMeVR.ini)
