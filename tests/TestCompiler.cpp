// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
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

const char* kHostOverrides = R"(
module host_overrides;
import vtkslang_shading;
public struct Tint : IMaterial {
  float3 tint;
  public MaterialSample evaluate(SurfaceSample s) {
    MaterialSample m = HostDrivenMaterial().evaluate(s);
    m.baseColor *= tint;
    return m;
  }
};
)";

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

const vtkslang::ReflectedBinding* findBinding(const vtkslang::Artifact& a, uint32_t binding)
{
  for (const auto& b : a.bindings)
    if (b.group == 0 && b.binding == binding)
      return &b;
  return nullptr;
}

void testReflection(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  vtkslang::CompileOptions reflect;
  reflect.reflect = true;

  vtkslang::Artifact host = compiler.compileProgram(surfaceHost(), reflect);
  check(host && host.source == vtkslang::ArtifactSource::Slang && host.bindings.size() == 3,
    "the host reflects 3 bindings");
  const BindingKind expected[3] = { BindingKind::UniformBuffer, BindingKind::SampledTexture,
    BindingKind::Sampler };
  const char* names[3] = { "scene", "colorTexture", "colorSampler" };
  bool kindsOk = true;
  for (uint32_t i = 0; i < 3; ++i) {
    const auto* b = findBinding(host, i);
    kindsOk = kindsOk && b && b->kind == expected[i] && b->used && b->name == names[i];
  }
  check(kindsOk, "binding kinds and names are reflected");
  const auto* scene = findBinding(host, 0);
  check(scene && scene->sizeBytes == 96 && scene->fields.size() == 3 &&
      scene->fields[1].name == "lightDirection" && scene->fields[1].offset == 64,
    "the host's uniform block is 96 bytes with its fields");

  const char* partlyUsed = R"(
[vk::binding(0, 0)] RWStructuredBuffer<float> written;
[vk::binding(1, 0)] StructuredBuffer<float>   ignored;
[vk::binding(0, 1)] Texture2D                 tex;
[vk::binding(1, 1)] SamplerState              samp;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  written[id.x] = tex.SampleLevel(samp, float2(0.5f, 0.5f), 0.f).r;
}
)";
  vtkslang::Artifact p = compiler.compileSource(partlyUsed);
  const vtkslang::ReflectedBinding* ignored = nullptr;
  const vtkslang::ReflectedBinding* tex     = nullptr;
  const vtkslang::ReflectedBinding* samp    = nullptr;
  for (const auto& b : p.bindings) {
    if (b.name == "ignored") ignored = &b;
    if (b.name == "tex") tex = &b;
    if (b.name == "samp") samp = &b;
  }
  check(ignored && !ignored->used, "unreferenced binding is reported unused");
  check(tex && tex->group == 1 && tex->binding == 0 && tex->kind == BindingKind::SampledTexture &&
      tex->used,
    "texture binding: group, slot, kind");
  check(samp && samp->kind == BindingKind::Sampler, "sampler binding kind");

  check(compiler.compileProgram(surfaceHost()).bindings.empty(),
    "precompiled artifacts carry no reflection unless asked");
}

std::set<std::string> wgslBindings(const std::string& wgsl)
{
  std::set<std::string> out;
  std::regex decl(R"(@binding\((\d+)\) @group\((\d+)\) var(<[^>]*>)? \w+ : ([\w_]+))");
  for (std::sregex_iterator it(wgsl.begin(), wgsl.end(), decl), end; it != end; ++it) {
    const std::string access = (*it)[3];
    const std::string type   = (*it)[4];
    std::string kind = "other";
    if (access == "<uniform>") kind = "uniform";
    else if (access == "<storage, read>") kind = "ro-storage";
    else if (access == "<storage, read_write>") kind = "storage";
    else if (type.rfind("texture_storage", 0) == 0) kind = "storage-texture";
    else if (type.rfind("texture", 0) == 0) kind = "texture";
    else if (type.rfind("sampler", 0) == 0) kind = "sampler";
    out.insert((*it)[2].str() + ":" + (*it)[1].str() + ":" + kind);
  }
  return out;
}

