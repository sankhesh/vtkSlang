// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangCompiler.h"
#include "vtkSlangParameters.h"
#include "vtkSlangReproducer.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef _WIN32
#  include <sys/wait.h>
#endif

#ifndef VTKSLANG_TINT_EXECUTABLE
#  define VTKSLANG_TINT_EXECUTABLE ""
#endif

namespace {

const char* kUsage = R"(usage: vtkslangc [options] <program.slang>
       vtkslangc [options] --replay BUNDLE

Compiles a Slang program, or a reproducer bundle written by a host
(VTKSLANG_DUMP_DIR), with the given modules and specializations, and validates
the WGSL with tint when one is available.

  -e NAME            entry point (repeatable)
  -m FILE            module source (repeatable; replaces a bundle's modules,
                     but not the program's own)
  -I DIR             search path (repeatable)
  -D NAME[=VALUE]    define (repeatable)
  -s IFACE=TYPE      specialize an interface (replaces it in a bundle)
  -o FILE            write the WGSL to FILE ('-' for stdout)
  --reflect          print the bindings and their uniform fields
  --schema FILE      write the parameter schema as JSON ('-' for stdout)
  --parameters NAME  top-level parameter the schema covers (repeatable;
                     default: shading and hookBlock)
  --tint PATH        validate the WGSL with this tint (default: $VTKSLANG_TINT,
                     then the tint found when vtkSlang was built)
  --no-tint          do not validate the WGSL
  --dump DIR         write a reproducer bundle under DIR
  --label NAME       name of the bundle
  -h, --help         show this help
)";

bool readFile(const std::string& path, std::string& text)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::stringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  return true;
}

