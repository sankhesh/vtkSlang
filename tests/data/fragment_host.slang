import vtkslang_hooks;
type_param H : IShaderHooks;
typealias Default_IShaderHooks = DefaultHooks;
[vk::binding(0, 0)] ConstantBuffer<HookBlock<H>> hookBlock;
struct VSOut { float4 position : SV_Position; float2 uv : UV; };
[shader("vertex")]
VSOut vertexMain(float3 p : POSITION)
{
  VertexContext v = VertexContext();
  v.positionMC = float4(p, 1.f);
  hookBlock.hooks.vertex(v);
  v.positionClip = v.positionMC;
  hookBlock.hooks.position(v);
  VSOut o;
  o.position = v.positionClip;
  o.uv = v.uv;
  return o;
}
[shader("fragment")]
float4 fragmentMain(VSOut i) : SV_Target
{
  FragmentContext f = FragmentContext();
  f.uv = i.uv;
  hookBlock.hooks.color(f);
  f.color = float4(f.diffuseColor, f.opacity);
  hookBlock.hooks.lit(f);
  hookBlock.hooks.output(f);
  return f.color;
}
