# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The awva plugins: every awva engine component (awva/component, nsc.h) as a CLAP, and through
# clap-wrapper (Sorbet AI's fork, the submodule third_party/clap-wrapper: docs/forked_dependencies.md)
# as a VST3 (Windows, macOS) and an AUv2 (macOS), all from the one generic source
# (src/plugin*.cpp, which see only nsc.h and CLAP). awva is the AWVA_COMMIT export in deps/ (fetch_deps.sh;
# AWVA=<checkout> exports that checkout's commit): changing AWVA_COMMIT changes the tree a build uses.
#
#   awva_<engine>_impl     the generic plugin, with clap_entry, on the engine's component
#                          (awva_<engine>_component: awva_add_component)
#   awva_<engine>_clap     awva_<engine>.clap
#   awva_<engine>_vst3     awva_<engine>.vst3
#   awva_<engine>_auv2     awva_<engine>.component (macOS)
#   clap_host_test         the CLI CLAP host the proofs run (tests/clap_host_test.cpp)
#   vst3_host_test         the CLI VST3 host (tests/vst3_host_test.cpp); au_host_test the AU's (macOS)
#   ctest -L plugin        per engine: info, the default performance bit for bit against awva_render,
#                          UMP and MIDI 1.0 bit for bit against the engine's component library, CLAP notes,
#                          20 overlapping CLAP notes (one NOTE_END each, per-note brightness), a MIDI
#                          file bit for bit against awva_render --midi, the expressive performances
#                          through each format's own events bit for bit against awva_render --ump
#   awva_bench             the CPU load and latency table of every engine and format (tools/bench.sh)
cmake_minimum_required(VERSION 3.25)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # one runtime with clap-wrapper's
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "universal")
set(CMAKE_OSX_DEPLOYMENT_TARGET 11.0 CACHE STRING "")
project(awva_plugins VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
  enable_language(OBJC OBJCXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/AWVA_COMMIT" AWVA_COMMIT_FULL LIMIT_COUNT 1)
string(SUBSTRING "${AWVA_COMMIT_FULL}" 0 7 AWVA_COMMIT)
set(AWVA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/deps/awva-${AWVA_COMMIT}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/AWVA_COMMIT")
set(CLAP_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/clap-1.2.10" CACHE PATH "")
set(VST3_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/vst3sdk-3.8.0" CACHE PATH "")
set(AUDIOUNIT_SDK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/deps/AudioUnitSDK-1.1.0" CACHE PATH "")
foreach(_d AWVA_DIR CLAP_SDK_ROOT VST3_SDK_ROOT)
  if(NOT EXISTS "${${_d}}")
    message(FATAL_ERROR "${_d}=${${_d}} doesn't exist (./fetch_deps.sh)")
  endif()
endforeach()

# --- awva: the engines, the SDK (the reference renderer and the component libraries) ------------------
add_subdirectory("${AWVA_DIR}/dsp" awva/dsp)
add_library(awva_host_options INTERFACE)
target_compile_options(awva_host_options INTERFACE -Wall -Wextra)
enable_testing()
set(AWVA_VERSION "${AWVA_COMMIT}") # (the components' version, nsc_desc.version: the pinned commit)
add_subdirectory("${AWVA_DIR}/sdk" awva/sdk EXCLUDE_FROM_ALL) # (only what the plugins and proofs use is built)
set(AWVA_PLUGIN_ENGINES "${AWVA_ENGINES}") # (every engine of the pinned awva: a new one is a plugin at the next configure)

# --- clap-wrapper (the submodule): its functions only -------------------------------------------------
set(CLAP_WRAPPER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/clap-wrapper")
if(NOT EXISTS "${CLAP_WRAPPER_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "no ${CLAP_WRAPPER_DIR} (git submodule update --init)")
endif()
set(CLAP_WRAPPER_DONT_ADD_TARGETS TRUE)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES FALSE)
add_subdirectory("${CLAP_WRAPPER_DIR}" clap-wrapper)

# --- the editor (src/editor): the awva app's screen drawn natively, its design tokens generated from
# app/design/tokens.json (include/awva_plugin/design_tokens.h); the font and the logo compiled in -------
set(AWVA_EDITOR_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/editor_assets.cpp")
add_custom_command(OUTPUT "${AWVA_EDITOR_ASSETS}"
  COMMAND "${CMAKE_COMMAND}" "-DFONT=${CMAKE_CURRENT_SOURCE_DIR}/app/assets/fonts/DMSans.ttf" "-DLOGO=${CMAKE_CURRENT_SOURCE_DIR}/src/editor/logo.png"
    "-DOUT=${AWVA_EDITOR_ASSETS}" -P "${CMAKE_CURRENT_SOURCE_DIR}/src/editor/embed.cmake"
  DEPENDS app/assets/fonts/DMSans.ttf src/editor/logo.png src/editor/embed.cmake VERBATIM)
set(AWVA_EDITOR_SOURCES src/editor/editor.cpp "${AWVA_EDITOR_ASSETS}")
if(WIN32)
  list(APPEND AWVA_EDITOR_SOURCES src/editor/editor_win.cpp)
  set(AWVA_EDITOR_LIBS d2d1 dwrite windowscodecs ole32)
elseif(APPLE)
  list(APPEND AWVA_EDITOR_SOURCES src/editor/editor_mac.mm)
  set_source_files_properties(src/editor/editor_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  set(AWVA_EDITOR_LIBS "-framework Cocoa" "-framework CoreText" "-framework CoreGraphics" "-framework ImageIO")
else() # Linux: X11, cairo, FreeType
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(AWVA_X11 REQUIRED IMPORTED_TARGET x11 cairo freetype2)
  list(APPEND AWVA_EDITOR_SOURCES src/editor/editor_x11.cpp)
  set(AWVA_EDITOR_LIBS PkgConfig::AWVA_X11)
endif()

# --- the generic plugin: CLAP and nsc.h only, compiled once --------------------------------------------
add_library(awva_plugin_core OBJECT src/plugin.cpp src/plugin_events.cpp src/plugin_state.cpp src/plugin_audio.cpp
  src/plugin_editor.cpp src/plugin_ports.cpp ${AWVA_EDITOR_SOURCES})
# (only clap_entry leaves the module: the parts' names are the module's own)
set_target_properties(awva_plugin_core PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(awva_plugin_core PUBLIC ${AWVA_EDITOR_LIBS}) # (Linux: its headers too)
target_include_directories(awva_plugin_core PUBLIC "${CLAP_SDK_ROOT}/include" "${AWVA_DIR}/component/include" include src
  "${CLAP_WRAPPER_DIR}/include") # (clapwrapper/auv2.h: the AU codes; clapwrapper/vst3.h)
target_compile_options(awva_plugin_core PRIVATE -Wall -Wextra -ffp-contract=off)
target_compile_definitions(awva_plugin_core PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)

# --- one plugin per engine -----------------------------------------------------------------------------
set(_codes "")
set(AWVA_PLUGIN_BUNDLES "")
set(_au_targets "")
foreach(v IN LISTS AWVA_PLUGIN_ENGINES)
  if(NOT v IN_LIST AWVA_ENGINES)
    message(FATAL_ERROR "AWVA_PLUGIN_ENGINES: ${v} is not an awva engine (${AWVA_ENGINES})")
  endif()
  # the AU subtype: the id's first four letters, the first capitalised, "_" filling ("va" -> Va__)
  string(SUBSTRING "${v}____" 0 4 _code)
  string(SUBSTRING "${_code}" 0 1 _c0)
  string(SUBSTRING "${_code}" 1 3 _c1)
  string(TOUPPER "${_c0}" _c0)
  set(_code "${_c0}${_c1}")
  if(_code IN_LIST _codes)
    message(FATAL_ERROR "the AU subtype ${_code} of ${v} is another engine's too: give it one")
  endif()
  list(APPEND _codes ${_code})
  set(AWVA_AU_SUBTYPE_${v} ${_code})

  # the engine's component (awva_add_component: the entry, the shim, the MIDI side, the declared
  # defaults, with awva's flags), linked in under the generic plugin
  awva_add_component(awva_${v}_component ${v} STATIC)
  add_library(awva_${v}_impl STATIC $<TARGET_OBJECTS:awva_plugin_core>)
  target_link_libraries(awva_${v}_impl PUBLIC awva_${v}_component ${AWVA_EDITOR_LIBS})
  target_include_directories(awva_${v}_impl PUBLIC "${CLAP_SDK_ROOT}/include")

  # the CLAP: the implementation whole (clap_entry is what the host looks for)
  add_library(awva_${v}_clap MODULE src/clap_module.cpp)
  target_link_libraries(awva_${v}_clap PRIVATE awva_${v}_impl)
  if(APPLE)
    set_target_properties(awva_${v}_clap PROPERTIES OUTPUT_NAME awva_${v} BUNDLE TRUE BUNDLE_EXTENSION clap
      MACOSX_BUNDLE_GUI_IDENTIFIER com.sorbet-ai.awva.${v} MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
      MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    target_link_options(awva_${v}_clap PRIVATE -Wl,-exported_symbol,_clap_entry)
  else()
    set_target_properties(awva_${v}_clap PROPERTIES OUTPUT_NAME awva_${v} PREFIX "" SUFFIX ".clap")
  endif()

  # the VST3 and the AUv2: clap-wrapper around the same implementation, linked in
  add_library(awva_${v}_vst3 MODULE src/clap_module.cpp)
  target_link_libraries(awva_${v}_vst3 PRIVATE awva_${v}_impl)
  # (in the build directory; on Windows the VST3 folder bundle, awva_<engine>.vst3/Contents/x86_64-win/)
  target_add_vst3_wrapper(TARGET awva_${v}_vst3 OUTPUT_NAME "awva_${v}" ASSET_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    SUPPORTS_ALL_NOTE_EXPRESSIONS TRUE BUNDLE_IDENTIFIER com.sorbet-ai.awva.${v}.vst3 BUNDLE_VERSION ${PROJECT_VERSION})
  list(APPEND AWVA_PLUGIN_BUNDLES awva_${v}_clap awva_${v}_vst3)
  if(APPLE)
    add_library(awva_${v}_auv2 MODULE src/clap_module.cpp)
    target_link_libraries(awva_${v}_auv2 PRIVATE awva_${v}_impl)
    # its description from the built CLAP (the AU's name, description and version are the CLAP
    # descriptor's, so the component's; the codes from plugin.cpp's AU info factory); its bundle
    # awva_<engine>.component
    target_add_auv2_wrapper(TARGET awva_${v}_auv2 OUTPUT_NAME "awva_${v}" CLAP_TARGET_FOR_CONFIG awva_${v}_clap
      BUNDLE_IDENTIFIER com.sorbet-ai.awva.${v}.component BUNDLE_VERSION ${PROJECT_VERSION}
      MANUFACTURER_NAME "Sorbet AI" MANUFACTURER_CODE Awva)
    list(APPEND _au_targets awva_${v}_auv2)
  endif()
endforeach()
add_custom_target(awva_plugins DEPENDS ${AWVA_PLUGIN_BUNDLES})
# the AU codes, for tests/auval_mac.sh and the validators
set(_au "")
foreach(v IN LISTS AWVA_PLUGIN_ENGINES)
  string(APPEND _au "${v} aumu ${AWVA_AU_SUBTYPE_${v}} Awva\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/au_codes.txt" "${_au}")

# --- the proofs ------------------------------------------------------------------------------------------
add_executable(make_test_midi tests/make_test_midi.cpp)
target_link_libraries(make_test_midi PRIVATE awva_sdk)
add_executable(make_expressive tests/make_expressive.cpp)
target_compile_definitions(make_expressive PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
add_executable(clap_host_test tests/clap_host_test.cpp tests/clap_host_proofs.cpp tests/clap_host_notes.cpp tests/clap_host_bench.cpp
  tests/clap_host_info.cpp tests/clap_host_stress.cpp) # (the shared host: tests/clap_host.h)
target_include_directories(clap_host_test PRIVATE "${CLAP_SDK_ROOT}/include" "${AWVA_DIR}/component/include" include)
target_link_libraries(clap_host_test PRIVATE awva_sdk awva_performances ${CMAKE_DL_LIBS})
target_compile_definitions(clap_host_test PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS>)
add_dependencies(clap_host_test awva_render nsc_components make_test_midi make_expressive)

# the component ABI's conformance (awva's awva_conformance, docs/conformance.md) on each engine's
# component as the plugins build it: the same awva_add_component, here as a loadable library. A
# component change that breaks the ABI contract fails this CI too. The engines' known failures are
# awva's own (sdk/tests/known_failures.cmake), reported as warnings as they are there.
include("${AWVA_DIR}/sdk/tests/known_failures.cmake")
add_custom_target(awva_plugin_conformance ALL)
add_dependencies(awva_plugin_conformance awva_conformance)
# editor_shot: the editor rendered offscreen, both themes (the screenshots; ctest plugin.editor_shot)
add_executable(editor_shot tests/editor_shot.cpp ${AWVA_EDITOR_SOURCES})
target_include_directories(editor_shot PRIVATE "${AWVA_DIR}/component/include" include src)
target_link_libraries(editor_shot PRIVATE ${AWVA_EDITOR_LIBS} ${CMAKE_DL_LIBS})
target_compile_definitions(editor_shot PRIVATE $<$<PLATFORM_ID:Windows>:_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE>)
add_dependencies(editor_shot nsc_components)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/editor_shots")
foreach(v IN ITEMS bowed va)
  if(v IN_LIST AWVA_ENGINES)
    add_test(NAME plugin.editor_shot.${v} COMMAND editor_shot "$<TARGET_FILE:nsc_${v}>" "${CMAKE_BINARY_DIR}/editor_shots")
    set_tests_properties(plugin.editor_shot.${v} PROPERTIES LABELS plugin)
  endif()
endforeach()

# editor_host_test: the editor through the CLAP gui extension, as a host uses it (Windows: a knob dragged
# in a hidden window must reach the host as a gesture and values)
add_executable(editor_host_test tests/editor_host_test.cpp)
target_include_directories(editor_host_test PRIVATE "${CLAP_SDK_ROOT}/include" include)
target_link_libraries(editor_host_test PRIVATE ${CMAKE_DL_LIBS})
if(NOT WIN32 AND NOT APPLE) # Linux: the host's own X connection and window
  target_link_libraries(editor_host_test PRIVATE PkgConfig::AWVA_X11)
endif()
foreach(v IN ITEMS bowed va)
  if(v IN_LIST AWVA_PLUGIN_ENGINES)
    add_dependencies(editor_host_test awva_${v}_clap)
    if(APPLE)
      set(_clap "$<TARGET_BUNDLE_DIR:awva_${v}_clap>")
    else()
      set(_clap "$<TARGET_FILE:awva_${v}_clap>")
    endif()
    add_test(NAME plugin.${v}.editor COMMAND editor_host_test "${_clap}")
    set_tests_properties(plugin.${v}.editor PROPERTIES LABELS plugin)
  endif()
endforeach()

# vst3_host_test: a minimal VST3 host (the VST3 SDK's hosting module): the preset list, the expressive
# proof, the bench
set(_vh "${VST3_SDK_ROOT}/public.sdk/source/vst/hosting")
add_executable(vst3_host_test tests/vst3_host_test.cpp "${_vh}/module.cpp" "${_vh}/hostclasses.cpp" "${_vh}/pluginterfacesupport.cpp"
  "${_vh}/eventlist.cpp" "${_vh}/parameterchanges.cpp"
  "${VST3_SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp" "${VST3_SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp"
  "${VST3_SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp" "${VST3_SDK_ROOT}/pluginterfaces/base/funknown.cpp"
  "${VST3_SDK_ROOT}/pluginterfaces/base/coreiids.cpp"
  $<$<PLATFORM_ID:Darwin>:${_vh}/module_mac.mm> $<$<PLATFORM_ID:Windows>:${_vh}/module_win32.cpp> $<$<PLATFORM_ID:Linux>:${_vh}/module_linux.cpp>)
target_include_directories(vst3_host_test PRIVATE "${VST3_SDK_ROOT}" "${AWVA_DIR}/component/include")
set_target_properties(vst3_host_test PROPERTIES CXX_STANDARD 17)
if(APPLE)
  target_link_libraries(vst3_host_test PRIVATE "-framework CoreFoundation" "-framework Foundation")
  set_source_files_properties("${_vh}/module_mac.mm" PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
  target_compile_definitions(vst3_host_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# au_host_test: a minimal AU host (AudioToolbox; the AU registered in this process only, never installed)
if(APPLE)
  add_executable(au_host_test tests/au_host_test.cpp)
  target_include_directories(au_host_test PRIVATE "${AWVA_DIR}/component/include")
  target_link_libraries(au_host_test PRIVATE "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreMIDI"
    "-framework CoreFoundation")
endif()
set(_xp -DOUT=${CMAKE_CURRENT_BINARY_DIR}/proof "-DRENDER=$<TARGET_FILE:awva_render>" "-DMAKE=$<TARGET_FILE:make_expressive>"
  "-DCLAP_HOST=$<TARGET_FILE:clap_host_test>")
set(_bench_cmds "")
foreach(v IN LISTS AWVA_PLUGIN_ENGINES)
  add_dependencies(clap_host_test awva_${v}_clap)
  add_dependencies(vst3_host_test awva_${v}_vst3)
  if(APPLE)
    add_dependencies(au_host_test awva_${v}_auv2_signed)
  endif()
  if(APPLE)
    set(_clap "$<TARGET_BUNDLE_DIR:awva_${v}_clap>")
  else()
    set(_clap "$<TARGET_FILE:awva_${v}_clap>")
  endif()
  set(_lib "$<TARGET_FILE:nsc_${v}>")
  add_test(NAME plugin.${v}.info COMMAND clap_host_test info "${_clap}" "${_lib}" "${AWVA_DIR}/presets/${v}")
  add_test(NAME plugin.${v}.perf COMMAND "${CMAKE_COMMAND}" "-DENGINE=${v}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/proof"
    "-DRENDER=$<TARGET_FILE:awva_render>" "-DHOST=$<TARGET_FILE:clap_host_test>" "-DCLAP=${_clap}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/perf_proof.cmake")
  add_test(NAME plugin.${v}.midi COMMAND "${CMAKE_COMMAND}" "-DENGINE=${v}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/proof"
    "-DRENDER=$<TARGET_FILE:awva_render>" "-DHOST=$<TARGET_FILE:clap_host_test>" "-DCLAP=${_clap}"
    "-DMAKE_MIDI=$<TARGET_FILE:make_test_midi>" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/midi_proof.cmake")
  add_test(NAME plugin.${v}.ump COMMAND clap_host_test ump "${_clap}" "${_lib}")
  add_test(NAME plugin.${v}.notes COMMAND clap_host_test notes "${_clap}")
  add_test(NAME plugin.${v}.stress COMMAND clap_host_test stress "${_clap}" 20)
  awva_add_component(awva_${v}_conformance_lib ${v} MODULE)
  set_target_properties(awva_${v}_conformance_lib PROPERTIES OUTPUT_NAME "conformance_${v}" PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/conformance")
  add_dependencies(awva_plugin_conformance awva_${v}_conformance_lib)
  # (one ctest per check: plugin.<engine>.conformance.<check>)
  awva_conformance_tests("plugin.${v}.conformance." "$<TARGET_FILE:awva_${v}_conformance_lib>" KNOWN ${AWVA_KNOWN_${v}} ${AWVA_KNOWN_COMPONENT}
    LABELS "plugin;conformance")
  add_test(NAME plugin.${v}.vst3_presets COMMAND vst3_host_test programs "${CMAKE_BINARY_DIR}/awva_${v}.vst3" "${_lib}")
  add_test(NAME plugin.${v}.vst3_rates COMMAND vst3_host_test rates "${CMAKE_BINARY_DIR}/awva_${v}.vst3")
  add_test(NAME plugin.${v}.many COMMAND clap_host_test many "${_clap}" "${_lib}")
  # the expressive performances through each format's own events, against awva_render --ump
  set(_vst3 "${CMAKE_BINARY_DIR}/awva_${v}.vst3")
  add_test(NAME plugin.${v}.expressive.clap COMMAND "${CMAKE_COMMAND}" -DENGINE=${v} -DFORMAT=clap ${_xp} "-DCLAP=${_clap}"
    "-DHOST=$<TARGET_FILE:clap_host_test>" "-DPLUGIN=${_clap}" "-DLIB=${_lib}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive_proof.cmake")
  add_test(NAME plugin.${v}.expressive.vst3 COMMAND "${CMAKE_COMMAND}" -DENGINE=${v} -DFORMAT=vst3 ${_xp} "-DCLAP=${_clap}"
    "-DHOST=$<TARGET_FILE:vst3_host_test>" "-DPLUGIN=${_vst3}" "-DLIB=${_lib}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive_proof.cmake")
  set(_expressive plugin.${v}.expressive.clap plugin.${v}.expressive.vst3)
  set(_bench_au "")
  if(APPLE)
    set(_au "$<TARGET_BUNDLE_DIR:awva_${v}_auv2>")
    add_test(NAME plugin.${v}.expressive.au COMMAND "${CMAKE_COMMAND}" -DENGINE=${v} -DFORMAT=au ${_xp} "-DCLAP=${_clap}"
      "-DHOST=$<TARGET_FILE:au_host_test>" "-DPLUGIN=${_au}" "-DLIB=${_lib}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/expressive_proof.cmake")
    list(APPEND _expressive plugin.${v}.expressive.au)
    set(_bench_au "-DAU_HOST=$<TARGET_FILE:au_host_test>" "-DAU=${_au}")
  endif()
  set_tests_properties(plugin.${v}.info plugin.${v}.perf plugin.${v}.ump plugin.${v}.notes plugin.${v}.many plugin.${v}.midi plugin.${v}.vst3_presets
    ${_expressive} PROPERTIES LABELS plugin)
  list(APPEND _bench_cmds COMMAND "${CMAKE_COMMAND}" -DENGINE=${v} "-DCLAP_HOST=$<TARGET_FILE:clap_host_test>" "-DCLAP=${_clap}" "-DLIB=${_lib}"
    "-DVST3_HOST=$<TARGET_FILE:vst3_host_test>" "-DVST3=${_vst3}" ${_bench_au} "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/bench"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench.cmake")
endforeach()
# the CPU load and latency of every engine and format, one at a time (build/bench/<engine>.txt, and
# the table: build/bench/table.md)
string(REPLACE ";" "," _engines "${AWVA_PLUGIN_ENGINES}")
add_custom_target(awva_bench ${_bench_cmds}
  COMMAND "${CMAKE_COMMAND}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/bench" "-DENGINES=${_engines}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/bench_table.cmake"
  USES_TERMINAL VERBATIM)
add_dependencies(awva_bench clap_host_test vst3_host_test)
if(APPLE)
  add_dependencies(awva_bench au_host_test)
endif()

# --- the standalone app's core (app/core: the Flutter app's engine, MIDI and audio) -----------------------
option(AWVA_APP "build the standalone app's core (app/core)" ON)
if(AWVA_APP)
  add_subdirectory(app/core)
endif()

# clap-wrapper's own libraries are C++17 code (char8_t paths and its fmt break under C++20): every
# target it made in this directory and its own
get_directory_property(_here BUILDSYSTEM_TARGETS)
get_directory_property(_wrap DIRECTORY "${CLAP_WRAPPER_DIR}" BUILDSYSTEM_TARGETS)
foreach(_t IN LISTS _here _wrap)
  if(_t MATCHES "clap-wrapper|base-sdk|build-helper|_auv2|_vst3$|filesystem|^sdk_|^pluginterfaces|^base$|^vst3")
    set_target_properties(${_t} PROPERTIES CXX_STANDARD 17)
  endif()
endforeach()

# macOS: the bundles signed ad hoc as a whole (the linker signs only the binary; the AU registrar
# skips a component whose resources aren't sealed). An AU is signed after clap-wrapper's
# <target>-info-plist has put its Info.plist in (again after a re-configure), whenever that or the
# binary changed.
if(APPLE)
  foreach(_b IN LISTS AWVA_PLUGIN_BUNDLES)
    add_custom_command(TARGET ${_b} POST_BUILD COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${_b}>" VERBATIM)
  endforeach()
  foreach(_b IN LISTS _au_targets)
    set(_signed "${CMAKE_CURRENT_BINARY_DIR}/${_b}.signed")
    add_custom_command(OUTPUT "${_signed}"
      COMMAND codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:${_b}>"
      COMMAND "${CMAKE_COMMAND}" -E touch "${_signed}"
      DEPENDS "$<TARGET_FILE:${_b}>" "${CMAKE_CURRENT_BINARY_DIR}/${_b}-build-helper-output/auv2_Info.plist.copied" VERBATIM)
    add_custom_target(${_b}_signed ALL DEPENDS "${_signed}")
    add_dependencies(${_b}_signed ${_b}-info-plist)
    add_dependencies(awva_plugins ${_b}_signed)
  endforeach()
endif()