std::set<std::string> reflectedBindings(const vtkslang::Artifact& a)
{
  using vtkslang::BindingKind;
  std::set<std::string> out;
  for (const auto& b : a.bindings) {
    if (!b.used)
      continue;
    const char* kind = "other";
    switch (b.kind) {
      case BindingKind::UniformBuffer: kind = "uniform"; break;
      case BindingKind::ReadOnlyStorageBuffer: kind = "ro-storage"; break;
      case BindingKind::StorageBuffer: kind = "storage"; break;
      case BindingKind::SampledTexture: kind = "texture"; break;
      case BindingKind::StorageTexture: kind = "storage-texture"; break;
      case BindingKind::Sampler: kind = "sampler"; break;
      default: break;
    }
    out.insert(std::to_string(b.group) + ":" + std::to_string(b.binding) + ":" + kind);
  }
  return out;
}

const vtkslang::ReflectedBinding* byName(const vtkslang::Artifact& a, const std::string& name)
{
  for (const auto& b : a.bindings)
    if (b.name == name)
      return &b;
  return nullptr;
}

const vtkslang::UniformField* field(const vtkslang::ReflectedBinding* b, const std::string& name)
{
  if (b)
    for (const auto& f : b->fields)
      if (f.name == name)
        return &f;
  return nullptr;
}

void testNestedReflection(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const char* nested = R"(
struct Material { float4 tint; float bands; Texture2D ramp; SamplerState samp; };
struct Actor { float4x4 model; float4 color; Material material; };
[vk::binding(0, 0)] RWStructuredBuffer<float4> outColor;
ParameterBlock<Actor> actor;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  float q = floor(actor.color.r * actor.material.bands) / actor.material.bands;
  float4 r = actor.material.ramp.SampleLevel(actor.material.samp, float2(q, 0.5f), 0.f);
  outColor[id.x] = mul(actor.model, actor.material.tint * r);
}
)";
  vtkslang::Artifact a = compiler.compileSource(nested);
  const auto* block = byName(a, "actor");
  const auto* ramp  = byName(a, "actor.material.ramp");
  const auto* samp  = byName(a, "actor.material.samp");
  check(block && block->group == 1 && block->binding == 0 && block->kind == BindingKind::UniformBuffer,
    "ParameterBlock opens group 1 with its uniform buffer at binding 0");
  check(ramp && ramp->group == 1 && ramp->binding == 1 && ramp->kind == BindingKind::SampledTexture &&
      samp && samp->group == 1 && samp->binding == 2 && samp->kind == BindingKind::Sampler,
    "resources nested in the block follow its uniform buffer");
  const auto* model = field(block, "model");
  const auto* color = field(block, "color");
  const auto* tint  = field(block, "material.tint");
  const auto* bands = field(block, "material.bands");
  check(model && model->offset == 0 && model->size == 64 && color && color->offset == 64 && tint &&
      tint->offset == 80 && bands && bands->offset == 96 && bands->size == 4,
    "uniform fields carry nested paths and byte offsets");
  check(a && reflectedBindings(a) == wgslBindings(a.wgsl),
    "nested reflection matches the WGSL declarations exactly");

  vtkslang::CompileOptions hosted;
  hosted.reflect         = true;
  hosted.modules         = { kHostOverrides };
  hosted.specializations = { { "IMaterial", "Tint" } };
  vtkslang::Artifact h   = compiler.compileProgram(surfaceHost(), hosted);
  check(h && h.bindings.size() == 4 && reflectedBindings(h) == wgslBindings(h.wgsl),
    "a host program's reflection matches its WGSL declarations exactly");
}

