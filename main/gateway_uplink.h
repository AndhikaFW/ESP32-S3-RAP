#pragma once

#include <cstdint>

// W5500 (SPI Ethernet -> RJ45) side of the Gateway node. Only ever used
// on the node whose provisioned id == kGatewayNodeId; relay nodes never
// touch this module.
namespace gateway_uplink {

// Brings up the SPI bus, W5500 MAC/PHY, and esp_netif with a static IP
// (see config.h kEthStatic*). Assumes esp_netif_init()/
// esp_event_loop_create_default() already ran. Returns false if the
// Ethernet driver could not be installed/started; the chain still runs,
// readings just fail to flush until the link recovers.
bool init();

// Hands one lane's reading to the uplink task, which sends it to the backend
// server over a plain TCP socket. Called once per StatusPacket the Gateway
// originates or receives (see chain_node.cpp) -- there's no more "one flush
// per lap", each reading goes out as soon as it arrives. NEVER blocks: it
// only enqueues (the chain's main loop calls this, so a slow or dead
// Ethernet side must not be able to stall ESP-NOW processing). If the queue
// is full, or the link is down when the task gets to it, the reading is
// dropped and counted -- see logStats().
void flush(uint8_t nodeId, uint8_t streamId, uint8_t occupied, const char *plate);

// One-line uplink health summary (link state, status/video sent/dropped).
void logStats();

// Queues one video frame -- already tagged by video_relay.cpp with which
// node/stream/sequence it is -- for delivery to the backend over a second
// socket, UDP rather than flush()'s TCP (see kVideoBackendPort's comment
// in config.h for why: video can tolerate a dropped frame, and skipping
// TCP's delivery-guarantee machinery buys back real throughput on this
// link). Split into kVideoUdpChunkBytes-sized chunks by gateway_uplink.cpp
// (see VideoChunkHeader there) since a frame is almost always bigger than
// one safe UDP datagram; the backend reassembles (see
// backend/video_listener.py), discarding a frame if its chunks don't all
// arrive within kVideoUdpFrameTimeoutMs. Always takes ownership of `data`
// (heap_caps_malloc'd by the caller): it gets freed here whether the frame
// is actually sent, dropped for a full queue, or dropped because the link
// isn't up / this isn't the Gateway. Safe to call from any task.
void flushVideo(uint8_t originNodeId, uint8_t streamId, uint16_t seq, uint8_t *data, uint32_t dataLen);

}  // namespace gateway_uplink
