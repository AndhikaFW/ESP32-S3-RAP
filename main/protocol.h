#pragma once

#include <cstdint>
#include "config.h"

// Wire format for the chain. All structs are packed and sent as raw bytes
// over esp_now_send()/esp_now_recv(). type is always the first byte so a
// receiver can dispatch before knowing the concrete struct.

enum PacketType : uint8_t {
  kPktHello = 1,
  kPktStatus = 2,
  kPktAck = 3,
};

// Discovery broadcast: "I am node_id, part of an N-node chain".
// Broadcast to FF:FF:FF:FF:FF:FF; sender MAC is read from the recv callback.
struct __attribute__((packed)) HelloPacket {
  uint8_t type = kPktHello;
  uint8_t node_id = 0;
  uint8_t chain_size = 0;
};

// One lane's parking-spot reading: is it occupied, and (if a car is present
// and OCR succeeded downstream) its plate text. Every node covers
// kVideoStreamsPerNode independent lanes (same split as the video streams --
// see luckfox_spi.h/video_relay.cpp), so a reading needs stream_id to say
// which one, exactly like VideoFrameHeader. Every node originates these on
// its own initiative (no shared/collective packet, no waiting for a token to
// pass through) and relays whatever it receives from `prev` toward `next` --
// same pattern as video_relay.cpp. The Gateway has no `next`: it flushes
// every packet it originates or receives straight to Ethernet instead of
// forwarding. Video frames and plate crop images never travel this path --
// they're far too large for ESP-NOW's 250-byte frame limit; only this small
// derived result does (plate crops go out over the video path instead, on a
// reserved stream_id range -- see luckfox_spi.h). See docs/network/topology.md.
//
// plate[] is "" (unresolved) coming straight off LuckFox: the LuckFox Pico
// Mini image has no OCR engine and no package manager to add one (see
// luckfox/parking_detector.py), so plate text recognition happens on the
// backend instead, against the plate crop image sent over the video path.
// This field is wired up but stays empty until that backend step exists.
struct __attribute__((packed)) StatusPacket {
  uint8_t type = kPktStatus;
  uint8_t origin_node_id = 0;
  uint8_t stream_id = 0;                // which of this node's lanes (0..kVideoStreamsPerNode-1)
  uint16_t seq = 0;                     // per-(origin,stream) counter, dedups retransmits
  uint8_t occupied = 0;                 // 0 = kosong, 1 = terisi
  char plate[kMaxPlateLen + 1] = {0};   // OCR'd plate text, "" if none/unreadable/unresolved
};

// Sent immediately by a node back to whoever sent it a StatusPacket, once
// that node has accepted it into its own outgoing queue (or, for the
// Gateway, flushed it to Ethernet). (origin_node_id, seq) identifies exactly
// which send is being acknowledged, which also makes retransmits of the
// same packet idempotent to detect.
struct __attribute__((packed)) AckPacket {
  uint8_t type = kPktAck;
  uint8_t origin_node_id = 0;
  uint16_t seq = 0;
};
