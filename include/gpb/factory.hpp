#pragma once
// String -> creator registries for both base classes, so that adding a source or a sink
// is one new file plus one registration line, and selecting one is a config edit.
//
// Deliberately not clever. A registry is all this needs; the dispatch cost is one
// indirect call per event, which is ~1ns against a latency budget measured in
// milliseconds of USB polling.

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "gpb/config.hpp"
#include "gpb/input_source.hpp"
#include "gpb/output_sink.hpp"

namespace gpb {

template <typename Base>
class Registry {
 public:
  using Creator = std::function<std::unique_ptr<Base>(const Config&)>;

  static Registry& instance() {
    static Registry r;
    return r;
  }

  void add(const std::string& name, Creator c) { creators_[name] = std::move(c); }

  std::unique_ptr<Base> create(const std::string& name, const Config& cfg) const {
    auto it = creators_.find(name);
    return it == creators_.end() ? nullptr : it->second(cfg);
  }

  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : creators_) out.push_back(k);
    return out;
  }

 private:
  std::map<std::string, Creator> creators_;
};

using SourceRegistry = Registry<InputSource>;
using SinkRegistry = Registry<OutputSink>;

// Registers the built-ins. Called once from main().
void register_builtin_sources();
void register_builtin_sinks();

}  // namespace gpb
