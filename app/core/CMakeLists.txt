# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The awva app's core (awva_app.h), built by the plugins' build (the same toolchain and awva), so the
# app plays what the plugins play:
#   awva_app_core          the shared library the Flutter UI loads (dart:ffi)
#   app/engines/<id>.*     the built-in engines: awva's package libraries (package_lib_<id>: the board's
#                          MIDI table sizes, the PC image a package carries), beside the core
#   awva_app_cli           the core without the UI: list, smoke (the null sink), render
#   ctest -L app           the core's checks, the smoke run, and the app's output against each plugin's
set(_src src/app.cpp src/catalog.cpp src/platform.cpp src/audio_null.cpp src/audio_query.cpp)
list(APPEND _src src/app_sound.cpp src/app_io.cpp src/app_render.cpp src/offline.cpp) # (the core's parts: app_core.h)
if(WIN32)
  list(APPEND _src src/audio_wasapi.cpp src/midi_winmm.cpp)
elseif(APPLE)
  list(APPEND _src src/audio_coreaudio.cpp src/midi_coremidi.cpp)
else()
  list(APPEND _src src/audio_alsa.cpp src/midi_alsa.cpp)
endif()
add_library(awva_app_objects OBJECT ${_src})
target_include_directories(awva_app_objects PUBLIC include src "${AWVA_DIR}/component/include")
# (C++17: std::filesystem's u8path/u8string as std::string)
set_target_properties(awva_app_objects PROPERTIES CXX_STANDARD 17 CXX_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_compile_options(awva_app_objects PRIVATE -Wall -Wextra -ffp-contract=off)
target_compile_definitions(awva_app_objects PUBLIC "AWVA_APP_AWVA_COMMIT=\"${AWVA_COMMIT_FULL}\""
  $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE>)
if(WIN32)
  set(_libs ole32 avrt winmm shell32 uuid)
elseif(APPLE)
  set(_libs "-framework CoreAudio" "-framework AudioToolbox" "-framework CoreMIDI" "-framework CoreFoundation" ${CMAKE_DL_LIBS})
else()
  find_package(ALSA REQUIRED)
  find_package(Threads REQUIRED)
  set(_libs ALSA::ALSA Threads::Threads ${CMAKE_DL_LIBS})
endif()

set(AWVA_APP_OUT "${CMAKE_BINARY_DIR}/app")
add_library(awva_app_core SHARED $<TARGET_OBJECTS:awva_app_objects>)
target_link_libraries(awva_app_core PRIVATE ${_libs})
set_target_properties(awva_app_core PROPERTIES PREFIX "" OUTPUT_NAME awva_app_core
  RUNTIME_OUTPUT_DIRECTORY "${AWVA_APP_OUT}" LIBRARY_OUTPUT_DIRECTORY "${AWVA_APP_OUT}")
if(APPLE)
  set_target_properties(awva_app_core PROPERTIES PREFIX "lib" INSTALL_NAME_DIR "@rpath" BUILD_WITH_INSTALL_NAME_DIR TRUE)
elseif(NOT WIN32)
  set_target_properties(awva_app_core PROPERTIES PREFIX "lib")
endif()
add_library(awva_app_static STATIC $<TARGET_OBJECTS:awva_app_objects>)
target_include_directories(awva_app_static PUBLIC include src "${AWVA_DIR}/component/include")
target_link_libraries(awva_app_static PUBLIC ${_libs})
target_compile_definitions(awva_app_static PUBLIC "AWVA_APP_AWVA_COMMIT=\"${AWVA_COMMIT_FULL}\"")

# the built-in engines: every awva engine's package library (awva's awva_add_package_library: the
# board's MIDI table sizes, the one PC image every host loads, as in a package), beside the core
set(_copies "")
set(_libs_built "")
foreach(v IN LISTS AWVA_ENGINES)
  if(NOT TARGET package_lib_${v})
    message(FATAL_ERROR "awva's package_lib_${v} is missing: the app's built-ins need Python 3 (awva's packages)")
  endif()
  list(APPEND _copies COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:package_lib_${v}>" "${AWVA_APP_OUT}/engines/")
  list(APPEND _libs_built package_lib_${v})
endforeach()
add_custom_target(awva_app_engines ALL COMMAND "${CMAKE_COMMAND}" -E make_directory "${AWVA_APP_OUT}/engines" ${_copies} VERBATIM)
add_dependencies(awva_app_engines ${_libs_built})
add_custom_target(awva_app ALL DEPENDS awva_app_core awva_app_engines)

add_executable(awva_app_cli tools/awva_app_cli.cpp)
target_link_libraries(awva_app_cli PRIVATE awva_app_static)
set_target_properties(awva_app_cli PROPERTIES CXX_STANDARD 17 RUNTIME_OUTPUT_DIRECTORY "${AWVA_APP_OUT}")
add_dependencies(awva_app_cli awva_app_engines)

add_executable(app_tests tests/app_tests.cpp)
target_link_libraries(app_tests PRIVATE awva_app_static)
set_target_properties(app_tests PROPERTIES CXX_STANDARD 17)
target_compile_definitions(app_tests PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
add_dependencies(app_tests awva_app_engines)

add_executable(render_equal tests/render_equal.cpp)
target_include_directories(render_equal PRIVATE "${CLAP_SDK_ROOT}/include")
target_link_libraries(render_equal PRIVATE awva_app_static)
set_target_properties(render_equal PROPERTIES CXX_STANDARD 17)
target_compile_definitions(render_equal PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)

add_test(NAME app.core COMMAND app_tests "${AWVA_APP_OUT}/engines" "${CMAKE_CURRENT_BINARY_DIR}/scratch")
add_test(NAME app.smoke COMMAND awva_app_cli smoke --engine builtin:va --seconds 3 --probe "${CMAKE_CURRENT_BINARY_DIR}/smoke.wav"
  --builtin "${AWVA_APP_OUT}/engines" --engines "${CMAKE_CURRENT_BINARY_DIR}/no-packages")
set_tests_properties(app.core app.smoke PROPERTIES LABELS app)
foreach(v IN LISTS AWVA_PLUGIN_ENGINES)
  add_dependencies(render_equal awva_${v}_clap)
  if(APPLE)
    set(_clap "$<TARGET_BUNDLE_DIR:awva_${v}_clap>")
  else()
    set(_clap "$<TARGET_FILE:awva_${v}_clap>")
  endif()
  add_test(NAME app.${v}.equals_plugin COMMAND render_equal "${_clap}" "$<TARGET_FILE:package_lib_${v}>")
  set_tests_properties(app.${v}.equals_plugin PROPERTIES LABELS app)
endforeach()
