#pragma once

#include <cstdint>

// Hop-by-hop video relay, separate from the ESP-NOW status/plate chain.
//
// Every node runs a SoftAP (identity "RAP-<node_id>") so its ring "prev"
// neighbor can find and connect to it, plus a TCP server on kVideoPort that
// accepts whatever prev relays. Every node except the Gateway also runs a
// WiFi STA joined to its "next" neighbor's AP and a TCP client that forwards
// frames onward -- both its own local frames (round-robin across the 3
// streams from its paired LuckFox) and whatever it just received from prev,
// interleaved in arrival order through one PSRAM-backed queue (falls back to
// internal heap if PSRAM isn't available, see allocFrameBuffer() in the
// .cpp). The Gateway has no "next": it sinks every frame it receives or
// produces by handing it to gateway_uplink::flushVideo() for delivery to the
// backend over Ethernet, tagged with origin_node_id/stream_id/seq so it's
// always traceable back to the node and camera it came from (see
// gateway_uplink.h).
//
// Requires WiFi already brought up in WIFI_MODE_APSTA with both the AP and
// STA esp_netif created (see main.cpp) -- this only fills in the SSID/
// password/channel and starts the server/client tasks.
namespace video_relay {

void init(uint8_t nodeId, uint8_t chainSize, bool isGateway);

}  // namespace video_relay
