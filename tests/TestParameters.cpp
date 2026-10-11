// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlang.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
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

std::string readSource(const std::string& path)
{
  std::ifstream     in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

const char* kBlocks = R"(
struct Extra { float4 v; SamplerState early; RWStructuredBuffer<float4> data; };
struct Material { TINT tint; float3x3 colorMatrix; Texture2D ramp; SamplerState rampSampler; };
[vk::binding(0, 0)] RWStructuredBuffer<float4> outColor;
ParameterBlock<Extra> extra;
ParameterBlock<Material> material;
[shader("compute")] [numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  float4 r = material.ramp.SampleLevel(material.rampSampler, float2(0.5f), 0.f) +
    material.ramp.SampleLevel(extra.early, float2(0.25f), 0.f);
  r.rgb = mul(material.colorMatrix, r.rgb) + float3(material.tint.rgb);
  extra.data[id.x] = r + extra.v;
  outColor[id.x] = r;
}
)";

const char* kShapes = R"(
struct Item { float3 a; float b; };
struct Params {
  float f; float3 v3; float4 v4; int2 i2; uint u;
  float2x2 m22; float3x3 m33; float3 a3[4]; Item items[2];
};
ConstantBuffer<Params> params;
RWStructuredBuffer<float4> outColor;
[shader("compute")] [numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  Params p = params;
  float4 r = float4(p.f, p.v3) + p.v4 + float4(mul(p.m33, p.v3), 1.f) +
    float4(mul(p.m22, p.v4.xy), p.i2) + float4(p.u, 0.f, 0.f, 0.f);
  r.xyz += p.a3[id.x];
  r.x += p.items[id.x].b;
  outColor[0] = r;
}
)";

vtkslang::BindingPlan planFor(vtkslang::vtkSlangCompiler& compiler, const std::string& source)
{
  vtkslang::Artifact a = compiler.compileSource(source);
  if (!a)
    std::printf("%s", a.diagnostics.c_str());
  return vtkslang::planBindings(
    a, [](const vtkslang::ReflectedBinding& b) { return b.name == "outColor"; });
}

std::string withTint(const char* type)
{
  return std::string("#define TINT ") + type + "\n" + kBlocks;
}

void testPlan(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const vtkslang::BindingPlan plan = planFor(compiler, withTint("float4"));
  check(plan.blocks.size() == 2 && plan.blocks[0].name == "extra" && plan.blocks[0].group == 1 &&
      plan.blocks[1].name == "material" && plan.blocks[1].group == 2 &&
      plan.blocks[1].binding == 0 && plan.blocks[1].sizeBytes % 16 == 0 &&
      vtkslang::findField(plan.blocks[1], "colorMatrix"),
    "uniform blocks are sorted by group and binding, padded to 16 bytes");
  check(plan.resources.size() == 3 && plan.resources[0].field == "early" &&
      plan.resources[0].kind == BindingKind::Sampler && plan.resources[0].texture == -1 &&
      plan.resources[1].name == "material.ramp" && plan.resources[1].field == "ramp" &&
      plan.resources[1].kind == BindingKind::SampledTexture &&
      plan.resources[1].viewDimension == vtkslang::TextureDimension::D2 &&
      plan.resources[2].field == "rampSampler" && plan.resources[2].texture == 1,
    "a sampler is paired with the texture declared before it");
  check(plan.unsupported.size() == 1 && plan.unsupported[0].name == "extra.data",
    "storage buffers are left for the host to refuse");
  check(planFor(compiler, withTint("float4")) == plan, "the same program gives an equal plan");
  check(planFor(compiler, withTint("int4")) != plan, "a field's scalar type is part of the plan");
  check(vtkslang::unmatched(plan, { "v", "nope", "tint" }) == std::vector<std::string>{ "nope" },
    "names that match no field are reported");
}

std::vector<uint8_t> at(const std::vector<uint8_t>& bytes, uint64_t offset, size_t count)
{
  return std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + offset + count);
}

std::vector<uint8_t> asBytes(const std::vector<float>& values)
{
  std::vector<uint8_t> out(values.size() * 4);
  std::memcpy(out.data(), values.data(), out.size());
  return out;
}