void testTextureDimensions()
{
  using vtkslang::TextureDimension;
  const char* src = R"(
[vk::binding(0, 0)] Texture1D t1;
[vk::binding(1, 0)] Texture2D t2;
[vk::binding(2, 0)] Texture2DArray t2a;
[vk::binding(3, 0)] Texture3D t3;
[vk::binding(4, 0)] TextureCube tc;
[vk::binding(5, 0)] TextureCubeArray tca;
[vk::binding(6, 0)] SamplerState s;
[vk::binding(7, 0)] RWStructuredBuffer<float4> outColor;
[shader("compute")] [numthreads(1, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  outColor[id.x] = t1.SampleLevel(s, 0.5f, 0.f) + t2.SampleLevel(s, float2(0.5f), 0.f) +
    t2a.SampleLevel(s, float3(0.5f), 0.f) + t3.SampleLevel(s, float3(0.5f), 0.f) +
    tc.SampleLevel(s, float3(1.f), 0.f) + tca.SampleLevel(s, float4(1.f), 0.f);
}
)";
  const std::string dir = std::string(VTKSLANG_TEST_TMP_DIR) + "/cache-dimensions";
  std::error_code   ec;
  std::filesystem::remove_all(dir, ec);
  for (int pass = 0; pass < 2; ++pass) {
    vtkslang::vtkSlangCompiler c;
    c.setCacheDirectory(dir);
    vtkslang::Artifact a = c.compileSource(src);
    auto dim = [&a](const char* name) {
      const auto* b = byName(a, name);
      return b ? b->viewDimension : TextureDimension::None;
    };
    const bool expected = dim("t1") == TextureDimension::D1 && dim("t2") == TextureDimension::D2 &&
      dim("t2a") == TextureDimension::D2Array && dim("t3") == TextureDimension::D3 &&
      dim("tc") == TextureDimension::Cube && dim("tca") == TextureDimension::CubeArray &&
      dim("s") == TextureDimension::None && dim("outColor") == TextureDimension::None;
    check(a && a.fromCache == (pass == 1) && expected,
      pass == 0 ? "textures report their view dimension"
                : "view dimensions survive the disk cache");
  }
  std::filesystem::remove_all(dir, ec);
}

void testSpecialization(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const char* shading = R"(
module test_shading;
public struct Sample { public float4 color; };
public interface ITestMaterial { float4 shade(Sample s); };
public interface ITestLight { float scale(); };
public struct PlainMaterial : ITestMaterial { public float4 shade(Sample s) { return s.color; } };
public struct UnitLight : ITestLight { public float scale() { return 1.f; } };
)";
  const char* shader = R"(
import test_shading;
type_param L : ITestLight;
type_param M : ITestMaterial;
typealias Default_ITestLight    = UnitLight;
typealias Default_ITestMaterial = PlainMaterial;
struct ActorBlock<T : ITestMaterial> { float4 color; T material; };
[vk::binding(0, 0)] RWStructuredBuffer<float4> outColor;
ParameterBlock<ActorBlock<M>> actor;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID)
{
  Sample s;
  s.color = actor.color;
  L light = L();
  outColor[id.x] = actor.material.shade(s) * light.scale();
}
)";
  const char* toon = R"(
