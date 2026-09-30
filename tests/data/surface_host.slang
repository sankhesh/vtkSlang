import vtkslang_shading;

type_param M : IMaterial;
type_param L : ILightingModel;
type_param N : INormalModifier;
type_param O : ISurfaceOutput;
type_param V : IVertexModifier;
typealias Default_IMaterial       = HostDrivenMaterial;
typealias Default_ILightingModel  = PhongLighting;
typealias Default_INormalModifier = GeometricNormal;
typealias Default_ISurfaceOutput  = StandardOutput;
typealias Default_IVertexModifier = IdentityVertex;

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
    VertexOutput o;
    o.position   = mul(scene.modelToClip, float4(v.position, 1.f));
    o.positionVC = v.position;
    o.normal     = v.normal;
    o.uv         = v.uv;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input, bool frontFacing : SV_IsFrontFace) : SV_Target
{
    SurfaceSample s = SurfaceSample();
    s.position       = input.positionVC;
    s.normal         = normalize(input.normal);
    s.viewDir        = normalize(-input.positionVC);
    s.uv             = input.uv;
    s.fragCoord      = input.position;
    s.frontFacing    = frontFacing;
    s.host.baseColor = scene.diffuseColor.rgb * colorTexture.Sample(colorSampler, input.uv).rgb;
    s.host.opacity   = scene.diffuseColor.a;
    if (!shading.output.keep(s))
        discard;
    s.normal = shading.normal.normal(s);
    MaterialSample m = shading.material.evaluate(s);
    LightSample l = LightSample();
    l.direction = normalize(scene.lightDirection.xyz);
    float3 radiance = shading.lighting.ambient(m, s, float3(0.1f)) + shading.lighting.direct(m, s, l);
    float4 color = float4(shading.lighting.resolve(radiance, m) + m.emissive, m.opacity);
    return shading.output.finish(color, m, s);
}
