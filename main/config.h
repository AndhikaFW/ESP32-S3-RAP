#pragma once

#include <cstdint>

#include "driver/spi_master.h"

// Node with this id is always the ring's Gateway (has the ENC28J60/RJ45 uplink).
// All other ids 1..(ring_size-1) are plain relay nodes. Every node runs this
// same firmware image; role is decided at runtime from the provisioned id.
constexpr uint8_t kGatewayNodeId = 0;

// Hard cap on ring size, used to size the TokenPacket::entries array.
// Increasing this grows every ESP-NOW packet, so keep it close to the real N.
constexpr uint8_t kMaxRingNodes = 32;

// Fixed WiFi channel all ring nodes use for ESP-NOW. Since there's no AP to
// negotiate a channel, every node must agree on one out of band.
constexpr uint8_t kEspNowChannel = 1;

// -- Discovery (auto-pairing) timing --
constexpr uint32_t kHelloIntervalMs = 500;      // broadcast period while discovering
constexpr uint32_t kHelloIdleIntervalMs = 5000; // slower keep-alive once RUNNING

// -- Ring flow-control timing --
constexpr uint32_t kAckTimeoutMs = 300;   // wait for downstream ACK before retransmit
constexpr uint8_t kMaxRetransmit = 5;     // retries per hop before logging a link warning

// -- NVS provisioning --
constexpr const char *kNvsNamespace = "rap";
constexpr const char *kNvsKeyNodeId = "node_id";
constexpr const char *kNvsKeyRingSize = "ring_size";
constexpr uint8_t kUnprovisioned = 0xFF;

// -- ENC28J60 SPI wiring (Gateway node only) --
constexpr spi_host_device_t kEncSpiHost = SPI2_HOST;
constexpr int kEncSpiClockMhz = 8;
constexpr int kEncCsPin = 10;
constexpr int kEncSckPin = 12;
constexpr int kEncMisoPin = 13;
constexpr int kEncMosiPin = 11;
constexpr int kEncIntPin = 9;

// Locally-administered MAC for the ENC28J60 side; only needs to be unique on
// the LAN segment the gateway's RJ45 plugs into (ENC28J60 has no burned-in
// MAC of its own).
constexpr uint8_t kEncMac[6] = {0x02, 0x52, 0x41, 0x50, 0x00, 0x01};

// -- Gateway uplink target (where collected ring data is sent over Ethernet) --
// TODO: point this at the real ParkingVision backend once known.
constexpr const char *kBackendHost = "192.168.1.100";
constexpr uint16_t kBackendPort = 5000;
