// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdlib>
#include <mutex>

#ifdef VTKSLANG_USE_SLANG
#  include <slang.h>
#  if __has_include(<slang-tag-version.h>)
#    include <slang-tag-version.h>
#  endif
#  ifndef SLANG_TAG_VERSION
#    define SLANG_TAG_VERSION "unknown"
#  endif
#endif

namespace vtkslang {

namespace {

bool slangDisabledByEnv()
{
  const char* disable = std::getenv("VTKSLANG_DISABLE_SLANG");
  return disable && disable[0] == '1';
}

#ifdef VTKSLANG_USE_SLANG

std::mutex& globalSessionMutex()
{
  static std::mutex m;
  return m;
}

slang::IGlobalSession* globalSession()
{
  static slang::IGlobalSession* session   = nullptr;
  static bool                   attempted = false;
  std::lock_guard<std::mutex> lock(globalSessionMutex());
  if (!attempted) {
    attempted = true;
    if (SLANG_FAILED(slang::createGlobalSession(&session)))
      session = nullptr;
  }
  return session;
}

#endif

}

std::string vtkSlangCompiler::slangVersion()
{
#ifdef VTKSLANG_USE_SLANG
  return SLANG_TAG_VERSION;
#else
  return {};
#endif
}

bool vtkSlangCompiler::slangAvailable()
{
#ifdef VTKSLANG_USE_SLANG
  return !slangDisabledByEnv() && globalSession() != nullptr;
#else
  return false;
#endif
}

}