import test_shading;
public struct ToonMaterial : ITestMaterial {
  public float4 tint; public float bands; public Texture2D ramp; public SamplerState samp;
  public float4 shade(Sample s) {
    float q = floor(s.color.r * bands) / bands;
    return tint * ramp.SampleLevel(samp, float2(q, 0.5f), 0.f);
  }
};
)";
  vtkslang::CompileOptions base;
  base.modules = { shading };

  vtkslang::Artifact defaults = compiler.compileSource(shader, {}, base);
  check(defaults && !byName(defaults, "actor.material.ramp") &&
      reflectedBindings(defaults) == wgslBindings(defaults.wgsl),
    "type params fall back to the shader's Default_ typealiases");
  if (!defaults)
    std::printf("%s", defaults.diagnostics.c_str());

  vtkslang::CompileOptions custom = base;
  custom.modules.push_back(toon);
  custom.specializations = { { "ITestMaterial", "ToonMaterial" } };
  vtkslang::Artifact t = compiler.compileSource(shader, {}, custom);
  const auto* block = byName(t, "actor");
  const auto* ramp  = byName(t, "actor.material.ramp");
  check(t && ramp && ramp->group == 1 && ramp->binding == 1 &&
      ramp->kind == BindingKind::SampledTexture && field(block, "material.tint") &&
      field(block, "material.bands") && reflectedBindings(t) == wgslBindings(t.wgsl),
    "a consumer type specializes one interface; its parameters land in the actor block");
  if (!t)
    std::printf("%s", t.diagnostics.c_str());
  check(t.wgsl != defaults.wgsl, "specialized code differs from the default");

  vtkslang::CompileOptions typo = custom;
  typo.specializations = { { "ITestMaterail", "ToonMaterial" } };
  vtkslang::Artifact bad = compiler.compileSource(shader, {}, typo);
  check(!bad && bad.diagnostics.find("ITestMaterail") != std::string::npos,
    "naming an interface the shader lacks is an error");

  vtkslang::CompileOptions missing = custom;
  missing.specializations = { { "ITestMaterial", "NoSuchMaterial" } };
  check(!compiler.compileSource(shader, {}, missing), "an unknown implementation type fails");

  const char* noDefault = R"(
import test_shading;
type_param M : ITestMaterial;
[vk::binding(0, 0)] RWStructuredBuffer<float4> outColor;
[shader("compute")] [numthreads(64, 1, 1)]
void run(uint3 id : SV_DispatchThreadID) { Sample s; s.color = float4(1.f); outColor[id.x] = M().shade(s); }
)";
  check(!compiler.compileSource(noDefault, {}, base),
    "a type param without a default needs an explicit specialization");
  vtkslang::CompileOptions explicitPlain = base;
  explicitPlain.specializations = { { "ITestMaterial", "PlainMaterial" } };
  check(static_cast<bool>(compiler.compileSource(noDefault, {}, explicitPlain)),
    "...and compiles once given one");

  vtkslang::CompileOptions spec;
  spec.specializations = { { "IMaterial", "Anything" } };
  check(!compiler.compileSource(R"([shader("compute")][numthreads(1,1,1)] void m() {})", {}, spec),
    "specializing a shader without type params is an error");

  const char* shadingV2 = R"(
module test_shading;
public struct Sample { public float4 color; };
public interface ITestMaterial { float4 shade(Sample s); };
public interface ITestLight { float scale(); };
public struct PlainMaterial : ITestMaterial { public float4 shade(Sample s) { return s.color.bgra; } };
public struct UnitLight : ITestLight { public float scale() { return 1.f; } };
)";
  vtkslang::CompileOptions edited;
  edited.modules = { shadingV2 };
  vtkslang::Artifact v2 = compiler.compileSource(shader, {}, edited);
  check(v2 && v2.wgsl != defaults.wgsl,
    "a module redefined with new source is recompiled, not reused");
  if (!v2 || v2.wgsl == defaults.wgsl)
    std::printf("v2 ok=%d same=%d\n%s", (int)static_cast<bool>(v2), (int)(v2.wgsl == defaults.wgsl), v2.diagnostics.c_str());
  check(compiler.compileSource(shader, {}, base).wgsl == defaults.wgsl,
    "switching back to the original module source works");

  vtkslang::CompileOptions reserved;
  reserved.modules = { "module vtkslang_core;\npublic float f() { return 1.f; }\n" };
  vtkslang::Artifact clash = compiler.compileSource(shader, {}, reserved);
  check(!clash && clash.diagnostics.find("reserved") != std::string::npos,
    "library module names are reserved");

  vtkslang::Artifact again = compiler.compileSource(shader, {}, custom);
  check(again.fromCache && again.wgsl == t.wgsl && !compiler.compileSource(shader, {}, base).wgsl.empty(),
    "specializations are part of the cache key");
}

