#pragma once

#include <cstdint>

// Core chain state machine: neighbor auto-discovery over ESP-NOW broadcast,
// then a chain of independently-originated StatusPackets passed node-to-node
// with per-hop stop-and-wait flow control -- every node (except the Gateway)
// both relays what it receives from `prev` and injects its own reading on
// its own schedule, same pattern as video_relay.cpp. The Gateway has no
// `next`: it flushes everything straight to Ethernet instead of forwarding.
// See docs/network/topology.md for the full write-up.
namespace chain_node {

void init(uint8_t nodeId, uint8_t chainSize);

// Call repeatedly from the main task (e.g. every ~10ms); drains ESP-NOW RX,
// runs HELLO/ACK timers.
void loop();

}  // namespace chain_node
