# vtkSlang

vtkSlang brings typed shader customization to WebGPU renderers. Write a
material, try a new lighting model, or add a shader hook in
[Slang](https://shader-slang.org); vtkSlang compiles the program to WGSL or
SPIR-V and reflects the layouts your renderer needs to bind its resources.

Materials, lighting models, normal and vertex modifiers, and shader hooks
share explicit interfaces. The compiler checks your implementation, while
reflection exposes its parameters for uniform packing and application
controls. Start with a color adjustment, then explore your own materials
and lighting models.

Two renderers use it:

- VTK's WebGPU backend. The integration is under review as a series of merge
  requests; [ARCHITECTURE.md](ARCHITECTURE.md) describes it.
- [anari-webgpu](https://github.com/sankhesh/anari-webgpu), an ANARI device on
  WebGPU.

VTK and anari-webgpu share these interfaces and the same surface shading
sequence, so compatible materials and hooks can move between renderers. Each
renderer keeps control of its own pipelines and GPU resources.

The compiled library makes no WebGPU calls and needs no WebGPU headers.
Your renderer creates shader modules, bind groups and pipelines through its
own WebGPU implementation, making vtkSlang easy to fit into an existing
rendering backend.

> **Note:** vtkSlang is at version 0.1.0, and its API may change before 1.0.
> Slang's WGSL target is
> [marked experimental](https://github.com/shader-slang/slang#readme) upstream.

## What it provides

Go from a user-defined shader type to compiled code, reflected parameters
and packed uniform data with one library:

| Piece | API | What it does |
| --- | --- | --- |
| Compile | `vtkSlangCompiler::compileSource()`, `compileProgram()` | Slang to WGSL or SPIR-V, with defines, search paths and user modules imported by name. Diagnostics come back as text in the `Artifact`, never printed |
| Specialize | `CompileOptions::specializations` | Interface name to type name. An interface left out uses the program's `Default_<Interface>` typealias |
| Reflect | `CompileOptions::reflect`, `Artifact::bindings` | Every binding with its group, kind and size; uniform fields with offsets, scalar types, shapes and `[Range]`/`[Label]` attributes; texture view dimensions |
| Plan and pack | `planBindings()`, `findField()`, `blockBytes()`, `pack()`, `unmatched()`, `parameterSchema()` | The blocks and resources the renderer doesn't own, packed from reflection, and a JSON schema a UI can build controls from |
| Precompile | `Program::precompiled` | A matching artifact returned without starting Slang when defines, user modules, specializations and search paths are empty and reflection is disabled |
| Cache | `setCacheDirectory()` | Memory and optional disk caches for eligible requests, keyed on compilation inputs, the Slang version and shared-module source hash. Requests with search paths bypass both caches |
| Reproduce | `setDumpDirectory()`, `VTKSLANG_DUMP_DIR`, `vtkslangc` | Capture compiler requests, including cache hits and failures on the dump path, for replay outside the renderer. Precompiled returns bypass dumping |
| WebGPU helper | `vtkSlangWebGPU.h` | Header-only `createShaderModule()` for renderers that link WebGPU directly |

Slang sits behind a pImpl: no header includes `slang.h`, and the library links
Slang privately.

## The shared Slang modules

The shared modules give you the building blocks for custom shading, from a
single hook to a complete material or lighting model. They are embedded in
the library and available by import name. Their sources are also installed
under `share/vtkSlang/shaders`, and
`vtkSlangCompiler::writeLibraryModules()` writes them to a directory for
editors and other tools.

| Module | Contents |
| --- | --- |
| `vtkslang_shading` | `IMaterial`, `ILightingModel`, `INormalModifier`, `ISurfaceOutput`, `IVertexModifier`, their sample structs, and defaults: `HostDrivenMaterial`, `PhongLighting`, `LambertLighting`, `CookTorranceLighting`, `GeometricNormal`, `StandardOutput`, `IdentityVertex` |
| `vtkslang_hooks` | `IShaderHooks` (`vertex`, `position`, `clip`, `color`, `normal`, `lit`, `output`, each a no-op unless overridden), `VertexContext`, `FragmentContext`, `DefaultHooks`, `HookBlock<H>` |
| `vtkslang_surface` | `ISurfaceSource` and the shading sequence both renderers share: `shadeVertex()`, `shadeSurface()` |
| `vtkslang_attributes` | `[Range]` and `[Label]`, which reach the parameter schema without changing the shader |
| `vtkslang_core` | `PI`, Reinhard tone mapping and gamma helpers |

Each interface module exports a version constant (`kShadingInterfaceVersion`,
`kHooksInterfaceVersion`, `kSurfaceInterfaceVersion`).

## Build and try it

vtkSlang needs CMake 3.21 or newer and a C++17 compiler. It looks for Slang
with `find_package(Slang)`; if that fails, CMake downloads the Slang 2026.13.1
release for macOS (arm64 or x86_64), Linux x86_64 or Windows x86_64.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
cmake --install build --prefix /path/to/install
```

| Option | Default | Effect |
| --- | --- | --- |
| `VTKSLANG_USE_SLANG` | `ON` | `OFF` builds the library without the Slang compiler, so only precompiled programs are available. Emscripten builds don't use Slang yet; see below |
| `VTKSLANG_BUILD_TOOLS` | `ON` when top level | Builds `vtkslangc` (needs Slang) |
| `VTKSLANG_BUILD_TESTING` | `ON` when top level | Builds the tests |
| `VTKSLANG_TINT_EXECUTABLE` | `tint` on `PATH` | The tint that `vtkslangc` uses to validate WGSL, and that two extra tests use |
| `FETCHCONTENT_SOURCE_DIR_SLANG_DIST` | | An extracted Slang release to use instead of downloading one |

On Emscripten, vtkSlang builds without the Slang compiler, so WebAssembly
renderers use precompiled programs and can't compile user modules at run time.

## Add vtkSlang to your renderer

```cmake
find_package(vtkSlang 0.1 REQUIRED)
target_link_libraries(my_renderer PRIVATE vtkSlang::vtkSlang)
```

Or fetch it at a commit, as anari-webgpu does:

```cmake
include(FetchContent)
FetchContent_Declare(vtkSlang
  GIT_REPOSITORY https://github.com/sankhesh/vtkSlang.git
  GIT_TAG <commit>)
FetchContent_MakeAvailable(vtkSlang)
```

## Try a custom hook

Start with a small change: `Dim` exposes an `amount` parameter and scales the
fragment color through the `lit()` hook. The renderer supplies the program;
the user supplies the hook.

The program declares a `type_param` for each interface it lets users replace,
with a `Default_<Interface>` typealias for its default implementation. Select
`Dim` by name, compile, and use reflection to pack its parameter:

```cpp
#include <vtkSlang.h>

#include <iostream>

int main()
{
  const char* program = R"(
import vtkslang_hooks;

type_param H : IShaderHooks;
typealias Default_IShaderHooks = DefaultHooks;

[vk::binding(0, 0)] ConstantBuffer<HookBlock<H>> hookBlock;

[shader("fragment")]
float4 fragmentMain(float2 uv : UV) : SV_Target
{
  FragmentContext f = FragmentContext();
  f.uv = uv;
  hookBlock.hooks.color(f);
  f.color = float4(f.diffuseColor, f.opacity);
  hookBlock.hooks.lit(f);
  hookBlock.hooks.output(f);
  return f.color;
}
)";

  const char* userModule = R"(
module dim;
import vtkslang_hooks;

public struct Dim : IShaderHooks
{
  [Range(0, 1)] [Label("Amount")] float amount;
  public override void lit(inout FragmentContext f) { f.color.rgb *= amount; }
};
)";

  vtkslang::vtkSlangCompiler compiler;

  vtkslang::CompileOptions options;
  options.modules = { userModule };
  options.specializations = { { "IShaderHooks", "Dim" } };
  options.reflect = true;

  vtkslang::Artifact artifact = compiler.compileSource(program, { "fragmentMain" }, options);
  if (!artifact)
  {
    std::cerr << artifact.diagnostics;
    return 1;
  }

  vtkslang::BindingPlan plan =
    vtkslang::planBindings(artifact, [](const vtkslang::ReflectedBinding&) { return false; });

  for (const auto& block : plan.blocks)
  {
    if (const vtkslang::UniformField* field = vtkslang::findField(block, "hooks.amount"))
    {
      float amount = 0.5f;
      std::vector<uint8_t> bytes = vtkslang::blockBytes(block);
      vtkslang::pack(*field, { vtkslang::ScalarType::Float32, 1, 1, &amount }, bytes);
    }
  }

  std::cout << artifact.wgsl;
  return 0;
}
```

`artifact.wgsl` is ready for `wgpuDeviceCreateShaderModule`. The plan has one
16-byte block, `hookBlock` at group 0, binding 0, with `hooks.amount` at
offset 0; the renderer uploads `bytes` to the uniform buffer it binds there.
The predicate passed to `planBindings()` returns true for the bindings the
renderer fills itself, which then stay out of the plan.

The program and modules are source text, so they can come from anywhere:
embedded in the renderer at build time, or read from `.slang` files. Imports
resolve through `CompileOptions::searchPaths`, so a module on disk can import
the modules next to it:

```cpp
std::ifstream file("materials/dim.slang");
options.modules = { std::string(std::istreambuf_iterator<char>(file), {}) };
options.searchPaths = { "materials" };
```

Diagnostics name such a module `vtkslang-embedded:/<module>.slang` rather than
its path on disk.

A mistake in the user's module fails the compile with Slang's error in that
module. Misspell `color` as `colour` and the diagnostics read:

```
error[E30027]: member not found
 --> vtkslang-embedded:/dim.slang:7:57
  |
7 | public override void lit(inout FragmentContext f) { f.colour.rgb *= amount; }
  |                                                       ^^^^^^ 'colour' is not a member of 'FragmentContext'.
```

## vtkslangc

Try a shader change without launching the renderer. `vtkslangc` compiles a
program with the same options, prints reflected parameters or a schema, and
validates WGSL with tint when available.

```sh
vtkslangc host.slang -m dim.slang -s IShaderHooks=Dim --reflect -o host.wgsl
vtkslangc host.slang -m dim.slang -s IShaderHooks=Dim --schema -
```

`--reflect` prints each binding and its fields:

```
group 0 binding 0: hookBlock (uniform, 16 bytes)
  hooks.amount: float at 0, 4 bytes, 1 x 1 every 4 bytes [Range(0, 1)] [Label(Amount)]
```

To bring a shader problem out of the renderer and into the command line, set
`VTKSLANG_DUMP_DIR` and run your application. Compiler requests that reach the
dump path write a bundle with the program, its modules, options, and output
or diagnostics. `vtkslangc --replay` compiles the bundle again; use `-m` or
`-s` to replace its user modules or specializations and try a fix.
Precompiled returns bypass dumping.

```sh
VTKSLANG_DUMP_DIR=/tmp/vtkslang-dumps ./my_renderer
vtkslangc --replay /tmp/vtkslang-dumps/compile-42ae86a615844535 -o -
```

## Environment variables

| Variable | Effect |
| --- | --- |
| `VTKSLANG_DUMP_DIR` | Enable compilation reproducer bundles, unless `setDumpDirectory()` named a directory |
| `VTKSLANG_DISABLE_SLANG` | `1` makes the process behave as if Slang were missing: `slangAvailable()` is false, and only precompiled artifacts come back. Useful for testing a renderer's fallback |
| `VTKSLANG_TINT` | The tint `vtkslangc` uses when `--tint` isn't given |

## License

vtkSlang is distributed under the BSD 3-Clause license; see
[LICENSE](LICENSE).
