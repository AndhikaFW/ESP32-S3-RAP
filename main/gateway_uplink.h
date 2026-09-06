#pragma once

#include <cstdint>

// ENC28J60 (SPI Ethernet -> RJ45) side of the Gateway node. Only ever used
// on the node whose provisioned id == kGatewayNodeId; relay nodes never
// touch this module.
namespace gateway_uplink {

// Brings up the SPI bus, ENC28J60 MAC/PHY, and esp_netif with a static IP
// (see config.h kEthStatic*). Assumes esp_netif_init()/
// esp_event_loop_create_default() already ran. Returns false if the
// Ethernet driver could not be installed/started; the chain still runs,
// readings just fail to flush until the link recovers.
bool init();

// Sends one lane's reading to the backend server over a plain TCP socket.
// Called once per StatusPacket the Gateway originates or receives (see
// chain_node.cpp) -- there's no more "one flush per lap", each reading goes
// out as soon as it arrives. Safe to call even if the link isn't up yet (it
// will just fail fast and log).
void flush(uint8_t nodeId, uint8_t streamId, uint8_t occupied, const char *plate);

// Queues one video frame -- already tagged by video_relay.cpp with which
// node/stream/sequence it is -- for delivery to the backend over a second,
// persistent TCP connection (kept open rather than reconnected per frame
// like flush() above, since video arrives far more often than a reading).
// Always takes ownership of `data` (heap_caps_malloc'd by the caller): it
// gets freed here whether the frame is actually sent, dropped for a full
// queue, or dropped because the link isn't up / this isn't the Gateway.
// Safe to call from any task.
void flushVideo(uint8_t originNodeId, uint8_t streamId, uint16_t seq, uint8_t *data, uint32_t dataLen);

}  // namespace gateway_uplink
