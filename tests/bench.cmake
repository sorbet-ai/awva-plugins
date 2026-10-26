# SPDX-FileCopyrightText: 2026 Sorbet AI
# SPDX-License-Identifier: Apache-2.0
#
# The CPU load and latency of one engine in every format (clap_host_test, vst3_host_test and on
# macOS au_host_test bench: 48 kHz, host blocks of 128, 1, 4 and the plugin's voices), one host at
# a time: OUT/ENGINE.txt, the hosts' "bench" and "latency" lines.
# -DENGINE -DOUT -DCLAP_HOST -DCLAP -DLIB -DVST3_HOST -DVST3 [-DAU_HOST -DAU]
file(MAKE_DIRECTORY "${OUT}")
execute_process(COMMAND "${CLAP_HOST}" voices "${CLAP}" RESULT_VARIABLE rc OUTPUT_VARIABLE nv OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rc EQUAL 0 OR NOT nv MATCHES "^[0-9]+$")
  message(FATAL_ERROR "clap_host_test voices: ${rc} ${nv}")
endif()
set(lines "")
function(bench)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}\n${out}${err}")
  endif()
  set(lines "${lines}${out}" PARENT_SCOPE)
endfunction()
bench("${CLAP_HOST}" bench "${CLAP}" "${LIB}")
bench("${VST3_HOST}" bench "${VST3}" ${nv})
if(AU_HOST)
  bench("${AU_HOST}" bench "${AU}" ${nv})
endif()
file(WRITE "${OUT}/${ENGINE}.txt" "${lines}")
message("${ENGINE}:\n${lines}")
