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

// Caches a fresh lane reading (motion -> parked-car check -> occupancy, see
// luckfox/parking_detector.py) for later pickup by the local-origination
// timer in loop(). Wired up as luckfox_spi's StatusCallback in
// video_relay::init() -- called from the SPI receiver task's context, so
// this just does a plain store (single-writer via that one task, single
// reader via loop() on the main task; a torn read is at worst one stale
// tick, not worth a lock for that).
void setLaneStatus(uint8_t stream_id, uint8_t occupied, const char *plate);

}  // namespace chain_node
