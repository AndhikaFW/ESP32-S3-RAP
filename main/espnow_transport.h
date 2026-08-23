#pragma once

#include <cstddef>
#include <cstdint>

// Thin wrapper around ESP-NOW: WiFi/esp_now bring-up, peer bookkeeping, and
// a queue that moves received packets out of the WiFi task and into the
// chain_node polling loop.
namespace espnow {

using RecvHandler = void (*)(const uint8_t mac[6], const uint8_t *data, size_t len);

// Brings up WiFi STA (no AP join) on a fixed channel and esp_now. Call once
// from app_main, after esp_netif_init()/esp_event_loop_create_default().
bool init();

// Registers the handler invoked from poll() for every received packet.
void setRecvHandler(RecvHandler handler);

// Adds/removes an unencrypted ESP-NOW peer. Safe to call repeatedly with the
// same MAC (a duplicate add is treated as success).
bool addPeer(const uint8_t mac[6]);
bool removePeer(const uint8_t mac[6]);

// Unicast / broadcast send. Both are fire-and-forget at this layer; delivery
// confidence comes from the application-level ACK in the chain protocol
// (chain_node.cpp), not from ESP-NOW's own send callback.
bool send(const uint8_t mac[6], const uint8_t *data, size_t len);
bool sendBroadcast(const uint8_t *data, size_t len);

// Drains the RX queue on the calling task (call from the main loop),
// invoking the registered RecvHandler synchronously for each queued packet.
void poll();

}  // namespace espnow
