// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdlib>
#include <mutex>
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

#  include <regex>
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

const char* const kLibraryModules[] = { "vtkslang_core" };

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

uint64_t fnv1a(std::string_view s)
{
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
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
  return m_slang->compile(*ts, merged, module, extra, entryPoints, std::move(diagnostics));
#else
  (void)source;
  (void)programModules;
  (void)entryPoints;
  return failure(options.target, "vtkSlang: built without Slang\n");
#endif
}

Artifact vtkSlangCompiler::compileProgram(const Program& program, const CompileOptions& options)
{
  const bool plain =
    options.defines.empty() && options.searchPaths.empty() && options.modules.empty();
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
  return compileText(program.source, program.modules, program.entryPoints, options);
}

}
