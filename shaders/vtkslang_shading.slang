// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

module vtkslang_shading;
import vtkslang_core;

public static const int kShadingInterfaceVersion = 2;

public struct HostMaterial
{
    public float3 baseColor     = float3(1.f);
    public float  opacity       = 1.f;
    public float  metallic      = 0.f;
    public float  roughness     = 1.f;
    public float3 emissive      = float3(0.f);
    public float3 ambientColor  = float3(0.f);
    public float3 specularColor = float3(0.f);
    public float  specularPower = 1.f;
};

public struct CameraSample
{
    public float4x4 shadingToClip = float4x4(1.f, 0.f, 0.f, 0.f,
                                             0.f, 1.f, 0.f, 0.f,
                                             0.f, 0.f, 1.f, 0.f,
                                             0.f, 0.f, 0.f, 1.f);
    public bool     parallel      = false;
};

public struct SurfaceSample
{
    public float3       position    = float3(0.f);
    public float3       normal      = float3(0.f, 0.f, 1.f);
    public float3       viewDir     = float3(0.f, 0.f, 1.f);
    public float2       uv          = float2(0.f);
    public float        scalar      = 0.f;
    public float4       fragCoord   = float4(0.f);
    public uint         primitiveId = 0u;
    public uint         cellId      = 0u;
    public bool         frontFacing = true;
    public uint4        userData    = uint4(0u);
    public CameraSample camera;
    public HostMaterial host;
};

public struct MaterialSample
{
    public float3 baseColor     = float3(1.f);
    public float  opacity       = 1.f;
    public float  metallic      = 0.f;
    public float  roughness     = 1.f;
    public float3 emissive      = float3(0.f);
    public float3 ambientColor  = float3(0.f);
    public float3 specularColor = float3(0.f);
    public float  specularPower = 1.f;
};

public struct LightSample
{
    public float3 direction = float3(0.f, 0.f, 1.f);
    public float3 radiance  = float3(1.f);
};

public interface IMaterial
{
    MaterialSample evaluate(SurfaceSample s);
};

public interface ILightingModel
{
    float3 direct(MaterialSample m, SurfaceSample s, LightSample light);
    float3 ambient(MaterialSample m, SurfaceSample s, float3 ambientRadiance);
    float3 resolve(float3 radiance, MaterialSample m);
};

public interface INormalModifier
{
    float3 normal(SurfaceSample s);
};

public interface ISurfaceOutput
{
    bool   keep(SurfaceSample s);
    float4 finish(float4 color, MaterialSample m, SurfaceSample s);
    float  depth(float depth, SurfaceSample s);
};

public struct VertexSample
{
    public float3   position = float3(0.f);
    public float3   normal   = float3(0.f, 0.f, 1.f);
    public float2   uv       = float2(0.f);
    public float4   color    = float4(1.f);
    public uint     pointId  = 0u;
    public uint     cellId   = 0u;
    public uint     primitiveId = 0u;
    public float4x4 modelToClip = float4x4(1.f, 0.f, 0.f, 0.f,
                                           0.f, 1.f, 0.f, 0.f,
                                           0.f, 0.f, 1.f, 0.f,
                                           0.f, 0.f, 0.f, 1.f);
};

public interface IVertexModifier
{
    VertexSample modify(VertexSample v);
};

public struct Shading<TM : IMaterial, TL : ILightingModel, TN : INormalModifier,
                      TO : ISurfaceOutput, TV : IVertexModifier>
{
    public TM material;
    public TL lighting;
    public TN normal;
    public TO output;
    public TV vertex;
};

public struct GeometricNormal : INormalModifier
{
    public float3 normal(SurfaceSample s) { return s.normal; }
};

public struct StandardOutput : ISurfaceOutput
{
    public bool   keep(SurfaceSample s) { return true; }
    public float4 finish(float4 color, MaterialSample m, SurfaceSample s) { return color; }
    public float  depth(float depth, SurfaceSample s) { return depth; }
};

