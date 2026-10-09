// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vtkSlangCompiler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vtkslang {

struct BindingPlan
{
  struct Block
  {
    std::string               name;
    uint32_t                  group     = 0;
    uint32_t                  binding   = 0;
    uint64_t                  sizeBytes = 0;
    std::vector<UniformField> fields;
  };

  struct Resource
  {
    std::string      name;
    std::string      field;
    uint32_t         group         = 0;
    uint32_t         binding       = 0;
    BindingKind      kind          = BindingKind::Other;
    TextureDimension viewDimension = TextureDimension::None;
    int              texture       = -1;
  };

  std::vector<Block>            blocks;
  std::vector<Resource>         resources;
  std::vector<ReflectedBinding> unsupported;

  VTKSLANG_EXPORT bool operator==(const BindingPlan& other) const;
  bool operator!=(const BindingPlan& other) const { return !(*this == other); }
};

struct HostValue
{
  ScalarType  scalar = ScalarType::Float32;
  uint32_t    width  = 1;
  uint32_t    runs   = 1;
  const void* data   = nullptr;
};

enum class PackStatus
{
  Packed,
  NoField,
  Unsupported,
  ScalarMismatch,
  ShapeMismatch,
};

VTKSLANG_EXPORT BindingPlan planBindings(
  const Artifact& artifact, const std::function<bool(const ReflectedBinding&)>& hostOwned);

VTKSLANG_EXPORT std::vector<uint8_t> blockBytes(const BindingPlan::Block& block);

VTKSLANG_EXPORT const UniformField* findField(
  const BindingPlan::Block& block, const std::string& name);

VTKSLANG_EXPORT PackStatus pack(
  const UniformField& field, const HostValue& value, std::vector<uint8_t>& bytes);

VTKSLANG_EXPORT std::vector<std::string> unmatched(
  const BindingPlan& plan, const std::vector<std::string>& names);

}
