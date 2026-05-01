// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vtkSlangCompiler.h"

#include <webgpu/webgpu.h>

#include <cstdio>
#include <cstring>

namespace vtkslang {

inline WGPUShaderModule createShaderModule(
  WGPUDevice device, const Artifact& artifact, const char* label = "vtkSlang")
{
  if (!artifact || !device)
    return nullptr;

  WGPUShaderModuleDescriptor desc{};
  desc.label = WGPUStringView{ label, std::strlen(label) };

  if (artifact.target == Target::SPIRV) {
    WGPUShaderSourceSPIRV spirv{};
    spirv.chain.sType = WGPUSType_ShaderSourceSPIRV;
    spirv.codeSize    = static_cast<uint32_t>(artifact.spirv.size());
    spirv.code        = artifact.spirv.data();
    desc.nextInChain  = &spirv.chain;
    return wgpuDeviceCreateShaderModule(device, &desc);
  }

  WGPUShaderSourceWGSL wgsl{};
  wgsl.chain.sType = WGPUSType_ShaderSourceWGSL;
  wgsl.code        = WGPUStringView{ artifact.wgsl.data(), artifact.wgsl.size() };
  desc.nextInChain = &wgsl.chain;
  WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &desc);

  if (module) {
    WGPUCompilationInfoCallbackInfo cb{};
    cb.mode     = WGPUCallbackMode_AllowSpontaneous;
    cb.callback = [](WGPUCompilationInfoRequestStatus, WGPUCompilationInfo const* info,
                    void* userdata, void*) {
      if (!info)
        return;
      for (size_t i = 0; i < info->messageCount; ++i) {
        const WGPUCompilationMessage& m = info->messages[i];
        if (m.type == WGPUCompilationMessageType_Error) {
          std::fprintf(stderr, "[vtkSlang] WGSL compile error in %s @%llu:%llu: %.*s\n",
            static_cast<const char*>(userdata), static_cast<unsigned long long>(m.lineNum),
            static_cast<unsigned long long>(m.linePos), static_cast<int>(m.message.length),
            m.message.data);
        }
      }
    };
    cb.userdata1 = const_cast<char*>(label);
    wgpuShaderModuleGetCompilationInfo(module, cb);
  }
  return module;
}

}
