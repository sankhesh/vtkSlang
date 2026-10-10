// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"
#include "vtkSlangReproducer.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#ifdef VTKSLANG_USE_SLANG
#  include <slang.h>
#  include <slang-com-ptr.h>
#  if __has_include(<slang-tag-version.h>)
#    include <slang-tag-version.h>
#  endif
#  ifndef SLANG_TAG_VERSION
#    define SLANG_TAG_VERSION "unknown"
#  endif

#  include <cstring>
#  include <filesystem>
#  include <fstream>
#  include <regex>
#  include <sstream>
#endif

#include "vtkSlangSources.h"

namespace vtkslang {

namespace {

bool slangDisabledByEnv()
{
  const char* disable = std::getenv("VTKSLANG_DISABLE_SLANG");
  return disable && disable[0] == '1';
}

Artifact failure(Target target, std::string message)
{
  Artifact a;
  a.target      = target;
  a.diagnostics = std::move(message);
  return a;
}

const char* const kLibraryModules[] = { "vtkslang_attributes", "vtkslang_core",
  "vtkslang_shading", "vtkslang_hooks", "vtkslang_surface" };

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

BindingKind bindingKind(slang::TypeLayoutReflection* type)
{
  switch (type->getKind()) {
  case slang::TypeReflection::Kind::ConstantBuffer:
    return BindingKind::UniformBuffer;
  case slang::TypeReflection::Kind::SamplerState:
    return BindingKind::Sampler;
  case slang::TypeReflection::Kind::Resource: {
    const unsigned shape    = type->getResourceShape() & SLANG_RESOURCE_BASE_SHAPE_MASK;
    const bool     readOnly = type->getResourceAccess() == SLANG_RESOURCE_ACCESS_READ;
    switch (shape) {
    case SLANG_STRUCTURED_BUFFER:
    case SLANG_BYTE_ADDRESS_BUFFER:
      return readOnly ? BindingKind::ReadOnlyStorageBuffer : BindingKind::StorageBuffer;
    case SLANG_TEXTURE_1D:
    case SLANG_TEXTURE_2D:
    case SLANG_TEXTURE_3D:
    case SLANG_TEXTURE_CUBE:
      return readOnly ? BindingKind::SampledTexture : BindingKind::StorageTexture;
    default:
      return BindingKind::Other;
    }
  }
  default:
    return BindingKind::Other;
  }
}

TextureDimension textureDimension(slang::TypeLayoutReflection* type)
{
  if (type->getKind() != slang::TypeReflection::Kind::Resource)
    return TextureDimension::None;
  const unsigned shape = type->getResourceShape();
  const bool     array = (shape & SLANG_TEXTURE_ARRAY_FLAG) != 0;
  switch (shape & SLANG_RESOURCE_BASE_SHAPE_MASK) {
  case SLANG_TEXTURE_1D:
    return TextureDimension::D1;
  case SLANG_TEXTURE_2D:
    return array ? TextureDimension::D2Array : TextureDimension::D2;
  case SLANG_TEXTURE_3D:
    return TextureDimension::D3;
  case SLANG_TEXTURE_CUBE:
    return array ? TextureDimension::CubeArray : TextureDimension::Cube;
  default:
    return TextureDimension::None;
  }
}

constexpr SlangParameterCategory kSlot     = SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT;
constexpr SlangParameterCategory kUniform  = SLANG_PARAMETER_CATEGORY_UNIFORM;
constexpr SlangParameterCategory kSubSpace = SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE;

std::string joinPath(const std::string& prefix, slang::VariableLayoutReflection* v)
{
  const char* name = v->getName();
  if (!name || !*name)
    return prefix;
  return prefix.empty() ? std::string(name) : prefix + "." + name;
}

const char* scalarName(slang::TypeReflection::ScalarType scalar)
{
  switch (scalar) {
  case slang::TypeReflection::ScalarType::Bool:
    return "bool";
  case slang::TypeReflection::ScalarType::Int32:
    return "int";
  case slang::TypeReflection::ScalarType::UInt32:
    return "uint";
  case slang::TypeReflection::ScalarType::Float16:
    return "half";
  case slang::TypeReflection::ScalarType::Float32:
    return "float";
  default:
    return "";
  }
}

std::string typeName(slang::TypeReflection* type)
{
  if (!type)
    return {};
  switch (type->getKind()) {
  case slang::TypeReflection::Kind::Scalar:
    return scalarName(type->getScalarType());
  case slang::TypeReflection::Kind::Vector:
    return std::string(scalarName(type->getScalarType())) +
      std::to_string(type->getElementCount());
  case slang::TypeReflection::Kind::Matrix:
    return std::string(scalarName(type->getScalarType())) +
      std::to_string(type->getRowCount()) + "x" + std::to_string(type->getColumnCount());
  case slang::TypeReflection::Kind::Array:
    return typeName(type->getElementType()) + "[" + std::to_string(type->getElementCount()) + "]";
  default:
    return type->getName() ? type->getName() : "";
  }
}

ScalarType scalarType(slang::TypeReflection::ScalarType scalar)
{
  switch (scalar) {
  case slang::TypeReflection::ScalarType::Float32:
    return ScalarType::Float32;
  case slang::TypeReflection::ScalarType::Int32:
    return ScalarType::Int32;
  case slang::TypeReflection::ScalarType::UInt32:
    return ScalarType::UInt32;
  default:
    return ScalarType::Unknown;
  }
}

void fieldShape(slang::TypeLayoutReflection* type, UniformField& field)
{
  uint64_t count         = 1;
  uint64_t elementStride = 0;
  if (type->getKind() == slang::TypeReflection::Kind::Array) {
    count         = type->getElementCount();
    elementStride = type->getElementStride(kUniform);
    type          = type->getElementTypeLayout();
  }
  uint32_t width = 0;
  uint32_t runs  = 0;
  switch (type->getKind()) {
  case slang::TypeReflection::Kind::Scalar:
    width = 1;
    runs  = 1;
    break;
  case slang::TypeReflection::Kind::Vector:
    width = static_cast<uint32_t>(type->getElementCount());
    runs  = 1;
    break;
  case slang::TypeReflection::Kind::Matrix: {
    const bool rowMajor = type->getMatrixLayoutMode() == SLANG_MATRIX_LAYOUT_ROW_MAJOR;
    width = rowMajor ? type->getColumnCount() : type->getRowCount();
    runs  = rowMajor ? type->getRowCount() : type->getColumnCount();
    break;
  }
  default:
    return;
  }
  const ScalarType scalar = scalarType(type->getScalarType());
  const uint64_t   size   = type->getSize(kUniform);
  if (scalar == ScalarType::Unknown || count == 0 || width == 0 || size % runs != 0)
    return;
  uint64_t stride = runs > 1 ? size / runs : size;
  if (count > 1) {
    if (runs == 1)
      stride = elementStride;
    else if (elementStride != runs * stride)
      return;
  }
  field.scalar = scalar;
  field.width  = width;
  field.runs   = static_cast<uint32_t>(count * runs);
  field.stride = stride;
}

UserAttribute userAttribute(slang::UserAttribute* attribute)
{
  UserAttribute result;
  result.name = attribute->getName() ? attribute->getName() : "";
  for (unsigned i = 0; i < attribute->getArgumentCount(); ++i) {
    slang::TypeReflection* type = attribute->getArgumentType(i);
    std::string            text;
    if (type && type->getKind() == slang::TypeReflection::Kind::Scalar &&
        type->getScalarType() != slang::TypeReflection::ScalarType::Float32 &&
        type->getScalarType() != slang::TypeReflection::ScalarType::Float16) {
      int value = 0;
      if (SLANG_SUCCEEDED(attribute->getArgumentValueInt(i, &value)))
        text = std::to_string(value);
    } else {
      float value = 0.f;
      size_t size = 0;
      if (const char* string = attribute->getArgumentValueString(i, &size))
        text.assign(string, size);
      else if (SLANG_SUCCEEDED(attribute->getArgumentValueFloat(i, &value))) {
        std::ostringstream stream;
        stream << value;
        text = stream.str();
      }
    }
    result.arguments.push_back(std::move(text));
  }
  return result;
}

void collectUniformFields(slang::VariableLayoutReflection* v, uint64_t base,
                          const std::string& prefix, std::vector<UniformField>& out)
{
  slang::TypeLayoutReflection* type = v->getTypeLayout();
  const uint64_t size = type->getSize(kUniform);
  if (size == 0)
    return;
  const uint64_t    offset = base + v->getOffset(kUniform);
  const std::string name   = joinPath(prefix, v);
  if (type->getKind() == slang::TypeReflection::Kind::Struct) {
    for (unsigned i = 0; i < type->getFieldCount(); ++i)
      collectUniformFields(type->getFieldByIndex(i), offset, name, out);
    return;
  }
  UniformField field;
  field.name   = name;
  field.offset = offset;
  field.size   = size;
  field.type   = typeName(type->getType());
  fieldShape(type, field);
  if (slang::VariableReflection* variable = v->getVariable()) {
    for (unsigned i = 0; i < variable->getUserAttributeCount(); ++i)
      field.attributes.push_back(userAttribute(variable->getUserAttributeByIndex(i)));
  }
  out.push_back(std::move(field));
}

void walkParameter(slang::VariableLayoutReflection* v, uint32_t space, uint32_t slot,
                   const std::string& prefix, std::vector<ReflectedBinding>& out);

void walkBlock(slang::TypeLayoutReflection* type, const std::string& name, uint32_t space,
               uint32_t slot, std::vector<ReflectedBinding>& out)
{
  slang::VariableLayoutReflection* container = type->getContainerVarLayout();
  slang::VariableLayoutReflection* element   = type->getElementVarLayout();
  slang::TypeLayoutReflection*     data      = element->getTypeLayout();

  const uint64_t size = data->getSize(kUniform);
  if (size > 0) {
    ReflectedBinding b;
    b.name      = name;
    b.group     = space;
    b.binding   = slot + static_cast<uint32_t>(container->getOffset(kSlot));
    b.kind      = BindingKind::UniformBuffer;
    b.sizeBytes = size;
    if (data->getKind() == slang::TypeReflection::Kind::Struct) {
      for (unsigned i = 0; i < data->getFieldCount(); ++i)
        collectUniformFields(data->getFieldByIndex(i), 0, "", b.fields);
    }
    out.push_back(std::move(b));
  }

  const uint32_t resourceBase = slot + static_cast<uint32_t>(element->getOffset(kSlot));
  if (data->getKind() == slang::TypeReflection::Kind::Struct) {
    for (unsigned i = 0; i < data->getFieldCount(); ++i)
      walkParameter(data->getFieldByIndex(i), space, resourceBase, name, out);
  }
}

void walkParameter(slang::VariableLayoutReflection* v, uint32_t space, uint32_t slot,
                   const std::string& prefix, std::vector<ReflectedBinding>& out)
{
  slang::TypeLayoutReflection* type  = v->getTypeLayout();
  const std::string            name  = joinPath(prefix, v);
  const uint32_t               group = space + static_cast<uint32_t>(v->getBindingSpace(kSlot));
  const uint32_t               here  = slot + static_cast<uint32_t>(v->getOffset(kSlot));

  switch (type->getKind()) {
  case slang::TypeReflection::Kind::ParameterBlock:
    walkBlock(type, name, space + static_cast<uint32_t>(v->getOffset(kSubSpace)), 0, out);
    return;
  case slang::TypeReflection::Kind::ConstantBuffer:
    walkBlock(type, name, group, here, out);
    return;
  case slang::TypeReflection::Kind::Struct:
    for (unsigned i = 0; i < type->getFieldCount(); ++i)
      walkParameter(type->getFieldByIndex(i), group, here, name, out);
    return;
  default:
    break;
  }

  if (type->getSize(kSlot) == 0)
    return;
  ReflectedBinding b;
  b.name    = name;
  b.group   = group;
  b.binding = here;
  b.kind    = bindingKind(type);
  b.viewDimension = textureDimension(type);
  out.push_back(std::move(b));
}

std::vector<ReflectedBinding> reflectBindings(slang::IComponentType* linked, SlangInt entryCount)
{
  std::vector<ReflectedBinding> bindings;
  slang::ProgramLayout* layout = linked->getLayout(0);
  if (!layout)
    return bindings;

  for (unsigned i = 0; i < layout->getParameterCount(); ++i)
    walkParameter(layout->getParameterByIndex(i), 0, 0, "", bindings);
  for (SlangUInt e = 0; e < layout->getEntryPointCount(); ++e) {
    slang::EntryPointReflection* ep = layout->getEntryPointByIndex(e);
    for (unsigned i = 0; i < ep->getParameterCount(); ++i)
      walkParameter(ep->getParameterByIndex(i), 0, 0, "", bindings);
  }

  for (ReflectedBinding& b : bindings) {
    bool usedAnywhere = false;
    for (SlangInt e = 0; e < entryCount && !usedAnywhere; ++e) {
      Slang::ComPtr<slang::IMetadata> metadata;
      bool used = false;
      if (SLANG_SUCCEEDED(linked->getEntryPointMetadata(e, 0, metadata.writeRef())) &&
        SLANG_SUCCEEDED(metadata->isParameterLocationUsed(
          SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT, b.group, b.binding, used)))
        usedAnywhere = used;
      else
        usedAnywhere = true;
    }
    b.used = usedAnywhere;
  }
  return bindings;
}

uint64_t fnv1a(std::string_view s)
{
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

const std::string& libraryHash()
{
  static const std::string hash = [] {
    std::string all;
    for (const char* name : kLibraryModules) {
      all += name;
      all += '\0';
      all += vtkSlangSources::get(name);
    }
    std::ostringstream ss;
    ss << std::hex << fnv1a(all);
    return ss.str();
  }();
  return hash;
}

void putU64(std::string& out, uint64_t v)
{
  out.append(reinterpret_cast<const char*>(&v), sizeof v);
}

void putStr(std::string& out, std::string_view s)
{
  putU64(out, s.size());
  out.append(s.data(), s.size());
}

struct Reader
{
  const std::string& data;
  size_t             pos = 0;
  bool               ok  = true;

  uint64_t u64()
  {
    uint64_t v = 0;
    if (pos + sizeof v > data.size()) {
      ok = false;
      return 0;
    }
    std::memcpy(&v, data.data() + pos, sizeof v);
    pos += sizeof v;
    return v;
  }

  std::string str()
  {
    uint64_t n = u64();
    if (!ok || n > data.size() - pos) {
      ok = false;
      return {};
    }
    std::string s = data.substr(pos, n);
    pos += n;
    return s;
  }
};

constexpr std::string_view kCacheMagic = "VTKSLANG-CACHE-5\n";

std::string serialize(const std::string& key, const Artifact& a)
{
  std::string out(kCacheMagic);
  putStr(out, key);
  putU64(out, static_cast<uint64_t>(a.target));
  if (a.target == Target::WGSL)
    putStr(out, a.wgsl);
  else
    putStr(out, std::string_view(reinterpret_cast<const char*>(a.spirv.data()),
                  a.spirv.size() * sizeof(uint32_t)));
  putU64(out, a.bindings.size());
  for (const ReflectedBinding& b : a.bindings) {
    putStr(out, b.name);
    putU64(out, b.group);
    putU64(out, b.binding);
    putU64(out, static_cast<uint64_t>(b.kind));
    putU64(out, b.used ? 1 : 0);
    putU64(out, b.sizeBytes);
    putU64(out, static_cast<uint64_t>(b.viewDimension));
    putU64(out, b.fields.size());
    for (const UniformField& f : b.fields) {
      putStr(out, f.name);
      putU64(out, f.offset);
      putU64(out, f.size);
      putStr(out, f.type);
      putU64(out, static_cast<uint64_t>(f.scalar));
      putU64(out, f.width);
      putU64(out, f.runs);
      putU64(out, f.stride);
      putU64(out, f.attributes.size());
      for (const UserAttribute& attribute : f.attributes) {
        putStr(out, attribute.name);
        putU64(out, attribute.arguments.size());
        for (const std::string& argument : attribute.arguments)
          putStr(out, argument);
      }
    }
  }
  putStr(out, a.diagnostics);
  return out;
}

bool deserialize(const std::string& data, const std::string& key, Artifact& a)
{
  if (data.compare(0, kCacheMagic.size(), kCacheMagic) != 0)
    return false;
  Reader r{ data, kCacheMagic.size() };
  if (r.str() != key || !r.ok)
    return false;
  a.target         = static_cast<Target>(r.u64());
  std::string code = r.str();
  if (a.target == Target::WGSL) {
    a.wgsl = std::move(code);
  } else {
    if (code.size() % sizeof(uint32_t) != 0)
      return false;
    a.spirv.resize(code.size() / sizeof(uint32_t));
    std::memcpy(a.spirv.data(), code.data(), code.size());
  }
  uint64_t count = r.u64();
  for (uint64_t i = 0; r.ok && i < count; ++i) {
    ReflectedBinding b;
    b.name      = r.str();
    b.group     = static_cast<uint32_t>(r.u64());
    b.binding   = static_cast<uint32_t>(r.u64());
    b.kind      = static_cast<BindingKind>(r.u64());
    b.used      = r.u64() != 0;
    b.sizeBytes = r.u64();
    b.viewDimension = static_cast<TextureDimension>(r.u64());
    uint64_t fieldCount = r.u64();
    for (uint64_t j = 0; r.ok && j < fieldCount; ++j) {
      UniformField f;
      f.name   = r.str();
      f.offset = r.u64();
      f.size   = r.u64();
      f.type   = r.str();
      f.scalar = static_cast<ScalarType>(r.u64());
      f.width  = static_cast<uint32_t>(r.u64());
      f.runs   = static_cast<uint32_t>(r.u64());
      f.stride = r.u64();
      uint64_t attributeCount = r.u64();
      for (uint64_t k = 0; r.ok && k < attributeCount; ++k) {
        UserAttribute attribute;
        attribute.name          = r.str();
        uint64_t argumentCount = r.u64();
        for (uint64_t n = 0; r.ok && n < argumentCount; ++n)
          attribute.arguments.push_back(r.str());
        f.attributes.push_back(std::move(attribute));
      }
      b.fields.push_back(std::move(f));
    }
    a.bindings.push_back(std::move(b));
  }
  a.diagnostics = r.str();
  a.source      = ArtifactSource::Slang;
  return r.ok && r.pos == data.size();
}

void appendDiagnostics(std::string& out, slang::IBlob* blob)
{
  if (blob && blob->getBufferSize() > 0) {
    out.append(static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize());
    if (out.back() != '\n')
      out.push_back('\n');
  }
}

#endif

}

struct vtkSlangCompiler::SlangState
{
#ifdef VTKSLANG_USE_SLANG
  struct TargetSession
  {
    Slang::ComPtr<slang::ISession> session;
    std::unordered_map<std::string, slang::IModule*> modules;
    std::unordered_map<std::string, uint64_t> moduleSourceHashes;
  };

