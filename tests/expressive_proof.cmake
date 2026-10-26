# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The expressive proof of one format: the performances of tests/expressive.h (per-note pressure,
# tuning and brightness; MPE) as their UMP streams (make_expressive), rendered by awva_render --ftz
# --ump (as many voices as the plugin runs, 48 and 44.1 kHz), and played through the format's own
# events by its host test (clap_host_test, vst3_host_test, au_host_test expressive), which compares.
# -DENGINE -DFORMAT=clap|vst3|au -DOUT=dir -DRENDER -DMAKE -DCLAP_HOST -DCLAP (for the voices)
# -DHOST -DPLUGIN -DLIB
set(dir "${OUT}/${ENGINE}_${FORMAT}") # (one per engine and format: the tests run in parallel)
file(MAKE_DIRECTORY "${dir}")
function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  message("${out}${err}")
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}")
  endif()
endfunction()
execute_process(COMMAND "${CLAP_HOST}" voices "${CLAP}" RESULT_VARIABLE rc OUTPUT_VARIABLE nv OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0 OR NOT nv MATCHES "^[0-9]+$")
  message(FATAL_ERROR "clap_host_test voices: ${rc} ${nv}")
endif()
foreach(rate IN ITEMS 48000 44100) # (44.1 kHz: steps between block starts, the late events' case)
  run("${MAKE}" "${dir}" ${rate})
  foreach(perf IN ITEMS per_note mpe mpe_vst3 mpe_midi2)
    run("${RENDER}" --engine ${ENGINE} --nv ${nv} --rate ${rate} --ftz --ump "${dir}/${perf}_${rate}.ump" --seconds 4
      --out "${dir}/ref_${perf}_${rate}.f32")
  endforeach()
  run("${HOST}" expressive "${PLUGIN}" "${LIB}" "${dir}" ${rate})
endforeach()