void testOverrides(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const char* stripes = R"(
module test_stripes;
import vtkslang_shading;
public struct StripedMaterial : IMaterial {
  public float4 stripeColor;
  public float  frequency;
  public MaterialSample evaluate(SurfaceSample s) {
    MaterialSample m = HostDrivenMaterial().evaluate(s);
    if (frac(s.position.y * frequency) < 0.5f)
      m.baseColor = stripeColor.rgb;
    return m;
  }
};
public struct ToonLighting : ILightingModel {
  public float3 direct(MaterialSample m, SurfaceSample s, LightSample l) {
    float n = max(dot(s.normal, l.direction), 0.f);
    return m.baseColor * l.radiance * (n > 0.5f ? 1.f : 0.3f);
  }
  public float3 ambient(MaterialSample m, SurfaceSample s, float3 a) { return m.baseColor * a; }
  public float3 resolve(float3 r, MaterialSample m) { return saturate(r); }
};
)";
  vtkslang::CompileOptions striped;
  striped.modules         = { stripes };
  striped.specializations = { { "IMaterial", "StripedMaterial" } };
  const vtkslang::Program host = surfaceHost();
  vtkslang::Artifact      r    = compiler.compileProgram(host, striped);
  const auto* block = byName(r, "shading");
  check(r && r.source == vtkslang::ArtifactSource::Slang && block && block->group == 1 &&
      block->kind == BindingKind::UniformBuffer && field(block, "material.stripeColor") &&
      field(block, "material.frequency") && findBinding(r, 2) &&
      reflectedBindings(r) == wgslBindings(r.wgsl),
    "the host takes a consumer IMaterial; its uniforms land in group 1, group 0 is unchanged");
  if (!r)
    std::printf("%s", r.diagnostics.c_str());

  size_t group0 = 0;
  for (const auto& b : r.bindings)
    group0 += b.group == 0 ? 1 : 0;
  check(group0 == 3, "the override leaves the host's 3 group-0 bindings intact");

  vtkslang::CompileOptions toon;
  toon.modules         = { stripes };
  toon.specializations = { { "ILightingModel", "ToonLighting" } };
  vtkslang::Artifact p = compiler.compileProgram(host, toon);
  check(p && p.wgsl != compiler.compileSource(host.source).wgsl && !byName(p, "shading"),
    "the host takes a consumer ILightingModel; a field-less override adds no bindings");
  if (!p)
    std::printf("%s", p.diagnostics.c_str());

  vtkslang::CompileOptions both = toon;
  both.specializations.push_back({ "IMaterial", "StripedMaterial" });
  check(static_cast<bool>(compiler.compileProgram(host, both)), "both interfaces at once");

  vtkslang::CompileOptions phong;
  phong.specializations = { { "ILightingModel", "PhongLighting" } };
  check(static_cast<bool>(compiler.compileProgram(host, phong)),
    "the library's PhongLighting can replace a shader's default");
}

void testStageInterfaces(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const char* stages = R"(
module test_stages;
import vtkslang_shading;
public struct Wobble : IVertexModifier {
  public float amplitude;
  public VertexSample modify(VertexSample v) {
    v.position += v.normal * amplitude * sin(float(v.pointId));
    return v;
  }
};
public struct Bumps : INormalModifier {
  public float strength;
  public float3 normal(SurfaceSample s) {
    return normalize(s.normal + strength * float3(sin(s.position.x), 0.f, 0.f));
  }
};
public struct Cutout : ISurfaceOutput {
  public float cutoff;
  public bool keep(SurfaceSample s) { return frac(s.position.y) > cutoff; }
  public float4 finish(float4 c, MaterialSample m, SurfaceSample s) { return float4(1.f - c.rgb, c.a); }
  public float depth(float d, SurfaceSample s) { return d * 0.5f; }
};
)";
  for (const char* shader : { "the host" }) {
    vtkslang::CompileOptions o;
    o.modules         = { stages };
    o.specializations = { { "IVertexModifier", "Wobble" }, { "INormalModifier", "Bumps" },
      { "ISurfaceOutput", "Cutout" } };
    vtkslang::Artifact a = compiler.compileProgram(surfaceHost(), o);
    const auto* block    = byName(a, "shading");
    size_t      group0   = 0;
    for (const auto& b : a.bindings)
      group0 += b.group == 0 ? 1 : 0;
    check(a && block && block->kind == BindingKind::UniformBuffer &&
        field(block, "vertex.amplitude") && field(block, "normal.strength") &&
        field(block, "output.cutoff") && group0 == 3 &&
        reflectedBindings(a) == wgslBindings(a.wgsl),
      (std::string(shader) + " takes vertex, normal, and output overrides with parameters").c_str());
    if (!a)
      std::printf("%s", a.diagnostics.c_str());
  }
}

