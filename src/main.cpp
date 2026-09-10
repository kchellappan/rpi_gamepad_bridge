#include <signal.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "gpb/bridge.hpp"
#include "gpb/factory.hpp"

namespace {
gpb::Bridge* g_bridge = nullptr;
void on_signal(int) {
  if (g_bridge) g_bridge->stop();
}

void usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s --config <file> [--source NAME] [--sink NAME] [--record FILE]\n"
               "\n"
               "  --source/--sink override the config's selection, so one config can be\n"
               "  reused for live play and for socket-driven replay.\n",
               argv0);
}
}  // namespace

int main(int argc, char** argv) {
  std::string config_path, source_override, sink_override, record_override;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* what) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires an argument\n", what);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--config") config_path = next("--config");
    else if (a == "--source") source_override = next("--source");
    else if (a == "--sink") sink_override = next("--sink");
    else if (a == "--record") record_override = next("--record");
    else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
    else { std::fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
  }
  if (config_path.empty()) { usage(argv[0]); return 2; }

  gpb::Config cfg;
  std::string err;
  if (!cfg.load(config_path, err)) {
    std::fprintf(stderr, "config: %s\n", err.c_str());
    return 1;
  }

  gpb::register_builtin_sources();
  gpb::register_builtin_sinks();

  const std::string source_name =
      source_override.empty() ? cfg.get("bridge.source", "evdev") : source_override;
  const std::string sink_name =
      sink_override.empty() ? cfg.get("bridge.sink", "ns_hid") : sink_override;

  auto src = gpb::SourceRegistry::instance().create(source_name, cfg);
  if (!src) {
    std::fprintf(stderr, "unknown source \"%s\"; available:", source_name.c_str());
    for (const auto& n : gpb::SourceRegistry::instance().names())
      std::fprintf(stderr, " %s", n.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }
  auto sink = gpb::SinkRegistry::instance().create(sink_name, cfg);
  if (!sink) {
    std::fprintf(stderr, "unknown sink \"%s\"; available:", sink_name.c_str());
    for (const auto& n : gpb::SinkRegistry::instance().names())
      std::fprintf(stderr, " %s", n.c_str());
    std::fprintf(stderr, "\n");
    return 1;
  }

  // Mapping layer. Parsed once here; nothing in it reads config again at runtime.
  gpb::StickProfile left, right;
  left.deadzone = static_cast<int16_t>(cfg.get_int("profile.left.deadzone", 2000));
  left.saturation = static_cast<int16_t>(cfg.get_int("profile.left.saturation", 32000));
  left.expo = cfg.get_float("profile.left.expo", 1.0f);
  left.invert_x = cfg.get_bool("profile.left.invert_x", false);
  left.invert_y = cfg.get_bool("profile.left.invert_y", false);
  right.deadzone = static_cast<int16_t>(cfg.get_int("profile.right.deadzone", 2000));
  right.saturation = static_cast<int16_t>(cfg.get_int("profile.right.saturation", 32000));
  right.expo = cfg.get_float("profile.right.expo", 1.0f);
  right.invert_x = cfg.get_bool("profile.right.invert_x", false);
  right.invert_y = cfg.get_bool("profile.right.invert_y", false);

  std::vector<std::unique_ptr<gpb::Transform>> transforms;
  transforms.push_back(std::make_unique<gpb::ProfileTransform>(
      left, right, static_cast<uint8_t>(cfg.get_int("profile.trigger_deadzone", 12))));

  gpb::BridgeOptions opts;
  opts.rumble = cfg.get_bool("bridge.rumble", false);
  opts.record_path = record_override.empty() ? cfg.get("bridge.record_path") : record_override;
  opts.record = !opts.record_path.empty();
  opts.record_ring_slots = static_cast<size_t>(cfg.get_int("bridge.record_ring_slots", 8192));
  opts.rt_priority = cfg.get_int("bridge.rt_priority", 0);
  opts.cpu_affinity = cfg.get_int("bridge.cpu_affinity", -1);

  gpb::Bridge bridge(std::move(src), std::move(sink), std::move(transforms), opts);
  if (!bridge.initialize(err)) {
    std::fprintf(stderr, "init failed: %s\n", err.c_str());
    return 1;
  }

  g_bridge = &bridge;
  struct sigaction sa {};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::fprintf(stderr, "[main] %s -> %s%s\n", source_name.c_str(), sink_name.c_str(),
               opts.record ? " (recording)" : "");
  return bridge.run();
}
