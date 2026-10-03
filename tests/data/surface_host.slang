import vtkslang_hooks;
import vtkslang_shading;

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

[shader("vertex")]
VertexOutput vertexMain(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0)
{
    VertexSample v = VertexSample();
    v.position    = position;
    v.normal      = normal;
    v.uv          = uv;
    v.modelToClip = scene.modelToClip;
    v = shading.vertex.modify(v);
    VertexContext c = VertexContext();
    c.positionMC  = float4(v.position, 1.f);
    c.normalMC    = v.normal;
    c.uv          = v.uv;
    c.modelToClip = v.modelToClip;
    hookBlock.hooks.vertex(c);
    c.positionClip = mul(scene.modelToClip, c.positionMC);
    hookBlock.hooks.position(c);
    VertexOutput o;
    o.position   = c.positionClip;
    o.positionVC = c.positionMC.xyz;
    o.normal     = c.normalMC;
    o.uv         = c.uv;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    FragmentContext f = FragmentContext();
    f.positionVC  = float4(input.positionVC, 1.f);
    f.normalVC    = normalize(input.normal);
    f.uv          = input.uv;
    f.fragCoord   = input.position;
    f.frontFacing = frontFacing;
    if (!hookBlock.hooks.clip(f))
        discard;
    f.diffuseColor = scene.diffuseColor.rgb * colorTexture.Sample(colorSampler, input.uv).rgb;
    f.opacity      = scene.diffuseColor.a;
    hookBlock.hooks.color(f);
    hookBlock.hooks.normal(f);

    SurfaceSample s = SurfaceSample();
    s.position       = f.positionVC.xyz;
    s.normal         = f.normalVC;
    s.viewDir        = normalize(-f.positionVC.xyz);
    s.uv             = f.uv;
    s.fragCoord      = f.fragCoord;
    s.frontFacing    = f.frontFacing;
    s.host.baseColor = f.diffuseColor;
    s.host.opacity   = f.opacity;
    if (!shading.output.keep(s))
        discard;
    s.normal = shading.normal.normal(s);
    MaterialSample m = shading.material.evaluate(s);
    LightSample l = LightSample();
    l.direction = normalize(scene.lightDirection.xyz);
    float3 radiance = shading.lighting.ambient(m, s, float3(0.1f)) + shading.lighting.direct(m, s, l);
    f.color = float4(shading.lighting.resolve(radiance, m) + m.emissive, m.opacity);
    hookBlock.hooks.lit(f);
    f.color = shading.output.finish(f.color, m, s);
    hookBlock.hooks.output(f);
    return f.color;
}
