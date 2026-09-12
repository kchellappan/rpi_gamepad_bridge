#include "gpb/capabilities.hpp"

namespace gpb {
namespace {

std::string quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}

std::string array(const std::vector<std::string>& v) {
  std::string out = "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) out += ",";
    out += quote(v[i]);
  }
  return out + "]";
}

}  // namespace

std::string Capabilities::to_json() const {
  return "{\"sink\":" + quote(sink) + ",\"target\":" + quote(target) +
         ",\"trigger_mode\":" + quote(trigger_mode) + ",\"axes\":" + array(axes) +
         ",\"buttons\":" + array(buttons) + ",\"notes\":" + quote(notes) + "}";
}

}  // namespace gpb
