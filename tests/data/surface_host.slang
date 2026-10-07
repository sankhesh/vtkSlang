import vtkslang_surface;

type_param M : IMaterial;
type_param L : ILightingModel;
type_param N : INormalModifier;
type_param O : ISurfaceOutput;
type_param V : IVertexModifier;
type_param H : IShaderHooks;
typealias Default_IMaterial       = HostDrivenMaterial;
typealias Default_ILightingModel  = PhongLighting;
typealias Default_INormalModifier = GeometricNormal;
typealias Default_ISurfaceOutput  = StandardOutput;
typealias Default_IVertexModifier = IdentityVertex;
typealias Default_IShaderHooks    = DefaultHooks;

struct Scene
{
    float4x4 modelToClip;
    float4   lightDirection;
    float4   diffuseColor;
};

[vk::binding(0, 0)] ConstantBuffer<Scene> scene;
[vk::binding(1, 0)] Texture2D colorTexture;
[vk::binding(2, 0)] SamplerState colorSampler;
[vk::binding(0, 1)] ConstantBuffer<Shading<M, L, N, O, V>> shading;
[vk::binding(0, 2)] ConstantBuffer<HookBlock<H>> hookBlock;

struct VertexOutput
{
    float4 position   : SV_Position;
    float3 positionVC : POSITIONVC;
    float3 normal     : NORMAL;
    float2 uv         : UV;
};

struct HostSource : ISurfaceSource
{
    float3 texel;

    void colors(inout FragmentContext f)
    {
        f.diffuseColor = scene.diffuseColor.rgb * texel;
        f.opacity      = scene.diffuseColor.a;
    }
    SurfaceSample sample(FragmentContext f) { return surfaceSample(f); }
    uint lightCount(SurfaceSample s) { return 1u; }
    LightSample light(uint i, SurfaceSample s)
    {
        LightSample l = LightSample();
        l.direction = normalize(scene.lightDirection.xyz);
        return l;
    }
    float3 ambientRadiance(SurfaceSample s) { return float3(0.1f); }
};

[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
    VertexSample v = VertexSample();
    v.position    = position;
    v.normal      = normal;
    v.uv          = uv;
    v.modelToClip = scene.modelToClip;
    VertexContext c;
    v = shadeVertex(shading, hookBlock.hooks, v, c);
    VertexOutput o;
    o.position   = projectVertex(hookBlock.hooks, c, mul(scene.modelToClip, float4(v.position, 1.f)));
    o.positionVC = v.position;
    o.normal     = v.normal;
    o.uv         = v.uv;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    HostSource source;
    source.texel = colorTexture.Sample(colorSampler, input.uv).rgb;
    FragmentContext f = FragmentContext();
    f.positionVC  = float4(input.positionVC, 1.f);
    f.normalVC    = normalize(input.normal);
    f.uv          = input.uv;
    f.fragCoord   = input.position;
    f.frontFacing = frontFacing;
    shadeSurface(shading, hookBlock.hooks, source, f);
    return f.color;
}
