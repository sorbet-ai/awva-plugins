# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The MIDI-file proof against awva_render --midi: the engine's default preset and a Standard MIDI
# File (make_test_midi: MPE notes with bends, pressure and CC74, a master-channel note, the pedal),
# rendered by awva_render (as many voices as the plugin runs, 48 and 44.1 kHz) and played through the plugin
# in the MIDI dialect (clap_host_test midifile). Both take the events by the same integer-frame
# step rule (awva's timed_midi.h).
# -DENGINE -DOUT=dir -DRENDER -DHOST -DCLAP -DMAKE_MIDI
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
set(mid "${OUT}/proof_${ENGINE}.mid") # (one per engine: the tests run in parallel)
run("${MAKE_MIDI}" "${mid}")
foreach(rate IN ITEMS 48000 44100) # (44.1 kHz: steps between block starts, the late events' case)
  set(ref "${OUT}/ref_midi_${ENGINE}_${rate}.f32")
  run("${RENDER}" --engine ${ENGINE} --nv ${nv} --rate ${rate} --midi "${mid}" --out "${ref}")
  run("${HOST}" midifile "${CLAP}" "${mid}" "${ref}" ${rate})
endforeach()
