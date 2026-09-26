import vtkslang_shading;

type_param M : IMaterial;
type_param L : ILightingModel;
typealias Default_IMaterial      = HostDrivenMaterial;
typealias Default_ILightingModel = LambertLighting;

struct Scene
{
    float4x4 modelToClip;
    float4   lightDirection;
    float4   diffuseColor;
};

struct HostShading<TM : IMaterial, TL : ILightingModel>
{
    TM material;
    TL lighting;
};

[vk::binding(0, 0)] ConstantBuffer<Scene> scene;
[vk::binding(1, 0)] Texture2D colorTexture;
[vk::binding(2, 0)] SamplerState colorSampler;
[vk::binding(0, 1)] ConstantBuffer<HostShading<M, L>> shading;

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
    VertexOutput o;
    o.position   = mul(scene.modelToClip, float4(position, 1.f));
    o.positionVC = position;
    o.normal     = normal;
    o.uv         = uv;
    return o;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target
{
    SurfaceSample s = SurfaceSample();
    s.position       = input.positionVC;
    s.normal         = normalize(input.normal);
    s.viewDir        = normalize(-input.positionVC);
    s.uv             = input.uv;
    s.host.baseColor = scene.diffuseColor.rgb * colorTexture.Sample(colorSampler, input.uv).rgb;
    s.host.opacity   = scene.diffuseColor.a;
    MaterialSample m = shading.material.evaluate(s);
    LightSample l = LightSample();
    l.direction = normalize(scene.lightDirection.xyz);
    float3 radiance = shading.lighting.ambient(m, s, float3(0.1f)) + shading.lighting.direct(m, s, l);
    return float4(shading.lighting.resolve(radiance, m) + m.emissive, m.opacity);
}
