#include "gpb/factory.hpp"

#include "gpb/sinks/ns_hid_sink.hpp"
#include "gpb/sources/evdev_source.hpp"
#include "gpb/sources/socket_source.hpp"

namespace gpb {

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

}  // namespace gpb
