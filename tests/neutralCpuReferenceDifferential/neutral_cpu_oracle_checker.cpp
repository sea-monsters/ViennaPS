#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

std::map<std::string, std::string> read(const char *path) {
  std::ifstream stream(path);
  if (!stream)
    return {};
  std::map<std::string, std::string> values;
  std::string line;
  while (std::getline(stream, line)) {
    const auto separator = line.find('=');
    if (separator == std::string::npos)
      continue;
    values.emplace(line.substr(0, separator), line.substr(separator + 1));
  }
  return values;
}

std::vector<std::string> tokens(const std::string &value) {
  std::istringstream stream(value);
  std::vector<std::string> result;
  std::string token;
  while (stream >> token)
    result.push_back(token);
  return result;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: neutral_cpu_oracle_checker reference mod\n";
    return 2;
  }
  const auto reference = read(argv[1]);
  const auto mod = read(argv[2]);
  if (reference.empty() || mod.empty()) {
    std::cerr << "RED: paired oracle output missing\n";
    return 1;
  }
  const std::vector<std::string> exactKeys{
      "precision",          "dimension",          "gridDelta",
      "xExtent",            "yExtent",            "seed",
      "raysPerPoint",       "maxReflections",     "active_scope",
      "empty.velocity.count", "active.velocity.count", "flux.count",
      "geometry.nodes.count", "geometry.lines.count", "process.levelsets",
      "process.metadata_level", "process.flux_cells"};
  for (const auto &key : exactKeys) {
    if (!reference.contains(key) || !mod.contains(key) ||
        reference.at(key) != mod.at(key)) {
      std::cerr << "first divergence key=" << key << '\n';
      return 1;
    }
  }
  const std::vector<std::string> vectorKeys{
      "empty.velocity", "active.velocity", "flux", "geometry.nodes",
      "geometry.lines"};
  for (const auto &key : vectorKeys) {
    const auto refValues = reference.find(key);
    const auto modValues = mod.find(key);
    if (refValues == reference.end() || modValues == mod.end()) {
      std::cerr << "first divergence key=" << key << " (missing)\n";
      return 1;
    }
    const auto refTokens = tokens(refValues->second);
    const auto modTokens = tokens(modValues->second);
    if (refTokens.size() != modTokens.size()) {
      std::cerr << "first divergence key=" << key << " index= count ref="
                << refTokens.size() << " mod=" << modTokens.size() << '\n';
      return 1;
    }
    for (std::size_t i = 0; i < refTokens.size(); ++i) {
      if (refTokens[i] != modTokens[i]) {
        std::cerr << "first divergence key=" << key << " index=" << i
                  << " ref=" << refTokens[i] << " mod=" << modTokens[i]
                  << " max_ulp=0\n";
        return 1;
      }
    }
  }
  std::cout << "paired neutral CPU differential PASS empty=exact active=exact "
               "flux=exact geometry=exact process=exact max_ulp=0\n";
  return 0;
}
