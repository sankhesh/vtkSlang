// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

module vtkslang_surface;
__exported import vtkslang_shading;
__exported import vtkslang_hooks;

public static const int kSurfaceInterfaceVersion = 1;

public interface ISurfaceSource
{
    void colors(inout FragmentContext f);
    void geometry(inout FragmentContext f) {}
    SurfaceSample sample(FragmentContext f);
    uint lightCount(SurfaceSample s);
    LightSample light(uint i, SurfaceSample s);
    float3 ambientRadiance(SurfaceSample s);
    bool opacity(MaterialSample m, inout float4 color) { return true; }
    float depth(FragmentContext f, SurfaceSample s) { return f.depth; }
    void reject() {}
};

public VertexContext vertexContext(VertexSample v)
{
    VertexContext c = VertexContext();
    c.positionMC  = float4(v.position, 1.f);
    c.normalMC    = v.normal;
    c.tangentMC   = v.tangent;
    c.uv          = v.uv;
    c.color       = v.color;
    c.pointId     = v.pointId;
    c.cellId      = v.cellId;
    c.primitiveId = v.primitiveId;
    c.modelToClip = v.modelToClip;
    return c;
}

public VertexSample vertexSample(VertexContext c, VertexSample v)
{
    v.position = c.positionMC.xyz;
    v.normal   = c.normalMC;
    v.tangent  = c.tangentMC;
    v.uv       = c.uv;
    v.color    = c.color;
    return v;
}

public SurfaceSample surfaceSample(FragmentContext f)
{
    SurfaceSample s = SurfaceSample();
    s.position    = f.positionVC.xyz;
    s.normal      = f.normalVC;
    s.tangent     = f.tangentVC;
    s.viewDir     = normalize(-f.positionVC.xyz);
    s.uv          = f.uv;
    s.fragCoord   = f.fragCoord;
    s.primitiveId = f.primitiveId;
    s.cellId      = f.cellId;
    s.frontFacing = f.frontFacing;
    s.host.baseColor     = f.diffuseColor;
    s.host.ambientColor  = f.ambientColor;
    s.host.specularColor = f.specularColor;
    s.host.specularPower = f.specularPower;
    s.host.opacity       = f.opacity;
    return s;
}

public FragmentContext fragmentContext(SurfaceSample s)
{
    FragmentContext f = FragmentContext();
    f.positionVC  = float4(s.position, 1.f);
    f.normalVC    = s.normal;
    f.tangentVC   = s.tangent;
    f.uv          = s.uv;
    f.fragCoord   = s.fragCoord;
    f.primitiveId = s.primitiveId;
    f.cellId      = s.cellId;
    f.frontFacing = s.frontFacing;
    f.depth       = s.fragCoord.z;
    return f;
}

public VertexSample shadeVertex<TM : IMaterial, TL : ILightingModel, TN : INormalModifier,
                                TO : ISurfaceOutput, TV : IVertexModifier, TH : IShaderHooks>(
    Shading<TM, TL, TN, TO, TV> shading, TH hooks, VertexSample v, out VertexContext c)
{
    v = shading.vertex.modify(v);
    c = vertexContext(v);
    hooks.vertex(c);
    return vertexSample(c, v);
}

public float4 projectVertex<TH : IShaderHooks>(TH hooks, inout VertexContext c, float4 positionClip)
{
    c.positionClip = positionClip;
    hooks.position(c);
    return c.positionClip;
}

public bool surfaceCoverage<TM : IMaterial, TL : ILightingModel, TN : INormalModifier,
                            TO : ISurfaceOutput, TV : IVertexModifier, TH : IShaderHooks,
                            TS : ISurfaceSource>(
    Shading<TM, TL, TN, TO, TV> shading, TH hooks, TS source, inout FragmentContext f,
    out SurfaceSample s)
{
    bool covered = hooks.clip(f);
    if (!covered)
        source.reject();
    source.colors(f);
    hooks.color(f);
    source.geometry(f);
    hooks.normal(f);
    s = source.sample(f);
    if (!shading.output.keep(s)) {
        covered = false;
        source.reject();
    }
    f.depth = shading.output.depth(source.depth(f, s), s);
    return covered;
}

public float outputDepth<TH : IShaderHooks>(TH hooks, FragmentContext f)
{
    hooks.output(f);
    return f.depth;
}

public bool surfaceShade<TM : IMaterial, TL : ILightingModel, TN : INormalModifier,
                         TO : ISurfaceOutput, TV : IVertexModifier, TH : IShaderHooks,
                         TS : ISurfaceSource>(
    Shading<TM, TL, TN, TO, TV> shading, TH hooks, TS source, inout FragmentContext f,
    SurfaceSample s)
{
    s.normal = shading.normal.normal(s);
    MaterialSample m = shading.material.evaluate(s);
    const uint lights = source.lightCount(s);
    if (lights == 0u) {
        f.color = float4(m.ambientColor + m.baseColor, m.opacity);
    } else {
        float3 radiance = shading.lighting.ambient(m, s, source.ambientRadiance(s));
        for (uint i = 0u; i < lights; ++i)
            radiance += shading.lighting.direct(m, s, source.light(i, s));
        f.color = float4(shading.lighting.resolve(radiance, m) + m.emissive, m.opacity);
    }
    const bool opaque = source.opacity(m, f.color);
    if (!opaque)
        source.reject();
    hooks.lit(f);
    f.color = shading.output.finish(f.color, m, s);
    hooks.output(f);
    return opaque;
}

public bool shadeSurface<TM : IMaterial, TL : ILightingModel, TN : INormalModifier,
                         TO : ISurfaceOutput, TV : IVertexModifier, TH : IShaderHooks,
                         TS : ISurfaceSource>(
    Shading<TM, TL, TN, TO, TV> shading, TH hooks, TS source, inout FragmentContext f)
{
    SurfaceSample s;
    const bool covered = surfaceCoverage(shading, hooks, source, f, s);
    const bool shaded  = surfaceShade(shading, hooks, source, f, s);
    return covered && shaded;
}
