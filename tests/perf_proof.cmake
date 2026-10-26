# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The bit-identity proof against awva_render: the engine's default preset and the SDK's default
# scripted performance, rendered by awva_render (as many voices as the plugin runs, 48 kHz) and
# played through the plugin (clap_host_test perf: its voice-controls test extension).
# -DENGINE -DOUT=dir -DRENDER -DHOST -DCLAP
file(MAKE_DIRECTORY "${OUT}")
function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("${out}${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}")
  endif()
endfunction()
execute_process(COMMAND "${HOST}" voices "${CLAP}" RESULT_VARIABLE rc OUTPUT_VARIABLE nv OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0 OR NOT nv MATCHES "^[0-9]+$")
  message(FATAL_ERROR "clap_host_test voices: ${rc} ${nv}")
endif()
set(ref "${OUT}/ref_${ENGINE}.f32")
run("${RENDER}" --engine ${ENGINE} --nv ${nv} --rate 48000 --ftz --out "${ref}") # (--ftz: as the component runs)
run("${HOST}" perf "${CLAP}" "${ref}")