void testHooks(vtkslang::vtkSlangCompiler& compiler)
{
  using vtkslang::BindingKind;
  const char* host = R"(
import vtkslang_hooks;
type_param H : IShaderHooks;
typealias Default_IShaderHooks = DefaultHooks;
[vk::binding(0, 0)] ConstantBuffer<HookBlock<H>> hookBlock;
struct VSOut { float4 position : SV_Position; float2 uv : UV; };
[shader("vertex")]
VSOut vertexMain(float3 p : POSITION)
{
  VertexContext v = VertexContext();
  v.positionMC = float4(p, 1.f);
  hookBlock.hooks.vertex(v);
  v.positionClip = v.positionMC;
  hookBlock.hooks.position(v);
  VSOut o;
  o.position = v.positionClip;
  o.uv = v.uv;
  return o;
}
[shader("fragment")]
float4 fragmentMain(VSOut i) : SV_Target
{
  FragmentContext f = FragmentContext();
  f.uv = i.uv;
  if (!hookBlock.hooks.clip(f))
    discard;
  hookBlock.hooks.color(f);
  hookBlock.hooks.normal(f);
  f.color = float4(f.diffuseColor, f.opacity);
  hookBlock.hooks.lit(f);
  hookBlock.hooks.output(f);
  return f.color;
}
)";
  const char* hooks = R"(
module test_hooks;
import vtkslang_hooks;
public struct Ramp : IShaderHooks {
  float3 tint;
  Texture2D ramp;
  SamplerState rampSampler;
  public override void color(inout FragmentContext f) {
    f.diffuseColor = ramp.Sample(rampSampler, f.uv).rgb * tint;
  }
  public override bool clip(inout FragmentContext f) { return f.uv.x > 0.1f; }
};
)";
  vtkslang::Artifact plain = compiler.compileSource(host);
  check(plain && !byName(plain, "hookBlock"),
    "the default hooks are no-ops and bind nothing");
  if (!plain)
    std::printf("%s", plain.diagnostics.c_str());

  vtkslang::CompileOptions o;
  o.modules         = { hooks };
  o.specializations = { { "IShaderHooks", "Ramp" } };
  vtkslang::Artifact a = compiler.compileSource(host, {}, o);
  const auto* block = byName(a, "hookBlock");
  const auto* ramp  = byName(a, "hookBlock.hooks.ramp");
  check(a && block && block->kind == BindingKind::UniformBuffer && field(block, "hooks.tint") &&
      ramp && ramp->kind == BindingKind::SampledTexture && a.wgsl != plain.wgsl &&
      reflectedBindings(a) == wgslBindings(a.wgsl),
    "an IShaderHooks override takes parameters and textures and changes only what it overrides");
  if (!a)
    std::printf("%s", a.diagnostics.c_str());
}

