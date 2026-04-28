struct Scene
{
    float4x4 modelToClip;
    float4   lightDirection;
    float4   diffuseColor;
};

[vk::binding(0, 0)] ConstantBuffer<Scene> scene;
[vk::binding(1, 0)] Texture2D colorTexture;
[vk::binding(2, 0)] SamplerState colorSampler;

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
    float3 color   = scene.diffuseColor.rgb * colorTexture.Sample(colorSampler, input.uv).rgb;
    float  diffuse = max(dot(normalize(input.normal), normalize(scene.lightDirection.xyz)), 0.f);
    return float4(color * (diffuse + 0.1f), scene.diffuseColor.a);
}
