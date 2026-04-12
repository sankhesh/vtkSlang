// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vtkSlangExport.h"

#include <string>

namespace vtkslang {

class VTKSLANG_EXPORT vtkSlangCompiler
{
public:
  static bool        slangAvailable();
  static std::string slangVersion();
};

}

using vtkSlangCompiler = vtkslang::vtkSlangCompiler;
