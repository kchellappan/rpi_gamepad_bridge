#pragma once
// Minimal INI-style config. Hand-rolled on purpose: this is the only configuration this
// project needs, and it is not worth a third-party parser dependency on a board we may
// end up cross-compiling for.
//
// Everything is parsed once at startup and baked into plain structs, so config lookups
// never appear in the hot path.

#include <map>
#include <string>

namespace gpb {

class Config {
 public:
  bool load(const std::string& path, std::string& err);

  // Keys are addressed as "section.key".
  std::string get(const std::string& key, const std::string& def = "") const;
  int get_int(const std::string& key, int def = 0) const;
  float get_float(const std::string& key, float def = 0.0f) const;
  bool get_bool(const std::string& key, bool def = false) const;
  bool has(const std::string& key) const;

  const std::map<std::string, std::string>& all() const { return kv_; }

 private:
  std::map<std::string, std::string> kv_;
};

}  // namespace gpb