void testPack(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::HostValue;
  using vtkslang::PackStatus;
  using vtkslang::ScalarType;
  const vtkslang::BindingPlan plan = planFor(compiler, kShapes);
  if (plan.blocks.size() != 1) {
    check(false, "the shapes program has one uniform block");
    return;
  }
  const auto& block = plan.blocks[0];
  auto        bytes = vtkslang::blockBytes(block);
  check(bytes.size() == block.sizeBytes && bytes.size() % 16 == 0 &&
      std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }),
    "block bytes start zeroed at the padded size");

  const auto* f   = vtkslang::findField(block, "f");
  const auto* m22 = vtkslang::findField(block, "m22");
  const auto* m33 = vtkslang::findField(block, "m33");
  const auto* a3  = vtkslang::findField(block, "a3");
  const auto* v3  = vtkslang::findField(block, "v3");
  const auto* v4  = vtkslang::findField(block, "v4");
  const auto* i2  = vtkslang::findField(block, "i2");
  const auto* items = vtkslang::findField(block, "items");
  if (!f || !m22 || !m33 || !a3 || !v3 || !v4 || !i2 || !items) {
    check(false, "the shapes program reflects every field");
    return;
  }

  const float scalar = 2.5f;
  check(vtkslang::pack(*f, HostValue{ ScalarType::Float32, 1, 1, &scalar }, bytes) ==
        PackStatus::Packed &&
      at(bytes, f->offset, 4) == asBytes({ 2.5f }),
    "a scalar lands at its offset");

  const float m2[] = { 1, 2, 3, 4 };
  check(vtkslang::pack(*m22, HostValue{ ScalarType::Float32, 2, 2, m2 }, bytes) ==
        PackStatus::Packed &&
      at(bytes, m22->offset, 32) == asBytes({ 1, 2, 0, 0, 3, 4, 0, 0 }),
    "float2x2 columns are 16 bytes apart");

  const float m3[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
  check(vtkslang::pack(*m33, HostValue{ ScalarType::Float32, 3, 3, m3 }, bytes) ==
        PackStatus::Packed &&
      at(bytes, m33->offset, 48) == asBytes({ 1, 2, 3, 0, 4, 5, 6, 0, 7, 8, 9, 0 }),
    "float3x3 columns are padded to 16 bytes");

  const float two[] = { 1, 2, 3, 4, 5, 6 };
  check(vtkslang::pack(*a3, HostValue{ ScalarType::Float32, 3, 2, two }, bytes) ==
        PackStatus::Packed &&
      at(bytes, a3->offset, 64) == asBytes({ 1, 2, 3, 0, 4, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0 }),
    "a shorter array fills its first elements and leaves the rest zero");

  const float rgb[] = { 0.1f, 0.2f, 0.3f };
  check(vtkslang::pack(*v4, HostValue{ ScalarType::Float32, 3, 1, rgb }, bytes) ==
        PackStatus::Packed &&
      at(bytes, v4->offset, 16) == asBytes({ 0.1f, 0.2f, 0.3f, 0 }),
    "a narrower vector fills the first components");

  const float rgba[] = { 1, 1, 1, 1 };
  const auto  before = bytes;
  check(vtkslang::pack(*v3, HostValue{ ScalarType::Float32, 4, 1, rgba }, bytes) ==
        PackStatus::ShapeMismatch &&
      vtkslang::pack(*m22, HostValue{ ScalarType::Float32, 2, 3, m3 }, bytes) ==
        PackStatus::ShapeMismatch &&
      bytes == before,
    "a value wider or longer than its field is refused untouched");

  const int ints[] = { 7, -8 };
  check(vtkslang::pack(*i2, HostValue{ ScalarType::Int32, 2, 1, ints }, bytes) ==
        PackStatus::Packed &&
      vtkslang::pack(*i2, HostValue{ ScalarType::Float32, 2, 1, rgb }, bytes) ==
        PackStatus::ScalarMismatch &&
      vtkslang::pack(*f, HostValue{ ScalarType::Int32, 1, 1, ints }, bytes) ==
        PackStatus::ScalarMismatch,
    "a value of another scalar type is refused");
  int packed[2] = {};
  std::memcpy(packed, bytes.data() + i2->offset, sizeof packed);
  check(packed[0] == 7 && packed[1] == -8, "int vectors keep their bits");

  check(vtkslang::pack(*items, HostValue{ ScalarType::Float32, 4, 1, rgba }, bytes) ==
      PackStatus::Unsupported,
    "a field with no shape is refused");
  check(!vtkslang::findField(block, "nope"), "an unknown name has no field");
}

const char* kRampHooks = R"(
module schema_hooks;
import vtkslang_hooks;
public struct Ramp : IShaderHooks {
  [Range(0, 2)] [Label("Tint")] float3 tint;
  Texture2D ramp;
  SamplerState rampSampler;
  public override void color(inout FragmentContext f) {
    f.diffuseColor = ramp.SampleLevel(rampSampler, f.uv, 0.f).rgb * tint;
  }
};
)";

bool contains(const std::string& text, const std::string& part)
{
  const bool found = text.find(part) != std::string::npos;
  if (!found)
    std::printf("  missing: %s\n", part.c_str());
  return found;
}

void testSchema(vtkslang::vtkSlangCompiler& compiler)
{
  vtkslang::CompileOptions o;
  o.modules         = { kRampHooks };
  o.specializations = { { "IShaderHooks", "Ramp" } };
  vtkslang::Artifact a =
    compiler.compileSource(readSource(VTKSLANG_SOURCE_DIR "/tests/data/surface_host.slang"), {}, o);
  if (!a)
    std::printf("%s", a.diagnostics.c_str());
  const std::string schema = vtkslang::parameterSchema(vtkslang::planBindings(
    a, [](const vtkslang::ReflectedBinding& b) { return b.group == 0; }));
  check(contains(schema, R"("name": "hooks.tint", "type": "float3", "scalar": "float", "width": 3, "runs": 1)") &&
      contains(schema, R"({"name": "Range", "arguments": ["0", "2"]}, {"name": "Label", "arguments": ["Tint"]})"),
    "the schema lists fields with their shape and attributes");
  check(contains(schema, R"("field": "hooks.ramp", "kind": "texture", "dimension": "2d")") &&
      contains(schema, R"("field": "hooks.rampSampler", "kind": "sampler")") &&
      contains(schema, R"("texture": 0})"),
    "the schema lists textures and pairs samplers with them");

  vtkslang::BindingPlan manual;
  manual.blocks.push_back({ "a\"b\\c\n", 0, 0, 16, {} });
  check(contains(vtkslang::parameterSchema(manual), R"("name": "a\"b\\c\u000a")"),
    "names are escaped");
  check(vtkslang::parameterSchema(vtkslang::BindingPlan()) ==
      "{\n  \"blocks\": [],\n  \"resources\": []\n}\n",
    "an empty plan has empty lists");
}

}

int main()
{
  if (!vtkslang::vtkSlangCompiler::slangAvailable()) {
    std::printf("SKIP: Slang unavailable\n");
    return 0;
  }
  vtkslang::vtkSlangCompiler compiler;
  testPlan(compiler);
  testPack(compiler);
  testSchema(compiler);
  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
