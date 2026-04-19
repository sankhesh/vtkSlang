// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, const char* what)
{
  std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    ++g_failures;
}

void testSlang(vtkslang::vtkSlangCompiler& compiler)
{
  const char* user = R"(
import vtkslang_core;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void doubleIt(uint3 id : SV_DispatchThreadID) { data[id.x] = data[id.x] * 2.f * PI; }
)";
  vtkslang::CompileOptions spirv;
  spirv.target = vtkslang::Target::SPIRV;
  vtkslang::Artifact a = compiler.compileSource(user, {}, spirv);
  check(a && a.source == vtkslang::ArtifactSource::Slang && !a.spirv.empty() &&
      a.spirv[0] == 0x07230203u,
    "a source compiles to SPIR-V");

  vtkslang::Artifact u = compiler.compileSource(user);
  check(u && u.wgsl.find("fn doubleIt") != std::string::npos,
    "user source importing vtkslang_core compiles, entry points discovered");

  vtkslang::Artifact named = compiler.compileSource(user, { "doubleIt" });
  check(named && named.source == vtkslang::ArtifactSource::Slang, "explicit entry point list");

  vtkslang::Artifact missing = compiler.compileSource(user, { "nope" });
  check(!missing && missing.diagnostics.find("nope") != std::string::npos,
    "missing entry point is reported");

  vtkslang::Artifact broken = compiler.compileSource("void f( { }");
  check(!broken && !broken.diagnostics.empty(), "syntax error yields diagnostics");

  vtkslang::vtkSlangCompiler second;
  check(static_cast<bool>(second.compileSource(user)),
    "a second compiler shares the global session");
}

void testOptions(vtkslang::vtkSlangCompiler& compiler)
{
  const char* scaled = R"(
#ifndef SCALE
#define SCALE 1.0f
#endif
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void scale(uint3 id : SV_DispatchThreadID) { data[id.x] = data[id.x] * SCALE; }
)";
  vtkslang::CompileOptions withDefine;
  withDefine.defines = { { "SCALE", "7.25f" } };
  vtkslang::Artifact plain   = compiler.compileSource(scaled);
  vtkslang::Artifact defined = compiler.compileSource(scaled, {}, withDefine);
  check(plain && defined && defined.wgsl.find("7.25") != std::string::npos &&
      plain.wgsl.find("7.25") == std::string::npos,
    "defines reach the preprocessor");

  const std::string dir = VTKSLANG_TEST_TMP_DIR;
  {
    std::ofstream out(dir + "/vtkslang_test_helpers.slang");
    out << "module vtkslang_test_helpers;\npublic float triple(float x) { return 3.f * x; }\n";
  }
  const char* importer = R"(
import vtkslang_test_helpers;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID) { data[id.x] = triple(data[id.x]); }
)";
  vtkslang::CompileOptions withPath;
  withPath.searchPaths = { dir };
  check(!compiler.compileSource(importer), "unknown import fails without a search path");
  check(static_cast<bool>(compiler.compileSource(importer, {}, withPath)),
    "search paths resolve imports");
}

void testDisableEnv(vtkslang::vtkSlangCompiler& compiler)
{
#ifdef _WIN32
  _putenv_s("VTKSLANG_DISABLE_SLANG", "1");
#else
  setenv("VTKSLANG_DISABLE_SLANG", "1", 1);
#endif
  check(!vtkslang::vtkSlangCompiler::slangAvailable(), "VTKSLANG_DISABLE_SLANG=1 disables Slang");
  check(!compiler.compileSource("[shader(\"compute\")][numthreads(1,1,1)] void m() {}"),
    "compileSource fails when Slang is disabled");
}

}

int main()
{
  vtkslang::vtkSlangCompiler compiler;
  if (vtkslang::vtkSlangCompiler::slangAvailable()) {
    testSlang(compiler);
    testOptions(compiler);
  } else {
    std::printf("SKIP: Slang unavailable\n");
  }
  testDisableEnv(compiler);

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