  static std::string declaredModuleName(const std::string& source)
  {
    static const std::regex decl(R"((^|\n)\s*module\s+([A-Za-z_][A-Za-z0-9_]*)\s*;)");
    std::smatch m;
    return std::regex_search(source, m, decl) ? m[2].str() : std::string();
  }

  bool loadOptionModules(TargetSession*& ts, const CompileOptions& options,
                         std::vector<slang::IModule*>& out, std::string& diagnostics)
  {
    for (int attempt = 0; attempt < 2; ++attempt) {
      out.clear();
      bool redefined = false;
      for (const std::string& source : options.modules) {
        std::string name = declaredModuleName(source);
        for (const char* lib : kLibraryModules) {
          if (name == lib) {
            diagnostics += "vtkSlang: module name '" + name + "' is reserved\n";
            return false;
          }
        }
        if (name.empty())
          name = "vtkslang_user_" + std::to_string(userModuleCount++);

        const uint64_t hash  = fnv1a(source);
        auto           known = ts->moduleSourceHashes.find(name);
        if (known != ts->moduleSourceHashes.end() && known->second != hash) {
          redefined = true;
          break;
        }
        slang::IModule* m = known != ts->moduleSourceHashes.end()
          ? ts->modules[name]
          : loadSource(*ts, name, source.c_str(), diagnostics);
        if (!m)
          return false;
        ts->moduleSourceHashes[name] = hash;
        out.push_back(m);
      }
      if (!redefined)
        return true;
      sessions.erase(sessionKey(options));
      ts = session(options, diagnostics);
      if (!ts)
        return false;
    }
    return false;
  }