public struct IdentityVertex : IVertexModifier
{
    public VertexSample modify(VertexSample v) { return v; }
};

public struct HostDrivenMaterial : IMaterial
{
    public MaterialSample evaluate(SurfaceSample s)
    {
        MaterialSample m = MaterialSample();
        m.baseColor = s.host.baseColor;
        m.opacity   = s.host.opacity;
        m.metallic  = s.host.metallic;
        m.roughness = s.host.roughness;
        m.emissive  = s.host.emissive;
        m.ambientColor  = s.host.ambientColor;
        m.specularColor = s.host.specularColor;
        m.specularPower = s.host.specularPower;
        return m;
    }
};

public struct PhongLighting : ILightingModel
{
    public float3 direct(MaterialSample m, SurfaceSample s, LightSample light)
    {
        float  df = max(dot(s.normal, light.direction), 0.f);
        float3 r  = reflect(-light.direction, s.normal);
        float  sf = pow(max(dot(s.viewDir, r), 0.f), m.specularPower);
        return (df * m.baseColor + sf * m.specularColor) * light.radiance;
    }
    public float3 ambient(MaterialSample m, SurfaceSample s, float3 ambientRadiance)
    {
        return m.ambientColor * ambientRadiance;
    }
    public float3 resolve(float3 radiance, MaterialSample m)
    {
        return radiance;
    }
};

public struct LambertLighting : ILightingModel
{
    public float3 direct(MaterialSample m, SurfaceSample s, LightSample light)
    {
        return m.baseColor * light.radiance * max(dot(s.normal, light.direction), 0.f);
    }
    public float3 ambient(MaterialSample m, SurfaceSample s, float3 ambientRadiance)
    {
        return m.baseColor * ambientRadiance;
    }
    public float3 resolve(float3 radiance, MaterialSample m)
    {
        return min(radiance, m.baseColor);
    }
};

public struct CookTorranceLighting : ILightingModel
{
    static float D_GGX(float NdotH, float alpha)
    {
        float a2 = alpha * alpha;
        float d  = NdotH * NdotH * (a2 - 1.f) + 1.f;
        return a2 / (PI * d * d + 1e-4f);
    }
    static float G_SchlickGGX(float NdotX, float roughness)
    {
        float r = roughness + 1.f;
        float k = (r * r) / 8.f;
        return NdotX / (NdotX * (1.f - k) + k + 1e-4f);
    }
    static float3 F_Schlick(float cosTheta, float3 F0)
    {
        float t  = clamp(1.f - cosTheta, 0.f, 1.f);
        float t5 = t * t * t * t * t;
        return F0 + (float3(1.f) - F0) * t5;
    }

    public float3 direct(MaterialSample m, SurfaceSample s, LightSample light)
    {
        float3 N = s.normal;
        float3 V = s.viewDir;
        float3 L = light.direction;
        float3 H = normalize(V + L);
        float NdotL = max(dot(N, L), 0.f);
        float NdotV = max(dot(N, V), 1e-4f);
        float NdotH = max(dot(N, H), 0.f);
        float HdotV = max(dot(H, V), 0.f);

        float3 F0       = lerp(float3(0.04f), m.baseColor, m.metallic);
        float  alpha    = m.roughness * m.roughness;
        float  D        = D_GGX(NdotH, alpha);
        float  G        = G_SchlickGGX(NdotV, m.roughness) * G_SchlickGGX(NdotL, m.roughness);
        float3 F        = F_Schlick(HdotV, F0);
        float3 specular = (D * G * F) / (4.f * NdotV * NdotL + 1e-4f);
        float3 kD       = (float3(1.f) - F) * (1.f - m.metallic);
        float3 diffuse  = kD * m.baseColor / PI;
        return (diffuse + specular) * light.radiance * NdotL;
    }
    public float3 ambient(MaterialSample m, SurfaceSample s, float3 ambientRadiance)
    {
        return ambientRadiance * m.baseColor;
    }
    public float3 resolve(float3 radiance, MaterialSample m)
    {
        return tonemapAndGamma(radiance);
    }
};
