// Copyright 2026 Sankhesh Jhaveri
// SPDX-License-Identifier: BSD-3-Clause

#include "vtkSlangReproducer.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace vtkslang {

namespace {

uint64_t hash(const std::string& s)
{
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

bool writeFile(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << text;
  return static_cast<bool>(out);
}

bool readFile(const std::filesystem::path& path, std::string& text)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::stringstream ss;
  ss << in.rdbuf();
  text = ss.str();
  return true;
}

std::pair<std::string, std::string> splitPair(const std::string& text)
{
  const size_t eq = text.find('=');
  if (eq == std::string::npos)
    return { text, {} };
  return { text.substr(0, eq), text.substr(eq + 1) };
}

std::string optionsText(const Reproducer& r)
{
  std::string text;
  if (!r.options.label.empty())
    text += "label " + r.options.label + "\n";
  text += std::string("target ") + (r.options.target == Target::WGSL ? "wgsl" : "spirv") + "\n";
  for (const std::string& e : r.entryPoints)
    text += "entry " + e + "\n";
  for (const auto& d : r.options.defines)
    text += "define " + d.first + "=" + d.second + "\n";
  for (const auto& s : r.options.specializations)
    text += "specialize " + s.first + "=" + s.second + "\n";
  for (const std::string& p : r.options.searchPaths)
    text += "search-path " + p + "\n";
  return text;
}

}

std::string reproducerName(const Reproducer& reproducer)
{
  std::string name = reproducer.options.label;
  if (name.empty()) {
    std::string all = optionsText(reproducer) + '\0' + reproducer.source;
    for (const std::string& m : reproducer.programModules)
      all += '\0' + m;
    for (const std::string& m : reproducer.options.modules)
      all += '\0' + m;
    std::ostringstream ss;
    ss << "compile-" << std::hex << hash(all);
    return ss.str();
  }
  for (char& c : name)
    c = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '-';
  return name;
}

bool writeReproducer(
  const std::string& directory, const Reproducer& reproducer, const Artifact& artifact)
{
  const std::filesystem::path dir(directory);
  std::error_code             ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  if (ec)
    return false;
  bool ok = writeFile(dir / "options.txt", optionsText(reproducer)) &&
    writeFile(dir / "program.slang", reproducer.source) &&
    writeFile(dir / "diagnostics.txt", artifact.diagnostics);
  for (size_t i = 0; ok && i < reproducer.programModules.size(); ++i)
    ok = writeFile(
      dir / ("program-module" + std::to_string(i) + ".slang"), reproducer.programModules[i]);
  for (size_t i = 0; ok && i < reproducer.options.modules.size(); ++i)
    ok = writeFile(dir / ("module" + std::to_string(i) + ".slang"), reproducer.options.modules[i]);
  if (ok && artifact.target == Target::WGSL && !artifact.wgsl.empty())
    ok = writeFile(dir / "output.wgsl", artifact.wgsl);
  if (ok && artifact.target == Target::SPIRV && !artifact.spirv.empty())
    ok = writeFile(dir / "output.spv",
      std::string(reinterpret_cast<const char*>(artifact.spirv.data()),
        artifact.spirv.size() * sizeof(uint32_t)));
  return ok;
}

bool readReproducer(const std::string& directory, Reproducer& reproducer, std::string& error)
{
  const std::filesystem::path dir(directory);
  std::string                 options;
  if (!readFile(dir / "options.txt", options)) {
    error = "no options.txt in " + directory;
    return false;
  }
  reproducer = Reproducer();
  std::istringstream lines(options);
  std::string        line;
  while (std::getline(lines, line)) {
    const size_t      space = line.find(' ');
    const std::string key   = line.substr(0, space);
    const std::string value = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (key == "label")
      reproducer.options.label = value;
    else if (key == "target")
      reproducer.options.target = value == "spirv" ? Target::SPIRV : Target::WGSL;
    else if (key == "entry")
      reproducer.entryPoints.push_back(value);
    else if (key == "define")
      reproducer.options.defines.push_back(splitPair(value));
    else if (key == "specialize")
      reproducer.options.specializations.push_back(splitPair(value));
    else if (key == "search-path")
      reproducer.options.searchPaths.push_back(value);
    else if (!key.empty()) {
      error = "unknown option '" + key + "' in " + (dir / "options.txt").string();
      return false;
    }
  }
  if (!readFile(dir / "program.slang", reproducer.source)) {
    error = "no program.slang in " + directory;
    return false;
  }
  for (size_t i = 0;; ++i) {
    std::string module;
    if (!readFile(dir / ("program-module" + std::to_string(i) + ".slang"), module))
      break;
    reproducer.programModules.push_back(std::move(module));
  }
  for (size_t i = 0;; ++i) {
    std::string module;
    if (!readFile(dir / ("module" + std::to_string(i) + ".slang"), module))
      break;
    reproducer.options.modules.push_back(std::move(module));
  }
  return true;
}

}
