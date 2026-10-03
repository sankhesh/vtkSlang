// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

module vtkslang_hooks;
__exported import vtkslang_attributes;

public static const int kHooksInterfaceVersion = 1;

public struct VertexContext
{
    public float4   positionMC  = float4(0.f, 0.f, 0.f, 1.f);
    public float3   normalMC    = float3(0.f, 0.f, 1.f);
    public float3   tangentMC   = float3(0.f);
    public float2   uv          = float2(0.f);
    public float4   color       = float4(1.f);
    public float4   positionClip = float4(0.f, 0.f, 0.f, 1.f);
    public uint     pointId     = 0u;
    public uint     cellId      = 0u;
    public uint     primitiveId = 0u;
    public float4x4 modelToClip = float4x4(1.f, 0.f, 0.f, 0.f,
                                           0.f, 1.f, 0.f, 0.f,
                                           0.f, 0.f, 1.f, 0.f,
                                           0.f, 0.f, 0.f, 1.f);
    public float3x3 normalModelToView = float3x3(1.f, 0.f, 0.f,
                                                 0.f, 1.f, 0.f,
                                                 0.f, 0.f, 1.f);
};

public struct FragmentContext
{
    public float4 positionVC    = float4(0.f, 0.f, 0.f, 1.f);
    public float3 normalVC      = float3(0.f, 0.f, 1.f);
    public float3 tangentVC     = float3(0.f);
    public float2 uv            = float2(0.f);
    public float3 ambientColor  = float3(0.f);
    public float3 diffuseColor  = float3(1.f);
    public float3 specularColor = float3(0.f);
    public float  specularPower = 1.f;
    public float  opacity       = 1.f;
    public float4 color         = float4(0.f, 0.f, 0.f, 1.f);
    public float  depth         = 0.f;
    public uint4  ids           = uint4(0u);
    public float4 fragCoord     = float4(0.f);
    public uint   primitiveId   = 0u;
    public uint   cellId        = 0u;
    public bool   frontFacing   = true;
};

public interface IShaderHooks
{
    void vertex(inout VertexContext v) {}

    void position(inout VertexContext v) {}

    bool clip(inout FragmentContext f) { return true; }

    void color(inout FragmentContext f) {}

    void normal(inout FragmentContext f) {}

    void lit(inout FragmentContext f) {}

    void output(inout FragmentContext f) {}
};

public struct DefaultHooks : IShaderHooks
{
};

public struct HookBlock<TH : IShaderHooks>
{
    public TH hooks;
};
