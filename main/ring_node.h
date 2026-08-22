#pragma once

#include <cstdint>

// Core Ring Access Protocol state machine: neighbor auto-discovery over
// ESP-NOW broadcast, then a single circulating token passed node-to-node
// with per-hop stop-and-wait flow control. See docs/network/topology.md in
// the parent repo for the full protocol write-up.
namespace ring_node {

void init(uint8_t nodeId, uint8_t ringSize);

// Call repeatedly from the main task (e.g. every ~10ms); drains ESP-NOW RX,
// runs HELLO/ACK timers.
void loop();

}  // namespace ring_node
