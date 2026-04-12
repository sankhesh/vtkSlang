# Copyright 2026 Sankhesh Jhaveri
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.21)

project(vtkSlang VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN")
set(CMAKE_INSTALL_MESSAGE LAZY)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(FetchContent)

option(VTKSLANG_USE_SLANG "Enable Slang JIT compilation" ON)
set(VTKSLANG_SLANG_ENABLED FALSE)

if(VTKSLANG_USE_SLANG AND NOT EMSCRIPTEN)
  find_package(Slang QUIET)

  if(NOT Slang_FOUND)
    set(_SLANG_VER "2026.13.1")
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
      set(_SLANG_ARCHIVE "slang-${_SLANG_VER}-macos-aarch64.zip")
    elseif(APPLE)
      set(_SLANG_ARCHIVE "slang-${_SLANG_VER}-macos-x86_64.zip")
    elseif(WIN32)
      set(_SLANG_ARCHIVE "slang-${_SLANG_VER}-windows-x86_64.zip")
    else()
      set(_SLANG_ARCHIVE "slang-${_SLANG_VER}-linux-x86_64.zip")
    endif()

    FetchContent_Declare(slang_dist
      URL "https://github.com/shader-slang/slang/releases/download/v${_SLANG_VER}/${_SLANG_ARCHIVE}"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(slang_dist)

    set(_SLANG_INC "${slang_dist_SOURCE_DIR}/include")
    if(APPLE)
      set(_SLANG_LIB "${slang_dist_SOURCE_DIR}/lib/libslang.dylib")
    elseif(WIN32)
      set(_SLANG_LIB "${slang_dist_SOURCE_DIR}/bin/slang.dll")
    else()
      set(_SLANG_LIB "${slang_dist_SOURCE_DIR}/lib/libslang.so")
    endif()

    if(EXISTS "${_SLANG_LIB}" AND EXISTS "${_SLANG_INC}/slang.h")
      add_library(Slang::Slang SHARED IMPORTED GLOBAL)
      set_target_properties(Slang::Slang PROPERTIES
        IMPORTED_LOCATION             "${_SLANG_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${_SLANG_INC}")
      set(Slang_FOUND TRUE)
    endif()
  endif()

  if(Slang_FOUND)
    set(VTKSLANG_SLANG_ENABLED TRUE)
    message(STATUS "vtkSlang: Slang found — JIT compilation enabled (SPIR-V + WGSL output)")
  else()
    message(STATUS "vtkSlang: Slang not found — compiling is unavailable")
  endif()
else()
  message(STATUS "vtkSlang: Slang disabled — compiling is unavailable")
endif()

set(_PUBLIC_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vtkSlangVersion.h.in"
  "${_PUBLIC_INCLUDE_DIR}/vtkSlangVersion.h"
  @ONLY)
foreach(_header IN ITEMS vtkSlang.h vtkSlangCompiler.h)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/${_header}" "${_PUBLIC_INCLUDE_DIR}/${_header}"
    COPYONLY)
endforeach()

add_library(vtkSlang SHARED
  vtkSlangCompiler.cpp)

add_library(vtkSlang::vtkSlang ALIAS vtkSlang)

target_include_directories(vtkSlang
  PUBLIC
    $<BUILD_INTERFACE:${_PUBLIC_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/vtkSlang>)

if(VTKSLANG_SLANG_ENABLED)
  target_link_libraries(vtkSlang PRIVATE Slang::Slang)
  target_compile_definitions(vtkSlang PRIVATE VTKSLANG_USE_SLANG)
endif()

include(GenerateExportHeader)
generate_export_header(vtkSlang
  EXPORT_MACRO_NAME VTKSLANG_EXPORT
  EXPORT_FILE_NAME  "${_PUBLIC_INCLUDE_DIR}/vtkSlangExport.h")

install(TARGETS vtkSlang
  EXPORT  vtkSlangTargets
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vtkSlang)

install(FILES
  "${_PUBLIC_INCLUDE_DIR}/vtkSlang.h"
  "${_PUBLIC_INCLUDE_DIR}/vtkSlangCompiler.h"
  "${_PUBLIC_INCLUDE_DIR}/vtkSlangVersion.h"
  "${_PUBLIC_INCLUDE_DIR}/vtkSlangExport.h"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vtkSlang)

install(EXPORT vtkSlangTargets
  FILE      vtkSlangTargets.cmake
  NAMESPACE vtkSlang::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vtkSlang)

export(EXPORT vtkSlangTargets
  FILE      "${CMAKE_CURRENT_BINARY_DIR}/vtkSlangTargets.cmake"
  NAMESPACE vtkSlang::)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vtkSlangConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkSlangConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vtkSlang")

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/vtkSlangConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/vtkSlangConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/vtkSlangConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/vtkSlang")