void testCache()
{
  const char* src = R"(
[vk::binding(0, 0)] RWStructuredBuffer<float> data;
[shader("compute")] [numthreads(64, 1, 1)]
void cached(uint3 id : SV_DispatchThreadID) { data[id.x] = data[id.x] + 1.f; }
)";
  {
    vtkslang::vtkSlangCompiler c;
    vtkslang::Artifact first  = c.compileSource(src);
    vtkslang::Artifact second = c.compileSource(src);
    check(first && !first.fromCache && second.fromCache && second.wgsl == first.wgsl &&
        second.bindings.size() == first.bindings.size(),
      "repeat compile is served from the memory cache");
  }

  const std::string dir = std::string(VTKSLANG_TEST_TMP_DIR) + "/cache";
  std::error_code   ec;
  std::filesystem::remove_all(dir, ec);

  std::string firstWGSL;
  {
    vtkslang::vtkSlangCompiler a;
    a.setCacheDirectory(dir);
    vtkslang::Artifact r = a.compileSource(src);
    firstWGSL            = r.wgsl;
    check(r && !r.fromCache, "first compile with a disk cache misses");
  }
  size_t entries = 0;
  std::filesystem::path entry;
  for (const auto& f : std::filesystem::directory_iterator(dir)) {
    ++entries;
    entry = f.path();
  }
  check(entries == 1, "one cache entry written");

  {
    vtkslang::vtkSlangCompiler b;
    b.setCacheDirectory(dir);
    vtkslang::Artifact r = b.compileSource(src);
    check(r && r.fromCache && r.wgsl == firstWGSL, "a new compiler reuses the disk cache");

    vtkslang::CompileOptions other;
    other.defines = { { "SOMETHING", "1" } };
    check(!b.compileSource(src, {}, other).fromCache, "different defines miss the cache");

    vtkslang::CompileOptions reflect;
    reflect.reflect = true;
    b.compileProgram(surfaceHost(), reflect);
    vtkslang::Artifact again = b.compileProgram(surfaceHost(), reflect);
    check(again.fromCache && !again.bindings.empty(), "reflected programs are cached with bindings");
  }

  {
    std::ofstream(entry, std::ios::binary | std::ios::trunc) << "not a cache entry";
    vtkslang::vtkSlangCompiler c;
    c.setCacheDirectory(dir);
    vtkslang::Artifact r = c.compileSource(src);
    check(r && !r.fromCache && r.wgsl == firstWGSL, "a corrupt entry is ignored and recompiled");
    vtkslang::vtkSlangCompiler d;
    d.setCacheDirectory(dir);
    check(d.compileSource(src).fromCache, "the recompiled entry replaces the corrupt one");
  }

  {
    vtkslang::vtkSlangCompiler c;
    vtkslang::CompileOptions withPath;
    withPath.searchPaths = { VTKSLANG_TEST_TMP_DIR };
    c.compileSource(src, {}, withPath);
    check(!c.compileSource(src, {}, withPath).fromCache, "compiles with search paths are not cached");
  }
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

  vtkslang::CompileOptions reflect;
  reflect.reflect      = true;
  vtkslang::Artifact r = compiler.compileProgram(p, reflect);
  check(r && r.source == vtkslang::ArtifactSource::Slang &&
      r.wgsl.find("fn fragmentMain") != std::string::npos &&
      r.wgsl.find("fn vertexMain") != std::string::npos,
    "a reflected program request compiles with Slang");

  for (const auto& variant : { std::make_pair("define", 0), std::make_pair("specialization", 1) }) {
    vtkslang::CompileOptions o;
    if (variant.second == 0)
      o.defines = { { "UNUSED", "1" } };
    else
      o.specializations = { { "ILightingModel", "CookTorranceLighting" } };
    vtkslang::Artifact v = compiler.compileProgram(p, o);
    check(v && v.source == vtkslang::ArtifactSource::Slang,
      (std::string("a program request with a ") + variant.first + " compiles with Slang").c_str());
  }

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
    testReflection(compiler);
    testNestedReflection(compiler);
    testTextureDimensions();
    testSpecialization(compiler);
    testOverrides(compiler);
    testStageInterfaces(compiler);
    testHooks(compiler);
    testCache();
  } else {
    std::printf("SKIP: Slang unavailable\n");
  }
  testDisableEnv(compiler);

  std::printf("%d failure(s)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
