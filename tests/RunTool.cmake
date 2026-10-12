# Copyright 2026 Sankhesh Jhaveri
# SPDX-License-Identifier: BSD-3-Clause

string(REPLACE "|" ";" _args "${ARGS}")
execute_process(COMMAND "${TOOL}" ${_args}
  RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT "${_result}" STREQUAL "${EXIT}")
  message(FATAL_ERROR "exit ${_result}, expected ${EXIT}:\n${_out}${_err}")
endif()
if(REGEX AND NOT "${_out}${_err}" MATCHES "${REGEX}")
  message(FATAL_ERROR "output does not match '${REGEX}':\n${_out}${_err}")
endif()
