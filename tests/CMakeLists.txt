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

