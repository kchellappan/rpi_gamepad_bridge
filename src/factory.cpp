#include "rgb/factory.hpp"

#include "rgb/sinks/ns_hid_sink.hpp"
#include "rgb/sources/evdev_source.hpp"
#include "rgb/sources/socket_source.hpp"

namespace rgb {

void register_builtin_sources() {
  SourceRegistry::instance().add("evdev", [](const Config& c) {
    return std::make_unique<EvdevSource>(c);
  });
  SourceRegistry::instance().add("socket", [](const Config& c) {
    return std::make_unique<SocketSource>(c);
  });
}

void register_builtin_sinks() {
  SinkRegistry::instance().add("ns_hid", [](const Config& c) {
    return std::make_unique<NsHidSink>(c);
  });
}

}  // namespace rgb
