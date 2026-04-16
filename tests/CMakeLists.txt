# Copyright 2026 Sankhesh Jhaveri
# SPDX-License-Identifier: BSD-3-Clause

add_executable(vtkSlangTestCompiler TestCompiler.cpp)
target_link_libraries(vtkSlangTestCompiler PRIVATE vtkSlang::vtkSlang)
add_test(NAME vtkSlang.Compiler COMMAND vtkSlangTestCompiler)