bool writeOutput(const std::string& path, const std::string& text)
{
  if (path == "-") {
    std::fwrite(text.data(), 1, text.size(), stdout);
    return true;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

void replace(std::vector<std::pair<std::string, std::string>>& pairs,
  const std::pair<std::string, std::string>& pair)
{
  for (auto& p : pairs)
    if (p.first == pair.first) {
      p.second = pair.second;
      return;
    }
  pairs.push_back(pair);
}

std::pair<std::string, std::string> splitPair(const std::string& text)
{
  const size_t eq = text.find('=');
  if (eq == std::string::npos)
    return { text, {} };
  return { text.substr(0, eq), text.substr(eq + 1) };
}

const char* kindName(vtkslang::BindingKind kind)
{
  switch (kind) {
  case vtkslang::BindingKind::UniformBuffer:
    return "uniform";
  case vtkslang::BindingKind::ReadOnlyStorageBuffer:
    return "read-only storage";
  case vtkslang::BindingKind::StorageBuffer:
    return "storage";
  case vtkslang::BindingKind::SampledTexture:
    return "texture";
  case vtkslang::BindingKind::StorageTexture:
    return "storage texture";
  case vtkslang::BindingKind::Sampler:
    return "sampler";
  default:
    return "other";
  }
}

std::string reflection(const vtkslang::Artifact& artifact)
{
  std::ostringstream out;
  for (const vtkslang::ReflectedBinding& b : artifact.bindings) {
    out << "group " << b.group << " binding " << b.binding << ": " << b.name << " ("
        << kindName(b.kind);
    if (b.sizeBytes)
      out << ", " << b.sizeBytes << " bytes";
    out << (b.used ? "" : ", unused") << ")\n";
    for (const vtkslang::UniformField& f : b.fields) {
      out << "  " << f.name << ": " << f.type << " at " << f.offset << ", " << f.size << " bytes";
      if (f.width)
        out << ", " << f.runs << " x " << f.width << " every " << f.stride << " bytes";
      for (const vtkslang::UserAttribute& a : f.attributes) {
        out << " [" << a.name << "(";
        for (size_t i = 0; i < a.arguments.size(); ++i)
          out << (i ? ", " : "") << a.arguments[i];
        out << ")]";
      }
      out << "\n";
    }
  }
  return out.str();
}

std::string shellQuote(const std::string& text)
{
#ifdef _WIN32
  return "\"" + text + "\"";
#else
  std::string out = "'";
  for (char c : text)
    out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
#endif
}

bool validateWithTint(const std::string& tint, const std::string& wgsl, std::string input)
{
  namespace fs = std::filesystem;
  const std::string stem      = "vtkslangc-" + std::to_string(std::hash<std::string>{}(wgsl));
  const bool        temporary = input.empty();
  if (temporary) {
    input = (fs::temp_directory_path() / (stem + ".wgsl")).string();
    if (!writeOutput(input, wgsl)) {
      std::fprintf(stderr, "vtkslangc: cannot write %s\n", input.c_str());
      return false;
    }
  }
  const fs::path    output  = fs::temp_directory_path() / (stem + ".out.wgsl");
  const std::string command = shellQuote(tint) + " --format wgsl " + shellQuote(input) + " -o " +
    shellQuote(output.string());
  int status = std::system(command.c_str());
#ifndef _WIN32
  status = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  std::error_code ec;
  fs::remove(output, ec);
  if (status != 0) {
    std::fprintf(stderr, "vtkslangc: tint rejected the WGSL in %s\n", input.c_str());
    return false;
  }
  if (temporary)
    fs::remove(input, ec);
  return true;
}

}

int main(int argc, char** argv)
{
  std::string              program, replay, outputPath, schemaPath, tint, dumpDir, label;
  std::vector<std::string> entries, modules, searchPaths;
  std::vector<std::pair<std::string, std::string>> defines, specializations;
  std::set<std::string>    parameters;
  bool                     reflect = false, noTint = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg   = argv[i];
    auto              value = [&](std::string& out) {
      if (i + 1 >= argc)
        return false;
      out = argv[++i];
      return true;
    };
    std::string v;
    bool        ok = true;
    if (arg == "-h" || arg == "--help") {
      std::fputs(kUsage, stdout);
      return 0;
    } else if (arg == "-e")
      ok = value(v) && (entries.push_back(v), true);
    else if (arg == "-m")
      ok = value(v) && (modules.push_back(v), true);
    else if (arg == "-I")
      ok = value(v) && (searchPaths.push_back(v), true);
    else if (arg == "-D")
      ok = value(v) && (defines.push_back(splitPair(v)), true);
    else if (arg == "-s")
      ok = value(v) && v.find('=') != std::string::npos && (specializations.push_back(splitPair(v)), true);
    else if (arg == "-o")
      ok = value(outputPath);
    else if (arg == "--replay")
      ok = value(replay);
    else if (arg == "--reflect")
      reflect = true;
    else if (arg == "--schema")
      ok = value(schemaPath);
    else if (arg == "--parameters")
      ok = value(v) && (parameters.insert(v), true);
    else if (arg == "--tint")
      ok = value(tint);
    else if (arg == "--no-tint")
      noTint = true;
    else if (arg == "--dump")
      ok = value(dumpDir);
    else if (arg == "--label")
      ok = value(label);
    else if (!arg.empty() && arg[0] != '-' && program.empty())
      program = arg;
    else
      ok = false;
    if (!ok) {
      std::fprintf(stderr, "vtkslangc: bad argument '%s'\n%s", arg.c_str(), kUsage);
      return 2;
    }
  }
  if ((!program.empty()) + (!replay.empty()) != 1) {
    std::fprintf(stderr, "vtkslangc: give one program or --replay\n%s", kUsage);
    return 2;
  }

  vtkslang::Reproducer r;
  std::string          error;
  if (!replay.empty() && !vtkslang::readReproducer(replay, r, error)) {
    std::fprintf(stderr, "vtkslangc: %s\n", error.c_str());
    return 2;
  }
  if (!program.empty() && !readFile(program, r.source)) {
    std::fprintf(stderr, "vtkslangc: cannot read %s\n", program.c_str());
    return 2;
  }
  if (!entries.empty())
    r.entryPoints = entries;
  if (!modules.empty()) {
    r.options.modules.clear();
    for (const std::string& path : modules) {
      std::string source;
      if (!readFile(path, source)) {
        std::fprintf(stderr, "vtkslangc: cannot read %s\n", path.c_str());
        return 2;
      }
      r.options.modules.push_back(std::move(source));
    }
  }
  for (const std::string& path : searchPaths)
    r.options.searchPaths.push_back(path);
  for (const auto& d : defines)
    replace(r.options.defines, d);
  for (const auto& s : specializations)
    replace(r.options.specializations, s);
  if (!label.empty())
    r.options.label = label;
  r.options.reflect = true;

  vtkslang::vtkSlangCompiler compiler;
  if (!vtkslang::vtkSlangCompiler::slangAvailable()) {
    std::fprintf(stderr, "vtkslangc: Slang is unavailable\n");
    return 1;
  }
  if (!dumpDir.empty())
    compiler.setDumpDirectory(dumpDir);
  vtkslang::Program p;
  p.source      = r.source;
  p.modules     = r.programModules;
  p.entryPoints = r.entryPoints;
  const vtkslang::Artifact a = compiler.compileProgram(p, r.options);
  std::fputs(a.diagnostics.c_str(), stderr);
  if (!a) {
    std::fprintf(stderr, "vtkslangc: the Slang compile failed\n");
    return 1;
  }

  if (!outputPath.empty() && !writeOutput(outputPath, a.wgsl)) {
    std::fprintf(stderr, "vtkslangc: cannot write %s\n", outputPath.c_str());
    return 1;
  }
  if (reflect)
    std::fputs(reflection(a).c_str(), stdout);
  if (!schemaPath.empty()) {
    if (parameters.empty())
      parameters = { "shading", "hookBlock" };
    const vtkslang::BindingPlan plan =
      vtkslang::planBindings(a, [&parameters](const vtkslang::ReflectedBinding& b) {
        return parameters.count(b.name.substr(0, b.name.find('.'))) == 0;
      });
    if (!writeOutput(schemaPath, vtkslang::parameterSchema(plan))) {
      std::fprintf(stderr, "vtkslangc: cannot write %s\n", schemaPath.c_str());
      return 1;
    }
  }

  if (tint.empty() && std::getenv("VTKSLANG_TINT"))
    tint = std::getenv("VTKSLANG_TINT");
  if (tint.empty())
    tint = VTKSLANG_TINT_EXECUTABLE;
  if (noTint || a.target != vtkslang::Target::WGSL)
    return 0;
  if (tint.empty()) {
    std::fprintf(stderr, "vtkslangc: no tint to validate the WGSL with (see --tint)\n");
    return 0;
  }
  return validateWithTint(tint, a.wgsl, outputPath == "-" ? std::string() : outputPath) ? 0 : 1;
}
