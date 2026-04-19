// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"

#include <cstdlib>
#include <mutex>
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
  };

  std::mutex mutex;
  std::unordered_map<std::string, TargetSession> sessions;
  unsigned userModuleCount = 0;

  static std::string sessionKey(const CompileOptions& options)
  {
    return (options.target == Target::WGSL) ? "wgsl" : "spirv";
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
#ifdef VTKSLANG_USE_SLANG
  if (slangDisabledByEnv())
    return failure(options.target, "vtkSlang: Slang is disabled\n");

  std::lock_guard<std::mutex> lock(m_slang->mutex);
  std::string diagnostics;
  auto* ts = m_slang->session(options, diagnostics);
  if (!ts)
    return failure(options.target, diagnostics);

  std::string name = "vtkslang_user_" + std::to_string(m_slang->userModuleCount++);
  slang::IModule* module = m_slang->loadSource(*ts, name, source.c_str(), diagnostics);
  if (!module)
    return failure(options.target, diagnostics);
  return m_slang->compile(*ts, options, module, entryPoints, std::move(diagnostics));
#else
  (void)source;
  (void)entryPoints;
  return failure(options.target, "vtkSlang: built without Slang\n");
#endif
}

}
