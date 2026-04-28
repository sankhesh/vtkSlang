// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::string readFile(const std::string& path)
{
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

vtkslang::Program surfaceHost()
{
  vtkslang::Program p;
  p.name   = "surface_host";
  p.source = readFile(VTKSLANG_SOURCE_DIR "/tests/data/surface_host.slang");
  vtkslang::Artifact precompiled;
  precompiled.target = vtkslang::Target::WGSL;
  precompiled.wgsl   = "// precompiled surface_host\n";
  p.precompiled.push_back(precompiled);
  return p;
}

void testSlang(vtkslang::vtkSlangCompiler& compiler)
{
  vtkslang::CompileOptions spirv;
  spirv.target = vtkslang::Target::SPIRV;
  vtkslang::Artifact a = compiler.compileProgram(surfaceHost(), spirv);
  check(a && a.source == vtkslang::ArtifactSource::Slang && !a.spirv.empty() &&
      a.spirv[0] == 0x07230203u,
    "a program compiles to SPIR-V");

  const char* user = R"(
import vtkslang_core;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void doubleIt(uint3 id : SV_DispatchThreadID) { data[id.x] = data[id.x] * 2.f * PI; }
)";
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

void testProgramWithoutSlang(vtkslang::vtkSlangCompiler& compiler)
{
  const vtkslang::Program p = surfaceHost();
  vtkslang::Artifact      a = compiler.compileProgram(p);
  check(a && a.source == vtkslang::ArtifactSource::Precompiled && a.wgsl == p.precompiled[0].wgsl,
    "a plain program request returns its precompiled artifact");
}

void testProgram(vtkslang::vtkSlangCompiler& compiler)
{
  const vtkslang::Program p = surfaceHost();

  vtkslang::CompileOptions spirv;
  spirv.target         = vtkslang::Target::SPIRV;
  vtkslang::Artifact s = compiler.compileProgram(p, spirv);
  check(s && s.source == vtkslang::ArtifactSource::Slang && !s.spirv.empty(),
    "a target without a precompiled artifact compiles with Slang");

  vtkslang::CompileOptions defined;
  defined.defines      = { { "UNUSED", "1" } };
  vtkslang::Artifact v = compiler.compileProgram(p, defined);
  check(v && v.source == vtkslang::ArtifactSource::Slang,
    "a program request with a define compiles with Slang");

  vtkslang::Program helper;
  helper.name    = "with_helper";
  helper.modules = { "module host_util;\npublic float brighten(float x) { return 2.f * x; }\n" };
  helper.source  = R"(
import host_util;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) { data[id.x] = brighten(data[id.x]); }
)";
  vtkslang::CompileOptions user;
  user.modules = { "module user_util;\nimport host_util;\npublic float twice(float x) { return brighten(brighten(x)); }\n" };
  check(static_cast<bool>(compiler.compileProgram(helper, user)),
    "program modules load before user modules, which can import them");
}

void testModules(vtkslang::vtkSlangCompiler& compiler)
{
  const char* user = R"(
import test_helpers;
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID) { data[id.x] = twice(data[id.x]); }
)";
  vtkslang::CompileOptions original;
  original.modules = { "module test_helpers;\npublic float twice(float x) { return 2.f * x; }\n" };
  vtkslang::Artifact a = compiler.compileSource(user, {}, original);
  check(a && a.wgsl.find("fn run") != std::string::npos,
    "a source imports a consumer module by its declared name");

  vtkslang::CompileOptions edited;
  edited.modules = { "module test_helpers;\npublic float twice(float x) { return 3.f * x; }\n" };
  vtkslang::Artifact b = compiler.compileSource(user, {}, edited);
  check(b && b.wgsl != a.wgsl, "a module redefined with new source is recompiled, not reused");
  check(compiler.compileSource(user, {}, original).wgsl == a.wgsl,
    "switching back to the original module source works");

  vtkslang::CompileOptions reserved;
  reserved.modules = { "module vtkslang_core;\npublic float f() { return 1.f; }\n" };
  vtkslang::Artifact clash = compiler.compileSource(user, {}, reserved);
  check(!clash && clash.diagnostics.find("reserved") != std::string::npos,
    "library module names are reserved");
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
  check(compiler.compileProgram(surfaceHost()).source == vtkslang::ArtifactSource::Precompiled,
    "precompiled programs still work when Slang is disabled");
}

}

int main()
{
  vtkslang::vtkSlangCompiler compiler;
  testProgramWithoutSlang(compiler);

  if (vtkslang::vtkSlangCompiler::slangAvailable()) {
    testProgram(compiler);
    testSlang(compiler);
    testOptions(compiler);
    testModules(compiler);
  } else {
    std::printf("SKIP: Slang unavailable\n");
  }
  testDisableEnv(compiler);

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
