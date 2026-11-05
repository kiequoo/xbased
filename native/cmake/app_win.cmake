# The PC executable (Windows, Linux): the SDL3 front end (app/win_main.c)
# on the core, with the presentation and the frame pacing of the Android
# app's (app/present.c, app/vsync.c), and the dashboard (app/ui/).
set(NC_APP_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_main.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_mods.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/win_auto.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/present.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/vsync.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/ui.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/i18n.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/config.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xbox_data.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/library.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/input_map.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/sound.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_settings.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/dash_memory.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/xemu_import.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/update.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/shortcut.c"
)
if(WIN32)
  enable_language(RC)
  add_executable(hakux WIN32 ${NC_APP_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/app/win/xbased.rc")
  # the linker's map: the profile's offsets resolved where there is no debug info (HAKUX_PROF)
  target_link_options(hakux PRIVATE "-Wl,-Map=$<TARGET_FILE_DIR:hakux>/xbased.map")
  target_link_libraries(hakux PRIVATE ole32 uuid shell32)   # the desktop shortcuts (app/ui/shortcut.c)
else()
  add_executable(hakux ${NC_APP_SOURCES})
endif()
# xbased (.exe), with its icon and version on Windows (app/win/xbased.rc)
set_target_properties(hakux PROPERTIES OUTPUT_NAME xbased)
target_link_libraries(hakux PRIVATE hakux_native hakux_hle SDL3::SDL3)
target_include_directories(hakux PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../third_party/stb")  # public domain, vendored
target_compile_options(hakux PRIVATE -Wall -Wno-unused-parameter -Wno-unused-function)
# the mods and cheats the game folders hold (mods/README.md), and the
# dashboard's fonts, beside the executable
add_custom_command(TARGET hakux POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/mods" "$<TARGET_FILE_DIR:hakux>/mods"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${NC_REPO}/cheats" "$<TARGET_FILE_DIR:hakux>/cheats"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/app/ui/fonts" "$<TARGET_FILE_DIR:hakux>/fonts")
