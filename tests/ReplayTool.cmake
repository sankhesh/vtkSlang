# Copyright 2026 Sankhesh Jhaveri
# SPDX-License-Identifier: BSD-3-Clause

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${TOOL}" "${DATA}/surface_host.slang" -m "${DATA}/good_hooks.slang"
    -s IShaderHooks=Halve --dump "${WORK}" --label halve -o "${WORK}/first.wgsl" --no-tint
  RESULT_VARIABLE _first)
execute_process(COMMAND "${TOOL}" --replay "${WORK}/halve" -o "${WORK}/second.wgsl" --no-tint
  RESULT_VARIABLE _second)
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK}/first.wgsl"
    "${WORK}/second.wgsl"
  RESULT_VARIABLE _same)
execute_process(COMMAND "${TOOL}" --replay "${WORK}/halve" -m "${DATA}/broken_hooks.slang"
    -s IShaderHooks=Broken --no-tint
  RESULT_VARIABLE _swapped OUTPUT_QUIET ERROR_VARIABLE _err)
if(NOT _first EQUAL 0 OR NOT _second EQUAL 0 OR NOT _same EQUAL 0)
  message(FATAL_ERROR "replaying a bundle did not reproduce its WGSL")
endif()
if(NOT _swapped EQUAL 1 OR NOT _err MATCHES "undefinedAmount")
  message(FATAL_ERROR "a module swapped into a bundle was not compiled:\n${_err}")
endif()

set(_program "${WORK}/with_program_module")
file(WRITE "${_program}/options.txt" "label with_program_module\ntarget wgsl\n")
file(WRITE "${_program}/program.slang" [=[
import host_util;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { data[id.x] = brighten(data[id.x]); }
]=])
file(WRITE "${_program}/program-module0.slang"
  "module host_util;\npublic float brighten(float x) { return 2.f * x; }\n")
file(WRITE "${_program}/module0.slang" "module user_a;\npublic float a() { return 1.f; }\n")
execute_process(COMMAND "${TOOL}" --replay "${_program}" -m "${DATA}/good_hooks.slang" --no-tint
  RESULT_VARIABLE _kept OUTPUT_QUIET ERROR_VARIABLE _err)
if(NOT _kept EQUAL 0)
  message(FATAL_ERROR "-m replaced a bundle's program modules:\n${_err}")
endif()
file(REMOVE_RECURSE "${WORK}")
