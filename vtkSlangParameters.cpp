// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangParameters.h"

#include <algorithm>
#include <cstring>
#include <tuple>

namespace vtkslang {

namespace {

bool sameAttributes(const std::vector<UserAttribute>& a, const std::vector<UserAttribute>& b)
{
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
    [](const UserAttribute& x, const UserAttribute& y) {
      return x.name == y.name && x.arguments == y.arguments;
    });
}

bool sameFields(const std::vector<UniformField>& a, const std::vector<UniformField>& b)
{
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
    [](const UniformField& x, const UniformField& y) {
      return x.name == y.name && x.offset == y.offset && x.size == y.size && x.type == y.type &&
        x.scalar == y.scalar && x.width == y.width && x.runs == y.runs && x.stride == y.stride &&
        sameAttributes(x.attributes, y.attributes);
    });
}

std::string relativeName(const std::string& name)
{
  const size_t dot = name.find('.');
  return dot == std::string::npos ? name : name.substr(dot + 1);
}

}

bool BindingPlan::operator==(const BindingPlan& other) const
{
  const bool sameBlocks = std::equal(blocks.begin(), blocks.end(), other.blocks.begin(),
    other.blocks.end(), [](const Block& x, const Block& y) {
      return x.name == y.name && x.group == y.group && x.binding == y.binding &&
        x.sizeBytes == y.sizeBytes && sameFields(x.fields, y.fields);
    });
  const bool sameResources = std::equal(resources.begin(), resources.end(),
    other.resources.begin(), other.resources.end(), [](const Resource& x, const Resource& y) {
      return x.name == y.name && x.field == y.field && x.group == y.group &&
        x.binding == y.binding && x.kind == y.kind && x.viewDimension == y.viewDimension &&
        x.texture == y.texture;
    });
  const bool sameUnsupported = std::equal(unsupported.begin(), unsupported.end(),
    other.unsupported.begin(), other.unsupported.end(),
    [](const ReflectedBinding& x, const ReflectedBinding& y) {
      return x.name == y.name && x.group == y.group && x.binding == y.binding && x.kind == y.kind;
    });
  return sameBlocks && sameResources && sameUnsupported;
}

BindingPlan planBindings(
  const Artifact& artifact, const std::function<bool(const ReflectedBinding&)>& hostOwned)
{
  BindingPlan plan;
  int         lastTexture = -1;
  for (const ReflectedBinding& b : artifact.bindings) {
    if (hostOwned && hostOwned(b))
      continue;
    switch (b.kind) {
    case BindingKind::UniformBuffer: {
      BindingPlan::Block block;
      block.name      = b.name;
      block.group     = b.group;
      block.binding   = b.binding;
      block.sizeBytes = (b.sizeBytes + 15) & ~uint64_t(15);
      block.fields    = b.fields;
      plan.blocks.push_back(std::move(block));
      break;
    }
    case BindingKind::SampledTexture:
    case BindingKind::Sampler: {
      BindingPlan::Resource resource;
      resource.name          = b.name;
      resource.field         = relativeName(b.name);
      resource.group         = b.group;
      resource.binding       = b.binding;
      resource.kind          = b.kind;
      resource.viewDimension = b.viewDimension;
      if (b.kind == BindingKind::Sampler)
        resource.texture = lastTexture;
      else
        lastTexture = static_cast<int>(plan.resources.size());
      plan.resources.push_back(std::move(resource));
      break;
    }
    default:
      plan.unsupported.push_back(b);
      break;
    }
  }
  std::sort(plan.blocks.begin(), plan.blocks.end(),
    [](const BindingPlan::Block& a, const BindingPlan::Block& b) {
      return std::tie(a.group, a.binding) < std::tie(b.group, b.binding);
    });
  return plan;
}

std::vector<uint8_t> blockBytes(const BindingPlan::Block& block)
{
  return std::vector<uint8_t>(block.sizeBytes, 0);
}

const UniformField* findField(const BindingPlan::Block& block, const std::string& name)
{
  for (const UniformField& f : block.fields)
    if (f.name == name)
      return &f;
  return nullptr;
}

PackStatus pack(const UniformField& field, const HostValue& value, std::vector<uint8_t>& bytes)
{
  if (field.width == 0 || field.runs == 0)
    return PackStatus::Unsupported;
  if (value.scalar != field.scalar)
    return PackStatus::ScalarMismatch;
  const uint64_t runBytes = uint64_t(value.width) * 4;
  if (!value.data || value.width == 0 || value.runs == 0 || value.width > field.width ||
    value.runs > field.runs ||
    field.offset + (value.runs - 1) * field.stride + runBytes > bytes.size())
    return PackStatus::ShapeMismatch;
  const auto* source = static_cast<const uint8_t*>(value.data);
  for (uint32_t i = 0; i < value.runs; ++i)
    std::memcpy(bytes.data() + field.offset + i * field.stride, source + i * runBytes, runBytes);
  return PackStatus::Packed;
}

std::vector<std::string> unmatched(const BindingPlan& plan, const std::vector<std::string>& names)
{
  std::vector<std::string> result;
  for (const std::string& name : names) {
    bool found = false;
    for (const BindingPlan::Block& block : plan.blocks)
      found = found || findField(block, name) != nullptr;
    if (!found)
      result.push_back(name);
  }
  return result;
}

}