  std::mutex mutex;
  std::unordered_map<std::string, TargetSession> sessions;
  unsigned userModuleCount = 0;

  std::string                               cacheDirectory;
  std::unordered_map<std::string, Artifact> memoryCache;
  std::string                               dumpDirectory;

  void dump(const Reproducer& reproducer, const Artifact& artifact) const
  {
    std::string directory = dumpDirectory;
    if (directory.empty()) {
      const char* env = std::getenv("VTKSLANG_DUMP_DIR");
      directory       = env ? env : "";
    }
    if (!directory.empty())
      writeReproducer(
        (std::filesystem::path(directory) / reproducerName(reproducer)).string(), reproducer,
        artifact);
  }

  static std::string cacheKey(const std::string& what, const std::string& source,
                              const std::vector<std::string>& entryPoints,
                              const CompileOptions& options)
  {
    if (!options.searchPaths.empty())
      return {};
    std::string key = sessionKey(options);
    key += "\nreflect=" + std::to_string(options.reflect ? 1 : 0);
    key += std::string("\nslang=") + SLANG_TAG_VERSION;
    key += "\nlibrary=" + libraryHash();
    for (const std::string& e : entryPoints)
      key += "\nentry=" + e;
    for (const auto& s : options.specializations)
      key += "\nspecialize=" + s.first + "=" + s.second;
    for (const std::string& m : options.modules)
      key += "\nmodule=" + std::to_string(m.size()) + ":" + m;
    key += "\n" + what + "\n" + source;
    return key;
  }

