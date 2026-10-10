// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vtkSlangExport.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vtkslang {

enum class Target
{
  WGSL,
  SPIRV,
};

enum class ArtifactSource
{
  None,
  Slang,
  Precompiled,
};

enum class BindingKind
{
  UniformBuffer,
  ReadOnlyStorageBuffer,
  StorageBuffer,
  SampledTexture,
  StorageTexture,
  Sampler,
  Other,
};

enum class TextureDimension
{
  None,
  D1,
  D2,
  D2Array,
  D3,
  Cube,
  CubeArray,
};

enum class ScalarType
{
  Unknown,
  Float32,
  Int32,
  UInt32,
};

struct UserAttribute
{
  std::string              name;
  std::vector<std::string> arguments;
};

struct UniformField
{
  std::string                name;
  uint64_t                   offset = 0;
  uint64_t                   size   = 0;
  std::string                type;
  ScalarType                 scalar = ScalarType::Unknown;
  uint32_t                   width  = 0;
  uint32_t                   runs   = 0;
  uint64_t                   stride = 0;
  std::vector<UserAttribute> attributes;
};

struct ReflectedBinding
{
  std::string name;
  uint32_t    group   = 0;
  uint32_t    binding = 0;
  BindingKind kind    = BindingKind::Other;
  bool        used    = true;
  uint64_t    sizeBytes = 0;
  TextureDimension viewDimension = TextureDimension::None;
  std::vector<UniformField> fields;
};

struct Artifact
{
  Target                        target = Target::WGSL;
  ArtifactSource                source = ArtifactSource::None;
  std::string                   wgsl;
  std::vector<uint32_t>         spirv;
  std::vector<ReflectedBinding> bindings;
  std::string                   diagnostics;
  bool fromCache = false;

  explicit operator bool() const { return source != ArtifactSource::None; }
};

struct CompileOptions
{
  Target target = Target::WGSL;

  std::vector<std::pair<std::string, std::string>> defines;

  std::vector<std::string> searchPaths;

  bool reflect = false;

  std::vector<std::pair<std::string, std::string>> specializations;

  std::vector<std::string> modules;

  std::string label;
};

struct Program
{
  std::string              name;
  std::string              source;
  std::vector<std::string> modules;
  std::vector<std::string> entryPoints;
  std::vector<Artifact>    precompiled;
};

class VTKSLANG_EXPORT vtkSlangCompiler
{
public:
  vtkSlangCompiler();
  ~vtkSlangCompiler();

  vtkSlangCompiler(const vtkSlangCompiler&)            = delete;
  vtkSlangCompiler& operator=(const vtkSlangCompiler&) = delete;
  vtkSlangCompiler(vtkSlangCompiler&&) noexcept;
  vtkSlangCompiler& operator=(vtkSlangCompiler&&) noexcept;

  static bool        slangAvailable();
  static std::string slangVersion();

  static std::vector<std::string> libraryModules();
  static std::string              librarySource(const std::string& name);
  static bool                     writeLibraryModules(const std::string& directory);

  Artifact compileSource(const std::string& source,
                         const std::vector<std::string>& entryPoints = {},
                         const CompileOptions& options = {});

  Artifact compileProgram(const Program& program, const CompileOptions& options = {});

  void setCacheDirectory(const std::string& directory);

  void setDumpDirectory(const std::string& directory);

private:
  Artifact compileText(const std::string& source, const std::vector<std::string>& programModules,
                       const std::vector<std::string>& entryPoints, const CompileOptions& options);

  struct SlangState;
  std::unique_ptr<SlangState> m_slang;
};

}

using vtkSlangCompiler = vtkslang::vtkSlangCompiler;
