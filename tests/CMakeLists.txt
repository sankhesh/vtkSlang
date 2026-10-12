# Copyright 2026 Sankhesh Jhaveri
# SPDX-License-Identifier: BSD-3-Clause

add_executable(vtkSlangTestCompiler TestCompiler.cpp)
target_link_libraries(vtkSlangTestCompiler PRIVATE vtkSlang::vtkSlang)
target_compile_definitions(vtkSlangTestCompiler
  PRIVATE VTKSLANG_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
          VTKSLANG_TEST_TMP_DIR="${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME vtkSlang.Compiler COMMAND vtkSlangTestCompiler)

add_executable(vtkSlangTestParameters TestParameters.cpp)
target_link_libraries(vtkSlangTestParameters PRIVATE vtkSlang::vtkSlang)
target_compile_definitions(vtkSlangTestParameters PRIVATE VTKSLANG_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME vtkSlang.Parameters COMMAND vtkSlangTestParameters)

if(TARGET vtkslangc)
  set(_data "${CMAKE_CURRENT_SOURCE_DIR}/data")
  function(vtkslangc_test name exit_code regex)
    string(REPLACE ";" "|" _args "${ARGN}")
    add_test(NAME vtkSlang.vtkslangc.${name}
      COMMAND "${CMAKE_COMMAND}" "-DTOOL=$<TARGET_FILE:vtkslangc>" "-DARGS=${_args}"
        "-DEXIT=${exit_code}" "-DREGEX=${regex}" -P "${CMAKE_CURRENT_SOURCE_DIR}/RunTool.cmake")
  endfunction()

  vtkslangc_test(Usage 2 "give one program")
  vtkslangc_test(Program 0 "" ${_data}/surface_host.slang --no-tint)
  vtkslangc_test(Reflect 0 "group 0 binding 0: scene" ${_data}/surface_host.slang --reflect --no-tint)
  vtkslangc_test(Schema 0 "hooks.amount.*Range.*Amount"
    ${_data}/surface_host.slang -m ${_data}/good_hooks.slang -s IShaderHooks=Halve --schema -
    --no-tint)
  vtkslangc_test(BrokenModule 1 "undefinedAmount"
    ${_data}/surface_host.slang -m ${_data}/broken_hooks.slang -s IShaderHooks=Broken --no-tint)
  vtkslangc_test(NonUniformWithoutTint 0 ""
    ${_data}/fragment_host.slang -m ${_data}/nonuniform_hooks.slang -s IShaderHooks=Sparkle
    --no-tint)
  if(VTKSLANG_TINT_EXECUTABLE)
    vtkslangc_test(UniformWithTint 0 ""
      ${_data}/fragment_host.slang -m ${_data}/good_hooks.slang -s IShaderHooks=Halve
      --tint ${VTKSLANG_TINT_EXECUTABLE})
    vtkslangc_test(NonUniformWithTint 1 "uniform control flow"
      ${_data}/fragment_host.slang -m ${_data}/nonuniform_hooks.slang -s IShaderHooks=Sparkle
      --tint ${VTKSLANG_TINT_EXECUTABLE})
  endif()
  add_test(NAME vtkSlang.vtkslangc.Replay
    COMMAND "${CMAKE_COMMAND}" "-DTOOL=$<TARGET_FILE:vtkslangc>" "-DDATA=${_data}"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/replay" -P "${CMAKE_CURRENT_SOURCE_DIR}/ReplayTool.cmake")
endif()
