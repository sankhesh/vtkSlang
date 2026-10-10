// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vtkSlangCompiler.h"

#include <string>
#include <vector>

namespace vtkslang {

struct Reproducer
{
  std::string              source;
  std::vector<std::string> programModules;
  std::vector<std::string> entryPoints;
  CompileOptions           options;
};

VTKSLANG_EXPORT std::string reproducerName(const Reproducer& reproducer);

VTKSLANG_EXPORT bool writeReproducer(
  const std::string& directory, const Reproducer& reproducer, const Artifact& artifact);

VTKSLANG_EXPORT bool readReproducer(
  const std::string& directory, Reproducer& reproducer, std::string& error);

}
