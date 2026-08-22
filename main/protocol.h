#pragma once

#include <cstdint>
#include "config.h"

// Wire format for the ring. All structs are packed and sent as raw bytes
// over esp_now_send()/esp_now_recv(). type is always the first byte so a
// receiver can dispatch before knowing the concrete struct.

enum PacketType : uint8_t {
  kPktHello = 1,
  kPktToken = 2,
  kPktAck = 3,
};

// Discovery broadcast: "I am node_id, part of a ring_size-node ring".
// Broadcast to FF:FF:FF:FF:FF:FF; sender MAC is read from the recv callback.
struct __attribute__((packed)) HelloPacket {
  uint8_t type = kPktHello;
  uint8_t node_id = 0;
  uint8_t ring_size = 0;
};

// One node's contribution to a lap of the ring.
struct __attribute__((packed)) TokenEntry {
  uint8_t node_id = 0;
  uint8_t value = 0;  // placeholder sensor/status reading, see ring_node.cpp
};

// The single circulating token. Each relay appends its own entry in place
// and forwards; the Gateway drains entries[] to Ethernet and starts a new
// lap (cycle_id + 1, count = 0).
struct __attribute__((packed)) TokenPacket {
  uint8_t type = kPktToken;
  uint16_t cycle_id = 0;
  uint8_t count = 0;
  TokenEntry entries[kMaxRingNodes];
};

// Sent immediately by a node back to whoever sent it a TokenPacket, once
// that node has accepted the token into its own (now-occupied) send slot.
// (cycle_id, count) identifies exactly which send is being acknowledged,
// which also makes retransmits of the same token idempotent to detect.
struct __attribute__((packed)) AckPacket {
  uint8_t type = kPktAck;
  uint16_t cycle_id = 0;
  uint8_t count = 0;
};
