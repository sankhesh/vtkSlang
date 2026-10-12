module nonuniform_hooks;
import vtkslang_hooks;
public struct Sparkle : IShaderHooks {
  Texture2D ramp;
  SamplerState rampSampler;
  public override void color(inout FragmentContext f) {
    if (f.uv.x > 0.5f)
      f.diffuseColor = ramp.Sample(rampSampler, f.uv).rgb;
  }
};