  std::filesystem::path cachePath(const std::string& key) const
  {
    std::ostringstream name;
    name << std::hex << fnv1a(key) << ".vtkslangcache";
    return std::filesystem::path(cacheDirectory) / name.str();
  }

  bool lookup(const std::string& key, Artifact& out)
  {
    if (key.empty())
      return false;
    auto it = memoryCache.find(key);
    if (it != memoryCache.end()) {
      out           = it->second;
      out.fromCache = true;
      return true;
    }
    if (cacheDirectory.empty())
      return false;
    std::ifstream in(cachePath(key), std::ios::binary);
    if (!in)
      return false;
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Artifact a;
    if (!deserialize(data, key, a))
      return false;
    memoryCache[key] = a;
    out              = std::move(a);
    out.fromCache    = true;
    return true;
  }

  void store(const std::string& key, const Artifact& a)
  {
    if (key.empty() || !a)
      return;
    memoryCache[key] = a;
    if (cacheDirectory.empty())
      return;
    std::error_code       ec;
    std::filesystem::path path = cachePath(key);
    std::filesystem::path tmp  = path;
    tmp += ".tmp" + std::to_string(fnv1a(std::to_string(reinterpret_cast<uintptr_t>(this))));
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out)
        return;
      std::string data = serialize(key, a);
      out.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!out)
        return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec)
      std::filesystem::remove(tmp, ec);
  }

  static std::string sessionKey(const CompileOptions& options)
  {
    std::string key = (options.target == Target::WGSL) ? "wgsl" : "spirv";
    for (const auto& d : options.defines)
      key += "\nD" + d.first + "=" + d.second;
    for (const auto& p : options.searchPaths)
      key += "\nI" + p;
    return key;
  }

  slang::IModule* loadEmbedded(TargetSession& ts, const char* name, std::string& diagnostics)
  {
    auto it = ts.modules.find(name);
    if (it != ts.modules.end())
      return it->second;

    const char* source = vtkSlangSources::get(name);
    if (!source) {
      diagnostics += std::string("vtkSlang: no embedded source named '") + name + "'\n";
      return nullptr;
    }
    return loadSource(ts, name, source, diagnostics);
  }

  slang::IModule* loadSource(TargetSession& ts, const std::string& name, const char* source,
                             std::string& diagnostics)
  {
    Slang::ComPtr<slang::IBlob> blob;
    std::string path = "vtkslang-embedded:/" + name + ".slang";
    slang::IModule* module =
      ts.session->loadModuleFromSourceString(name.c_str(), path.c_str(), source, blob.writeRef());
    appendDiagnostics(diagnostics, blob);
    if (module)
      ts.modules[name] = module;
    return module;
  }

  TargetSession* session(const CompileOptions& options, std::string& diagnostics)
  {
    const Target   target = options.target;
    TargetSession& ts     = sessions[sessionKey(options)];
    if (ts.session)
      return &ts;

    slang::IGlobalSession* global = globalSession();
    if (!global) {
      diagnostics += "vtkSlang: failed to create the Slang global session\n";
      return nullptr;
    }

    slang::TargetDesc td{};
    td.format = (target == Target::WGSL) ? SLANG_WGSL : SLANG_SPIRV;
    if (target == Target::SPIRV)
      td.profile = global->findProfile("spirv_1_5");

    slang::SessionDesc sd{};
    sd.targets     = &td;
    sd.targetCount = 1;
    sd.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;

    std::vector<slang::PreprocessorMacroDesc> macros;
    for (const auto& d : options.defines)
      macros.push_back({ d.first.c_str(), d.second.c_str() });
    sd.preprocessorMacros     = macros.data();
    sd.preprocessorMacroCount = static_cast<SlangInt>(macros.size());

    std::vector<const char*> paths;
    for (const auto& p : options.searchPaths)
      paths.push_back(p.c_str());
    sd.searchPaths     = paths.data();
    sd.searchPathCount = static_cast<SlangInt>(paths.size());

    {
      std::lock_guard<std::mutex> lock(globalSessionMutex());
      if (SLANG_FAILED(global->createSession(sd, ts.session.writeRef()))) {
        diagnostics += "vtkSlang: failed to create a Slang session\n";
        return nullptr;
      }
    }

    for (const char* name : kLibraryModules) {
      if (!loadEmbedded(ts, name, diagnostics)) {
        ts.session = nullptr;
        ts.modules.clear();
        return nullptr;
      }
    }
    return &ts;
  }

  bool specializationArgs(slang::IComponentType* composite, const CompileOptions& options,
                          std::vector<std::string>& typeNames, std::string& diagnostics)
  {
    const SlangInt count = composite->getSpecializationParamCount();
    if (count == 0) {
      if (!options.specializations.empty()) {
        diagnostics += "vtkSlang: specializations given, but the shader has no type parameters\n";
        return false;
      }
      return true;
    }

    Slang::ComPtr<slang::IBlob> blob;
    slang::ProgramLayout*       layout = composite->getLayout(0, blob.writeRef());
    appendDiagnostics(diagnostics, blob);
    if (!layout)
      return false;
    if (static_cast<SlangInt>(layout->getTypeParameterCount()) != count) {
      diagnostics += "vtkSlang: generic entry points are not supported; use global type_param\n";
      return false;
    }

    std::vector<bool> consumed(options.specializations.size(), false);
    for (unsigned i = 0; i < layout->getTypeParameterCount(); ++i) {
      slang::TypeParameterReflection* param = layout->getTypeParameterByIndex(i);
      if (param->getConstraintCount() != 1) {
        diagnostics += std::string("vtkSlang: type_param '") + param->getName() +
          "' must be constrained by exactly one interface\n";
        return false;
      }
      const std::string iface = param->getConstraintByIndex(0)->getName();
      std::string       chosen = "Default_" + iface;
      for (size_t s = 0; s < options.specializations.size(); ++s) {
        if (options.specializations[s].first == iface) {
          chosen      = options.specializations[s].second;
          consumed[s] = true;
        }
      }
      typeNames.push_back(chosen);
    }
    for (size_t s = 0; s < options.specializations.size(); ++s) {
      if (!consumed[s]) {
        diagnostics += "vtkSlang: the shader has no type parameter for interface '" +
          options.specializations[s].first + "'\n";
        return false;
      }
    }
    return true;
  }

  Artifact compile(TargetSession& ts, const CompileOptions& options, slang::IModule* module,
                   const std::vector<slang::IModule*>& extra,
                   const std::vector<std::string>& entryPoints, std::string diagnostics)
  {
    const Target target = options.target;

    std::vector<Slang::ComPtr<slang::IEntryPoint>> eps;
    if (entryPoints.empty()) {
      for (SlangInt32 i = 0; i < module->getDefinedEntryPointCount(); ++i) {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_SUCCEEDED(module->getDefinedEntryPoint(i, ep.writeRef())))
          eps.push_back(ep);
      }
      if (eps.empty())
        return failure(target, diagnostics + "vtkSlang: the source defines no entry points\n");
    } else {
      for (const std::string& name : entryPoints) {
        Slang::ComPtr<slang::IEntryPoint> ep;
        if (SLANG_FAILED(module->findEntryPointByName(name.c_str(), ep.writeRef())))
          return failure(target, diagnostics + "vtkSlang: entry point '" + name + "' not found\n");
        eps.push_back(ep);
      }
    }

    std::vector<slang::IComponentType*> components{ module };
    for (slang::IModule* m : extra)
      components.push_back(m);
    for (auto& ep : eps)
      components.push_back(ep.get());

    Slang::ComPtr<slang::IBlob>          blob;
    Slang::ComPtr<slang::IComponentType> composite;
    SlangResult r = ts.session->createCompositeComponentType(components.data(),
      static_cast<SlangInt>(components.size()), composite.writeRef(), blob.writeRef());
    appendDiagnostics(diagnostics, blob);
    if (SLANG_FAILED(r))
      return failure(target, diagnostics);

    std::vector<std::string> typeNames;
    if (!specializationArgs(composite, options, typeNames, diagnostics))
      return failure(target, diagnostics);
    if (!typeNames.empty()) {
      std::vector<slang::SpecializationArg> args;
      for (const std::string& t : typeNames)
        args.push_back(slang::SpecializationArg::fromExpr(t.c_str()));
      Slang::ComPtr<slang::IComponentType> specialized;
      blob = nullptr;
      r    = composite->specialize(
        args.data(), static_cast<SlangInt>(args.size()), specialized.writeRef(), blob.writeRef());
      appendDiagnostics(diagnostics, blob);
      if (SLANG_FAILED(r))
        return failure(target, diagnostics);
      composite = specialized;
    }

    Slang::ComPtr<slang::IComponentType> linked;
    blob = nullptr;
    r    = composite->link(linked.writeRef(), blob.writeRef());
    appendDiagnostics(diagnostics, blob);
    if (SLANG_FAILED(r))
      return failure(target, diagnostics);

    Slang::ComPtr<slang::IBlob> code;
    blob = nullptr;
    r    = linked->getTargetCode(0, code.writeRef(), blob.writeRef());
    appendDiagnostics(diagnostics, blob);
    if (SLANG_FAILED(r) || !code)
      return failure(target, diagnostics);

    Artifact a;
    a.target      = target;
    a.source      = ArtifactSource::Slang;
    a.bindings    = reflectBindings(linked, static_cast<SlangInt>(eps.size()));
    a.diagnostics = std::move(diagnostics);
    const char* data = static_cast<const char*>(code->getBufferPointer());
    if (target == Target::WGSL) {
      a.wgsl.assign(data, code->getBufferSize());
    } else {
      const uint32_t* words = reinterpret_cast<const uint32_t*>(data);
      a.spirv.assign(words, words + code->getBufferSize() / sizeof(uint32_t));
    }
    return a;
  }
