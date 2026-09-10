#include "rgb/config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rgb {
namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

bool Config::load(const std::string& path, std::string& err) {
  std::ifstream in(path);
  if (!in) {
    err = "cannot open config: " + path;
    return false;
  }
  std::string line, section;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    // Strip comments, but only when '#' or ';' starts a token -- device paths and
    // descriptor bytes legitimately contain neither, yet this keeps us honest.
    size_t hash = line.find_first_of("#;");
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;

    if (line.front() == '[') {
      if (line.back() != ']') {
        err = path + ":" + std::to_string(lineno) + ": malformed section header";
        return false;
      }
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      err = path + ":" + std::to_string(lineno) + ": expected key = value";
      return false;
    }
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    kv_[section.empty() ? key : section + "." + key] = val;
  }
  return true;
}

std::string Config::get(const std::string& key, const std::string& def) const {
  auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

bool Config::has(const std::string& key) const { return kv_.count(key) > 0; }

int Config::get_int(const std::string& key, int def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  return static_cast<int>(std::strtol(it->second.c_str(), nullptr, 0));
}

float Config::get_float(const std::string& key, float def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  return std::strtof(it->second.c_str(), nullptr);
}

bool Config::get_bool(const std::string& key, bool def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  const std::string& v = it->second;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace rgb