#endif
};

vtkSlangCompiler::vtkSlangCompiler()
  : m_slang(std::make_unique<SlangState>())
{
}

vtkSlangCompiler::~vtkSlangCompiler()                                       = default;
vtkSlangCompiler::vtkSlangCompiler(vtkSlangCompiler&&) noexcept            = default;
vtkSlangCompiler& vtkSlangCompiler::operator=(vtkSlangCompiler&&) noexcept = default;

std::vector<std::string> vtkSlangCompiler::libraryModules()
{
  return std::vector<std::string>(std::begin(kLibraryModules), std::end(kLibraryModules));
}

std::string vtkSlangCompiler::librarySource(const std::string& name)
{
  for (const char* module : kLibraryModules) {
    if (name == module) {
      const char* source = vtkSlangSources::get(module);
      return source ? source : "";
    }
  }
  return {};
}

bool vtkSlangCompiler::writeLibraryModules(const std::string& directory)
{
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  for (const char* module : kLibraryModules) {
    std::ofstream file(std::filesystem::path(directory) / (std::string(module) + ".slang"),
                       std::ios::binary | std::ios::trunc);
    file << vtkSlangSources::get(module);
    if (!file)
      return false;
  }
  return true;
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

Artifact vtkSlangCompiler::compileSource(const std::string& source,
                                         const std::vector<std::string>& entryPoints,
                                         const CompileOptions& options)
{
  return compileText(source, {}, entryPoints, options);
}

Artifact vtkSlangCompiler::compileText(const std::string& source,
                                       const std::vector<std::string>& programModules,
                                       const std::vector<std::string>& entryPoints,
                                       const CompileOptions& options)
{
#ifdef VTKSLANG_USE_SLANG
  if (slangDisabledByEnv())
    return failure(options.target, "vtkSlang: Slang is disabled\n");

  CompileOptions merged = options;
  merged.modules.insert(merged.modules.begin(), programModules.begin(), programModules.end());

  std::lock_guard<std::mutex> lock(m_slang->mutex);
  auto run = [&]() -> Artifact {
    const std::string key = SlangState::cacheKey("source", source, entryPoints, merged);
    Artifact cached;
    if (m_slang->lookup(key, cached))
      return cached;

    std::string diagnostics;
    auto* ts = m_slang->session(merged, diagnostics);
    if (!ts)
      return failure(merged.target, diagnostics);
    std::vector<slang::IModule*> extra;
    if (!m_slang->loadOptionModules(ts, merged, extra, diagnostics))
      return failure(merged.target, diagnostics);

    std::string name = "vtkslang_user_" + std::to_string(m_slang->userModuleCount++);
    slang::IModule* module = m_slang->loadSource(*ts, name, source.c_str(), diagnostics);
    if (!module)
      return failure(merged.target, diagnostics);
    Artifact a =
      m_slang->compile(*ts, merged, module, extra, entryPoints, std::move(diagnostics));
    m_slang->store(key, a);
    return a;
  };
  Artifact a = run();
  m_slang->dump(Reproducer{ source, programModules, entryPoints, options }, a);
  return a;
#else
  (void)source;
  (void)programModules;
  (void)entryPoints;
  return failure(options.target, "vtkSlang: built without Slang\n");
#endif
}

Artifact vtkSlangCompiler::compileProgram(const Program& program, const CompileOptions& options)
{
  const bool plain = options.defines.empty() && options.searchPaths.empty() && !options.reflect &&
    options.specializations.empty() && options.modules.empty();
  if (plain) {
    for (const Artifact& precompiled : program.precompiled) {
      const bool present = precompiled.target == Target::WGSL ? !precompiled.wgsl.empty()
                                                              : !precompiled.spirv.empty();
      if (precompiled.target == options.target && present) {
        Artifact a = precompiled;
        a.source   = ArtifactSource::Precompiled;
        return a;
      }
    }
  }
  CompileOptions labeled = options;
  if (labeled.label.empty())
    labeled.label = program.name;
  return compileText(program.source, program.modules, program.entryPoints, labeled);
}

void vtkSlangCompiler::setDumpDirectory(const std::string& directory)
{
#ifdef VTKSLANG_USE_SLANG
  std::lock_guard<std::mutex> lock(m_slang->mutex);
  m_slang->dumpDirectory = directory;
#else
  (void)directory;
#endif
}

void vtkSlangCompiler::setCacheDirectory(const std::string& directory)
{
#ifdef VTKSLANG_USE_SLANG
  std::lock_guard<std::mutex> lock(m_slang->mutex);
  m_slang->cacheDirectory = directory;
  if (!directory.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
  }
#else
  (void)directory;
#endif
}

}
